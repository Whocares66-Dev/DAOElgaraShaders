"""The game's own shaders, changed: what Elgara Shaders draws with.

    python shaders/build_shaders.py <folder to write to>
        [show-occlusion] [show-share] [no-smaa]
    python shaders/build_shaders.py inputs

`inputs` lists, separated by semicolons, every file the build reads (the
game's archives, fxc.exe, the HLSL and SMAA files and the Python it runs):
CMake builds the shaders again when one of them changes, and only then.

Each shader is read from the game's shaders.erf, listed by fxc, turned into
HLSL (tools/shader_hlsl.py), given a function of ours in place of or after
some of its own statements, and compiled again under its own name, so that
the folder, as a plugin's override folder, replaces the game's. Nothing of
the game's is kept here: it is made from the player's own install on every
build.

The shadows (shadow.hlsl, RECEIVERS). The change is the same in every one of
the shaders that receive the sun's shadows, because they were compiled from
one source: the point's side of the plane that divides the two maps is
`dp3 <side>, v<N>, c<ShadowData + 8>`, which says which input is the point in
the world, and the shadow is settled by the first `cmp <lit>, -<...>` after
the last tap of the near map, the choice between the two maps. DaoShadow's
answer is put into <lit> after it, where the plugin says (Improved lighting).

The options' numbers (look.hlsl). What the level's and the figures' shaders
do of ours is by how much the plugin says, in registers of its own: the
relief, the materials, the shadows and the sun's shadow on figures. With no
plugin they are 0 and those shaders draw as the game's.

The baked light (bake.hlsl, with_shaped_bake). Where a level's shader reads
its lightmap, the colour is shaped by the surface's normal map.

The materials (materials.hlsl, with_materials). The shaders of bodies, armour
and weapons that gather their highlights the common way scale them by their
sharpness and their mirror colour by the angle it is seen at.

The picture (grade.hlsl). The game's colour grade reads the picture with
its first `texld <r>, v<N>, s0`, which becomes DaoPicture's reading of it,
darkened by the ambient occlusion, and DaoGrade has the last word on what
the shader writes.

It needs the game (tools/script_commands.py's GAME, or DAO_GAME_DIR) and
fxc.exe of the Windows SDK; without either it says so and writes nothing.
"""

import collections
import concurrent.futures
import ctypes
import ctypes.wintypes
import glob
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
# DAOLib's tools (script_commands.py: where the game is, and its archives):
# where the build says, else the submodule, else the sibling checkout.
for tools in (
    os.environ.get("DAOLIB_TOOLS", ""),
    os.path.join(HERE, "..", "extern", "DAOLib", "tools"),
    os.path.join(HERE, "..", "..", "DAOLib", "tools"),
):
    if tools and os.path.exists(os.path.join(tools, "script_commands.py")):
        sys.path.insert(0, tools)
        break
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
sys.path.insert(0, HERE)
import script_commands  # noqa: E402
import shader_hlsl  # noqa: E402
import smaa  # noqa: E402

# The shaders that read the shadow maps, shader model 3 (read 2026-10-10: the
# twelve of shaders.erf that name smpShadowPartition1).
RECEIVERS = [
    "static1",
    "static1_alpha",
    "static1_fast",
    "static1_punch",
    "static1_relief",
    "terrain1",
    "terrain2",
    "terrain3",
    "prop1",
    "prop1_punch",
    "spt_branch",
    "spt_fronds",
]

# The shaders of the Character material by the start of their names: bodies
# and armour, cloth, faces, weapons, hair and eyes.
CHARACTERS = ("ch1", "ch2", "cloth", "face", "weapon", "hair", "eye")


def find_fxc():
    """The newest fxc.exe of the Windows SDKs installed, or None."""
    kits = os.path.join(
        os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
        "Windows Kits",
        "10",
        "bin",
    )
    found = sorted(glob.glob(os.path.join(kits, "*", "x86", "fxc.exe")))
    return found[-1] if found else None


# The game's archives the build reads, in packages/core/data.
GAME_ARCHIVES = ("shaders.erf", "postprocesseffects.erf", "materials.erf", "states.erf")


def inputs():
    """Every file the build reads, as full paths with forward slashes."""
    data = os.path.join(script_commands.GAME, "packages", "core", "data")
    files = [os.path.join(data, name) for name in GAME_ARCHIVES]
    files.append(find_fxc() or "")
    files += glob.glob(os.path.join(HERE, "*.hlsl"))
    files += glob.glob(os.path.join(HERE, "smaa", "*"))
    files += [
        os.path.abspath(__file__),
        script_commands.__file__,
        shader_hlsl.__file__,
        smaa.__file__,
    ]
    return sorted(
        {
            os.path.abspath(f).replace("\\", "/")
            for f in files
            if f and os.path.isfile(f)
        }
    )


def with_our_shadow(shader):
    """Put DaoShadow's answer where the shader settles its own."""
    data = shader.parameters["g_vShadowData"]
    side = shader.find(rf"^dp3 r\d+\.[xyzw], v\d+, c{data.base + 8}$")
    world = re.search(r"(v\d+),", shader.statements[side].asm).group(1)
    taps = [
        i
        for i, s in enumerate(shader.statements)
        if re.search(r"^texld\w* .*, s7$", s.asm)
    ]
    if not taps:
        raise shader_hlsl.ShaderError("no tap of the near map (s7)")
    choice = shader.find(r"^cmp r\d+\.[xyzw], -", taps[-1])
    lit = re.match(r"cmp (r\d+\.[xyzw]),", shader.statements[choice].asm).group(1)
    shader.add_input("pixel", "float2", "VPOS")
    shader.insert(
        choice,
        [
            f"if (DAO_SHADOWS > 0) {lit} = DaoShadow({world}.xyz, input.pixel);",
            f"daoSunShadow = {lit};",
        ],
    )
    with_shadow_show(shader)


def with_shadow_show(shader):
    """A developer's look at the sun's shadow (look.hlsl's DAO_SHADOW_SHOW):
    the lightmap's alpha, where the bake says the sun reaches, as red, and
    the shadow maps' answer as green, in place of the surface's colour. A
    shader with no lightmap shows the maps' answer alone, red as 1."""
    lightmap = next(
        (
            (i, found.group(1))
            for i, statement in enumerate(shader.statements)
            if (found := re.match(r"(r\d+) = tex2D\(smpLightmap,", statement.hlsl))
        ),
        None,
    )
    if lightmap is not None:
        # Where the shadow maps are fitted to the view, they say where the
        # sun reaches within their reach, not the lightmap's alpha, which was
        # baked with the level's own sun and none of its trees (the user,
        # 2026-10-16: the live sun rules where the two disagree).
        shader.insert(
            lightmap[0],
            [
                f"daoBakedSun = {lightmap[1]}.w;",
                f"{lightmap[1]}.w = lerp({lightmap[1]}.w, 1, daoShadowCovered);",
            ],
        )
    shader.insert(
        len(shader.statements) - 1,
        ["if (DAO_SHADOW_SHOW > 0) oC0.xyz = float3(daoBakedSun, daoSunShadow, 0);"],
    )
    shader.insert(-1, ["float daoBakedSun = 1;", "float daoSunShadow = 1;"])


# How far towards the sun from a figure's surface its shadow is asked for, in
# metres: past the figure's own body, so it is not shaded by itself.
SHADOW_LIFT = 1.5
# What is left of a figure's directional light in full shadow.
SHADOW_LEFT = 0.3

# What the Character material declares for every one of its shaders and none
# of them reads (character.mat.xml's "Common Character Params", read
# 2026-10-10): the shadow maps and their data, as the level's shaders have.
# These are the names it gives the maps, which shadow.hlsl reads them by.
CHARACTER_SHADOW_MAPS = ("smpShadowPartition1", "smpShadowPartition2")

# The names a figure's shader is handed the shadow maps by, where a
# definition that draws it gives them names of its own: by the shader's
# file's name, lower case (find_shadow_maps). The Face and the HairAlpha
# definitions call them smpShadowPart1 and 2, the Cloth and the Eye have
# neither them nor their data, and the Weapon and the Canopy are as the
# Character (read 2026-10-12). A shader that read them by the Character's
# names where they are not given so read whatever the last draw had left
# there: in Redcliffe's sun the lit side of every face was in shadow while
# the neck under it was not, which was a third of a face's light (the user,
# 2026-10-12: "the armor looks really dark. so does char's face").
SHADOW_MAPS = {}

FIGURE_SHADERS = rf"^({'|'.join(CHARACTERS)})\w*\.psh$"


def figure_passes(material):
    """A material definition's passes that a figure's pixel shader draws:
    (where the pass's opening tag ends, the shader's file's name in lower
    case, and all the pass is handed: its own text and that of the parameter
    sets it refers to)."""
    sets = {
        found.group(1): found.group(2)
        for found in re.finditer(
            r'<ParameterSet\s+Name\s*=\s*"([^"]+)"\s*>(.*?)</ParameterSet>',
            material,
            re.DOTALL,
        )
    }
    for found in re.finditer(r"(<Pass\b[^>]*>)(.*?)</Pass>", material, re.DOTALL):
        drawn_by = re.search(r'PixelShader\s*=\s*"([^"]+)"', found.group(1))
        if drawn_by and re.match(FIGURE_SHADERS, drawn_by.group(1), re.IGNORECASE):
            handed = found.group(2) + "".join(
                sets.get(ref, "")
                for ref in re.findall(
                    r'<ParameterSetRef\s+Ref\s*=\s*"([^"]+)"', found.group(2)
                )
            )
            yield found.end(1), drawn_by.group(1).lower() + ".fxo", handed


def shadow_maps_in(handed):
    """The sampler names a pass's parameters give the two shadow maps, or
    None where it is not handed both."""
    names = tuple(
        found.group(1)
        if (
            found := re.search(
                rf'SamplerName\s*=\s*"([^"]+)"\s+Usage\s*=\s*"ShadowTexture_{which}"',
                handed,
            )
        )
        else None
        for which in ("Primary", "Secondary")
    )
    return names if all(names) else None


def find_shadow_maps(materials):
    """Fill SHADOW_MAPS from the game's material definitions. One shader
    handed the maps under two pairs of names is refused."""
    for body in materials.values():
        for _, name, handed in figure_passes(
            body.decode("utf-8-sig", errors="replace")
        ):
            names = shadow_maps_in(handed)
            if names and SHADOW_MAPS.setdefault(name, names) != names:
                raise shader_hlsl.ShaderError(
                    f"{name} is handed the shadow maps as {SHADOW_MAPS[name]} and as {names}"
                )


def shadow_parameters(stem):
    """What a figure's shader declares to read the shadow maps and their
    data: the maps by the names it is handed them by, which shadow.hlsl then
    reads as the Character's."""
    handed = SHADOW_MAPS.get(stem + ".psh.fxo", CHARACTER_SHADOW_MAPS)
    return (
        "\nfloat4 g_vShadowData[9];\n"
        + "".join(f"sampler2D {name};\n" for name in handed)
        + "".join(
            f"#define {ours_} {theirs}\n"
            for ours_, theirs in zip(CHARACTER_SHADOW_MAPS, handed, strict=True)
            if ours_ != theirs
        )
    )


def with_shadow_parameters(material):
    """A material definition as text, the shadow maps and their data added
    to every pass a figure's shader draws that is not handed them, as the
    Character definition hands them to all of its own."""
    added = []
    for at, name, handed in figure_passes(material):
        lines = ""
        if shadow_maps_in(handed) is None:
            for number, (sampler, usage) in enumerate(
                zip(
                    SHADOW_MAPS.get(name, CHARACTER_SHADOW_MAPS),
                    ("Primary", "Secondary"),
                    strict=True,
                ),
                1,
            ):
                lines += (
                    f'\n            <InternalParameter Type="Texture" Name="mml_tShadowPartition{number}"'
                    f' SamplerName="{sampler}" Usage="ShadowTexture_{usage}"/>'
                )
        if not re.search(r'Name\s*=\s*"g_vShadowData"', handed):
            lines += (
                '\n            <InternalParameter Type="InternalDataReference" Name="g_vShadowData"'
                ' Usage="ShadowData"/>'
            )
        if lines:
            added.append((at, lines))
    for at, lines in reversed(added):
        material = material[:at] + lines + material[at:]
    return material


def takes_sun_shadow(stem, listing):
    """Whether a shader is a character's that the sun's shadow can be put on:
    lit by g_vLightParams, and not given the shadow's data already."""
    return (
        stem.startswith(CHARACTERS)
        and "g_vLightParams" in listing
        and "g_vShadowData" not in listing
        and "ps_3_0" in listing
    )


def with_sun_shadow(shader):
    """Shade a character by what stands between it and the sun. A character's
    shader has one directional light, g_vLightParams[2] its direction and [3]
    its colour, and reads no shadow map: a figure under a roof is as lit as
    one in the open. That light is not the sun that casts the shadows; it is
    set to show a figure off, from its own side. Two forms came before this
    (both seen in Redcliffe). The colour multiplied by the shadow at the
    figure's own surface darkened every front, each figure standing in its
    own shadow while its light came from the other side (2026-10-10). The
    light turned to the sun's own direction, with a fill, was true to the
    scene and left a face dark in conversation whenever the sun stood behind
    it, where the game's light is at its best (2026-10-11). So the light is
    left as the engine has it, and its colour is multiplied by the shadow
    asked for a little way towards the sun, past the figure's own body: a
    building's or a tree's shadow falls on the figure, its own does not. The
    point in the world is the input the first point light's direction is
    taken from (`add r, c<lights>, -v<N>`). A shader with no such light in it
    is left as it is."""
    lights = shader.parameters["g_vLightParams"]
    sun = f"{lights.name}[3]"
    uses = [i for i, s in enumerate(shader.statements) if sun in s.hlsl]
    worlds = (
        re.search(rf"^add r\d+\.xyz, c{lights.base}, -(v\d+)$", s.asm)
        for s in shader.statements
    )
    world = next((m.group(1) for m in worlds if m), None)
    if not uses or world is None:
        return
    for use in uses:
        statement = shader.statements[use]
        statement.hlsl = statement.hlsl.replace(sun, f"({sun} * daoSun)")
    # Before the block the first use is in, where every use can see it.
    before = uses[0]
    while shader.statements[before].depth > 0:
        before -= 1
    shader.add_input("pixel", "float2", "VPOS")
    shader.insert(
        before - 1,
        [
            # The near map's depth axis is the way the sun's light travels.
            f"const float3 daoLifted = {world}.xyz - normalize(g_vShadowData[2].xyz) * {SHADOW_LIFT};",
            f"const float daoSun = lerp(1, lerp({SHADOW_LEFT}, 1, DaoShadow(daoLifted, input.pixel)),"
            " DAO_SHADOW_ON_FIGURES);",
        ],
    )


# The lights' highlights are gathered, tripled and joined with the cubemap's
# reflection by one statement: `mad <sum>.xyz, <sum>, c<K>.<n>, <reflection>`,
# c<K>.<n> a 3. The power they were raised to, the specular mask's alpha made
# 1 to 256, is what every `pow <h>, <N.H>, <p>.w` names, set by the last
# statement before the first of them that writes <p>.w.
MATERIAL_POW = r"^pow (r\d+)\.\w, r\d+\.\w, (r\d+)\.w$"
MATERIAL_JOIN = r"^mad(?:_pp)? (r\d+)\.xyz, (r\d+), (c\d+)\.(\w), (r\d+)$"
# What the reflection is looked up by starts from how the surface faces the
# eye: `dp3 <f>.x, -<view>, <normal>`, which is minus N.V.
MATERIAL_FACING = r"^dp3 (r\d+)\.x, -r\d+(?:\.\w+)?, r\d+$"
# The light a surface scatters: `mad <d>.xyz, <light>, <colour>, <rest>`, all
# three registers.
MATERIAL_SPREAD = r"^mad(?:_pp)? (r\d+)\.xyz, (r\d+)(?:\.\w+)?, (r\d+), (r\d+)$"

# What a material says it is, to the shaders with_materials changes: put into
# the game's material definitions for each pass drawn by such a shader, with
# what they are when a material object says nothing (no metal; a map of all
# white). A material object says them in its own file, which the plugin's
# build writes from data/materials.ini. That the engine hands a shader a
# parameter of our own so was seen 2026-10-12 (dev/NOTES.md).
# (The five the table's materials are of; the build gives the parameters to
# every definition a shader of MATERIAL_SHADERS draws for.)
MATERIAL_DEFINITIONS = [
    "static.mat.xml",
    "prop.mat.xml",
    "character.mat.xml",
    "weapon.mat.xml",
    "cloth.mat.xml",
]
MATERIAL_PARAMETERS = (
    '<Parameter Type = "Float" Name = "mml_fDaoMetal" Default = "0.0"/>'
    '<Parameter Type = "Texture" Name = "mml_tDaoMetal" SamplerName = "smpDaoMetal" Default = "Default_White"/>'
    '<Parameter Type = "Vector4f" Name = "mml_vDaoBumps" Default = "0 0 0 0"/>'
    '<Parameter Type = "Vector4f" Name = "mml_vDaoBumpsFar" Default = "0 0 0 0"/>'
    '<Parameter Type = "Vector4f" Name = "mml_vDaoClear" Default = "0 0 0 0"/>'
)
MATERIAL_SHADERS = (
    r"^(ch[12](?!\w*_vl)\w*|weapon1|cloth1|prop1(_punch)?|propalpha1|static1\w*)\.psh$"
)


# What a pass's render state does with the alpha its shader writes, which is
# what decides whether that alpha can carry the ambient share
# (with_ambient_share):
#   plain  nothing: the alpha goes into the picture as it is written
#   test   draws a pixel only where the alpha is some least or more (a cut-out)
#   blend  mixes the pixel into the picture by it
PLAIN, TEST, BLEND = "plain", "test", "blend"


def state_values(state):
    """A render state's file as {state: value}."""
    return dict(re.findall(r'State\s*=\s*"(DS_\w+)"\s+Value\s*=\s*"([^"]+)"', state))


def pass_kind(state):
    """(kind, least): what a render state does with a shader's alpha, and for
    a test the least alpha, of 255, it lets through. A state that is not to
    be had, or tests in a way the game's cut-outs do not (on, greater or
    equal, a reference said), is taken to blend: its alpha is left alone."""
    if state is None:
        return BLEND, None
    values = state_values(state)
    if values.get("DS_ALPHABLENDENABLE", "TRUE").upper() != "FALSE":
        return BLEND, None
    if values.get("DS_ALPHATESTENABLE", "FALSE").upper() != "TRUE":
        return PLAIN, None
    if (
        values.get("DS_ALPHAFUNC", "").upper() != "CMP_GREATEREQUAL"
        or "DS_ALPHAREF" not in values
    ):
        return BLEND, None
    return TEST, int(values["DS_ALPHAREF"])


def without_alpha_test(state):
    """A render state's file under our name for it, with its alpha test off:
    for a pass whose shader cuts itself out."""
    state, count = re.subn(
        r'(State\s*=\s*"DS_ALPHATESTENABLE"\s+Value\s*=\s*")TRUE(")',
        r"\1FALSE\2",
        state,
    )
    if count != 1:
        raise shader_hlsl.ShaderError("a cut-out's state has no alpha test to take out")
    return re.sub(r'(<MaterialState\s+Name\s*=\s*")', r"\1Dao", state, count=1)


def with_share_kept(state):
    """A blending render state's file under our name for it, with the
    picture's alpha blended by itself: what is drawn takes the share under it
    away by its own opacity (the alpha that is there, times one less the
    alpha drawn), in place of mixing its opacity into it. What is drawn
    blended is not in the distance the occlusion reads, so where it is solid
    nothing of the occlusion behind it is to show on it. Nothing (None) for a
    state that does not mix by its alpha the common way, or that blends its
    alpha by itself already."""
    values = state_values(state)
    if (
        values.get("DS_SRCBLEND", "").upper() != "BLEND_SRCALPHA"
        or values.get("DS_DESTBLEND", "").upper() != "BLEND_INVSRCALPHA"
        or "DS_SEPARATEALPHABLENDENABLE" in values
    ):
        return None
    state, count = re.subn(
        r'(<DeviceState\s+State\s*=\s*"DS_DESTBLEND"[^>]*>)',
        r"""\1
    <DeviceState State="DS_SEPARATEALPHABLENDENABLE" Value="TRUE" />
    <DeviceState State="DS_SRCBLENDALPHA" Value="BLEND_ZERO" />
    <DeviceState State="DS_DESTBLENDALPHA" Value="BLEND_INVSRCALPHA" />
    <DeviceState State="DS_BLENDOPALPHA" Value="BLENDOP_ADD" />""",
        state,
        count=1,
    )
    if count != 1:
        return None
    return re.sub(r'(<MaterialState\s+Name\s*=\s*")', r"\1Dao", state, count=1)


# How the game draws each shader that can say a share (MATERIAL_SHADERS), by
# its file's name, lower case: the kinds of its passes over all the game's
# material definitions; and for one that is drawn as a cut-out the least
# alpha, and whether every definition that draws it so has a depth pass that
# is cut by the texture too (DEPTH_VERTICES), so that it is in the distance
# the occlusion reads.
USES = {}
CUTS = {}


# The shaders that can say an ambient share: those of the materials' model,
# and a face's, hair's and an eye's, which say it of what they scatter
# (with_scattered_share).
# Hair says no share of its own: it lies on the head, takes the head's
# occlusion, and leaves the share under it (Graphics.cpp's HoldHairBack).
SHARE_SHADERS = MATERIAL_SHADERS[: -len(r")\.psh$")] + r"|face[02]|eye0)\.psh$"


def passes_of(material):
    """A material definition's passes for shader model 3 that a shader which
    can say a share draws: (the pass's text, its pixel shader, its state)."""
    for found in re.finditer(r"<Pass\b[^>]*>", material):
        drawn_by = re.search(r'PixelShader\s*=\s*"([^"]+)"', found.group(0))
        state = re.search(r'State\s*=\s*"([^"]+)"', found.group(0))
        if (
            drawn_by
            and state
            and re.match(SHARE_SHADERS, drawn_by.group(1), re.IGNORECASE)
        ):
            yield found.group(0), drawn_by.group(1), state.group(1)


def state_text(states, state):
    body = states.get(state.lower() + ".xml")
    return None if body is None else body.decode("utf-8-sig", errors="replace")


def find_uses(materials, states):
    """Fill USES and CUTS from the game's material definitions and render
    states. One shader cut out at two different leasts is refused."""
    seen = {name for _, _, names, _ in DEPTH_VERTICES for name in names}
    for definition, body in materials.items():
        for _, drawn_by, state in passes_of(body.decode("utf-8-sig", errors="replace")):
            name = drawn_by.lower() + ".fxo"
            kind, least = pass_kind(state_text(states, state))
            USES.setdefault(name, set()).add(kind)
            if kind == TEST:
                had = CUTS.setdefault(name, (least, True))
                if had[0] != least:
                    raise shader_hlsl.ShaderError(
                        f"{name} is cut out at {had[0]} and at {least}"
                    )
                CUTS[name] = (least, had[1] and definition in seen)


def own_alpha(name):
    """What a shader's file under its own name does with its alpha: says the
    share where every pass that draws it is plain, and is left as the game's
    where any blends or tests, or none is known."""
    return "share" if USES.get(name) == {PLAIN} else "keep"


def with_pass_variants(material, states, made):
    """A material definition as text, each pass drawn by a shader that can
    say a share given what its own state allows:
      a plain pass of a shader that is also drawn blended or cut out is
        pointed at that shader's copy which says its share (DaoShare<name>);
      a cut-out's pass at the shader's copy which cuts itself out
        (Dao<name>) and a copy of its state with the alpha test off, the
        alpha being wanted for the share;
      a blended pass at a copy of its state which keeps the share
        (with_share_kept).
    The states' copies are put in `made` by their files' names."""

    def pointed(found):
        text = found.group(0)
        this = next(iter(passes_of(text)), None)
        if this is None:
            return text
        _, drawn_by, state = this
        name = drawn_by.lower() + ".fxo"
        theirs = state_text(states, state)
        kind, _ = pass_kind(theirs)
        shader, ours_ = None, None
        if kind == PLAIN and own_alpha(name) != "share":
            shader = "DaoShare" + drawn_by
        elif kind == TEST:
            shader, ours_ = "Dao" + drawn_by, without_alpha_test(theirs)
        elif kind == BLEND and theirs is not None:
            ours_ = with_share_kept(theirs)
        if ours_ is not None:
            made["dao" + state.lower() + ".xml"] = ours_
            text = text.replace(f'"{state}"', f'"Dao{state}"', 1)
        if shader is not None:
            text = text.replace(f'"{drawn_by}"', f'"{shader}"', 1)
        return text

    return re.sub(r"<Pass\b[^>]*>", pointed, material)


def with_material_parameters(material):
    """A material definition as text, MATERIAL_PARAMETERS added to every
    pass a shader of MATERIAL_SHADERS draws."""

    def added(found):
        drawn_by = re.search(r'PixelShader\s*=\s*"([^"]+)"', found.group(0))
        if drawn_by and re.match(MATERIAL_SHADERS, drawn_by.group(1), re.IGNORECASE):
            return found.group(0) + MATERIAL_PARAMETERS
        return found.group(0)

    return re.sub(r"<Pass\b[^>]*>", added, material)


# The shaders with_materials changed, for the build to say: for each its name,
# and whether its scattered light and its vertex's falloff were found too.
MATERIALS_REACHED = []


def with_materials(shader):
    """Light a surface by the standard model (materials.hlsl): its colour, how
    much it is metal, which its material says, and its roughness, which is
    its highlight's power.

    Vanilla builds the mirrored light the same way in every shader of its
    common block, bodies, weapons, props and level surfaces (read
    2026-10-12): `pow(N.H, power)` for each light, the power the specular
    mask's alpha; minus N.V for the reflection, and the area's cube read; the
    lights' sum times 3 plus the reflection (the join); that times the mask's
    colour (the tint), times a falloff of the vertex's; and the scattered
    light times the surface's colour, before the tint or after it. A shader
    that has not all of the first four is left as it is; the rest are changed
    where they are found, and the build says which shaders were changed whole
    (MATERIALS_REACHED)."""
    st = shader.statements
    try:
        raised = shader.find(MATERIAL_POW)
    except shader_hlsl.ShaderError:
        return
    held = re.match(MATERIAL_POW, st[raised].asm).group(2)
    lobes = [
        i
        for i, each in enumerate(st)
        if (m := re.match(MATERIAL_POW, each.asm)) and m.group(2) == held
    ]
    try:
        facing = shader.find(MATERIAL_FACING, lobes[-1])
        join = shader.find(MATERIAL_JOIN, facing)
    except shader_hlsl.ShaderError:
        return
    power = next(
        (i for i in range(raised, -1, -1) if re.match(rf"\w+ {held}\.w,", st[i].asm)),
        None,
    )
    total, lights, constant, component, mirrored = re.match(
        MATERIAL_JOIN, st[join].asm
    ).groups()
    tripled = shader.definitions.get(constant)
    if (
        power is None
        or st[power].depth != 0
        or st[facing].depth != 0
        or st[join].depth != 0
        or tripled is None
        or float(tripled["xyzw".index(component)]) != 3
    ):
        return
    tinted = rf"^mul(?:_pp)? (r\d+)\.xyz, (?:{total}, (r\d+)|(r\d+), {total})$"
    try:
        tint = shader.find(tinted, join)
    except shader_hlsl.ShaderError:
        return
    made, *colours = re.match(tinted, st[tint].asm).groups()
    colour = next(g for g in colours if g)
    # The vertex's falloff, on the tint's result: alone, or with the
    # scattered light added in the same statement.
    falloff = next(
        (
            i
            for i in range(tint + 1, min(tint + 3, len(st)))
            if re.match(rf"^mul(?:_pp)? {made}\.xyz, {made}, v\d+\.w$", st[i].asm)
            or re.match(rf"^mad(?:_pp)? r\d+\.xyz, {made}, v\d+\.w, r\d+$", st[i].asm)
        ),
        None,
    )
    # The scattered light, by what it feeds: added to the tint's result after
    # it (`mad <made>, <light>, <colour>, <made>`), or made before it and
    # added by the falloff's own statement (`mad <d>, <made>, v.w, <rest>`:
    # the last `mad <rest>, <light>, <colour>, ...` before the tint). By its
    # shape alone the armour-tint shaders, which most suits are drawn by, gave
    # up the statement that mixes the tint into the colour instead (found
    # 2026-10-12: they had no scattered light changed and no colour told).
    last = falloff if falloff is not None else tint
    spread = next(
        (
            i
            for i in range(tint + 1, min(last + 3, len(st)))
            if re.match(
                rf"^mad(?:_pp)? {made}\.xyz, r\d+(?:\.\w+)?, r\d+, {made}$", st[i].asm
            )
        ),
        None,
    )
    if spread is None and falloff is not None:
        fed = re.match(
            rf"^mad(?:_pp)? r\d+\.xyz, {made}, v\d+\.w, (r\d+)$", st[falloff].asm
        )
        if fed:
            spread = next(
                (
                    i
                    for i in range(tint - 1, join, -1)
                    if re.match(
                        rf"^mad(?:_pp)? {fed.group(1)}\.xyz, r\d+(?:\.\w+)?, r\d+, r\d+$",
                        st[i].asm,
                    )
                ),
                None,
            )
    if spread is not None and " + " not in st[spread].hlsl:
        spread = None
    # The surface's colour, which a metal mirrors in: whole when the tint
    # reads it, or left out.
    own = "0"
    if spread is not None:
        albedo = re.match(MATERIAL_SPREAD, st[spread].asm).group(3)
        if not any(
            re.match(rf"\w+ {albedo}[\.,]", st[i].asm)
            for i in range(min(spread, tint), max(spread, tint))
        ):
            own = f"{albedo}.xyz"
    faces = re.match(MATERIAL_FACING, st[facing].asm).group(1)
    into = st[tint].hlsl.partition(" = ")[0]
    ambient = next(
        (
            register
            for _, _, semantic, register, _ in shader.inputs
            if semantic.startswith("COLOR")
        ),
        None,
    )
    lightmapped = "smpLightmap" in shader.parameters
    # Each change with its place; made last place first, so the places stay.
    changes = []
    # How much of it is metal: the material's word, at the place on the
    # surface the specular mask was read at.
    mask = next(
        (
            found
            for i in range(power, -1, -1)
            if (
                found := re.match(
                    r"^r\d+ = tex2D\(smpSpecularMask, (.+)\);", st[i].hlsl
                )
            )
        ),
        None,
    )
    if mask:
        changes.append(
            (
                power,
                "after",
                f"daoMetal = DaoMetal({mask.group(1)}, mml_fSpecularReflectionMult);",
            )
        )
    # A shield's or a banner's arms are paint laid over it as it is drawn
    # (smpHeraldry; no material's own textures have them): where they lie the
    # surface is no metal. The heraldry's alpha is how much of the surface
    # under it shows, by which the game itself takes the mirror colour away
    # there (weapon1.psh, read 2026-10-12).
    for i in range(power + 1, tint):
        arms = re.match(r"^(r\d+) = tex2D\(smpHeraldry, .+\);$", st[i].hlsl)
        if arms and st[i].depth == 0:
            changes.append((i, "after", f"daoMetal *= {arms.group(1)}.w;"))
    # The power the highlights and the reflection are made by, calmed where
    # the normal turns fast (DaoCalmed). The normal is the one the facing
    # reads; where it is whole before the first light, the lights' lobes are
    # calmed too, and otherwise the reflection alone.
    flat = next(
        (
            found.group(2)
            for i in range(facing)
            if (
                found := re.match(
                    r"^mad(?:_pp)? (r\d+)\.xyz, \1\.z, (v\d+), ", st[i].asm
                )
            )
        ),
        None,
    )
    # Where the normal map is read, for what it adds to a lobe's width from
    # afar (DaoBumps).
    bumped = next(
        (
            f"DaoBumps({found.group(1)})"
            for i in range(facing)
            if (found := re.match(r"^r\d+ = tex2D\(smpNormalMap, (.+)\);", st[i].hlsl))
        ),
        "0",
    )
    seen = re.match(r"^\S+ = dot\(-(.+), (r\d+)\.xyz\);$", st[facing].hlsl)
    lobed = "daoPower"
    mirrored_by = "daoPower"
    if seen:
        normal = seen.group(2)
        writes = [
            i
            for i in range(facing)
            if re.match(rf"\w+ {normal}(?:\.[xyzw]*[xyz][xyzw]*)?,", st[i].asm)
        ]
        whole = writes[-1] if writes else None
        if whole is not None and whole < lobes[0] and st[whole].depth == 0:
            changes.append(
                (
                    max(whole, power),
                    "after",
                    f"daoCalm = DaoCalmed(daoPower, {normal}.xyz, {bumped});",
                )
            )
            lobed = "daoCalm"
        else:
            changes.append(
                (
                    facing,
                    "after",
                    f"daoCalm = DaoCalmed(daoPower, {normal}.xyz, {bumped});",
                )
            )
        mirrored_by = "daoCalm"
    if spread is not None:
        lit_into, _, lit = st[spread].hlsl.partition(" = ")
        scattered, _, rest = lit.rpartition(" + ")
        changes.append(
            (
                spread,
                "set",
                f"{lit_into} = ({scattered}) * DaoScattered(daoMetal) + {rest}",
            )
        )
    if falloff is not None:
        changes.append(
            (
                falloff,
                "set",
                re.sub(r"(v\d+)\.www", r"DaoFalloff(\1.w)", st[falloff].hlsl, count=1),
            )
        )
    changes.append(
        (
            join,
            "set",
            f"daoLights = {lights}.xyz * DaoTripled(); daoEnv = {mirrored}.xyz; "
            f"{total}.xyz = daoLights + daoEnv;",
        )
    )
    changes.append((facing, "after", f"daoFacing = -{faces}.x;"))
    # The reflection's reading of the area's cube, between the two.
    for i in range(facing, join):
        cube = re.match(r"^(r\d+) = texCUBElod\((\w+), (r\d+)\.xyzw\);$", st[i].hlsl)
        if cube and st[i].asm.startswith("texldl"):
            changes.append(
                (
                    i,
                    "set",
                    f"{cube.group(1)} = DaoReflected({cube.group(2)}, {cube.group(3)}.xyzw, {mirrored_by});",
                )
            )
    # Each light's highlight, and its own weight towards the edge (DaoEdge):
    # by V.H, which is L.H. The half vector is the last `nrm` before the
    # lobe, of a sum whose first part is the way to the eye at length 1 (the
    # vertex's, times a scale: `mad <sum>.xyz, v<N>, <scale>, <light>`).
    # And, at the first light every pixel has, how the surface itself faces
    # the eye, by its vertices' normal and not its normal map's (the input
    # the map's own z is multiplied by: `mad <sum>.xyz, <sum>.z, v<N>, ...`):
    # what the surroundings' reflection is weighed by towards the edge
    # (DaoMirror's `facing`). A normal map's facets are drawn at the size of
    # the flat surface they lie on, those turned from the eye as large as
    # those turned to it, where on a real surface they would be mostly out of
    # sight: weighed each by its own facing, a floor seen low was streaked
    # white facet by facet (the tower's, seen 2026-10-12). Vanilla weighs its
    # own falloff by the vertex too.
    faced = "daoFacing"
    edged = 0
    for count, lobe in enumerate(lobes):
        written = re.match(r"^(\S+) = pow\((.+), \S+\);$", st[lobe].hlsl)
        if not written:
            continue
        changes.append(
            (lobe, "set", f"{written.group(1)} = DaoLobe({written.group(2)}, {lobed});")
        )
        since = lobes[count - 1] if count else power
        half = next(
            (
                i
                for i in range(lobe - 1, since, -1)
                if re.match(r"^nrm(?:_pp)? r\d+\.xyz, r\d+$", st[i].asm)
            ),
            None,
        )
        if half is None or st[half].depth != st[lobe].depth:
            continue
        halved, summed = re.match(
            r"^nrm(?:_pp)? (r\d+)\.xyz, (r\d+)$", st[half].asm
        ).groups()
        sum_at = next(
            (
                i
                for i in range(half - 1, since, -1)
                if re.match(
                    rf"^mad(?:_pp)? {summed}\.xyz, v\d+, r\d+\.[xyzw], \S+$", st[i].asm
                )
            ),
            None,
        )
        if sum_at is None or st[sum_at].depth != st[lobe].depth:
            continue
        eye = st[sum_at].hlsl.partition(" = ")[2].rpartition(" + ")[0]
        changes.append(
            (half, "after", f"daoEdgeNow = DaoEdge(dot({halved}.xyz, {eye}));")
        )
        if flat and faced == "daoFacing" and st[lobe].depth == 0:
            changes.append(
                (half, "after", f"daoFlat = abs(dot(normalize({flat}.xyz), {eye}));")
            )
            faced = "daoFlat"
        changes.append(
            (
                lobe,
                "after",
                f"daoEdged += {written.group(1)} * daoEdgeNow; daoLobes += {written.group(1)};",
            )
        )
        edged += 1
    changes.append(
        (
            tint,
            "set",
            f"{into} = DaoMirror(daoLights, daoEnv * DaoLocal(daoLocal), {colour}.xyz, {own}, daoMetal, {faced}, {mirrored_by}, "
            "daoEdged / max(daoLobes, 1e-6), mml_fSpecularReflectionMult);",
        )
    )
    changes.append((power, "after", f"const float daoPower = DaoPower({held}.w);"))
    # What the ambient share is made of (with_ambient_share): the surface's
    # colour and all it sends to the eye, as they stand after the last of the
    # three; and its ambient light, a figure's or a prop's first colour input.
    if spread is not None:
        albedo = re.match(MATERIAL_SPREAD, st[spread].asm).group(3)
        after = max(spread, tint, falloff if falloff is not None else tint)
        result = st[after].hlsl.partition(" = ")[0]
        if not any(
            re.match(rf"\w+ {albedo}[\.,]", st[i].asm)
            for i in range(spread + 1, after + 1)
        ):
            changes.append(
                (after, "after", f"daoAlbedo = {albedo}.xyz; daoLit = {result};")
            )
            shader.share = f"{ambient}.xyz" if ambient else None
    # At one place, what is put in after it goes in first: it is then after
    # what is set there.
    # (Of two put in after one place, the one that names daoPower's making
    # goes in last, so it stands first.)
    for place, how, hlsl in sorted(
        changes,
        key=lambda c: (-c[0], c[1] != "after", c[2].startswith("const float daoPower")),
    ):
        if how == "set":
            shader.statements[place].hlsl = hlsl
        else:
            shader.insert(place, [hlsl])
    shader.insert(
        -1,
        [
            "float3 daoEnv = 0;",
            "float3 daoLights = 0;",
            "float daoMetal = 0;",
            "float daoFacing = 1;",
            "float daoCalm = 1;",
            "float daoEdgeNow = 0;",
            "float daoFlat = 1;",
            # The light that falls where the surface is (DaoLocal): a
            # figure's or a prop's ambient light, its first colour input; a
            # level's baked light, which with_shaped_bake says once it is
            # read (shader.local is what is still owed).
            f"float3 daoLocal = {ambient + '.xyz' if ambient and not lightmapped else '1'};",
            "float daoEdged = 0;",
            "float daoLobes = 0;",
            "float3 daoAlbedo = 0;",
            "float3 daoLit = 1;",
            "float3 daoBaked = 0;",
        ],
    )
    shader.local = lightmapped
    shader.mirrored = tint
    shader.materials = (spread is not None, falloff is not None and edged == len(lobes))


# A figure's normal with its normal map is `nrm <normal>.xyz, <sum>`, the sum
# ended by the map's own z times the vertex's normal: `mad <sum>.xyz, <sum>.z,
# v<N>, <sum>.xyww`.
FIGURE_NORMAL = r"^mad (r\d+)\.xyz, \1\.z, (v\d+), \1\.xyww$"


# The shaders of the things a level is furnished with, which have a normal
# map and no lightmap: their ambient light comes as a figure's does. The
# third is what a prop drawn blended is drawn by (Prop.mat's `Blend`: a
# scarecrow, a straw dummy, shackles, a gong, a rack, spiders' webs, 29
# materials). It was left as the game has it until 2026-10-12, so such a
# prop was lit by the engine's lamps at their full strength and by the
# game's formula for a highlight beside props lit by the look's and the
# materials' model. It reads no shadow map, in the game or here.
PROPS = ("prop1", "prop1_punch", "propalpha1")


def with_ambient_relief(shader):
    """Shape a figure's or a prop's ambient light by its normal map, as a
    level's baked light is shaped (bake.hlsl). The ambient light is a colour the vertex
    shader works out for each vertex and hands down, the one input declared a
    colour that is added to the lights' sum (`add <sum>.xyz, <sum>, v<C>`);
    the normal map has no say in it, so armour away from any lamp is as flat
    as a wall in shade. A shader with no normal map, or that takes its ambient
    light another way, is left as it is."""
    try:
        flat_at = shader.find(FIGURE_NORMAL)
    except shader_hlsl.ShaderError:
        return
    total, flat = re.match(FIGURE_NORMAL, shader.statements[flat_at].asm).groups()
    made = re.match(r"nrm (r\d+)\.xyz, (r\d+)$", shader.statements[flat_at + 1].asm)
    if not made or made.group(2) != total or shader.statements[flat_at].depth != 0:
        return
    colours = [
        register
        for _, _, semantic, register, _ in shader.inputs
        if semantic.startswith("COLOR")
    ]
    for index, statement in enumerate(shader.statements):
        added = re.match(
            r"add(?:_pp)? r\d+\.\w+, r\d+(?:\.\w+)?, (v\d+)(?:\.\w+)?$",
            statement.asm,
        )
        if not added or added.group(1) not in colours or index < flat_at:
            continue
        into, _, read = statement.hlsl.partition(" = ")
        ambient = f"{added.group(1)}.xyz"
        if ambient not in read:
            return
        statement.hlsl = f"{into} = " + read.replace(
            ambient, f"({ambient} * daoAmbient)", 1
        )
        shader.insert(
            flat_at + 1,
            [
                f"const float daoAmbient = DaoReliefFactor(DaoRelief({made.group(1)}.xyz, {flat}.xyz, 0));"
            ],
        )
        return


# A figure's point lights as the engine hands them, each a place with its
# reach in w and a colour: g_vLightParams[0] and [1], [5] and [6], [7] and [8],
# [9] and [11].
POINT_LIGHTS = ((0, 1), (5, 6), (7, 8), (9, 11))


def with_fill_strength(shader):
    """Let the plugin say how strong an area's fills are on what it lights
    (fills.hlsl): each point light's colour, wherever it is read, multiplied
    by DaoFillKept of its reach, which is 1 for every light but a fill."""
    lights = shader.parameters["g_vLightParams"].name
    reached = False
    for statement in shader.statements:
        for place, colour in POINT_LIGHTS:
            if f"{lights}[{colour}]" in statement.hlsl:
                reached = True
                statement.hlsl = statement.hlsl.replace(
                    f"{lights}[{colour}]",
                    f"({lights}[{colour}] * DaoFillKept({lights}[{place}].w))",
                )
    if reached:
        FILLS_KEPT.append(shader)


FILLS_KEPT = []


# How an instruction reads its sources' components: one for each of the
# destination's, by position (the default), all of the first three or four
# whatever the destination's, or each named once (a scalar's).
_READS_THREE = {"dp3", "nrm", "crs"}
_READS_FOUR = {"dp4", "texld", "texldl", "texldb", "texldp", "texldd"}
_READS_SCALAR = {"rcp", "rsq", "exp", "log", "pow", "sincos", "expp", "logp"}
_CONTROL = re.compile(
    r"^(if|else|endif|loop|endloop|rep|endrep|break|breakc|call|ret|label|texkill)\b"
)


def _operand(text):
    """A register and its components as an operand names them: (name, the
    components in order, padded to four as the swizzle repeats its last)."""
    text = text.strip().lstrip("-")
    text = re.sub(r"_(abs|bias|bx2|x2|dz|dw)$", "", text)
    m = re.match(r"^([a-zA-Z]+\d*)(?:\[[^\]]*\])?(?:\.([xyzw]+))?$", text)
    if not m:
        return None, ""
    swizzle = m.group(2) or "xyzw"
    return m.group(1), (swizzle + swizzle[-1] * 4)[:4]


def _accesses(asm):
    """What one instruction writes and reads: ({register: components written},
    {register: components read})."""
    asm = asm.strip()
    if not asm or _CONTROL.match(asm):
        reads = {}
        for name in re.findall(r"\b([rcvb]\d+)\b", asm):
            reads.setdefault(name, set()).update("xyzw")
        return {}, reads
    op, _, rest = asm.partition(" ")
    op = op.split("_")[0]
    operands = [o for o in rest.split(",") if o.strip()]
    if not operands:
        return {}, {}
    dest, _ = _operand(operands[0])
    mask = re.search(r"\.([xyzw]+)", operands[0])
    mask = mask.group(1) if mask else "xyzw"
    writes = {dest: set(mask)} if dest else {}
    reads = {}
    for operand in operands[1:]:
        name, swizzle = _operand(operand)
        if name is None:
            continue
        if op in _READS_THREE:
            comps = set(swizzle[:3])
        elif op in _READS_FOUR:
            comps = set(swizzle)
        elif op in _READS_SCALAR:
            named = re.search(r"\.([xyzw]+)", operand)
            comps = set(named.group(1)) if named else set("xyzw")
        else:
            comps = {swizzle["xyzw".index(c)] for c in mask}
        reads.setdefault(name, set()).update(comps)
    return writes, reads


def _hlsl_reads(hlsl):
    """The temporaries an added statement of ours reads, by its HLSL."""
    reads = {}
    for name, comps in re.findall(r"\b(r\d+)(?:\.([xyzw]+))?", hlsl):
        reads.setdefault(name, set()).update(comps or "xyzw")
    return reads


def with_effect_lights(shader):
    """Every effect's light on this surface (effect_lights.hlsl): the
    shader's own reckoning of one point light, run once more for each light
    in daoEffectLights, its share added to the sums that light was added to.

    The light whose reckoning is run is the one the engine gives the effects'
    lights to, g_vLightParams [9] and [11] (its unflagged one), where the
    shader has it, else its first, [0] and [1] (terrain, hair, eyes: one
    light or three). The reckoning is the statements from the first reading
    of the light's place to the last reading of its colour, with any branch
    begun among them to its end; what they write is kept in locals of the
    loop's own, and of that what the rest of the shader reads (the light's
    diffuse and specular sums) starts at nothing and is added on. The engine
    is kept from choosing the effects' lights for anything itself
    (EffectLights.cpp), so none is counted twice.

    In the copy, the engine's word that its light [9] is there
    (g_bShaderControlBooleans[0], set exactly when it is: logged 2026-10-17)
    is true, every light of the loop being there; and a texture read in the
    reckoning (a face's scattering table) is read at its first level, a loop
    as long as the lights allowing no gradient. A shader that reckons no
    point light whose sum it uses (water; the probes' projection) is left
    as it is."""
    lights = shader.parameters["g_vLightParams"]
    base = lights.base
    statements = shader.statements
    for place, colour in ((9, 11), (0, 1)):
        if colour >= lights.size:
            continue  # that register is another parameter's
        reads_place = [
            i
            for i, st in enumerate(statements)
            if re.search(rf"\bc{base + place}\b", st.asm)
        ]
        if reads_place:
            break
    else:
        EFFECT_UNLIT.append("no point light")
        return
    first = reads_place[0]
    reads_colour = [
        i
        for i, st in enumerate(statements)
        if i >= first and re.search(rf"\bc{base + colour}\b", st.asm)
    ]
    if not reads_colour:
        EFFECT_UNLIT.append("no point light's colour")
        return
    depth = statements[first].depth
    # The reckoning, with a branch begun in it taken to its end.
    last = reads_colour[-1]
    open_branches = 0
    index = first
    while index <= last or open_branches > 0:
        asm = statements[index].asm.strip()
        if asm.startswith("if"):
            open_branches += 1
        elif asm.startswith("endif"):
            open_branches -= 1
            if open_branches < 0:
                raise shader_hlsl.ShaderError(
                    "a point light's reckoning leaves the branch it is in"
                )
        elif _CONTROL.match(asm) and not asm.startswith("else"):
            raise shader_hlsl.ShaderError(
                f"a point light's reckoning has {asm.split()[0]} in it"
            )
        index += 1
    body = statements[first:index]
    last = index - 1
    written = {}
    for st in body:
        for name, comps in _accesses(st.asm)[0].items():
            if name.startswith("r"):
                written.setdefault(name, set()).update(comps)
    # Where the loop goes: after the branch the reckoning is in, if any, so
    # that it runs whether or not the engine gave the shader a light there.
    after = last
    if depth > 0:
        after = next(
            (
                i
                for i in range(last + 1, len(statements))
                if statements[i].depth < depth
                and statements[i].asm.strip().startswith("endif")
            ),
            None,
        )
        if after is None:
            raise shader_hlsl.ShaderError(
                "no end to the branch of a point light's reckoning"
            )
    # What the rest of the shader reads of what the reckoning wrote, before
    # writing it again: the light's sums.
    outputs = {name: set() for name in written}
    killed = {name: set() for name in written}
    for st in statements[after + 1 :]:
        writes, reads = (
            _accesses(st.asm) if st.asm.strip() else ({}, _hlsl_reads(st.hlsl))
        )
        for name, comps in reads.items():
            if name in written:
                outputs[name] |= (comps & written[name]) - killed[name]
        if st.depth <= depth:
            for name, comps in writes.items():
                if name in written:
                    killed[name] |= comps
    outputs = {name: comps for name, comps in outputs.items() if comps}
    if not outputs:
        EFFECT_UNLIT.append("a point light's sum that nothing reads")
        return

    # The point the light is reckoned at: its way is `add <w>.xyz, <place>,
    # -<point>` (the world's place of the surface, an input).
    world = next(
        (
            m.group(1)
            for st in body
            if (
                m := re.match(
                    rf"^add r\d+\.xyz, c{base + place}, -(v\d+)$", st.asm.strip()
                )
            )
        ),
        None,
    )
    if world is None:
        EFFECT_UNLIT.append("no point a light is reckoned at")
        return

    def mask(comps):
        return "".join(c for c in "xyzw" if c in comps)

    def local(text):
        for name in written:
            text = re.sub(rf"\b{name}\b", f"dao_{name}", text)
        text = re.sub(rf"\b{lights.name}\[{place}\]", "daoEffectLights[2 * daoI]", text)
        text = re.sub(rf"\b{lights.name}\[{colour}\]", "daoEffectColour", text)
        if place == 9:
            text = re.sub(r"\bg_bShaderControlBooleans\[0\]", "true", text)
        return re.sub(r"\btex2D\(", "DaoTexFirstLevel(", text)

    lines = ["[loop] for (int daoI = 0; daoI < daoEffectCount.x; daoI++)", "{"]
    # The light's colour, less what stands between it and the point.
    lines += [
        "    const float4 daoEffectColour = float4(daoEffectLights[2 * daoI + 1].rgb *"
        f" DAO_EFFECT_SHADOW(daoI, {world}.xyz), daoEffectLights[2 * daoI + 1].a);"
    ]
    lines += [f"    float4 dao_{name} = {name};" for name in sorted(written)]
    lines += [
        f"    dao_{name}.{mask(comps)} = 0;" for name, comps in sorted(outputs.items())
    ]
    lines += ["    " + local(st.hlsl) for st in body]
    lines += [
        f"    {name}.{mask(comps)} += dao_{name}.{mask(comps)};"
        for name, comps in sorted(outputs.items())
    ]
    lines += ["}"]
    shader.insert(after, lines)
    EFFECT_LIT.append(
        (f"[{place}]", len(body), {n: mask(c) for n, c in outputs.items()})
    )


def with_effect_lights_on_ground(shader):
    """Every effect's light on a surface whose shader reckons no point light
    of its own (the ground's: the engine gives terrain the sun alone, and an
    effect never lit it): effect_lights.hlsl's DaoEffectDiffuse added to the
    light the surface scatters, with the normal the sun's facing is taken
    from (`dp3 <f>, <normal>, -<sun's way>`) and the point the sun's shadow
    is asked at (with_our_shadow's). The sun's sum, `mad <sum>.xyz, <N.L>,
    <sun colour>, <the vertex's light>`, is shaded after (`mad <lit>.xyz,
    <sum>, <shadow>.w, <the lightmap's>`), and a fire's light added to it was
    shaded by the moon's shadow: the pyre lit nothing in the shadow of the
    wall beside it (the user, 2026-10-22: "Fire light isn't overcoming moon
    shadow"). So it is reckoned where the sun's sum is and added to what the
    shadow has been laid on. A shader without the four is left as it is."""
    lights = shader.parameters["g_vLightParams"]
    data = shader.parameters.get("g_vShadowData")
    if lights.size < 4 or data is None:
        return
    try:
        total = shader.find(
            rf"^mad(?:_pp)? (r\d+)\.xyz, r\d+\.\w+, c{lights.base + 3}, v\d+$"
        )
        facing = shader.find(rf"^dp3 r\d+\.\w, (r\d+), -c{lights.base + 2}$")
        side = shader.find(rf"^dp3 r\d+\.[xyzw], v\d+, c{data.base + 8}$")
    except shader_hlsl.ShaderError:
        return
    sum_register = re.match(r"\S+ (r\d+)", shader.statements[total].asm).group(1)
    shaded = next(
        (
            i
            for i, st in enumerate(shader.statements)
            if i > total
            and re.match(rf"^mad(?:_pp)? (r\d+)\.xyz, {sum_register}, r\d+\.\w, r\d+", st.asm)
        ),
        None,
    )
    if shaded is None:
        return
    lit_register = re.match(r"\S+ (r\d+)", shader.statements[shaded].asm).group(1)
    normal = re.search(r", (r\d+), ", shader.statements[facing].asm).group(1)
    world = re.search(r"(v\d+),", shader.statements[side].asm).group(1)
    if facing > total:
        raise shader_hlsl.ShaderError("the sun's facing after the light it scatters")
    shader.insert(shaded, [f"{lit_register}.xyz += daoGroundEffects;"])
    shader.insert(
        total,
        [f"const float3 daoGroundEffects = DaoEffectDiffuse({world}.xyz, daoGroundNormal);"],
    )
    shader.insert(facing - 1, [f"const float3 daoGroundNormal = {normal}.xyz;"])
    EFFECT_LIT.append(("ground", 0, {lit_register: "xyz"}))


EFFECT_UNLIT = []
EFFECT_LIT = []


def without_cut(shader):
    """Leave a surface as bright as it is lit (look.hlsl's DaoCut). The
    shaders of buildings, furnishings and figures end their lighting with
    `mul_sat <colour>.xyz, <colour>, <tint>`, a cut at white, just before the
    fog is laid on (`... mml_vFogParams[0]`); the ground's have none. A
    shader with no such statement is left as it is."""
    fog = next(
        (i for i, s in enumerate(shader.statements) if "mml_vFogParams[0]" in s.hlsl),
        None,
    )
    if fog is None:
        return
    for index in range(fog - 1, max(fog - 4, -1), -1):
        statement = shader.statements[index]
        if (
            re.match(r"mul_sat r\d+\.[xyzw]{3}, ", statement.asm)
            and "saturate(" in statement.hlsl
        ):
            statement.hlsl = statement.hlsl.replace("saturate(", "DaoCut(", 1)
            return


def with_our_grade(shader, occlusion=False, show=False):
    """Read the picture through DaoPicture, and give DaoGrade the last word.
    With `occlusion` the picture read is darkened by DaoOcclusion (the
    grade's own pass; the fading form is another chain's and is not given
    it); with `show` the right half of the screen is that darkening alone,
    to tune it by, and with "share" the whole screen is what the occlusion
    pass hands the grade."""
    read = shader.find(r"^texld\w* r\d+, v\d+, s0$")
    register, coordinate = re.match(
        r"texld\w* (r\d+), (v\d+),", shader.statements[read].asm
    ).groups()
    picture = next(
        p.name for p in shader.parameters.values() if p.set == "s" and p.base == 0
    )
    # The grade's own pass lifts the shade (grade.hlsl's fDaoLift); the
    # fading form, run by another chain, does not.
    lift = "fDaoLift" if occlusion else "0"
    # The grade's own pass takes each channel through the area's curve (s1)
    # by `dp4 <c>, <picture>, v0` and v1 and v2, the colour matrix's rows:
    # the picture comes to it as bright as it is, and the three are rolled
    # off together into what the curve can show (grade.hlsl's DaoIntoCurve).
    # The fading form has no curve: it reads the picture rolled off.
    rows = [
        i
        for i, st in enumerate(shader.statements)
        if re.match(rf"^dp4\w* r\d+\.\w, {register}, v[012]$", st.asm.strip())
    ]
    curve = next(
        (p.name for p in shader.parameters.values() if p.set == "s" and p.base == 1),
        None,
    )
    into_curve = len(rows) == 3 and curve is not None
    colour = f"DaoPicture({picture}, {coordinate}.xy, {lift}, {'false' if into_curve else 'true'})"
    open_ = f"DaoOcclusion({coordinate}.xy)"
    if occlusion:
        colour += f" * {open_}"
    if into_curve:
        for i in rows:
            target, row = re.match(
                rf"^dp4\w* (r\d+\.\w), {register}, v([012])$",
                shader.statements[i].asm.strip(),
            ).groups()
            shader.statements[i].hlsl = f"{target} = daoGraded.{'rgb'[int(row)]};"
        shader.insert(
            rows[0] - 1,
            [
                f"const float3 daoGraded = DaoIntoCurve(float3(dot({register}, v0), dot({register}, v1),"
                f" dot({register}, v2)), {curve});"
            ],
        )
    shader.replace(read, read, [f"{register} = float4({colour}, 1);"])
    shader.add_input("pixel", "float2", "VPOS")
    last = ["oC0.xyz = DaoGrade(oC0.xyz, input.pixel);"]
    if occlusion and show == "share":
        # The openness in red, the ambient share in green, and what the
        # picture is multiplied by, the one on the other, in blue.
        last.append(
            f"oC0.xyz = float3(tex2D(smpOcclusion, {coordinate}.xy).rg, {open_});"
        )
    elif occlusion and show:
        last.append(f"if ({coordinate}.x > 0.5) oC0.xyz = {open_};")
    shader.insert(len(shader.statements) - 1, last)


# The chain's values of ours, each (its name in the chain, the pass that
# reads it, what it is when no one sets it, the least and the most it may
# be). A shader reads one as f<name>; a plugin sets one by its name on
# da_hsvmatrix (dao/Framebuffer.h), which is how Graphics.cpp applies the
# options. A value
# two passes read names both, with a + between.
LOOK = [
    ("DaoHighlights", "grade", 0.0, 0, 1),
    # The picture's exposure, a factor (Graphics.cpp's Exposure slider).
    ("DaoExposure", "grade", 1.0, 0.25, 4),
    # The shade's lift, in stops (grade.hlsl; Graphics.cpp's kShadowLift).
    ("DaoLift", "grade", 0.0, 0, 1),
    # The picture's width and height on a plane one unit before the eye
    # (Graphics.cpp's KeepLens), each frame.
    ("DaoLensX", "occlusion+haze", 1.0, 0, 64),
    ("DaoLensY", "occlusion+haze", 1.0, 0, 64),
    ("DaoOcclusion", "occlusion", 1.0, 0, 6),
    ("DaoOcclusionRadius", "occlusion", 0.7, 0.1, 3),
    # The air's light (haze_pass.hlsl), from the area's sun, sky and fog
    # and the camera's way (Outdoors.cpp).
    ("DaoHaze", "haze", 0.0, 0, 1),
    ("DaoFogR", "haze", 0.0, 0, 16),
    ("DaoFogG", "haze", 0.0, 0, 16),
    ("DaoFogB", "haze", 0.0, 0, 16),
    ("DaoSunX", "haze", 0.0, -1, 1),
    ("DaoSunY", "haze", 0.0, -1, 1),
    ("DaoSunZ", "haze", 0.0, -1, 1),
    ("DaoAirR", "haze", 0.0, 0, 16),
    ("DaoAirG", "haze", 0.0, 0, 16),
    ("DaoAirB", "haze", 0.0, 0, 16),
    ("DaoSunR", "haze", 0.0, 0, 16),
    ("DaoSunG", "haze", 0.0, 0, 16),
    ("DaoSunB", "haze", 0.0, 0, 16),
    ("DaoHazeG", "haze", 0.7, -1, 1),
    # The fog's intensity (the area's fog_intensity: the engine keeps
    # exp(-(intensity d)^2) of a surface's light at d metres), and the
    # camera's place and axes in the world, for the sun's shadows in the
    # air (haze_pass.hlsl's DaoSunInAir).
    ("DaoFogIntensity", "haze", 0.0, 0, 1),
    # The most of a surface's light the fog takes (the area's fog_cap), and the
    # fog's zenith, in degrees: the sky fogs itself less as it rises.
    ("DaoFogCap", "haze", 1.0, 0, 1),
    ("DaoFogZenith", "haze", 45.0, 0, 90),
    ("DaoEyeX", "haze", 0.0, -1000000, 1000000),
    ("DaoEyeY", "haze", 0.0, -1000000, 1000000),
    ("DaoEyeZ", "haze", 0.0, -1000000, 1000000),
    ("DaoRightX", "haze", 1.0, -1, 1),
    ("DaoRightY", "haze", 0.0, -1, 1),
    ("DaoRightZ", "haze", 0.0, -1, 1),
    ("DaoDownX", "haze", 0.0, -1, 1),
    ("DaoDownY", "haze", 0.0, -1, 1),
    ("DaoDownZ", "haze", -1.0, -1, 1),
    ("DaoAheadX", "haze", 0.0, -1, 1),
    ("DaoAheadY", "haze", 1.0, -1, 1),
    ("DaoAheadZ", "haze", 0.0, -1, 1),
    ("DaoHazeShow", "haze", 0.0, 0, 2),
]


def look_resources():
    """The look's values as resources of the chain."""
    return "".join(
        f'\n        <float id="{name}" value="{value}" range="{least} {most}" expose="true" animatable="true" />'
        for name, _, value, least, most in LOOK
    )


def look_settings(reader):
    """The lines of a filter that hand one pass its values."""
    return "".join(
        f"\n                f{name} = {name}"
        for name, by, *_ in LOOK
        if reader in by.split("+")
    )


def look_parameters(reader):
    """One pass's values as the material declares them."""
    return [
        f'<Parameter Type="Float" Name="f{name}" Default="{value}"/>'
        for name, by, value, *_ in LOOK
        if reader in by.split("+")
    ]


# What the colour grade's chain is given for the ambient occlusion: the
# engine's depth target and focal plane, as the game's depth of field takes
# them (da_dof.ffx.xml), a target of one float for the depth in metres
# (occlusion_depth.hlsl), two targets for the occlusion, and the blur's two
# axes.
CHAIN_RESOURCES = """
        <backbuffer id="SceneDepthTexture" index="1" />
        <runtime id="focalplane" value="scene_focalplane" />
        <target id="DaoDepthMetres" width="BACKBUFFER_WIDTH" height="BACKBUFFER_HEIGHT" format="R32F" shared="true"/>
        <target id="DaoHazed" width="BACKBUFFER_WIDTH" height="BACKBUFFER_HEIGHT" format="A8R8G8B8" shared="true"/>
        <target id="DaoOcclusionA" width="BACKBUFFER_WIDTH" height="BACKBUFFER_HEIGHT" format="A8R8G8B8" shared="true"/>
        <target id="DaoOcclusionB" width="BACKBUFFER_WIDTH" height="BACKBUFFER_HEIGHT" format="A8R8G8B8" shared="true"/>
        <float id="DaoAcross" value="0.0" range="0.0 1.0" />
        <float id="DaoDown" value="1.0" range="0.0 1.0" />"""

# The first two passes, before the anti-aliasing's: the depth in metres, and
# the picture with the air's light (DaoHazed, which the anti-aliasing and the
# grade read in place of the game's BackBufferCopy). A target the size of
# the picture of eight bits is made 16 by the plugin as the game asks for it
# (Graphics.cpp's CreateTexture), so the air's light keeps what is over white.
FIRST_FILTERS = """<filter id="Dao Depth" library="DAEffects" material="HSLMatrix">
                SceneDepth  = SceneDepthTexture
                fFocalPlane = focalplane
                bind DaoDepthMetres at 0
                run semantic DaoDepth
            </filter>

            <filter id="Dao Haze" library="DAEffects" material="HSLMatrix">
                Input       = BackBufferCopy
                Metres      = DaoDepthMetres
                SceneDepth  = SceneDepthTexture
                fFocalPlane = focalplane
{haze_look}
                bind DaoHazed at 0
                run semantic DaoHaze
            </filter>

            """

# The four passes after them, before the grade's own: the occlusion into A,
# blurred across into B, blurred down into A again.
CHAIN_FILTERS = """<filter id="Dao Occlusion" library="DAEffects" material="HSLMatrix">
                Metres      = DaoDepthMetres
                Input       = BackBufferCopy
{occlusion_look}
                bind DaoOcclusionA at 0
                run semantic DaoOcclusion
            </filter>

            <filter id="Dao Occlusion Across" library="DAEffects" material="HSLMatrix">
                Occlusion   = DaoOcclusionA
                Metres      = DaoDepthMetres
                fAxis       = DaoAcross
                bind DaoOcclusionB at 0
                run semantic DaoOcclusionBlur
            </filter>

            <filter id="Dao Occlusion Down" library="DAEffects" material="HSLMatrix">
                Occlusion   = DaoOcclusionB
                Metres      = DaoDepthMetres
                fAxis       = DaoDown
                bind DaoOcclusionA at 0
                run semantic DaoOcclusionBlur
            </filter>

            """

# The passes as the material knows them, drawn like the grade's own: a
# quad over the screen, the grade's own vertex shader (shader model 3, as a
# pixel shader of model 3 wants; only its position and texture coordinate are
# used), our pixel shader. HSVMatrixBlend.sta reads sampler 0 filtered and
# sampler 1 not.
MATERIAL_SEMANTIC = """  <Semantic Name="{name}">
    <Logic Note="Default logic block">
      <Logic Note="Shader Model 3">
        <LogicTest ShaderModel="3"/>
        <Technique Name="DefaultTechnique">
          <Pass Name="Pass 1" VertexShader="HSLMatrix.vsh" PixelShader="{name}.psh" State="{state}">
            {parameters}
            <RequiredStreamData Name="POSITION" />
            <RequiredStreamData Name="TEXCOORD0" />
          </Pass>
        </Technique>
      </Logic>
    </Logic>
  </Semantic>
"""
DEPTH_PARAMETERS = [
    '<Parameter Type="Texture" Name="SceneDepth" SamplerName="smpDepth" />',
    '<Parameter Type="Float" Name="fFocalPlane" Default="1000.0"/>',
]
METRES_PARAMETER = '<Parameter Type="Texture" Name="Metres" SamplerName="smpDepth" />'
SCENE_PARAMETER = '<Parameter Type="Texture" Name="Input" SamplerName="smpScene" />'
# The air's light's pass reads the depth in metres unfiltered, at sampler 1,
# and the game's distance target, for the fog each surface kept.
HAZE_PARAMETERS = [
    '<Parameter Type="Texture" Name="Input" SamplerName="smpScene" />',
    '<Parameter Type="Texture" Name="Metres" SamplerName="smpMetres" />',
    '<Parameter Type="Texture" Name="SceneDepth" SamplerName="smpDepth" />',
    '<Parameter Type="Float" Name="fFocalPlane" Default="1000.0"/>',
]
OCCLUSION_PARAMETER = (
    '<Parameter Type="Texture" Name="Occlusion" SamplerName="smpOcclusion" />'
)

# Our own pixel shaders, compiled as they are: the file here, and the name the
# material asks for.
OURS = [
    ("occlusion_depth.hlsl", "daodepth.psh.fxo"),
    ("haze_pass.hlsl", "daohaze.psh.fxo"),
    ("occlusion.hlsl", "daoocclusion.psh.fxo"),
    ("occlusion_blur.hlsl", "daoocclusionblur.psh.fxo"),
    ("shadow_cast.hlsl", "daoshadow.psh.fxo"),
    ("shadow_measure.hlsl", "daomeasure.psh.fxo"),
    ("shadow_cut.hlsl", "daoshadowcut.psh.fxo"),
    ("shadow_copy_vertex.hlsl", "daoshadowcopy.vsh.fxo"),
    ("shadow_copy.hlsl", "daoshadowcopy.psh.fxo"),
]


def with_our_passes(chain, material, antialias=True):
    """The colour grade's chain and material as text, with our passes before
    the grade's own: the anti-aliasing's three (smaa.py), whose picture the
    grade then reads in place of the game's, and the ambient occlusion's
    four, whose result it is given. Both are the game's files with ours put
    in at places found by their text, so a game whose files differ is
    refused, not guessed at."""

    def put(text, anchor, new, before=False):
        if anchor not in text:
            raise shader_hlsl.ShaderError(f"the game's file has no {anchor!r}")
        return text.replace(anchor, new + anchor if before else anchor + new, 1)

    def semantic(name, parameters, state="HSVMatrixBlend.sta"):
        return MATERIAL_SEMANTIC.format(
            name=name, state=state, parameters="\n            ".join(parameters)
        )

    # The grade's own reading of the picture, by its spacing: the
    # anti-aliasing's filters read BackBufferCopy too, and keep it.
    reads = "Input       = BackBufferCopy"
    if reads not in chain:
        raise shader_hlsl.ShaderError(f"the game's file has no {reads!r}")
    picture = smaa.PICTURE if antialias else "DaoHazed"
    chain = chain.replace(
        reads,
        f"Input       = {picture}\n                Occlusion   = DaoOcclusionA"
        + look_settings("grade"),
        1,
    )
    chain = put(
        chain,
        '<requires id="da_copybackbuffer" effectid="da_copybackbuffer"/>',
        CHAIN_RESOURCES
        + look_resources()
        + (smaa.CHAIN_RESOURCES if antialias else ""),
    )
    chain = put(
        chain,
        '<filter id="HSL Filter"',
        FIRST_FILTERS.replace("\n{haze_look}", look_settings("haze"))
        + (
            smaa.CHAIN_FILTERS.replace("Input   = BackBufferCopy", "Input   = DaoHazed")
            if antialias
            else ""
        )
        + CHAIN_FILTERS.replace("\n{occlusion_look}", look_settings("occlusion")),
        before=True,
    )
    # The picture's parameter comes four times: the grade's pass and its
    # fading form, each for shader model 3 and then 2. Model 3's two are given
    # the chain's values, so the fading form, which another chain runs and no
    # filter of ours sets, draws with their defaults; the grade's own is given
    # the occlusion too.
    picture_parameter = '<Parameter Type="Texture" Name="Input" SamplerName="s0" />'
    pieces = material.split(picture_parameter)
    if len(pieces) != 5:
        raise shader_hlsl.ShaderError(
            f"the game's material reads the picture {len(pieces) - 1} times, not 4"
        )
    values = "".join("\n            " + line for line in look_parameters("grade"))
    pieces[1] = "\n            " + OCCLUSION_PARAMETER + values + pieces[1]
    pieces[3] = values + pieces[3]
    material = picture_parameter.join(pieces)
    semantics = (
        semantic("DaoDepth", DEPTH_PARAMETERS)
        + semantic("DaoHaze", [*HAZE_PARAMETERS, *look_parameters("haze")])
        + semantic(
            "DaoOcclusion",
            [METRES_PARAMETER, SCENE_PARAMETER, *look_parameters("occlusion")],
        )
        + semantic(
            "DaoOcclusionBlur",
            [
                OCCLUSION_PARAMETER,
                METRES_PARAMETER,
                '<Parameter Type="Float" Name="fAxis" Default="0.0"/>',
            ],
        )
    )
    if antialias:
        semantics += "".join(
            semantic(name, parameters, smaa.STATE)
            for name, parameters in smaa.SEMANTICS
        )
    material = put(material, "</Material>", semantics, before=True)
    return chain, material


def takes_baked_light(listing):
    """Whether a shader lays a baked lightmap under the engine's lights."""
    return (
        "smpLightmap" in listing and "g_vLightParams" in listing and "ps_3_0" in listing
    )


def with_shaped_bake(shader):
    """Shape a level's baked light by the surface's normal map (bake.hlsl has
    why). The lightmap's colour, as it is read, becomes DaoShapedBake's
    answer; its alpha, which says where the sun reaches, is left. The
    surface's normal with its normal map is what the sun's direction is
    measured against (`dp3 r, <normal>, -c<lights + 2>`), made by the first
    `nrm <normal>.xyz, <sum>`; the normal without it is the vertex's, the
    input the map's own z is multiplied by just before: `mad <sum>.xyz,
    <sum>.z, v<N>, ...` in a building's shader, `nrm <r>.xyz, v<N>` in the
    terrain's. A shader with no such normal is left as it is."""
    lights = shader.parameters["g_vLightParams"]
    lightmap = shader.parameters["smpLightmap"]
    if lights.set != "c" or lights.size < 4:
        return
    normal = next(
        (
            m.group(1)
            for st in shader.statements
            if (
                m := re.match(
                    rf"dp3 r\d+\.[xyzw], (r\d+), -c{lights.base + 2}$", st.asm
                )
            )
        ),
        None,
    )
    if normal is None:
        return
    try:
        made = shader.find(rf"^nrm {normal}\.xyz, r\d+$")
    except shader_hlsl.ShaderError:
        return
    flat = None
    for index in range(made - 1, max(made - 4, -1), -1):
        asm = shader.statements[index].asm
        found = re.match(r"mad (r\d+)\.xyz, \1\.z, (v\d+), ", asm) or re.match(
            r"nrm (r\d+)\.xyz, (v\d+)$", asm
        )
        if found:
            flat = found.group(2)
            break
    if flat is None:
        return
    read = shader.find(rf"^texld\w* r\d+, .*, s{lightmap.base}$")
    register = re.match(r"texld\w* (r\d+),", shader.statements[read].asm).group(1)
    if hasattr(shader, "share"):
        shader.share = "daoBaked"
    # The baked light is what a cube of ours is mirrored by (with_materials),
    # if it is read before the mirrored light is made.
    owed = getattr(shader, "local", False) and any(
        "DaoMirror(" in st.hlsl for st in shader.statements[read:]
    )
    if owed:
        shader.local = False
    # Last first, so the places found stay where they are.
    shader.insert(
        read,
        [
            f"{register}.xyz = DaoShapedBake({register}.xyz, daoRelief);",
            *([f"daoBaked = {register}.xyz;"] if hasattr(shader, "share") else []),
            *([f"daoLocal = {register}.xyz;"] if owed else []),
        ],
    )
    shader.insert(
        made,
        [f"daoRelief = DaoRelief({normal}.xyz, {flat}.xyz, -{lights.name}[2].xyz);"],
    )
    shader.insert(-1, ["float daoRelief = 0;"])


def with_baked_tree_light(shader):
    """Multiply a tree's ambient light by the hour's baked light (look.hlsl's
    DAO_BAKED_LIGHT): g_vColorMultiplier, what a leaf, frond or branch has
    with no sun on it, which went on lighting the crowns yellow-green at
    night (seen 2026-10-21) where the level and its grass had gone blue."""
    for statement in shader.statements:
        statement.hlsl = statement.hlsl.replace(
            "g_vColorMultiplier", "(g_vColorMultiplier * float4(DAO_BAKED_LIGHT, 1))"
        )


def with_baked_light(shader):
    """The level's baked light by the hour, where the lightmap is read
    (night_sky.hlsl): the day's colour, times the hour's (look.hlsl's
    DAO_BAKED_LIGHT) as the sun goes, and toward the night sky's even light
    by the night's share. Its alpha, which says where the sun reaches, is
    kept. Run after with_shaped_bake, so that its line comes first after the
    read and what the shaped bake keeps of the light (daoBaked) has it."""
    lightmap = shader.parameters["smpLightmap"]
    read = shader.find(rf"^texld\w* r\d+, .*, s{lightmap.base}$")
    register = re.match(r"texld\w* (r\d+),", shader.statements[read].asm).group(1)
    shader.insert(
        read,
        [f"{register}.xyz = lerp({register}.xyz * DAO_BAKED_LIGHT, daoNightSky.yzw, daoNightSky.x);"],
    )


# The water shaders, each with the input its light comes in by: the sky's sun
# scattered (the vertex shader's TEXCOORD2, from mml_vSkyParams), which they
# light it by in place of the sun's own colour, as N.L times it and, in
# waterflowing0, a share of it as is. water0 is a lake's surface with its
# highlight, water0nospec without, waterflowing0 a river's or a fall's, with
# its foam (smpDecal) added on top of the light.
WATERS = {"water0": "v1", "water0nospec": "v1", "waterflowing0": "v2"}


def with_water_light(shader, stem):
    """Water by the hour's light (water_light.hlsl): the sky's light it is lit
    by, and a river's foam, as strong as the hour's light makes them, the
    light as pale as the night's is (DayNight.h's Water). Both were the
    sky's, never the moon's, and Redcliffe's falls stayed a lit cream at
    01:00, as bright as at noon (the user, 2026-10-22: "should be a dimmer
    white")."""
    sky = WATERS[stem]
    shader.insert(
        -1,
        [
            f"{sky}.xyz = lerp(dot({sky}.xyz, float3(0.2126, 0.7152, 0.0722)).xxx, {sky}.xyz, daoWaterLight.y)"
            " * daoWaterLight.x;"
        ],
    )
    if "smpDecal" not in shader.parameters:
        return
    # waterflowing0.psh: two reads of the foam, summed, then added on.
    decal = shader.parameters["smpDecal"].base
    second = shader.find(rf"^texld r\d+, .*, s{decal}$", shader.find(rf"^texld r\d+, .*, s{decal}$") + 1)
    summed = shader.find(r"^add r\d+\.\w+, r\d+\.\w+, r\d+\.\w+$", second + 1)
    target = re.match(r"add (r\d+\.\w+),", shader.statements[summed].asm).group(1)
    shader.insert(summed, [f"{target} *= daoWaterLight.x;"])


# The depth-only pass the game draws a cut-out surface's depth with (a
# material's DepthWrite): its pixel shader, the game's and ours made from it.
DEPTH_PIXEL = ("static_depth.psh.fxo", "daostatic_depth.psh.fxo")
# The vertex shaders that pass is drawn with, each (the game's, ours made from
# it, the materials whose DepthWrite pass is pointed at ours, the function
# here that makes ours): level geometry
# and props, and what the level scatters by the hundred, grass and bushes
# (seen in play 2026-10-11 with the first alone: foliage kept faint dark boxes
# about it). A tree's leaves have a third, depth_speedtree: NO_DEPTH_PASS.
DEPTH_VERTICES = [
    (
        "static_depth.vsh.fxo",
        "daostatic_depth.vsh.fxo",
        ["static.mat.xml", "prop.mat.xml"],
        "depth_vertex_with_coordinates",
    ),
    (
        "instancing_depthwrite.vsh.fxo",
        "daoinstancing_depthwrite.vsh.fxo",
        ["instancingmaterial.mat.xml"],
        "depth_vertex_instanced",
    ),
]


# The level's vertex shaders as the plugin draws the level into the engine's
# sun shadow maps (Graphics.cpp's level shadows), each (the game's, ours made
# from it): terrain, buildings and furnishings, and a tree's trunk and
# branches. Not the distance (vista3.vsh, bakedlowlod.psh): a backdrop of
# mountains and towers about the level, which in a map fitted to the figures
# stood nearer the sun than all of them over the whole map (2026-10-16: every
# sample of both maps at depth 0). The engine draws only figures and what they carry into
# those maps (pssmmesh.vsh, pssmskinmesh.vsh; traced 2026-10-16), and the
# level's shadows are in its lightmaps, fixed where they were baked.
SHADOW_VERTICES = [
    (name + ".vsh.fxo", "daoshadow_" + name + ".vsh.fxo")
    for name in (
        "terrain1",
        "terrain2",
        "terrain3",
        "static1",
        "static1_relief",
        "static2",
        "static3",
        "prop1",
        "prop2",
        "instancing1",
        "spt_branch",
    )
] + [
    # A tree's fronds and leaves, cut out by their texture as they are drawn
    # (shadow_cut.hlsl): the variant keeps the texture's place.
    (name + ".vsh.fxo", "daoshadowcut_" + name + ".vsh.fxo")
    for name in ("spt_fronds", "spt_leafmeshes")
]
# The pixel shader they are drawn with (shadow_cast.hlsl).
SHADOW_PIXEL = "daoshadow.psh.fxo"
# And the same shaders as a kept draw's bounds are measured by
# (shadow_measure.hlsl, with_measured_place).
MEASURE_VERTICES = [
    (theirs, re.sub(r"^daoshadow(cut)?_", "daomeasure_", ours_))
    for theirs, ours_ in SHADOW_VERTICES
]


def by_rows(shader, matrix):
    """Whether a shader takes a matrix's registers as its rows (`dot(M[i],
    v)`, terrain2.vsh) or as its columns (`M[0] * v.x + M[1] * v.y + ...`,
    static1.vsh): the game's shaders have both. The engine's shadow pass takes
    its partition's by rows."""
    return any(f"dot({matrix}[" in statement.hlsl for statement in shader.statements)


def with_cut_shadow_place(shader):
    """with_shadow_place for a surface cut out by its texture (a tree's
    fronds and leaves), the texture's place (TEXCOORD0) handed down too."""
    shader.cut = True
    with_shadow_place(shader)


def with_measured_place(shader):
    """The shadow place's shader made a measuring one: drawn with the
    partition's matrix the identity, it hands down the vertex's place in the
    world (TEXCOORD8), and every vertex is drawn to the one pixel the plugin
    says (daoMeasureSlot, c244), as a point (LevelShadows.cpp)."""
    with_shadow_place(shader)
    shader.insert(len(shader.statements) - 1, ["o0 = daoMeasureSlot;"])


def with_shadow_place(shader):
    """Place each vertex in a sun shadow map: by the partition's matrix the
    plugin sets (daoPartition, c240 up: the engine's own shadow pass's
    g_mPSSMPartitionMatrix, the sun's projection of a slice of the view, from
    the world), and hand that place to the pixel shader (TEXCOORD8). The
    game's shaders place a vertex by its model's matrix and the view's in one
    (aur_mWorldViewProj), which is then the partition's times the model's
    (aur_mWorld); or a tree's by the view's alone, the tree's own code having
    made its place in the world (g_mViewProjection), which is then the
    partition's: either way the game's own code, a tree's wind with the
    rest, places what is drawn, its matrix's registers taken as that shader
    takes them (by_rows). (The partition's matrix read from c4 in every
    one of the engine's shadow shaders was a bone's in its skinned one's, and
    taken for one of the view after its projection, 2026-10-16: it is the
    world's, read from each shader's own constant table, LevelShadows.cpp.)"""
    if "aur_mWorldViewProj" in shader.parameters and "aur_mWorld" in shader.parameters:
        whole, model = (
            shader.parameters["aur_mWorldViewProj"].name,
            shader.parameters["aur_mWorld"].name,
        )
        if by_rows(shader, whole):
            matrix = f"mul(daoPartition, {model})"
        else:
            matrix = f"mul({model}, transpose(daoPartition))"
    elif "g_mViewProjection" in shader.parameters:
        whole = shader.parameters["g_mViewProjection"].name
        matrix = "daoPartition" if by_rows(shader, whole) else "transpose(daoPartition)"
    else:
        raise shader_hlsl.ShaderError("no matrix it places a vertex by")
    for statement in shader.statements:
        statement.hlsl = statement.hlsl.replace(whole, "daoShadowMatrix")
    shader.insert(-1, [f"const float4x4 daoShadowMatrix = {matrix};"])
    # Only the place is handed down, and a cut-out's texture's place: a shadow
    # map wants nothing else, and static1.vsh's eleven outputs leave no room
    # for another.
    kept = ("POSITION", "TEXCOORD0") if getattr(shader, "cut", False) else ("POSITION",)
    dropped = [field for field in shader.outputs if not field[2].startswith(kept)]
    shader.outputs = [field for field in shader.outputs if field[2].startswith(kept)]
    # What the game's code still writes them as: locals, written and dropped.
    shader.insert(-1, [f"float4 {register} = 0;" for _, _, _, register, _ in dropped])
    # What lies beyond the map's depth is pressed onto its ends, not cut
    # away (shadow_cast.hlsl): the place handed down is the place itself.
    shader.insert(
        len(shader.statements) - 1,
        ["daoShadowPlace = o0;", "o0.z = clamp(o0.z, 0, o0.w);"],
    )
    shader.insert(-1, ["float4 daoShadowPlace = 0;"])
    shader.add_output("daoShadowPlace", "float4", "TEXCOORD8", "daoShadowPlace")


# The materials whose depth pass is taken out. A tree's or a bush's leaves
# are cards, drawn into the picture through their texture's alpha, and into
# the depth by a pass that reads no texture (Depth_SpeedTree.vsh, which has no
# texture coordinate among its inputs, and Static_Depth.psh): every card is in
# the depth whole, a rectangle standing in the air, and the ambient occlusion
# shaded each one's outline on what is behind it (the user, 2026-10-11:
# "foliage have subtle black boxes around them"; with this pass off every one
# of them went, seen the same day). Without the pass the leaves are not in the
# depth at all: nothing shades them and they shade nothing, and the depth of
# field takes them for what is behind them.
#
# Their depth through the texture was tried and is not to be had this way
# (2026-10-11): the pass drawn with the leaves' own vertex shader
# (SPT_LeafCards.vsh, which the converter can now rebuild) and given the
# parameters the picture's pass is given crashed the game on the first frame,
# reading 16 bytes from a null pointer as it set a material's parameter
# (0x008E25E0 from 0x0050FB6D). For a tree's depth pass the engine sets three
# parameters and no more (the tree's scale, its place, its wind offset); the
# camera's angles and the leaves' scalars, which turn each card to the eye,
# are set for the picture and for the shadow maps only, and a vector the pass
# names that nothing sets has no data behind it. The camera's angles could be
# worked out in the shader from the view's matrix; the leaves' scalars are a
# tree's own.
NO_DEPTH_PASS = ["spt_leafcards.mat.xml"]


def without_depth_pass(material):
    """A material as text, its DepthWrite semantic under another name, so
    the engine finds none to draw."""
    material, count = re.subn(
        r'(<Semantic\s+Name\s*=\s*")DepthWrite"', r'\1DaoNoDepthWrite"', material
    )
    if count != 1:
        raise shader_hlsl.ShaderError("the game's material has no DepthWrite")
    return material


def depth_vertex_with_coordinates(shader):
    """Hand the depth pass's pixel shader the surface's texture coordinates:
    the game's hands it the depth alone."""
    shader.add_input("uv", "float2", "TEXCOORD0")
    shader.add_output("uv", "float2", "TEXCOORD1", "input.uv")


def depth_vertex_instanced(shader):
    """The same for what the level scatters, and the depth itself with it.
    The game's vertex shader for an instance's depth (instancing_depthwrite)
    hands its pixel shader nothing: the depth that shader packs is whatever
    an input no one set holds, so every tuft of grass was written as at no
    distance from the camera, card and all (read 2026-10-11). The depth is
    the position's w, which is the distance in front of the camera, as
    static_depth.vsh works it out from the model's and the view's matrix."""
    depth_vertex_with_coordinates(shader)
    shader.add_output("depth", "float", "TEXCOORD0", "abs(o0.w)")


def with_far_depth(shader):
    """The depth pass's distance no more than 0.9 of the focal plane, as
    with_far_distance writes it for far things where the picture's own pass
    writes the distance. Where the game's anti-aliasing is on, the distance
    is this pass's (a target of its own), and what lay past the focal plane,
    the mountains behind Redcliffe's castle, was written 1, the sky's, and
    the air's light's pass took it for sky."""
    index = shader.find(r"^mul_sat r0\.x, r0\.x, v0\.x$")
    shader.insert(index, ["r0.x = min(r0.x, 0.9);"])


def depth_pixel_by_texture(shader):
    """Write no depth where the surface's texture is clear. The game's depth
    pass reads no texture, so a bush's or a fence's whole card is written, and
    the ambient occlusion shaded the card's outline on what stands behind it
    (seen in Redcliffe, 2026-10-11). The test is the one the picture's own
    pass is drawn with (DAMshPnch1S.sta: alpha over 127 of 255)."""
    with_far_depth(shader)
    shader.add_input("uv", "float2", "TEXCOORD1")
    # Only by a texture whose alpha is coverage: a solid thing's texture may
    # hold none at all, and the mountains behind Redcliffe's castle
    # (vst_mount_d.dds, its alpha 0 throughout) were cut out of the pass
    # whole, left the sky's distance, and the air's light's pass took them
    # for sky: dark blue shapes in the haze toward a low sun where the game's
    # anti-aliasing is on (the user, 2026-10-21). The smallest of the
    # texture's pictures is its alpha's mean.
    shader.insert(
        0,
        [
            "const float covered = tex2Dlod(smpDecal, float4(input.uv, 0, 16)).a;",
            "clip(covered > 0 ? tex2D(smpDecal, input.uv).a - 0.5 : 1);",
        ],
    )


def with_textured_depth(material, vertex):
    """A material as text, its DepthWrite pass for shader model 3 pointed at
    our two shaders (`vertex` is the game's vertex shader and ours) and given
    what they read: the surface's texture, and the texture coordinates among
    the vertex's streams."""
    semantic = re.search(r'<Semantic\s+Name\s*=\s*"DepthWrite"', material)
    if not semantic:
        raise shader_hlsl.ShaderError("the game's material has no DepthWrite")
    start = material.index("<Pass", semantic.start())
    end = material.index("</Pass>", start)
    block = material[start:end]
    for kind, (theirs, ours) in (("Vertex", vertex), ("Pixel", DEPTH_PIXEL)):
        named = re.compile(
            rf'{kind}Shader\s*=\s*"{re.escape(theirs[: -len(".fxo")])}"', re.IGNORECASE
        )
        block, count = named.subn(f'{kind}Shader="{ours[: -len(".fxo")]}"', block)
        if count != 1:
            raise shader_hlsl.ShaderError(
                f"the game's DepthWrite pass is not drawn with {theirs}"
            )
    block += (
        '  <Parameter Type="Texture" Name="mml_tDiffuse" SamplerName="smpDecal" Default="Default_Missing"/>\n'
        '            <RequiredStreamData Name="TEXCOORD0" />\n          '
    )
    return material[:start] + block + material[end:]


def list_shader(fxc, name, body, work):
    """fxc's listing of one of the game's compiled shaders."""
    compiled = os.path.join(work, name)
    with open(compiled, "wb") as f:
        f.write(body)
    return subprocess.run(
        [fxc, "/nologo", "/dumpbin", compiled],
        capture_output=True,
        text=True,
        check=True,
    ).stdout


def build(fxc, name, listing, out, changes, preamble, work, named=None):
    """One shader of the game's, changed by each of `changes` in turn,
    compiled into `out` under its own name, or `named`."""
    shader = shader_hlsl.Shader(listing)
    for change in changes:
        change(shader)
    if getattr(shader, "materials", None) is not None:
        whole, edged = shader.materials
        MATERIALS_REACHED.append((named or name, whole, edged and not shader.local))
    source = os.path.join(work, (named or name) + ".hlsl")
    with open(source, "w", encoding="utf-8") as f:
        f.write(shader.render(preamble))
    compile_shader(fxc, shader.profile, source, os.path.join(out, named or name))


def build_ours(fxc, source, name, out):
    """One of this folder's own shaders, compiled into `out`, its warnings
    errors: they are the shaders' lint. A vertex shader by its name's .vsh."""
    profile = "vs_3_0" if ".vsh." in name else "ps_3_0"
    compile_shader(
        fxc, profile, os.path.join(HERE, source), os.path.join(out, name), strict=True
    )


# The warnings of the game's shaders rewritten, by shader: reported, not
# errors, their code being partly the game's, decompiled.
THEIR_WARNINGS = {}


def compile_shader(fxc, profile, source, compiled, strict=False):
    done = subprocess.run(
        [
            fxc,
            "/nologo",
            "/T",
            profile,
            "/E",
            "main",
            # fxc's most careful register allocation: at its default (/O1) the
            # scenery's lightmapped shaders (static1 and its kind) ran out of
            # their 32 temporary registers with the shadows' blend, and fit at
            # /O2 and above (2026-10-21).
            "/O3",
            "/Fo",
            compiled,
            *(["/WX"] if strict else []),
            source,
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    said = (done.stdout + done.stderr).strip()
    if done.returncode != 0:
        raise shader_hlsl.ShaderError(said)
    warnings = [line for line in said.splitlines() if "warning X" in line]
    if warnings:
        THEIR_WARNINGS[os.path.basename(compiled)] = warnings


def ours(name):
    """One of this folder's HLSL files."""
    with open(os.path.join(HERE, name), encoding="utf-8") as f:
        return f.read()


# The sky and its clouds, and the forms of each the engine fades between.
SKIES = ("sky", "clouds", "fadesky", "fadeclouds")


def with_lit_sky_fog(shader):
    """The sky's fog colour made haze.hlsl's DaoSkyFog, the air's light seen
    the way the pixel looks. Each of the four fogs towards
    mml_vFogParams[0] and has the way it is seen in, from the eye, in its
    TEXCOORD1 (sky.psh's dome, clouds.psh's layer: read 2026-10-16), and the
    sun's in g_vLightParams[2], as its own sun's glow reads it."""
    way = next(
        (
            register
            for _, _, semantic, register, _ in shader.inputs
            if semantic == "TEXCOORD1"
        ),
        None,
    )
    fogged = [s for s in shader.statements if "mml_vFogParams[0]" in s.hlsl]
    if way is None or not fogged or "g_vLightParams" not in shader.parameters:
        raise shader_hlsl.ShaderError("a sky with no fog colour, way or sun to read")
    for statement in fogged:
        statement.hlsl = statement.hlsl.replace("mml_vFogParams[0]", "daoSkyFog")
    # The sky is cut at white as it is written (sky.psh's mad_sat): a bright
    # sky with the sun's glow in its haze was a white hole (Ostagar's through
    # an arch, 2026-10-16). With the haze ours it is left to the picture's
    # roll-off, which keeps its hue (grade.hlsl's DaoShoulder).
    for statement in shader.statements:
        if statement.hlsl.startswith("oC0.xyz = saturate("):
            statement.hlsl = statement.hlsl.replace(
                "oC0.xyz = saturate(", "oC0.xyz = DaoSkyCut(", 1
            )
    shader.insert(
        -1,
        [
            f"const float4 daoSkyFog = DaoSkyFog({way}.xyz, g_vLightParams[2].xyz, mml_vFogParams[0]);"
        ],
    )


# The sky's disc's colour and whether it is ours (Graphics.cpp's kDiscRegister).
DISC = "float4 daoDisc : register(c214);\n"


def with_one_disc(shader):
    """Draw the sky's disc once, where the light comes from, in the hour's
    colour (DayNight.h's Disc). The disc is the celestial body's texture laid
    by a matrix of the light's way, and nothing checks which side of the eye
    it is on, so it is drawn where the light comes from and again opposite
    (lib-agent-3, read in sky.vsh and the sky's recompute 0x0055BFA0,
    2026-10-21): vanilla's opposite is under the horizon, a sun moved by the
    hour put it in the sky. A sky with no disc is left as it is."""
    if "smpCelestialBody" not in shader.parameters:
        return
    body = shader.parameters["smpCelestialBody"]
    way = next(
        register
        for _, _, semantic, register, _ in shader.inputs
        if semantic == "TEXCOORD1"
    )
    read = shader.find(rf"^texld\w* r\d+, .*, s{body.base}$")
    register = re.match(r"texld\w* (r\d+),", shader.statements[read].asm).group(1)
    shader.insert(
        read,
        [
            f"{register} *= dot(normalize({way}.xyz), -g_vLightParams[2].xyz) > 0 ? 1 : 0;",
            f"{register}.xyz *= daoDisc.w > 0 ? daoDisc.xyz : 1;",
        ],
    )


def keeps_fog(stem, listing):
    """Whether a shader fogs what it draws and writes the distance beside it:
    every surface's but the sky's and the toolset's (85 of shaders.erf's,
    counted 2026-10-16). The toolset's water fogs otherwise and is not the
    game's."""
    return (
        stem not in SKIES
        and not stem.endswith("toolset")
        and "mml_vFogParams" in listing
        and re.search(r"\boC1\b", listing) is not None
        and "ps_3_0" in listing
    )


def fogs_unseen(stem, listing):
    """Whether a shader fogs what it draws and writes no distance beside it:
    the effects (vfx0, vfx0_atm, the chunky ones), hair, blended props
    (propalpha1). The air's light's pass cannot see what these draw."""
    return (
        stem not in SKIES
        and not stem.endswith("toolset")
        and "mml_vFogParams" in listing
        and re.search(r"\boC1\b", listing) is None
        and "ps_3_0" in listing
    )


def with_mean_haze_fog(shader):
    """The fog colour made haze.hlsl's DaoMeanFog: the air's light on the
    mean over all ways, for what writes no distance. With the area's fog
    colour black, a mist's cards over Redcliffe's hill and windmill faded to
    black where the game had them fade to its fog (seen 2026-10-16)."""
    fogged = [s for s in shader.statements if "mml_vFogParams[0]" in s.hlsl]
    if not fogged:
        raise shader_hlsl.ShaderError("no fog colour read")
    for statement in fogged:
        statement.hlsl = statement.hlsl.replace("mml_vFogParams[0]", "daoMeanFog")
    shader.insert(-1, ["const float4 daoMeanFog = DaoMeanFog(mml_vFogParams[0]);"])


def with_fog_kept(shader):
    """How much of its own light a surface keeps through the fog written in
    the alpha of the distance beside it (oC1, which every such shader writes
    as 1), for the air's light's pass (haze_pass.hlsl): 1 less it is how much
    the fog took, as the engine reckoned it for that pixel. Every one of
    them fogs in one statement, `mad oC0.xyz, <kept>, <colour>, <fog
    colour>` (read 2026-10-16), the kept part reckoned per vertex and laid
    across each triangle, which a pass reckoning it again from the distance
    alone cannot match: on Redcliffe's hill it had a fog 20 levels of 255
    lighter than the engine's (measured the same day)."""
    fog = shader.parameters["mml_vFogParams"]
    index = shader.find(rf"^mad(_pp)? oC0\.xyz, r\d+\.[xyzw], r\d+, c{fog.base}$")
    kept = re.match(r"mad\w* oC0\.xyz, (r\d+\.[xyzw]),", shader.statements[index].asm)[
        1
    ]
    # From the end back, so each place found is still where it was.
    shader.insert(len(shader.statements) - 1, ["oC1.w = daoFogKept;"])
    shader.insert(index, [f"daoFogKept = {kept};"])
    shader.insert(-1, ["float daoFogKept = 1;"])


# The distance the sky writes beside the picture (sky.psh's oC1), as the
# listing has the constant.
SKY_DISTANCE = "0.344639987, 0.99609375, 0.996084154"


def writes_sky_distance(stem, listing):
    """Whether a shader that is not the sky's fogs and writes the sky's
    distance: the forms of things drawn far off (vista3, bakedlowlod,
    terrain3, static3_alpha, prop2, cloth2, weapon2: read 2026-10-16)."""
    return (
        stem not in SKIES
        and SKY_DISTANCE in listing
        and "mml_vFogParams" in listing
        and "ps_3_0" in listing
    )


def with_far_distance(shader):
    """A far thing's distance written as 0.9 of the focal plane, not the
    sky's. The forms of things drawn far off write the very constant the sky
    does (Redcliffe's castle is a vista and its cliff a low form), so the
    air's light's pass took them for sky and gave them none of it: with the
    area's fog colour black they were black shapes. Fogged to the cap by
    their own vertex shaders, as anything that far is, they are given the
    air's light as any surface is. The game's depth of field reads them as
    just as far."""
    index = shader.find(r"^mov oC1, c\d+$")
    shader.replace(index, index, ["oC1 = float4(0, 0, 0.9, 1);"])


# The bloom's first pass, which picks out what is bright in the picture (the
# plain form and the one da_bloom runs).
BLOOM_MASKS = ("illuminationmask", "illuminationmaskmult")


def with_bounded_bloom(shader):
    """Give the bloom the picture no brighter than white. Its mask grows with
    the picture's brightness, which was never more than 1; with the scene in
    a 16-bit target (Graphics.cpp) a sunlit rock is, and the bloom it threw
    lifted the whole frame six percent, the Vanilla look's too (seen in
    Redcliffe, 2026-10-11). The bloom stays what the game made it; what is
    above white is the tone curve's to bring down (grade.hlsl)."""
    read = shader.find(r"^texld r\d+, t\d+, s0$")
    register = re.match(r"texld (r\d+),", shader.statements[read].asm).group(1)
    # The mask is the picture times its alpha, and with the ambient occlusion
    # on the alpha is our ambient share (look.hlsl's DaoShare), not the
    # game's: a figure's two thirds took a third of its bloom, and the figure
    # was a tenth darker for the occlusion being on with nothing occluded
    # (Grey Warden plate in Redcliffe by day, the occlusion's power set to 0;
    # 2026-10-15). The alpha is taken as 1 then, a solid surface's in
    # vanilla; the plugin says when (da_bloom's DaoAlphaIsShare,
    # with_bloom_switch). What is blended is blended by its own alpha, as
    # ever: this is the mask's reading of the finished picture alone.
    shader.insert(
        read,
        [
            f"{register} = saturate({register});",
            f"{register}.w = lerp({register}.w, 1, saturate(fDaoAlphaIsShare));",
        ],
    )
    # And what it writes: the mask is the picture's brightness plus the
    # chain's Brightness, over 1 for anything half bright, and was cut at 1
    # by the eight bits it was written into. Bounding what it reads alone
    # changed nothing (seen the same day).
    shader.insert(len(shader.statements) - 1, ["oC0 = saturate(oC0);"])


def with_bloom_switch(effect, material):
    """The game's bloom (da_bloom.ffx.xml) and its material (dabloom.mat.xml)
    given one value of ours, DaoAlphaIsShare, which the plugin sets while
    the ambient occlusion is on and the mask passes read (with_bounded_bloom)."""
    effect, count = re.subn(
        r"(\n\s*)</resources>",
        r'\1    <float id="DaoAlphaIsShare" value="0.0" range="0.0 1.0" expose="true" animatable="true" />\1</resources>',
        effect,
        count=1,
    )
    if count != 1:
        raise SystemExit("da_bloom.ffx.xml: no resources")
    effect, count = re.subn(
        r"(\n(\s*)fContrast\s*=\s*Contrast)",
        r"\1\n\2fDaoAlphaIsShare = DaoAlphaIsShare",
        effect,
    )
    if count != 1:
        raise SystemExit(
            f"da_bloom.ffx.xml: {count} filters hand fContrast, one was looked for"
        )
    material, count = re.subn(
        r'(\n(\s*)<Parameter Type = "Float" Name = "fContrast" Default = "[^"]*"/>)',
        r'\1\n\2<Parameter Type="Float" Name="fDaoAlphaIsShare" Default="0.0"/>',
        material,
    )
    if count != 2:
        raise SystemExit(f"dabloom.mat.xml: {count} mask passes, two were looked for")
    return effect, material


def with_scattered_share(shader):
    """Work out the ambient share of a shader the materials' model does not
    reach, a face's, hair's or an eye's, from what it scatters: these add
    their ambient light, a colour input, to the sum of their lights' light
    in one statement (`add <all>.xyz, <lights>, v<C>`: read in face0, face2,
    hair0, hair2 and eye0, 2026-10-12), and all of it is then scattered
    through the surface's colour, so the ambient part of what they send is
    the ambient light's part of that sum. Their highlights, which are the
    lights' own, are left out of the count, and are small beside it.

    They said nothing, so the occlusion shaded all of their light: a face
    in the sun, in a high collar, was half as bright again with the
    occlusion off (the user, 2026-10-12: "is something wrong w/ the skin
    material such that it's not taking light?")."""
    if getattr(shader, "share", None) is not None:
        return
    ambient = next(
        (
            register
            for _, _, semantic, register, _ in shader.inputs
            if semantic.startswith("COLOR")
        ),
        None,
    )
    if ambient is None:
        return
    for index, statement in enumerate(shader.statements):
        found = re.match(
            rf"^add(?:_pp)? (r\d+)\.xyz, (r\d+), {ambient}$", statement.asm
        )
        if found and statement.depth == 0:
            shader.insert(
                index + 1,
                [f"daoScatteredShare = DaoShare({ambient}.xyz, {found.group(1)}.xyz);"],
            )
            shader.insert(-1, ["float daoScatteredShare = 1;"])
            shader.scattered = True
            return


def alpha_as(mode, least=None, seen=False):
    """A change that says what a build of a shader does with its alpha
    (with_ambient_share): "share", "keep", or "cut" at `least` of 255, and
    for a cut-out whether it is `seen` in the distance the occlusion reads."""

    def mark(shader):
        shader.alpha = (mode, least, seen)

    return mark


def with_ambient_share(shader):
    """Say how much of the surface's light is ambient, in the picture's own
    alpha (look.hlsl's DaoShare; issue 8), where the pass that draws the
    shader does nothing else with its alpha (alpha_as; USES).

    The picture's alpha and not the distance target's, where it was first
    (2026-10-12, a day): the game binds the distance target beside the
    picture only when it does not anti-alias. With its anti-aliasing on, the
    picture's target has samples and the distance target cannot stand beside
    it, so the game draws the distance in a pass of its own before the scene
    (a frame traced at 8 samples: 118 draws, a copy, a clear, the scene's 348
    into one target), and what a shader wrote for the second target went
    nowhere: every pixel's share read 1 and the occlusion shaded all of
    every surface's light. A share forced to a quarter read back as 1 there
    and as a quarter with the anti-aliasing off; in the picture's alpha it
    reads a quarter either way.

    But the alpha is a pass's to use, and what it does with it is its render
    state's: the same shader is drawn plain by one pass and blended or cut
    out by another (a figure's second level of detail draws its cut-outs by
    the solid shaders and a blending state; cloth, a prop's cut-outs and the
    level's are the same). The share written by name of shader, as it was
    for two hours, was blended by and tested against. So by pass: three
    builds of a shader, and each pass pointed at the one its state allows
    (with_pass_variants).
      share  the share. Only where with_materials found what it is made of
             and an ambient light is known (a figure's or a prop's probe
             light, the level's baked light); else the game's alpha, 1, and
             the surface is shaded whole.
      keep   the game's own alpha, for a pass that blends by it.
      cut    the shader cuts itself out at the state's least, and then: the
             share, where the cut-out is in the distance the occlusion reads
             (the level's and a prop's, whose depth pass is cut by the
             texture too); nothing, where it is not. Where the game
             anti-aliases, that distance has in it only what is drawn as
             solid (a material's `Opaque` render class): a plumed helm,
             drawn cut out or blended, is not there, and at its pixels the
             distance is that of what stands behind it, the collar it sits
             in, a sword's hilt, a shield's rim. The occlusion worked out
             for those was laid on the helm, their outlines with it, which
             is what "transparent" was (the user, 2026-10-12, eight times;
             seen at last in the distance itself, `show-depth`: the figure
             with no helm on it). What the occlusion cannot see it is not to
             shade.

    And only into the frame's own picture (look.hlsl's DAO_SHARE_ASKED): in
    any other target the alpha stays the game's, whatever the build."""
    mode, least, seen = getattr(shader, "alpha", ("keep", None, False))
    if mode == "keep":
        return
    ambient = getattr(shader, "share", None)
    share = (
        f"DaoShare({ambient} * daoAlbedo * DaoScattered(daoMetal) + DaoEnvSent(), daoLit)"
        if ambient is not None
        else "daoScatteredShare"
        if getattr(shader, "scattered", False)
        else None
    )
    said = []
    if mode == "cut":
        said.append(f"clip(oC0.w - {(least - 0.5) / 255.0!r});")
        if not seen:
            share = "0"
    if share is not None:
        said.append(f"oC0.w = lerp(oC0.w, {share}, saturate(DAO_SHARE_ASKED));")
    shader.insert(len(shader.statements) - 1, said)


# The shaders that write their alpha as a sum they do not clamp, which can
# pass 1 (issue 25): found 2026-10-16 by listing every one of shaders.erf's
# of model 3. Of the 69 that write an alpha unclamped, the rest multiply
# values of 0..1, take a minimum or choose between constants, which cannot
# leave 0..1.
SUMMED_ALPHA = ("water0", "water0nospec", "physics1", "water_wireframe")


def with_alpha_clamped(shader):
    """The alpha clamped to 0..1 as the shader leaves it, as the game's own
    target of eight bits clamped it before blending (issue 25). In the 16-bit
    scene nothing does: the water's alpha, its blend factor, is more than 1
    where the moon's highlight lies on it (water0.psh's `mad oC0.w, r0.x,
    r2.w, r1.y`), and Lake Calenhad by night was a white glare with rainbow
    fringes, the lake under the water taken away. Its colour is left as the
    shader writes it."""
    shader.insert(len(shader.statements) - 1, ["oC0.w = saturate(oC0.w);"])


def effect_shadows_sampler(listing, preamble):
    """The declaration of the effects' lights' shadows' atlas, on the last
    sampler the game's shader leaves free (the plugin finds it by its name,
    LevelShadows.cpp). With the samplers the game's shader uses and those our
    changes add (the sun's maps for a figure, the metal's map) sixteen, none
    is left, as for a face (fourteen of the game's and the sun's two): there
    the effects' lights are not shadowed (effect_lights.hlsl's
    DAO_NO_EFFECT_SHADOWS)."""
    used = {int(n) for n in re.findall(r"^//\s+\w+\s+s(\d+)\s+\d+", listing, re.M)}
    added = len(re.findall(r"^sampler2D \w+;$", preamble, re.M))
    free = [n for n in range(15, -1, -1) if n not in used]
    if len(used) + added >= 16 or not free:
        EFFECT_UNSHADOWED.append(1)
        return "#define DAO_NO_EFFECT_SHADOWS\n"
    return f"sampler2D smpDaoEffectShadows : register(s{free[0]});\n"


EFFECT_UNSHADOWED = []


def takes_point_lights(listing):
    """Whether a pixel shader of model 3 reckons a point light: it reads
    g_vLightParams from c0, the first light's place and colour among it."""
    m = re.search(r"g_vLightParams\s+c(\d+)\s+(\d+)", listing)
    return (
        m is not None
        and m.group(1) == "0"
        and int(m.group(2)) >= 2
        and "ps_3_0" in listing
    )


def changes_for(stem, listing, show):
    """What this build changes in one of the game's pixel shaders: the
    functions that change it, and the HLSL they call."""
    changes, preamble = [], ""
    shadow = ours("look.hlsl") + ours("shadow.hlsl")
    if stem in RECEIVERS:
        changes.append(with_our_shadow)
        preamble += shadow
    if takes_sun_shadow(stem, listing):
        changes += [with_sun_shadow, with_materials, without_cut]
        # Not a face: skin is soft, and its map is pores and lines, which
        # want no sharpening.
        if not stem.startswith("face"):
            changes.append(with_ambient_relief)
        preamble += (
            ours("look.hlsl")
            + shadow_parameters(stem)
            + shadow
            + ours("materials.hlsl")
            + ours("bake.hlsl")
        )
    elif (
        stem.startswith(CHARACTERS)
        and "g_vLightParams" in listing
        and "ps_3_0" in listing
    ):
        # A figure's shader the sun's shadow is not put on (it has the
        # shadow's data already): left uncut as the others are.
        changes.append(without_cut)
        preamble += ours("look.hlsl")
    if stem == "hslmatrix":
        changes.append(lambda s: with_our_grade(s, occlusion=True, show=show))
        preamble += ours("grade.hlsl")
    if stem == "hslmatrixfade":
        changes.append(with_our_grade)
        preamble += ours("grade.hlsl")
    if stem in SKIES:
        changes += [with_lit_sky_fog, with_one_disc]
        preamble += ours("look.hlsl") + ours("haze.hlsl") + DISC
    if writes_sky_distance(stem, listing):
        changes.append(with_far_distance)
    # The depth pass of vista and terrain, the game's own (vista.mat.xml,
    # terrain.mat.xml); static's and prop's are daostatic_depth (DEPTH_PIXEL).
    if stem == "static_depth":
        changes.append(with_far_depth)
    if stem in BLOOM_MASKS:
        changes.append(with_bounded_bloom)
        # A shader of model 2, which reaches no register of look.hlsl's: the
        # switch is a value of the bloom's own (with_bloom_switch).
        preamble += "float fDaoAlphaIsShare;\n"
    if stem in PROPS:
        changes += [with_ambient_relief, without_cut]
        preamble += ours("look.hlsl") + ours("bake.hlsl")
    if takes_baked_light(listing):
        changes += [with_shaped_bake, with_baked_light, without_cut]
        if "DaoShapedBake" not in preamble:
            preamble += ours("look.hlsl") + ours("bake.hlsl")
        preamble += ours("night_sky.hlsl")
    # The level's farther versions lay a lightmap too, with no engine lights
    # (bakedlowlod, vista3, static2, static3, terrain3): left out of the
    # hour's light, the hills behind Redcliffe's lake stayed day-lit at night
    # (seen 2026-10-21).
    if "smpLightmap" in listing and "ps_3_0" in listing and with_baked_light not in changes:
        changes.append(with_baked_light)
        if "DAO_LOOK_HLSL" not in preamble:
            preamble += ours("look.hlsl")
        preamble += ours("night_sky.hlsl")
    if stem.startswith("spt_") and "g_vColorMultiplier" in listing:
        changes.append(with_baked_tree_light)
        if "DAO_LOOK_HLSL" not in preamble:
            preamble += ours("look.hlsl")
    # The materials, for a prop and a level's surface as for a figure: first,
    # before another change rewrites a statement they are found by.
    if (stem in PROPS or takes_baked_light(listing)) and with_materials not in changes:
        changes.insert(0, with_materials)
        preamble += ours("materials.hlsl")
    # The ambient share, last: what it writes is the build's to say
    # (alpha_as), and nothing where none is said.
    if with_materials in changes:
        changes += [with_scattered_share, with_ambient_share]
    # The fog kept for the air's light's pass, after all else: it is found by
    # the game's own last statement, which no other change rewrites.
    if keeps_fog(stem, listing):
        changes.append(with_fog_kept)
    if fogs_unseen(stem, listing):
        changes.append(with_mean_haze_fog)
        if "DAO_LOOK_HLSL" not in preamble:
            preamble += ours("look.hlsl")
        preamble += ours("haze.hlsl")
    # How strong the area's fills are on figures and props.
    if "g_vLightParams" in listing and "ps_3_0" in listing:
        changes.append(with_fill_strength)
        preamble += ours("fills.hlsl")
    # Every effect's light, on whatever reckons a point light: after the
    # materials and the fills, whose reckoning of a light it runs again.
    if "g_vLightParams" in listing and "ps_3_0" in listing:
        changes.append(
            with_effect_lights
            if takes_point_lights(listing)
            else with_effect_lights_on_ground
        )
        preamble = (
            effect_shadows_sampler(listing, preamble)
            + preamble
            + ours("effect_lights.hlsl")
        )
    if stem in WATERS:
        changes.append(lambda s: with_water_light(s, stem))
        preamble += ours("water_light.hlsl")
    # The alpha clamped, last: whatever else wrote it.
    if stem in SUMMED_ALPHA:
        changes.append(with_alpha_clamped)
    return changes, preamble


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    if argv[0] == "inputs":
        print(";".join(inputs()))
        return 0
    out = argv[0]
    # Below normal priority, which its eight fxc processes inherit, run alone as
    # under tools/build.ps1: other sessions' builds and games share the cores
    # (the user, 2026-10-18).
    if sys.platform == "win32":
        below_normal_priority_class = 0x4000
        kernel32 = ctypes.windll.kernel32
        # The handle is a pointer: as ctypes' default int, a 64-bit Python passes a bad one.
        kernel32.GetCurrentProcess.restype = ctypes.wintypes.HANDLE
        kernel32.SetPriorityClass.argtypes = (
            ctypes.wintypes.HANDLE,
            ctypes.wintypes.DWORD,
        )
        kernel32.SetPriorityClass(
            kernel32.GetCurrentProcess(), below_normal_priority_class
        )
    show = "share" if "show-share" in argv else "show-occlusion" in argv
    antialias = "no-smaa" not in argv
    data = os.path.join(script_commands.GAME, "packages", "core", "data")
    archive = os.path.join(data, "shaders.erf")
    fxc = find_fxc()
    if not os.path.exists(archive) or fxc is None:
        print(
            f"build_shaders: nothing written: {'no fxc.exe' if fxc is None else 'no ' + archive}",
            file=sys.stderr,
        )
        return 0
    os.makedirs(out, exist_ok=True)
    # The folder is this build's own: what an earlier build wrote and this one
    # no longer does would still be what the game draws with (five tree
    # shaders of a change since taken out were found so, 2026-10-11).
    # But for the materials' maps, which are another step's
    # (elgarashaders_materials objects, and its list of what it wrote): the
    # sweep took them every build and that step runs only when its table
    # changes, so every part-metal material named a map that was not there
    # (found 2026-10-12).
    kept = set()
    if os.path.exists(os.path.join(out, "materials.made")):
        with open(os.path.join(out, "materials.made"), encoding="utf-8") as f:
            kept = {line.strip().lower() for line in f}
    for stale in os.listdir(out):
        if (
            stale.lower().endswith((".fxo", ".xml", ".dds"))
            and stale.lower() not in kept
        ):
            os.remove(os.path.join(out, stale))
    chain, material = with_our_passes(
        script_commands.erf_entries(os.path.join(data, "postprocesseffects.erf"))[
            "da_hsvmatrix.ffx.xml"
        ].decode("utf-8-sig"),
        script_commands.erf_entries(os.path.join(data, "materials.erf"))[
            "hslmatrix.mat.xml"
        ].decode("utf-8-sig"),
        antialias,
    )
    if antialias:
        smaa.write_files(out)
    bloom, bloom_material = with_bloom_switch(
        script_commands.erf_entries(os.path.join(data, "postprocesseffects.erf"))[
            "da_bloom.ffx.xml"
        ].decode("utf-8-sig"),
        script_commands.erf_entries(os.path.join(data, "materials.erf"))[
            "dabloom.mat.xml"
        ].decode("utf-8-sig"),
    )
    for name, text in (
        ("da_hsvmatrix.ffx.xml", chain),
        ("hslmatrix.mat.xml", material),
        ("da_bloom.ffx.xml", bloom),
        ("dabloom.mat.xml", bloom_material),
    ):
        with open(os.path.join(out, name), "w", encoding="utf-8", newline="") as f:
            f.write(text)

    materials = script_commands.erf_entries(os.path.join(data, "materials.erf"))
    states = script_commands.erf_entries(os.path.join(data, "states.erf"))
    find_uses(materials, states)
    find_shadow_maps(materials)
    solid_states = {}
    written = set()
    for theirs, ours_, names, _ in DEPTH_VERTICES:
        for name in names:
            text = with_textured_depth(
                materials[name].decode("utf-8-sig"), (theirs, ours_)
            )
            text = with_pass_variants(
                with_material_parameters(with_shadow_parameters(text)),
                states,
                solid_states,
            )
            with open(os.path.join(out, name), "w", encoding="utf-8", newline="") as f:
                f.write(text)
            written.add(name)

    # Every definition with a pass one of our shaders draws, whichever it is:
    # a shader reads the parameters whatever material it is drawn for, and one
    # a definition does not name is whatever the last draw left in its place
    # (a tree's canopy and the distortion mask are drawn by ch1alphmultnt and
    # static1_alpha and were not in the list of five: found 2026-10-12).
    for name in sorted(materials):
        if (
            name.endswith(".mat.xml")
            and name not in written
            and name not in NO_DEPTH_PASS
        ):
            theirs = materials[name].decode("utf-8-sig")
            ours_ = with_pass_variants(
                with_material_parameters(with_shadow_parameters(theirs)),
                states,
                solid_states,
            )
            if ours_ != theirs:
                with open(
                    os.path.join(out, name), "w", encoding="utf-8", newline=""
                ) as f:
                    f.write(ours_)
                written.add(name)
    for name, text in solid_states.items():
        with open(os.path.join(out, name), "w", encoding="utf-8", newline="") as f:
            f.write(text)
    missing = [name for name in MATERIAL_DEFINITIONS if name not in written]
    if missing:
        raise shader_hlsl.ShaderError(f"no pass of ours in {missing}")
    for name in NO_DEPTH_PASS:
        with open(os.path.join(out, name), "w", encoding="utf-8", newline="") as f:
            f.write(without_depth_pass(materials[name].decode("utf-8-sig")))

    # The game's pixel shaders of shader model 3 (its sm2_ ones are for
    # hardware that has none), each built if this changes anything in it.
    shaders = script_commands.erf_entries(archive)
    entries = {
        name: body
        for name, body in shaders.items()
        if name.endswith(".psh.fxo") and not name.startswith("sm2_")
    }
    built, failed = 0, 0
    with tempfile.TemporaryDirectory() as work:
        for (theirs, ours_), change, preamble in (
            [
                ((theirs, ours_), globals()[change], "")
                for theirs, ours_, _, change in DEPTH_VERTICES
            ]
            + [(DEPTH_PIXEL, depth_pixel_by_texture, "sampler2D smpDecal;\n")]

            + [
                (
                    (theirs, ours_),
                    with_cut_shadow_place
                    if "daoshadowcut_" in ours_
                    else with_shadow_place,
                    "row_major float4x4 daoPartition : register(c240);\n",
                )
                for theirs, ours_ in SHADOW_VERTICES
            ]
            + [
                (
                    (theirs, ours_),
                    with_measured_place,
                    "row_major float4x4 daoPartition : register(c240);\n"
                    "float4 daoMeasureSlot : register(c244);\n",
                )
                for theirs, ours_ in MEASURE_VERTICES
            ]
        ):
            try:
                listing = list_shader(fxc, theirs, shaders[theirs], work)
                build(fxc, theirs, listing, out, [change], preamble, work, ours_)
                built += 1
            except (shader_hlsl.ShaderError, subprocess.CalledProcessError) as e:
                failed += 1
                print(f"build_shaders: {ours_}: {e}", file=sys.stderr)

        def one(item):
            name, body = item
            try:
                listing = list_shader(fxc, name, body, work)
                changes, preamble = changes_for(name[: -len(".psh.fxo")], listing, show)
                if not changes:
                    return None
                build(
                    fxc,
                    name,
                    listing,
                    out,
                    [alpha_as(own_alpha(name)), *changes],
                    preamble,
                    work,
                )
                # And the builds the passes that draw it another way are
                # pointed at (with_pass_variants).
                uses = USES.get(name, set())
                if TEST in uses:
                    cut_out = alpha_as("cut", *CUTS[name])
                    build(
                        fxc,
                        name,
                        listing,
                        out,
                        [cut_out, *changes],
                        preamble,
                        work,
                        "dao" + name,
                    )
                if PLAIN in uses and own_alpha(name) != "share":
                    build(
                        fxc,
                        name,
                        listing,
                        out,
                        [alpha_as("share"), *changes],
                        preamble,
                        work,
                        "daoshare" + name,
                    )
            except (shader_hlsl.ShaderError, subprocess.CalledProcessError) as e:
                return f"build_shaders: {name}: {e}"
            return ""

        with concurrent.futures.ThreadPoolExecutor(8) as pool:
            for result in pool.map(one, sorted(entries.items())):
                if result:
                    failed += 1
                    print(result, file=sys.stderr)
                elif result == "":
                    built += 1
    for source, name in OURS + (smaa.OURS if antialias else []):
        try:
            build_ours(fxc, source, name, out)
            built += 1
        except shader_hlsl.ShaderError as e:
            failed += 1
            print(f"build_shaders: {name}: {e}", file=sys.stderr)
    print(f"build_shaders: {built} shaders in {out}, {failed} failed")
    print(f"build_shaders: the fills' strength in {len(FILLS_KEPT)} shaders")
    by_light = collections.Counter(place for place, _, _ in EFFECT_LIT)
    print(
        f"build_shaders: effects' lights in {len(EFFECT_LIT)} shaders, by the reckoning of light "
        + ", ".join(
            f"{place} in {count}" for place, count in sorted(by_light.items(), key=str)
        )
        + (
            f"; unshadowed, no sampler left: {len(EFFECT_UNSHADOWED)}"
            if EFFECT_UNSHADOWED
            else ""
        )
        + (
            f"; left as they are: {', '.join(sorted(set(EFFECT_UNLIT)))} ({len(EFFECT_UNLIT)})"
            if EFFECT_UNLIT
            else ""
        )
    )
    if THEIR_WARNINGS:
        codes = collections.Counter(
            re.search(r"warning (X\d+)", w).group(1)
            for ws in THEIR_WARNINGS.values()
            for w in ws
        )
        print(
            f"build_shaders: the game's shaders rewritten warn in {len(THEIR_WARNINGS)} of them: "
            + ", ".join(f"{code} {count}x" for code, count in codes.most_common())
        )
    whole = sum(1 for _, spread, falloff in MATERIALS_REACHED if spread and falloff)
    print(
        f"build_shaders: materials in {len(MATERIALS_REACHED)} shaders, {whole} of them whole: "
        + " ".join(
            sorted(
                name.split(".")[0] + ("" if s and f else "*")
                for name, s, f in MATERIALS_REACHED
            )
        )
    )
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
