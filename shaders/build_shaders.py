"""The game's own shaders, changed: what Elgara Shaders draws with.

    python shaders/build_shaders.py <folder to write to> [map size]
        [show-occlusion] [no-smaa]

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
answer is put into <lit> after it.

The look's numbers (look.hlsl). What the level's and the figures' shaders do
of ours is by how much the plugin says, in registers of its own: the relief,
the materials, the sun on figures. With no plugin they are 0 and those
shaders draw as the game's.

The baked light (bake.hlsl, with_shaped_bake). Where a level's shader reads
its lightmap, the colour is shaped by the surface's normal map.

The materials (materials.hlsl, with_materials). The shaders of bodies, armour
and weapons that gather their highlights the common way scale them by their
sharpness and their mirror colour by the angle it is seen at.

The colours (grade.hlsl, GRADES). The game's colour grade reads the picture
with its first `texld <r>, v<N>, s0`, which becomes DaoSharpen's reading of
it, and DaoGrade has the last word on what the shader writes.

It needs the game (tools/script_commands.py's GAME, or DAO_GAME_DIR) and
fxc.exe of the Windows SDK; without either it says so and writes nothing.
"""

import concurrent.futures
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
    shader.insert(choice, [f"{lit} = DaoShadow({world}.xyz, input.pixel);"])


# How far towards the sun from a figure's surface its shadow is asked for, in
# metres: past the figure's own body, so it is not shaded by itself.
SHADOW_LIFT = 1.5
# What is left of a figure's directional light in full shadow.
SHADOW_LEFT = 0.3

# What the Character material declares for every one of its shaders and none
# of them reads (character.mat.xml's "Common Character Params", read
# 2026-10-10): the shadow maps and their data, as the level's shaders have.
CHARACTER_SHADOW_PARAMETERS = """
float4 g_vShadowData[9];
sampler2D smpShadowPartition1;
sampler2D smpShadowPartition2;
"""


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
MATERIAL_POW = r"^pow r\d+\.\w, r\d+\.\w, (r\d+)\.w$"
MATERIAL_JOIN = r"^mad(?:_pp)? (r\d+)\.xyz, \1, (c\d+)\.(\w), r\d+$"
# What the reflection is looked up by starts from how the surface faces the
# eye: `dp3 <f>.x, -<view>, <normal>`, which is minus N.V.
MATERIAL_FACING = r"^dp3 (r\d+)\.x, -r\d+(?:\.\w+)?, r\d+$"


def with_materials(shader):
    """Scale a character's highlights by their sharpness and its mirror
    colour by the angle it is seen at (materials.hlsl). Only a shader built
    the common way is changed: 33 of the Character material's, bodies, armour
    and one of the weapons' (read 2026-10-11). The rest are left as they
    are."""
    try:
        raised = shader.find(MATERIAL_POW)
        facing = shader.find(MATERIAL_FACING, raised)
        join = shader.find(MATERIAL_JOIN, facing)
    except shader_hlsl.ShaderError:
        return
    held = re.match(MATERIAL_POW, shader.statements[raised].asm).group(1)
    power = next(
        (
            i
            for i in range(raised, -1, -1)
            if re.match(rf"\w+ {held}\.w,", shader.statements[i].asm)
        ),
        None,
    )
    if power is None:
        return
    total, constant, component = re.match(
        MATERIAL_JOIN, shader.statements[join].asm
    ).groups()
    tripled = shader.definitions.get(constant)
    if (
        tripled is None
        or float(tripled["xyzw".index(component)]) != 3
        or shader.statements[power].depth != 0
    ):
        return
    # The statement after the join's that multiplies the sum by the mirror
    # colour, which is on either side: `mul <d>.xyz, <sum>, <colour>`; and
    # after it the one that adds the light the surface scatters, its diffuse
    # light times its own colour: `mad <d>.xyz, <light>, <colour>, <d>`.
    tinted = rf"^mul(?:_pp)? (r\d+)\.xyz, (?:{total}, (r\d+)|(r\d+), {total})$"
    try:
        tint = shader.find(tinted, join)
        made, *colours = re.match(tinted, shader.statements[tint].asm).groups()
        spread = shader.find(rf"^mad(?:_pp)? {made}\.xyz, r\d+, (r\d+), {made}$", tint)
    except shader_hlsl.ShaderError:
        return
    colour = next(g for g in colours if g)
    mirrored = shader.statements[join].asm.rsplit(", ", 1)[1]
    faces = re.match(MATERIAL_FACING, shader.statements[facing].asm).group(1)
    into = shader.statements[tint].hlsl.partition(" = ")[0]
    lit_into, _, lit = shader.statements[spread].hlsl.partition(" = ")
    scattered, plus, rest = lit.rpartition(" + ")
    if not plus:
        return
    # Last first, so the places found stay where they are.
    shader.statements[
        spread
    ].hlsl = f"{lit_into} = ({scattered.rstrip(';')}) * DaoScattered(daoMetal) + {rest}"
    shader.statements[
        tint
    ].hlsl = f"{into} = DaoSpecular({total}.xyz, daoEnv, {colour}.xyz, daoMetal, daoFacing, daoPower);"
    shader.insert(
        tint - 1,
        [f"daoMetal = DaoMetal({colour}.xyz, daoPower);"],
    )
    shader.statements[
        join
    ].hlsl = f"{total}.xyz = {total}.xyz * DaoHighlight(daoPower);"
    shader.insert(join - 1, [f"daoEnv = {mirrored}.xyz;"])
    shader.insert(facing, [f"const float daoFacing = -{faces}.x;"])
    shader.insert(power, [f"const float daoPower = {held}.w;"])
    shader.insert(
        -1,
        ["float3 daoEnv = 0;", "float daoMetal = 0;"],
    )


# A figure's normal with its normal map is `nrm <normal>.xyz, <sum>`, the sum
# ended by the map's own z times the vertex's normal: `mad <sum>.xyz, <sum>.z,
# v<N>, <sum>.xyww`.
FIGURE_NORMAL = r"^mad (r\d+)\.xyz, \1\.z, (v\d+), \1\.xyww$"


# The shaders of the things a level is furnished with, which have a normal
# map and no lightmap: their ambient light comes as a figure's does.
PROPS = ("prop1", "prop1_punch")


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


def reads_lamp(listing):
    """Whether a shader reads the point light a figure's lamp is handed in,
    g_vLightParams [9] and [11]."""
    lights = re.search(r"//\s+g_vLightParams\s+c(\d+)\s+(\d+)", listing)
    if lights is None or int(lights.group(2)) < 12 or "ps_3_0" not in listing:
        return False
    colour = rf"\bc{int(lights.group(1)) + 11}\b"
    return re.search(colour, listing.split("ps_3_0")[-1]) is not None


# Notes on the light of a figure's place, 2026-10-11. The fills could go
# altogether if a figure had the level's own light where it stands, and that
# light is baked into lightmaps a figure has no part of. Two things the engine
# does hand a figure were tried for it, each with no fills in four areas, and
# neither is it:
# - The light probe's cube map (the materials' aur_cmLightProbe0), read along
#   the normal at its smallest level. The engine chooses a figure the nearest
#   probe of its own room (a probe's score is one over its distance,
#   0x0053D3E0), and in the Circle the map is the room's warm light. In the
#   Dead Trenches it is a dim yellow-brown in a room of pale stone, the frame
#   24 percent darker than vanilla; and hair, faces and eyes have no cube map.
# - The ambient light kept to the figure's room. An ambient light's score is
#   its brightness alone (0x0053D040), with none of the room test a point
#   light's and a probe's have (0x0053D210); with that test put on it nothing
#   changed in the Trenches or the Circle. The Trenches' orange ambient is its
#   room's own.
# So what the engine has of a place's light for a figure is what its artists
# gave it, and they lit their figures by the fills instead.
#
# Where that light comes from, read from the game's files the same day. A
# level has light probes, each a node named LightProbe in a room's layout
# (<level>_<room>_ll.mmh, GFF4): its place, the names of a cube map (.dds, 256
# a side, nine levels) and an irradiance file (.mtx), and the irradiance
# itself as three 4x4 matrices (MMHLightProbeIrradianceRed, Green, Blue):
# Ramamoorthi and Hanrahan's matrix of nine spherical-harmonics coefficients,
# divided by pi (the game's own .mtx files check against the coefficients
# they list to 1.5e-8). The engine takes the matrices from the layout, not the
# .mtx. The same layouts hold the room's lamps (MMHNodePointLightColor,
# Radius, IsStatic, MMHNodeLightAffectDomain) and its ambient light.
# Of the campaign's 232 areas, by the level each area's own file names, 150
# have one probe or none; the Dead Trenches has 11, the Arl's dungeon 14.
# A layout in a plugin's override folder is the one the game uses: a room's
# archive is a source of priority 100007, the last of all (measured at
# 0x004C5BA0), and seen in the mage origin's Circle (level lak303d, not
# lak503d, the campaign's tower, which a first test patched by mistake) with
# its probes made blue and its lamps green. So a level can be given more
# probes with files alone; baking their light and placing them is not begun.


def with_point_lights(shader):
    """Let the look say how much of each of a figure's point lights it takes
    (look.hlsl's DaoPointLight): a lamp of the room it stands in, or one of
    the fill lights the engine stands far off to light every figure of an
    area alike. The engine hands a figure the best four it has for it, each a
    place and reach and a colour, [0] and [1], [5] and [6], [7] and [8], [9]
    and [11], and with Graphics.cpp's lamps the lamps that reach the figure
    come before the fills. The colour is multiplied wherever it is read."""
    lights = shader.parameters["g_vLightParams"].name
    for place, colour in ((0, 1), (5, 6), (7, 8), (9, 11)):
        for statement in shader.statements:
            statement.hlsl = re.sub(
                rf"{lights}\[{colour}\]",
                f"({lights}[{colour}] * DaoPointLight({lights}[{place}].w, {lights}[3].xyz, {lights}[{colour}].w))",
                statement.hlsl,
            )


def with_our_grade(shader, occlusion=False, show=False):
    """Read the picture through DaoSharpen, and give DaoGrade the last word.
    With `occlusion` the picture read is darkened by DaoOcclusion; with `show`
    the right half of the screen is that darkening alone, to tune it by."""
    read = shader.find(r"^texld\w* r\d+, v\d+, s0$")
    register, coordinate = re.match(
        r"texld\w* (r\d+), (v\d+),", shader.statements[read].asm
    ).groups()
    picture = next(
        p.name for p in shader.parameters.values() if p.set == "s" and p.base == 0
    )
    colour = f"DaoSharpen({picture}, {coordinate}.xy)"
    open_ = f"DaoOcclusion({coordinate}.xy)"
    if occlusion:
        colour += f" * {open_}"
    shader.replace(read, read, [f"{register} = float4({colour}, 1);"])
    shader.add_input("pixel", "float2", "VPOS")
    last = ["oC0.xyz = DaoGrade(oC0.xyz, input.pixel);"]
    if occlusion and show:
        last.append(f"if ({coordinate}.x > 0.5) oC0.xyz = {open_};")
    shader.insert(len(shader.statements) - 1, last)


# The look's values, each (its name in the chain, the pass that reads it,
# what it is when no one sets it, the least and the most it may be). A shader
# reads one as f<name>; a plugin sets one by its name on da_hsvmatrix
# (dao/Framebuffer.h), which is how Graphics.cpp applies graphics.ini.
LOOK = [
    ("DaoSharpen", "grade", 0.6, 0, 1),
    ("DaoVibrance", "grade", 0.3, 0, 2),
    ("DaoContrast", "grade", 0.25, 0, 1),
    ("DaoHighlights", "grade", 0.0, 0, 1),
    ("DaoOcclusion", "occlusion", 1.0, 0, 6),
    ("DaoOcclusionRadius", "occlusion", 0.7, 0.1, 3),
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
        f"\n                f{name} = {name}" for name, by, *_ in LOOK if by == reader
    )


def look_parameters(reader):
    """One pass's values as the material declares them."""
    return [
        f'<Parameter Type="Float" Name="f{name}" Default="{value}"/>'
        for name, by, value, *_ in LOOK
        if by == reader
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
        <target id="DaoOcclusionA" width="BACKBUFFER_WIDTH" height="BACKBUFFER_HEIGHT" format="A8R8G8B8" shared="true"/>
        <target id="DaoOcclusionB" width="BACKBUFFER_WIDTH" height="BACKBUFFER_HEIGHT" format="A8R8G8B8" shared="true"/>
        <float id="DaoAcross" value="0.0" range="0.0 1.0" />
        <float id="DaoDown" value="1.0" range="0.0 1.0" />"""

# The four passes before the grade's own: the depth in metres, the occlusion
# into A, blurred across into B, blurred down into A again.
CHAIN_FILTERS = """<filter id="Dao Depth" library="DAEffects" material="HSLMatrix">
                SceneDepth  = SceneDepthTexture
                fFocalPlane = focalplane
                bind DaoDepthMetres at 0
                run semantic DaoDepth
            </filter>

            <filter id="Dao Occlusion" library="DAEffects" material="HSLMatrix">
                Metres      = DaoDepthMetres
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
OCCLUSION_PARAMETER = (
    '<Parameter Type="Texture" Name="Occlusion" SamplerName="smpOcclusion" />'
)

# Our own pixel shaders, compiled as they are: the file here, and the name the
# material asks for.
OURS = [
    ("occlusion_depth.hlsl", "daodepth.psh.fxo"),
    ("occlusion.hlsl", "daoocclusion.psh.fxo"),
    ("occlusion_blur.hlsl", "daoocclusionblur.psh.fxo"),
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
    picture = smaa.PICTURE if antialias else "BackBufferCopy"
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
        (smaa.CHAIN_FILTERS if antialias else "")
        + CHAIN_FILTERS.replace("\n{occlusion_look}", look_settings("occlusion")),
        before=True,
    )
    # The picture's parameter comes four times: the grade's pass and its
    # fading form, each for shader model 3 and then 2. Model 3's two are given
    # the look's values, so the fading form, which another chain runs and no
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
        + semantic("DaoOcclusion", [METRES_PARAMETER, *look_parameters("occlusion")])
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
    # Last first, so the places found stay where they are.
    shader.insert(
        read,
        [f"{register}.xyz = DaoShapedBake({register}.xyz, daoRelief);"],
    )
    shader.insert(
        made,
        [f"daoRelief = DaoRelief({normal}.xyz, {flat}.xyz, -{lights.name}[2].xyz);"],
    )
    shader.insert(-1, ["float daoRelief = 0;"])


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


def depth_pixel_by_texture(shader):
    """Write no depth where the surface's texture is clear. The game's depth
    pass reads no texture, so a bush's or a fence's whole card is written, and
    the ambient occlusion shaded the card's outline on what stands behind it
    (seen in Redcliffe, 2026-10-11). The test is the one the picture's own
    pass is drawn with (DAMshPnch1S.sta: alpha over 127 of 255)."""
    shader.add_input("uv", "float2", "TEXCOORD1")
    shader.insert(0, ["clip(tex2D(smpDecal, input.uv).a - 0.5);"])


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
    source = os.path.join(work, (named or name) + ".hlsl")
    with open(source, "w", encoding="utf-8") as f:
        f.write(shader.render(preamble))
    compile_shader(fxc, shader.profile, source, os.path.join(out, named or name))


def build_ours(fxc, source, name, out):
    """One of this folder's own pixel shaders, compiled into `out`."""
    compile_shader(fxc, "ps_3_0", os.path.join(HERE, source), os.path.join(out, name))


def compile_shader(fxc, profile, source, compiled):
    done = subprocess.run(
        [fxc, "/nologo", "/T", profile, "/E", "main", "/Fo", compiled, source],
        capture_output=True,
        text=True,
        check=False,
    )
    if done.returncode != 0:
        raise shader_hlsl.ShaderError((done.stdout + done.stderr).strip())


def ours(name):
    """One of this folder's HLSL files."""
    with open(os.path.join(HERE, name), encoding="utf-8") as f:
        return f.read()


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
    shader.insert(read, [f"{register} = saturate({register});"])
    # And what it writes: the mask is the picture's brightness plus the
    # chain's Brightness, over 1 for anything half bright, and was cut at 1
    # by the eight bits it was written into. Bounding what it reads alone
    # changed nothing (seen the same day).
    shader.insert(len(shader.statements) - 1, ["oC0 = saturate(oC0);"])


def changes_for(stem, listing, map_size, show):
    """What this build changes in one of the game's pixel shaders: the
    functions that change it, and the HLSL they call."""
    changes, preamble = [], ""
    shadow = f"#define DAO_SHADOW_MAP_SIZE {float(map_size)}\n" + ours("shadow.hlsl")
    if stem in RECEIVERS:
        changes.append(with_our_shadow)
        preamble += shadow
    if takes_sun_shadow(stem, listing):
        changes += [with_sun_shadow, with_materials, with_point_lights]
        # Not a face: skin is soft, and its map is pores and lines, which
        # want no sharpening.
        if not stem.startswith("face"):
            changes.append(with_ambient_relief)
        preamble += (
            ours("look.hlsl")
            + CHARACTER_SHADOW_PARAMETERS
            + shadow
            + ours("materials.hlsl")
            + ours("bake.hlsl")
        )
    elif stem.startswith(CHARACTERS) and reads_lamp(listing):
        # A figure's shader the sun's shadow is not put on (it has the
        # shadow's data already): its lamp and fills are the look's all the
        # same, or it would be lit by a lamp at the engine's full strength
        # beside others at the look's.
        changes.append(with_point_lights)
        preamble += ours("look.hlsl")
    if stem == "hslmatrix":
        changes.append(lambda s: with_our_grade(s, occlusion=True, show=show))
        preamble += ours("grade.hlsl")
    if stem == "hslmatrixfade":
        changes.append(with_our_grade)
        preamble += ours("grade.hlsl")
    if stem in BLOOM_MASKS:
        changes.append(with_bounded_bloom)
    if stem in PROPS:
        changes.append(with_ambient_relief)
        preamble += ours("look.hlsl") + ours("bake.hlsl")
    if takes_baked_light(listing):
        changes.append(with_shaped_bake)
        if "daoLook" not in preamble:
            preamble += ours("look.hlsl") + ours("bake.hlsl")
    return changes, preamble


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    out = argv[0]
    map_size = int(argv[1]) if len(argv) > 1 else 1024
    show = "show-occlusion" in argv
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
    for stale in os.listdir(out):
        if stale.lower().endswith((".fxo", ".xml", ".dds")):
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
    for name, text in (
        ("da_hsvmatrix.ffx.xml", chain),
        ("hslmatrix.mat.xml", material),
    ):
        with open(os.path.join(out, name), "w", encoding="utf-8", newline="") as f:
            f.write(text)

    materials = script_commands.erf_entries(os.path.join(data, "materials.erf"))
    for theirs, ours_, names, _ in DEPTH_VERTICES:
        for name in names:
            text = with_textured_depth(
                materials[name].decode("utf-8-sig"), (theirs, ours_)
            )
            with open(os.path.join(out, name), "w", encoding="utf-8", newline="") as f:
                f.write(text)
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
        for (theirs, ours_), change, preamble in [
            ((theirs, ours_), globals()[change], "")
            for theirs, ours_, _, change in DEPTH_VERTICES
        ] + [(DEPTH_PIXEL, depth_pixel_by_texture, "sampler2D smpDecal;\n")]:
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
                changes, preamble = changes_for(
                    name[: -len(".psh.fxo")], listing, map_size, show
                )
                if not changes:
                    return None
                build(fxc, name, listing, out, changes, preamble, work)
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
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
