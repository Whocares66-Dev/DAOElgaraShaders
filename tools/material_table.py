"""What each of the game's solid materials is: data\\materials.ini, and a metalness map for
each material that is metal in part.

    python tools/material_table.py [--game <folder>] [--ini data/materials.ini]
                                   [--out build/materials] [--no-maps] [--no-review] [--no-levels]
    python tools/material_table.py --why <regex>    the class and the reason of each material so named
    python tools/material_table.py --names          the level's and the props', by the word that classed them

The game's textures are a specular and gloss set: a diffuse colour, a specular mask whose
RGB is the mirror colour and whose alpha is the gloss, a normal map. They have no
metalness, and the standard model the shaders are written to (a metal mirrors its base
colour and scatters nothing; a non-metal mirrors 0.08 x its mask and scatters its base
colour) wants one. A shader that guessed it per pixel from gloss and mirror colour was
wrong in ways that showed (a painted shield went dark as a metal, stone benches with a
bright painted specular were mirrors), so what a surface is is DATA, made here once, to
be read and corrected by hand (the user, 2026-10-12).

Read: every material object (<name>.mao, a small XML) of the six solid, lit definitions,
from the game's archives; the others (effects, the interface, sky, water, lava, hair, eyes,
faces, terrain) are counted and left.

Written:

data\\materials.ini
    One line a material object, `name = class`, sorted. A line whose comment begins
    `by hand` is kept as it is by every later run. The classes:

    metal    the whole surface is metal: metallic 1
    mixed    metal and not, in one texture: its metalness is a map (below)
    stone, wood, cloth, leather, skin, foliage, other
             not metal: metallic 0. Kept apart because they are useful to read and may
             matter later; `other` is also what a material is where the evidence does not
             say. A wrong `metal` is worse than a missed one.

<out>\\<specular mask's name, without its s>_m.dds
    For each `mixed` material, how much of each texel is metal: DXT1, read in its green,
    a quarter of the specular mask's side and 256 at most, with its mips. DXT1 and not L8
    because the game's own loader is known to read it (5789 of the game's textures are
    DXT1 and one is L8, read 2026-10-12), it is half L8's size, and a mask that is flat
    but for its edges loses nothing to a block's four levels; green because it has six
    bits where red and blue have five.

<out>\\review\\<class>_<n>.jpg, summary.md
    Contact sheets to check the classes by eye (each material's diffuse, its mirror
    colour, its gloss, and for `mixed` its map), and the counts, the list of `other` and
    the classifications least sure.

HOW A MATERIAL IS CLASSED. By what it is called first: its own name, its diffuse
texture's name and the project it was exported from (a comment in each .mao names it:
ARM_MAS, ARM_LGT, CTH_COM, Prop_Kitchen ...), through the tables below. The footstep
sound a level material carries is used only where the names say nothing. A name can say
`metal` outright (an anvil, a chain). Where it says only that the thing has metal in it
(armour, a weapon, a shield, a helmet, a suit on a stand, a heap of coin) the textures say
how much, by the rule for a texel: nearly all of it and it is `metal`, some and it is
`mixed`, none and it is what else its family is made of (leather, wood). The textures
never make a metal of a material whose name does not allow one: a level's and a prop's
gloss is one flat value and its painted mirror is as bright on stone as on iron, so paint
alone cannot tell them.

THE RULE FOR A TEXEL (metal_texels) is stated once, there.

WHAT THIS CANNOT SEE. A shield's heraldry is not in its textures: Weapon.mat blends a
heraldry map (mml_tHeraldryMap, set per item; the h_* material objects name no texture)
over the shield at run time, so a steel shield's map says steel under the paint and the
shader must take the paint's share off its metalness. A tint (mml_tTintMask and an item's
colours) is likewise the game's at run time: the rule reads the diffuse as it is stored,
grey under a tint of gold or of dragonbone's red. The add-ins' own material objects are in
enciphered archives (ERF V2.2) and are listed in the summary, unread.
"""

import argparse
import collections
import io
import os
import re
import struct
import sys

try:  # the classes by name need neither; the textures need both
    import numpy as np
    from PIL import Image, ImageDraw
except ImportError:  # pragma: no cover
    np = None

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
GAME = os.environ.get("DAO_GAME_DIR", r"C:\Program Files (x86)\GOG Galaxy\Games\Dragon Age Origins")

SOLID = ("static.mat", "prop.mat", "character.mat", "weapon.mat", "cloth.mat", "canopy.mat")
CLASSES = ("metal", "mixed", "stone", "wood", "cloth", "leather", "skin", "foliage", "other")
# What a name can say besides a class: "the textures say how much of this is metal".
PAINT = "paint"

# --------------------------------------------------------------------------------------
# The rule for a texel. All of it is here; no material has a number of its own.
#
# Measured over the game's armour, weapons and shields, 2026-10-12 (the mirror is the
# specular mask's brightest channel, the surface's own colour the diffuse's):
#   plate, blades, rims, bosses, buckles and studs are painted with a mirror of 0.2 to 0.9
#   and a gloss of 0.15 to 0.6, and their mirror is a grey copy of their own colour, 0.5
#   to 1.5 times as bright (massive plate's median 0.7 to 0.9, a kite shield's 0.67);
#   leather, cloth, wood and skin have a mirror under 0.15 and a gloss under 0.1 nearly
#   everywhere, and a mirror a tenth to two fifths of their own colour;
#   the exceptions are sheens. Gold and bronze have a mirror tinted their own hue. Some
#   leather suits (pm_arm_lgta, lgtb, pf_arm_lgtc) have a BLUE mirror as bright as mail's
#   (0.22, with a gloss of 0.12) over brown or grey leather, its saturation 0.13 to 0.5;
#   steel's mirror is grey (0.02), but a cold steel's can be as blue as 0.3 (a round
#   shield's iron rim, 0.17), and that has a gloss of 0.2 and a mirror of 0.4.
MIRROR_MIN = 0.16  # a metal mirrors brightly
GLOSS_MIN = 0.10  # and is glossy (a power of 26 and up, of the game's 1 to 256)
MIRROR_OF_OWN = 0.35  # its mirror is no dimmer than this share of its own colour
COLOURED = 0.30  # a saturation from which a surface's own colour is a colour, not a grey
TINTED = 0.12  # a mirror's saturation from which it has a tint
SHEEN = 0.30  # and from which it is a colour of its own
GLOSS_STEEL = 0.16  # the gloss and the mirror from which a mirror tinted cool is cold steel's,
MIRROR_STEEL = 0.30  # not a sheen
HUE_APART = 45.0  # degrees between a surface's hue and its mirror's that make them two colours
WARM = (15.0, 65.0)  # the hues, in degrees, of copper, bronze, brass and gold
SEEN = 0.12  # a colour darker than this has no hue to speak of
USED = 0.04  # a texel black in both textures is the atlas's unused space
SURE = 2.0  # a texel past the least mirror and gloss by this factor is metal with no neighbour's say
# A material whose name says "has metal in it":
SHARE_MIXED = 0.02  # this share of its used texels metal, and it is `mixed`
SHARE_METAL = 0.85  # this share, and all of it is `metal`


def hue_saturation(rgb):
    """A colour's hue in degrees, its saturation and its brightest channel."""
    top = rgb.max(axis=2)
    low = rgb.min(axis=2)
    spread = np.maximum(top - low, 1e-6)
    red, green, blue = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    hue = np.where(top == red, ((green - blue) / spread) % 6, np.where(top == green, (blue - red) / spread + 2, (red - green) / spread + 4))
    return hue * 60, (top - low) / np.maximum(top, 1e-4), top


def metal_texels(diffuse, mirror, gloss, surely=False, shaded=False):
    """Which texels are metal: a mask, from the diffuse colour (h, w, 3), the specular
    mask's colour (h, w, 3) and its gloss (h, w), all 0 to 1 and of one size.

    A texel is metal where ALL of these hold:
      1. it mirrors brightly: its mirror is MIRROR_MIN or more;
      2. it is glossy: GLOSS_MIN or more;
      3. its mirror is as bright as its own colour (MIRROR_OF_OWN of it or more): a metal
         was painted with a mirror copied from its colour, a cloth or a bone with a dim
         one under a bright colour. A dark surface with a bright mirror passes, which is
         what a metal is;
      4. its mirror is its own colour:
         where the surface is coloured, the mirror is tinted the same hue (gold mirrors
         gold) -- a paint, a dye, a leather or a wood mirrors white, or another colour.
         But brass studs and gilt trim are painted yellow under a WHITE mirror, so a
         surface of a warm colour (WARM: orange to yellow, the hues of copper, bronze,
         brass and gold) with a grey mirror is metal too where that mirror is bright and
         glossy twice over (SURE): as no leather's or wood's is;
         where the surface is grey or too dark to have a hue, the mirror is grey, or
         tinted warm, or tinted cool (green to violet) only a little (under SHEEN) and
         then with a steel's gloss and brightness (GLOSS_STEEL, MIRROR_STEEL): a cool
         tint on a dull or a dim mirror is the sheen painted on leather, and no metal's
         mirror is blue outright.
    Why not gloss and mirror alone: they pass painted heraldry, dyed cloth with a satin
    highlight and oiled leather, which were the bugs.

    With surely, the same asked twice as hard: SURE times the least mirror and the least
    gloss, and no cool tint at all. That is a stud, a rivet, a buckle: a white dot on a
    black mask, which needs no neighbour to be believed (metal_map).

    With shaded, the same asked half as hard of the mirror and the gloss (1 / SURE of the
    least; what it is beside its own colour, and its hue, as ever): a metal with shade
    painted over it, which is believed only where it joins metal (metal_map).
    """
    surface_hue, surface_saturation, own = hue_saturation(diffuse)
    mirror_hue, mirror_saturation, bright = hue_saturation(mirror)
    coloured = (surface_saturation >= COLOURED) & (own >= SEEN)
    apart = np.abs((surface_hue - mirror_hue + 180) % 360 - 180)
    same_hue = (mirror_saturation >= TINTED) & (apart <= HUE_APART)
    cool = (mirror_saturation >= TINTED) & (mirror_hue > 65) & (mirror_hue < 345)
    sheen = cool & ((mirror_saturation >= SHEEN) | (gloss < GLOSS_STEEL) | (bright < MIRROR_STEEL))
    twice = (bright >= SURE * MIRROR_MIN) & (gloss >= SURE * GLOSS_MIN)
    gilt = (surface_hue >= WARM[0]) & (surface_hue <= WARM[1]) & (mirror_saturation < TINTED) & twice
    own_colour = np.where(coloured, same_hue | gilt, ~(cool if surely else sheen))
    least = SURE if surely else 1.0 / SURE if shaded else 1.0
    return (bright >= least * MIRROR_MIN) & (gloss >= least * GLOSS_MIN) & (bright >= MIRROR_OF_OWN * own) & own_colour


def box(a, side=3):
    """Each texel's mean over the square of that (odd) side about it, the edge repeated."""
    half = side // 2
    p = np.pad(a, [(half, half), (half, half)] + [(0, 0)] * (a.ndim - 2), mode="edge")
    h, w = a.shape[:2]
    rows = sum(p[y : y + h] for y in range(side))
    return sum(rows[:, x : x + w] for x in range(side)) / float(side * side)


def resized(a, width, height):
    """An array (h, w) or (h, w, n) averaged down (or up) to width by height."""
    if a.shape[1] == width and a.shape[0] == height:
        return a
    planes = a.reshape(a.shape[0], a.shape[1], -1)
    out = [np.asarray(Image.fromarray(planes[..., i].astype(np.float32), "F").resize((width, height), Image.BOX)) for i in range(planes.shape[2])]
    return np.stack(out, axis=2).reshape((height, width) + a.shape[2:])


def map_size(width, height):
    """A quarter of the specular mask's side, 256 at most, 4 at least."""
    width, height = max(4, width // 4), max(4, height // 4)
    while max(width, height) > 256:
        width, height = max(4, width // 2), max(4, height // 2)
    return width, height


def joined(seeds, may):
    """`seeds` and every texel of `may` that is joined to one of them, edge to edge,
    through texels of `may`."""
    grown = seeds.copy()
    may = may | seeds
    while True:
        wider = grown.copy()
        wider[1:, :] |= grown[:-1, :]
        wider[:-1, :] |= grown[1:, :]
        wider[:, 1:] |= grown[:, :-1]
        wider[:, :-1] |= grown[:, 1:]
        wider &= may
        if (wider == grown).all():
            return grown
        grown = wider


PLATE = re.compile(r"ARM_(MAS|HVY|DWP)", re.I)  # the projects of massive, heavy and dwarven plate


def is_plate(material):
    """Whether a material is of a suit of plate, by the project it was exported from:
    its armour, and the helms, gloves and boots made with it."""
    return bool(PLATE.search(material.project))


def metal_map(diffuse, specular, plate=False):
    """The metalness of a material from its diffuse and its specular mask (h, w, 4 each,
    0 to 1): (map, share). The map is at map_size, 0 to 1; the share is of the texels in
    use. The rule is asked at the specular mask's own size, of colours each averaged with
    its neighbours (DXT's noise is a texel wide). A texel that passes then goes with the
    majority of its neighbourhood, a square about a 128th of the texture across (3 texels
    of a mask of 256, 5 of 512, 9 of 1024), which takes out the specks of a paint that
    only just passes and fills the pinholes of one that only just fails; but a texel that
    passes twice over (surely) stands whatever is about it, or the majority would take
    every stud off studded leather (it did, 2026-10-12). The mask is then averaged down
    to the map, which is all the blur there is: an edge is one map texel wide, and a stud
    smaller than a map texel is a grey one, not lost.

    And shade is not a material. The game's textures have shade painted into them, colour,
    mirror and gloss alike: a helm's skull is dark where it goes under its brow plate. By
    the least mirror and gloss alone the shaded half of that skull was no metal, drawn
    matte and pale beside the rest's dark mirror, with an edge where the paint's shade
    began, and in play the edge read as the helm's plume seen through it (the user,
    2026-10-12, a fifth time). So what passes for metal but for being dim (metal_texels'
    `shaded`) is metal where it joins metal that passes outright, through texels like
    itself: the brush's shade leads back to the lit metal it was laid on. A dim thing
    that touches no metal is what it was, a strap, a cloth, the dark between plates.

    Only with `plate`, in a suit of plate (is_plate). Shade cannot be told from a dark
    thing by the textures: tried for every material, the same rule made metal of a
    leather boot's dark glossy hide where it met its gilt trim, and of a shield's dark
    ground. In plate what is dim, glossy and joined to plate is plate. The share is of
    the rule without it, so no material's class moves by it.
    """
    height, width = specular.shape[:2]
    if max(width, height) > 1024:  # a level's atlas: the rule at a quarter is the same answer
        width, height = width // 4, height // 4
        specular = resized(specular, width, height)
    diffuse = resized(diffuse[..., :3], width, height)
    used = (diffuse.max(axis=2) > USED) | (specular[..., :3].max(axis=2) > USED)
    colours = box(diffuse), box(specular[..., :3]), box(specular[..., 3])
    metal = box(metal_texels(*colours).astype(np.float32), max(3, -(-max(width, height) // 128) | 1)) > 0.5
    metal |= metal_texels(*colours, surely=True)
    share = float(metal[used].mean()) if used.any() else 0.0
    if plate:
        metal = joined(metal, metal_texels(*colours, shaded=True) & used)
    return resized(metal.astype(np.float32), *map_size(width, height)), share


# --------------------------------------------------------------------------------------
# A map as a file.


def dxt1_blocks(grey):
    """A grey image (h, w; 0 to 1; sides multiples of 4) as DXT1 blocks: each 4x4 block is
    its brightest and darkest texels as 5:6:5 colours and a 2-bit choice a texel among
    them and the two thirds between."""
    h, w = grey.shape
    blocks = grey.reshape(h // 4, 4, w // 4, 4).transpose(0, 2, 1, 3).reshape(-1, 16)
    top = np.rint(blocks.max(axis=1) * 63).astype(np.uint32)
    low = np.rint(blocks.min(axis=1) * 63).astype(np.uint32)
    # green's six bits carry the value; red and blue follow it, so the file opens as a grey picture
    c0 = ((top >> 1) << 11) | (top << 5) | (top >> 1)
    c1 = ((low >> 1) << 11) | (low << 5) | (low >> 1)
    g0 = ((top << 2) | (top >> 4)).astype(np.float32)[:, None] / 255
    g1 = ((low << 2) | (low >> 4)).astype(np.float32)[:, None] / 255
    palette = np.stack([g0, g1, (2 * g0 + g1) / 3, (g0 + 2 * g1) / 3], axis=2)
    choice = np.abs(blocks[:, :, None] - palette).argmin(axis=2).astype(np.uint32)
    choice[c0 <= c1] = 0  # one colour: with c0 not over c1 the choices mean other things
    bits = (choice << (2 * np.arange(16, dtype=np.uint32))).sum(axis=1, dtype=np.uint32)
    out = np.empty(len(blocks), dtype=[("c0", "<u2"), ("c1", "<u2"), ("bits", "<u4")])
    out["c0"], out["c1"], out["bits"] = c0, c1, bits
    return out.tobytes()


def dds_dxt1(grey):
    """A grey image as a DDS file: DXT1, with every mip down to one texel."""
    height, width = grey.shape
    levels = []
    level = grey.astype(np.float32)
    while True:
        h, w = level.shape
        padded = np.pad(level, [(0, -h % 4), (0, -w % 4)], mode="edge")
        levels.append(dxt1_blocks(np.clip(padded, 0, 1)))
        if h == 1 and w == 1:
            break
        level = resized(level, max(1, w // 2), max(1, h // 2))
    linear = max(1, (width + 3) // 4) * max(1, (height + 3) // 4) * 8
    # DDSD_CAPS | HEIGHT | WIDTH | PIXELFORMAT | MIPMAPCOUNT | LINEARSIZE
    header = struct.pack("<4s7I44x", b"DDS ", 124, 0x000A1007, height, width, linear, 0, len(levels))
    header += struct.pack("<2I4s5I", 32, 0x4, b"DXT1", 0, 0, 0, 0, 0)  # DDPF_FOURCC
    header += struct.pack("<5I", 0x00401008, 0, 0, 0, 0)  # DDSCAPS_COMPLEX | TEXTURE | MIPMAP
    return header + b"".join(levels)


def map_name(specular):
    """The map's file for a specular mask's resource name, its s (or _s, or _spec) taken
    off and a level of detail's mark kept: pn_arm_masa_0s.dds -> pn_arm_masa_0_m.dds,
    pn_arm_masa_0sl2.dds -> pn_arm_masa_0l2_m.dds, tti_master_s.dds -> tti_master_m.dds."""
    stem = os.path.splitext(specular.lower())[0]
    found = re.fullmatch(r"(.*?)(?:_spec|_?s)((?:_?l[23])?)", stem)
    return (found.group(1) + found.group(2) if found else stem) + "_m.dds"


# --------------------------------------------------------------------------------------
# The classes by name. Each table is read from its top and the first line that matches
# says. A pattern is a regular expression over "name diffuse project", lower case.

# The people's clothes and armour, by the code in the name: <who>_<piece>_<kind><variant>.
# The projects are ARM_MAS (massive: plate), ARM_HVY (heavy: plate over mail and scale),
# ARM_DWP (dwarven plate), ARM_MED (medium: mail, splint and scale over leather), ARM_LGT
# (light: leather, some of it studded), ARM_NUD (the bare body), CTH_ (clothes), ROB_ (robes).
WORN = (
    (PAINT, "other", r"(?<![a-z])(arm|boo|glv)_(mas|hvy|dwp|med)"),  # plate and mail: the textures say where
    (PAINT, "leather", r"(?<![a-z])(arm|boo|glv)_lgt"),  # leather: studs and buckles, if it has them
    ("skin", None, r"(?<![a-z])arm_nud"),
    ("cloth", None, r"(?<![a-z])(cth|rob|chd)_"),  # clothes, robes, the children
    (PAINT, "cloth", r"(?<![a-z])hlh_g\d"),  # the mages' cowls (the project is ROB_ENC)
    (PAINT, "leather", r"(?<![a-z])hlh_l\d"),  # the light helmets: leather caps
    ("metal", None, r"(?<![a-z])hlh_crn|crown"),  # the crown: gold, painted with a grey mirror, which the rule takes for paint
    (PAINT, "other", r"(?<![a-z])hl[fh]_|(?<![a-z])hmf_"),  # helmets
)

# The creatures (c_, cn_, cb_, ch_ on Character.mat). Bare bodies but for these.
CREATURES = (
    ("other", None, r"default|^mat_\d|baked|_bak_|mediumred"),  # and a whole figure baked into one sheet for a crowd
    ("wood", None, r"grandoak|_(bdy|bra)_(gra|wsv)"),  # the sylvans' trunks
    ("stone", None, r"cn_golem(?![a-z])|_stga|_sng|sng_"),  # the stone golem, Shale
    ("other", None, r"ashwr"),  # a wraith of ash
    ("other", None, r"_skl"),  # the skeleton: bone, and the rule found metal on its skull
    ("cloth", None, r"_arc(?![a-z])|_arh(?![a-z])"),  # the arcane horror: a skeleton in a robe
    # A head is never asked of the paint. Seen 2026-10-12: the rule marked the armoured
    # ogre's horns, a hurlock's teeth, a genlock's wet face and the revenant's hood, and
    # left the revenant's mail; only one genlock's crest came out right.
    ("other", None, r"_hed_rev"),  # a skull in a hood and mail
    ("skin", None, r"^ch_|_hed_"),
    # the bodies of those that wear plate: hurlocks, genlocks, the Disciples, the armoured
    # ogre, the revenant (and its sword and shield), the steel golem, Duncan
    (PAINT, "skin", r"hlk|dshulok|glk|_dis|_aog|_low_"),
    (PAINT, "other", r"_rev|golemb|_stgb|bdy_dun"),
    ("skin", None, r"^c[bn]?_"),
)

# Weapons: w_<kind>_<which>.
WEAPONS = (
    ("other", None, r"default|_msk(?![a-z])"),
    ("stone", None, r"w_bld_"),  # the ogre's boulder
    # staffs, wands and arrows are wood, and quivers leather, whatever their paint: a pale
    # staff with a high gloss came out three quarters metal, a quiver's stitching as studs
    ("wood", None, r"w_(stf|wnd|arr)_"),
    ("leather", None, r"w_qui_"),
    (PAINT, "wood", r"w_(sbw|lbw|cbw)_"),  # bows: some have a metal grip or limbs
    (PAINT, "wood", r"w_shd_"),  # shields
    (PAINT, "other", r"w_"),  # blades, axes, maces, mauls
)

# Props and level surfaces (Prop.mat, Static.mat), by the words in their names. A word is
# written as its makers ran it together (cratelarge, bedlrgwood, canldeholder); a word here
# may be a regular expression, and (?<![a-z]) keeps a short one from being found inside a
# longer one. What was seen in the review sheets and changed a line is said beside it.
THINGS = (
    # not things at all: defaults, placeholders, the toolset's own, effects' meshes
    ("other", None, r"default_ placeholder grey_dif gray50 (?<![a-z])tools_ test_sphere gizmo ^mesh(?![a-z]) ^fx_ zz_ textureatlas ^palette ^cs_ ^darkspawn"),
    ("other", None, r"black0 blacksquare dwi_black dci_black prp_black (?<![a-z])water waterpool poolwater pool_water liquid lprop blood slime ice(wall|_mesh) snow mousnow"),
    # glass, crystal, and things that glow
    ("other", None, r"lyrium cryst gem\d ruby mirror(?!bronze) window skybeam glass vial bottle decanter phylact apparatus spirtapp spiritapp"),
    # the dead, and what grows in the Deep Roads
    ("other", None, r"corps deadbody dead(ox|halla) hang(ing)?(fe)?male hurlockhead bonepile bnepile bneple bones skull eaten fleshpod fleshtile tileflash cocoon eggsac msac msask eggs_ tentical dragonegg nugplatter rstnug hngingnug hanging_nug spider sipder web totem drkspstuf barrdrksp dstools tooltorture torture_tools bonfire fire0 curio reliquary sextant sexant turk cage_?human tub(?![a-z]) p_tub wash_?basin"),
    # food, wax, ash, earth, paper and paint
    ("other", None, r"food bread cheese fish(?!ing|net|_net|_carv|carv) meat fruit (?<![a-z])pie(?![a-z]) pumpkin cabbage garlic pepper veggie vegetable candled?(?![a-z_]) ash(es|e)(?![a-z]) bonfireash dirt sand(?![a-z]) filth trash footpri paint poster map(01|_01)?(?![a-z]) lake_map scidraw science_drawing recipe book scrol prt_ portrait"),
    # iron and steel, bronze, brass, pewter, gold: things that are metal through
    # (an armour stand is not here: it is a stand of planks, seen 2026-10-12)
    ("metal", None, r"(?<!hro_)anvil(?!_?lyr|_floor|_wall|_?base) chain shackle grate goldmetal blackiron ironblack dwmetal dwmet(?![a-z]) fci_metal prp_metal metal_0?1 metallatch wallarch(iron|bar) cabinet_?steel chest_?iron"),
    ("metal", None, r"cauldron couldron pewter brasstorch mirrorbronze goblet gobwyrm coal_?pan incen(t|se) trpblade bladetrap trptciblade trpbladetci bird_?cage tongs cog[co]?\d canldeholdermetal candlemetal crown"),
    # a suit of armour, weapons in a rack, a blade with a wooden grip: painted as the worn
    # and the wielded are, so the textures say where the metal is
    (PAINT, "other", r"armorsuit medarm medboo hmf_ coin"),
    # (a rack of bows is wood; the smith's tools are drawn on a white ground that the rule
    # took for the metal, and are left)
    ("other", None, r"wpnpage blacksmith"),
    (PAINT, "wood", r"wpn(axe|d?sword|barrel) sword swrd butcher_knife hammer tree_?saw gong chand\d chand_ portcullis"),
    # wood that a word for cloth would otherwise take
    ("wood", None, r"wood_?(and)?_?rope tci_wood tent_?pole"),
    # cloth, rope and stuffing
    ("cloth", None, r"cloth clth rug(?![a-z]) rug(blue|red|green|end) dwfrug carp(et)?(?![a-z]) bluecarp redcarp tapestr curt banner flag falg tent(?!ical) canvas tarp sack bedding bed_?roll pillow matress rags bandage rope net(01|_01)(?![a-z]) fish_?net couch straw_?mat"),
    ("leather", None, r"leather fur(?!n) hide bridal whip saddle dryingrack"),
    # things that grow
    ("foliage", None, r"leaf leaves lv(?![a-z]) brdl shrb frond ivy plant flower shrub(?!bark) moss(?!tile) grass herb(?!o) tomato pine_?ne ter_pine needle hay(?![a-z]) haystack heystack hay_bale haybale straw scarecrow small_?tree treeline branch"),
    # stone that a wooden word would otherwise take: the dwarves' doors and furniture, the
    # Tevinter benches and tables of the mages' tower, marble and stone tables
    ("stone", None, r"door\w*dwa?r?f dwarf_door dwfsecretdoor doorcitydwf orz_ent prp_dni_ dw_torch demon_torch tev(bench|table|desk|chair|wartable|pot|statu) dw_?(bench|table|chair|bed|box|couch|chest|slab|vat|amph|oven|sarco|floorart|rubble|rock|carving) table_?stone bench_?marble stone_well stnwell bath_?stone rock_topiary rcktopiary d[cw]i_throne dwi_beam doorwy tvetower rubptc"),
    # wood
    ("wood", None, r"wood log(?!hain) bark tree stump root plank beam barrel barr(?![a-z]) crate cask keg table chair chra desk shelf shlve shelve slf\d bkslf bkshlf shlfs cabinet armoire dresser vanity bed(?!ding|_?roll|room) bunk door _dr(?![a-z]) gate gt_ cart wagon waggon carriage boat buoy float ladder lader fence fnc stool bucket bowl cup plate_?wood throne pulpit pew(?![a-z]) loom scaf stage stg(ladder|stair) sign sighn bridge(?!cobble|rocks|stone|trims|wall) pole plough wheel trough saw_?horse hitching (?<!c)rack coff chest chst easel stall stand stn?d watchtower hermithut aravel instrument musinst toy basket washboard chopping chopblock butcher_?board lever panel platform trapdoor coop dummy stretcher feeder spit invention woodendragon carving(?!\d) pinewall eci_wall cage torch scale ballista market_ wpn(bow|quiver) chantry_?altar ferldnaltar ferelden_altar germaltar german_altar head_?base statuferel ferelden_statue wall_?carv chamber_?pot iroof interiorroof"),
    # stone, and what a wall or a floor is where its name says no more
    ("stone", None, r"stone stn rock rck marble brick cobble statu stat(mnstr|paragn|andraste) paragon pillar grave tomb sarco altar wall floor stair trim plaster stucco concrete cave boulder rubble slab carv fountain fnt\d well(?![a-z]) wellint bath pedestal head_?base idol sundial grindstone oven forge fire_?pit fire0 cliff mountain mount(?![a-z]) vista vis_mt ruin arch ceiling igrnd egrnd ground roof eagle demonhead tci_dh plug support lavark lavapan amph vat(?![a-z]) urn vase pot(?![a-z])|pot\d|pot_|potcook jar jug dish doll pkr circle drak fort castle city bldng blding pal_ estate estwall doorwy ter_ terfade fade_?rock face_rock fie_ fei_ fadportal sulf steamvent magicrock mage(stat|c) goldmage dwhead dbone silver sconce sonce brazier registry mage01"),
)

# A tileset's one sheet, <code>_master: walls, floors, beams and fittings in one texture.
# It is classed by what most of it is, which is the tileset's fabric, and its iron is missed.
TILESETS = {
    "stone": "tti tci tce tve tvi tvm tfi fca fci fce fdi fne ftr chi che cav cai cdr dni dci dce dne ace vst hro fie fei",
    "wood": "fhi fti fvi fve fhe dsi dse ave vki ppm",
}
# The footstep sound a level material carries (mml_iSoundMaterialType), where it is one
# that names a class. Most are 0, and a 5 is on wood as often as a builder forgot it.
SOUNDS = {1: "wood", 2: "cloth", 3: "foliage", 4: "foliage", 5: "stone", 13: "metal", 15: "cloth"}

TABLES = {name: tuple((cls, fallback, re.compile("|".join(words.split()))) for cls, fallback, words in table) for name, table in (("worn", WORN), ("creatures", CREATURES), ("weapons", WEAPONS), ("things", THINGS))}
# A prop that wears a weapon's texture, or a suit's or a helmet's, is classed as it.
WIELDED = re.compile(r"w_(lsw|gsw|dgr|axe|gax|mal|mce|shd|sbw|lbw|cbw|stf|wnd|qui|arr)_")
WORN_TEXTURE = re.compile(r"(p[nmf]|prp)_(arm|boo|glv|hlf|hlh)_")
LOD = re.compile(r"_l[23]$")


class Verdict:
    """What the names say of a material: a class, or PAINT with the class it falls back
    to; why; and how unsure (0 to 1)."""

    def __init__(self, cls, why, doubt=0.1, fallback=None):
        self.cls, self.why, self.doubt, self.fallback = cls, why, doubt, fallback


def look_up(table, text):
    for cls, fallback, pattern in TABLES[table]:
        found = pattern.search(text)
        if found:
            return cls, fallback, found.group(0).strip()
    return None


def by_name(name, definition, diffuse="", project="", sound=None):
    """The Verdict of the names alone: the material's own, its diffuse texture's and its
    project's, and for a level material its footstep sound. Needs no game."""
    name = LOD.sub("", name.lower())
    stem = re.sub(r"(_0?d|_d|_diff|_dif|d)?(_?l[23])?\.dds$", "", diffuse.lower())
    project = re.sub(r"^readonly-\d+-rev-\d+-|\.matproj$", "", project.lower())
    text = f"{name} {stem} {project}"
    if definition == "canopy.mat":
        return Verdict("foliage", "Canopy.mat: a sylvan's leaves and twigs")
    if definition == "cloth.mat":
        if "default" in name:
            return Verdict("other", "a default")
        return Verdict("cloth", "Cloth.mat: what the cloth simulation moves")
    if definition == "weapon.mat":
        cls, fallback, word = look_up("weapons", text) or ("other", None, "")
        return Verdict(cls, f"a weapon ({word})" if word else "Weapon.mat, no name known", 0.1 if word else 0.6, fallback)
    if definition == "character.mat":
        found = look_up("worn", text)
        if found:
            return Verdict(found[0], f"worn ({found[2]})", 0.1, found[1])
        found = look_up("creatures", text)
        if found:
            return Verdict(found[0], f"a creature ({found[2]})", 0.15 if found[0] != PAINT else 0.3, found[1])
        return Verdict("other", "Character.mat, no name known", 0.6)
    # a level's or a prop's
    if not diffuse:
        return Verdict("other", "no diffuse texture" + (": heraldry, drawn over a shield" if "heraldry" in project else ""), 0.0)
    master = re.match(r"([a-z]{3})_(test)?master|([a-z]{3})_mastersheet", name)
    if master:
        code = master.group(1) or master.group(3)
        for cls, codes in TILESETS.items():
            if code in codes.split():
                return Verdict(cls, f"the {code} tileset's sheet: mostly {cls}, its fittings with it", 0.3)
    found = look_up("weapons", stem) if WIELDED.match(stem) else look_up("worn", stem) if WORN_TEXTURE.match(stem) else None
    if found:
        return Verdict(found[0], f"wears {stem}, a {'weapon' if stem.startswith('w_') else 'worn thing'}'s texture", 0.2, found[1])
    found = look_up("things", text)
    if found:
        cls, fallback, word = found
        heard = SOUNDS.get(sound)
        if heard and heard not in ("metal", "stone") and cls in ("metal", PAINT):
            # its makers gave it a wooden footstep, or a cloth's: too much against a name (a stone's is
            # what a tileset's pieces were given whatever they are: fci_metallatch has it)
            return Verdict(heard, f"named {word}, which could be metal, but sounds of {heard} underfoot", 0.5)
        disagree = heard and cls not in (PAINT, "other", heard)
        return Verdict(cls, f"named {word}" + (f"; but sounds of {heard} underfoot" if disagree else ""), 0.35 if disagree else 0.1, fallback)
    for cls, codes in TILESETS.items():
        if name[:3] in codes.split() and name[3:4] == "_":
            return Verdict(cls, f"of the {name[:3]} tileset", 0.4)
    if sound in SOUNDS and SOUNDS[sound] != "metal":
        return Verdict(SOUNDS[sound], f"sounds of {SOUNDS[sound]} underfoot, and no name known", 0.5)
    return Verdict("other", "no name known" + ("; sounds of metal underfoot, which is too little to call it one" if sound == 13 else ""), 0.6)


def settle(verdict, share, ragged=False):
    """The class of a material whose name said PAINT, from the share of its texels the rule
    found metal (None: it has no specular mask to ask): (class, why, doubt). A creature's
    hide whose map came out as specks (ragged) is left a hide: wet skin and horn are
    painted bright and glossy too, and the rule cannot be believed there."""
    if verdict.cls != PAINT:
        return verdict.cls, verdict.why, verdict.doubt
    if share is None:
        return verdict.fallback, f"{verdict.why}; no specular mask to ask, so {verdict.fallback}", 0.5
    said = f"{verdict.why}; {share:.0%} of it painted as metal"
    if share >= SHARE_METAL:
        return "metal", said, verdict.doubt + (0.3 if share < SHARE_METAL + 0.05 else 0.0)
    if ragged and verdict.fallback == "skin" and share < SHARE_METAL:
        return "skin", f"{said}, but in specks, so skin", verdict.doubt + 0.3
    if share >= SHARE_MIXED:
        near = share < 2 * SHARE_MIXED or share > SHARE_METAL - 0.05
        return "mixed", said, verdict.doubt + (0.3 if near else 0.0)
    expected = verdict.fallback == "other"  # plate, a blade: metal was expected and not found
    return verdict.fallback, f"{said}, so {verdict.fallback}", verdict.doubt + (0.6 if expected else 0.3 if share > SHARE_MIXED / 2 else 0.1)


# --------------------------------------------------------------------------------------
# The game's files.


def read_toc(path):
    """{name: (offset, length)} of an ERF V2.0 archive (a UTF-16 magic, a count, then per
    entry a 32-character UTF-16 name, an offset and a length); for a V2.2 one, whose
    contents are enciphered (the add-ins'), its names with None; None for anything else."""
    with open(path, "rb") as f:
        head = f.read(56)
        magic = head[:16].decode("utf-16le", errors="replace")
        if magic == "ERF V2.0":
            start, size = 32, 72
        elif magic == "ERF V2.2":
            start, size = 56, 76
        else:
            return None
        (count,) = struct.unpack_from("<I", head, 16)
        f.seek(start)
        table = f.read(size * count)
    out = {}
    for i in range(count):
        name = table[i * size : i * size + 64].decode("utf-16le", errors="replace").split("\0")[0].lower()
        out[name] = struct.unpack_from("<II", table, i * size + 64) if size == 72 else None
    return out


COMMENT = re.compile(r"<!--.*?-->", re.S)


class Material:
    """A material object as its .mao says it."""

    def __init__(self, name, text, archive):
        self.name, self.archive, self.text = name, archive, text
        found = re.search(r'<Material Name="([^"]+)"', text)
        self.definition = found.group(1).lower() if found else "?"
        found = re.search(r"dc:source=file:///(\S+?) ?-->", text)
        self.project = found.group(1).rsplit("/", 1)[-1] if found else ""
        self.textures = {slot: res.lower() for slot, res in re.findall(r'<Texture Name="([^"]+)" ResName="([^"]*)"', text)}
        found = re.search(r'Name="mml_iSoundMaterialType"\s+value="(\d+)"', text)
        self.sound = int(found.group(1)) if found else None

    @property
    def diffuse(self):
        return self.textures.get("mml_tDiffuse", "")

    @property
    def specular(self):
        return self.textures.get("mml_tSpecularMask", "")


class Game:
    """The installed game's material objects and where its textures are. With levels, the
    level archives (packages\\core\\env and the add-ins') are read too, which takes a
    minute and has only ever found terrain, water and vistas there (2026-10-12)."""

    def __init__(self, root=GAME, levels=False):
        self.root = root
        self.materials = {}
        self.textures = {}
        self.skipped = collections.Counter()
        self.unread = {}
        self.differing = set()
        self._images = collections.OrderedDict()
        archives = []
        for top in ("packages", "Addins"):
            for folder, folders, files in os.walk(os.path.join(root, top)):
                if not levels:
                    folders[:] = [f for f in folders if f.lower() != "env"]
                archives += [os.path.join(folder, f) for f in files if f.lower().endswith((".erf", ".rim"))]
        seen = {}
        said = {}
        for path in sorted(archives, key=self._rank):
            toc = read_toc(path)
            if not toc:
                continue
            rel = os.path.relpath(path, root)
            maos = [n for n in toc if n.endswith(".mao")]
            if maos and toc[maos[0]] is None:
                self.unread[rel] = sorted(n[:-4] for n in maos)
                continue
            level = os.sep + "env" + os.sep in rel.lower()
            if not level:
                rank = 0 if os.sep + "high" + os.sep in rel.lower() else 2 if os.sep + "medium" + os.sep in rel.lower() else 1
                for name, at in toc.items():
                    if name.endswith(".dds") and at and (name not in self.textures or rank < self.textures[name][0]):
                        self.textures[name] = (rank, path) + at
            if not maos:
                continue
            with open(path, "rb") as f:
                for name in maos:
                    f.seek(toc[name][0])
                    text = f.read(toc[name][1]).decode("latin-1")
                    what = COMMENT.sub("", text)
                    if name[:-4] in seen:
                        if said[name[:-4]] != what:
                            self.differing.add(name[:-4])
                        continue
                    material = Material(name[:-4], text, rel)
                    seen[material.name] = material.definition
                    said[material.name] = what
                    if material.definition in SOLID:
                        self.materials[material.name] = material
                    else:
                        self.skipped[material.definition] += 1

    def _rank(self, path):
        """The order archives are believed in, for a material object and for a texture of
        one quality alike: a patch; Awakening (packages\\core_ep1); the game's own material
        objects; the rest of the game; the add-ins. Awakening before the game because
        where the two have a name in common and differ, the game's is a placeholder: its
        pf_arm_lgte, pn_boo_lgte and pn_glv_lgte are orange sheets of dots that Awakening's
        pack has as a leather suit (seen 2026-10-12, when the rule made the dots metal)."""
        rel = os.path.relpath(path, self.root).lower()
        tier = 0 if os.sep + "patch" + os.sep in rel else 4 if rel.startswith("addins") else 1 if "core_ep1" in rel else 2 if rel.endswith("materialobjects.erf") else 3
        return tier, [-ord(c) for c in rel] if tier == 0 else [ord(c) for c in rel]

    def image(self, resource):
        """A texture (h, w, 4), 0 to 1, or None if the game has none of that name or it
        will not open."""
        if resource in self._images:
            self._images.move_to_end(resource)
            return self._images[resource]
        picture = None
        if resource in self.textures:
            _, path, offset, length = self.textures[resource]
            with open(path, "rb") as f:
                f.seek(offset)
                body = f.read(length)
            try:
                picture = np.asarray(Image.open(io.BytesIO(body)).convert("RGBA"), dtype=np.float32) / 255
            except (OSError, ValueError, NotImplementedError):
                picture = None
        self._images[resource] = picture
        if len(self._images) > 6:
            self._images.popitem(last=False)
        return picture


# --------------------------------------------------------------------------------------
# The table.


class Row:
    """A material's line: its class, why, how unsure, and for one the textures were asked
    about, its map and the share of it metal."""

    def __init__(self, material):
        self.material = material
        self.cls = self.why = None
        self.doubt = 0.0
        self.share = None
        self.map = None
        self.hand = None


def classify(game, material, want_map=True):
    """A material's Row, from its names and, where they say PAINT, its textures."""
    row = Row(material)
    verdict = by_name(material.name, material.definition, material.diffuse, material.project, material.sound)
    if verdict.cls == PAINT:
        specular = game.image(material.specular) if material.specular else None
        diffuse = game.image(material.diffuse) if material.diffuse else None
        if specular is not None and diffuse is not None:
            row.map, row.share = metal_map(diffuse, specular, is_plate(material))
            if not want_map:
                row.map = None
    row.cls, row.why, row.doubt = settle(verdict, row.share, ragged(row.map))
    if row.cls != "mixed":
        row.map = None
    elif ragged(row.map):
        row.why += "; its map is ragged"
        row.doubt += 0.25
    return row


def ragged(metal):
    """Whether a map is more edge than area: specks, where the paint did not say clearly."""
    if metal is None:
        return False
    some = float((metal > 0.15).mean())
    edge = float(((metal > 0.15) & (metal < 0.85)).mean())
    return bool(some) and edge / some > 0.6


HAND = re.compile(r"^\s*([^;=\s]+)\s*=\s*([a-z]+)\s*;\s*by hand", re.I)


def hand_edits(path):
    """{name: (class, the line as it stands)} of the lines of an existing table that a
    hand has marked: `name = class ; by hand`, with anything after."""
    out = {}
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            for line in f:
                found = HAND.match(line)
                if found:
                    out[found.group(1).lower()] = (found.group(2).lower(), line.rstrip("\r\n"))
    return out


def build_table(game, hands=None, progress=None):
    """{name: Row} for every solid material: classed, a level-of-detail copy (_l2, _l3)
    given its base's class, and a hand's line put over the lot."""
    hands = hands or {}
    rows = {}
    for count, name in enumerate(sorted(game.materials)):
        rows[name] = classify(game, game.materials[name])
        if progress and count % 200 == 0:
            progress(f"{count} of {len(game.materials)}")
    for name, row in rows.items():
        base = rows.get(LOD.sub("", name))
        if base is not None and base is not row and row.cls != base.cls:
            had = row.cls
            row.cls, row.why, row.doubt = base.cls, f"as {base.material.name} ({base.why}); alone it was {had}", base.doubt
        if name in hands:
            row.cls, row.hand = hands[name][0], hands[name][1]
            row.why, row.doubt = "by hand", 0.0
        if row.cls == "mixed" and row.map is None:
            material = row.material
            specular = game.image(material.specular) if material.specular else None
            diffuse = game.image(material.diffuse) if material.diffuse else None
            if specular is not None and diffuse is not None:
                row.map, row.share = metal_map(diffuse, specular, is_plate(material))
        elif row.cls != "mixed":
            row.map = None
    return rows


HEADER = """\
; What each of the game's solid materials is, for the shaders' metal and roughness model.
; Made by tools\\material_table.py from the material objects' names and textures; one line
; a material object (<name>.mao), sorted.
;
;   metal    the whole surface is metal: metallic 1
;   mixed    metal and not in one texture: its metalness is a map made beside this table
;            (build\\materials\\<its specular mask>_m.dds); the comment says what share of
;            its texels came out metal, and the map's name
;   stone, wood, cloth, leather, skin, foliage, other
;            not metal: metallic 0. They are kept apart because they are useful to read
;            and may matter later. `other` is everything else, and what a material is
;            where the evidence does not say: a wrong metal is worse than a missed one.
;
; A line may be edited by hand. End it with `; by hand` (and a reason, if you like) and
; the tool keeps it as it is from then on:
;
;   plc_statparagnm_01 = stone ; by hand: the paragon's statue is gilded, not gold
;
; A level-of-detail copy (<name>_l2, _l3) is given its base's class, a hand's too, unless
; it has a line by hand of its own. A material made `mixed` by hand gets a map by the rule.
;
; The rules, the rule for a map's texel and their reasons are in the tool; the pictures to
; check this by are in build\\materials\\review.
"""


def table_text(rows, hands):
    lines = [HEADER]
    names = sorted(set(rows) | set(hands))
    width = max(len(n) for n in names)
    for name in names:
        if name in hands:
            lines.append(hands[name][1])
            continue
        row = rows[name]
        line = f"{name:{width}} = {row.cls}"
        if row.cls == "mixed" and row.share is not None:
            line += f"  ; {row.share:.0%} metal, {map_name(row.material.specular)}"
        lines.append(line)
    return "\n".join(lines) + "\n"


# --------------------------------------------------------------------------------------
# The pictures to check it by.

THUMB = 128


def thumb(a, size=THUMB):
    """A picture (h, w) or (h, w, 3), 0 to 1, as a square thumbnail; None as a blank."""
    if a is None:
        return Image.new("RGB", (size, size), (48, 16, 48))
    a = np.clip(a, 0, 1)
    if a.ndim == 2:
        a = np.repeat(a[..., None], 3, axis=2)
    small = resized(a, size, size) if max(a.shape[:2]) > size else a
    return Image.fromarray((small * 255).astype(np.uint8), "RGB").resize((size, size), Image.NEAREST)


def review_sheets(game, rows, folder, log):
    """<class>_<n>.jpg: twenty materials a sheet (sixteen of the mixed), each its diffuse,
    its mirror colour, its gloss, and for a mixed one its map, over its name and class. A
    level-of-detail copy is not drawn beside its base."""
    os.makedirs(folder, exist_ok=True)
    for old in os.listdir(folder):
        if old.endswith(".jpg"):
            os.remove(os.path.join(folder, old))
    written = collections.Counter()
    for cls in CLASSES:
        names = sorted((n for n, r in rows.items() if r.cls == cls and (LOD.sub("", n) == n or LOD.sub("", n) not in rows)), key=lambda n: (rows[n].material.definition, n))
        cells = 4 if cls == "mixed" else 3
        columns = 4 if cls == "mixed" else 5
        per = columns * 4
        for start in range(0, len(names), per):
            sheet = Image.new("RGB", (columns * (cells * THUMB + 8), 4 * (THUMB + 30)), (24, 24, 24))
            draw = ImageDraw.Draw(sheet)
            for i, name in enumerate(names[start : start + per]):
                row = rows[name]
                material = row.material
                x, y = (i % columns) * (cells * THUMB + 8), (i // columns) * (THUMB + 30)
                diffuse = game.image(material.diffuse) if material.diffuse else None
                specular = game.image(material.specular) if material.specular else None
                sheet.paste(thumb(None if diffuse is None else diffuse[..., :3]), (x, y))
                sheet.paste(thumb(None if specular is None else specular[..., :3]), (x + THUMB, y))
                sheet.paste(thumb(None if specular is None else specular[..., 3]), (x + 2 * THUMB, y))
                if cls == "mixed":
                    sheet.paste(thumb(row.map), (x + 3 * THUMB, y))
                copies = sum(1 for n in rows if n != name and LOD.sub("", n) == name)
                share = f"  {row.share:.0%} metal" if row.share is not None else ""
                draw.text((x + 2, y + THUMB + 1), f"{name}{' (+%d lod)' % copies if copies else ''}", fill=(255, 255, 255))
                draw.text((x + 2, y + THUMB + 13), f"{cls}  {material.definition[:-4]}{share}{'  BY HAND' if row.hand else ''}", fill=(170, 200, 255))
            written[cls] += 1
            sheet.save(os.path.join(folder, f"{cls}_{written[cls]:02}.jpg"), quality=85)
        log(f"  {cls}: {len(names)} drawn on {written[cls]} sheets")
    return written


def summary_text(game, rows, sheets, clashes):
    definitions = [d for d in SOLID if any(r.material.definition == d for r in rows.values())]
    counts = collections.Counter((r.cls, r.material.definition) for r in rows.values())
    out = ["# The material table, as made", ""]
    out.append(f"{len(rows)} material objects of the solid, lit definitions; {sum(1 for r in rows.values() if r.hand)} of their lines are by hand.")
    out += ["", "| class | " + " | ".join(d[:-4] for d in definitions) + " | all | sheets |", "|---|" + "---:|" * (len(definitions) + 2)]
    for cls in CLASSES:
        each = [counts[cls, d] for d in definitions]
        out.append(f"| {cls} | " + " | ".join(str(n) for n in each) + f" | {sum(each)} | {sheets.get(cls, 0)} |")
    out.append("| all | " + " | ".join(str(sum(counts[c, d] for c in CLASSES)) for d in definitions) + f" | {len(rows)} | {sum(sheets.values())} |")
    out += ["", "## Left out", ""]
    out.append("Counted and not classed, by definition: " + ", ".join(f"{d} {n}" for d, n in game.skipped.most_common()) + ".")
    if game.unread:
        out.append("")
        out.append(f"Not read: {sum(len(v) for v in game.unread.values())} material objects in {len(game.unread)} enciphered add-in archives (ERF V2.2), whatever their definitions: " + "; ".join(f"{a} ({len(n)})" for a, n in sorted(game.unread.items())) + ".")
    mixed = [r for r in rows.values() if r.cls == "mixed"]
    out += ["", "## The maps", ""]
    out.append(f"{len(mixed)} mixed materials, {len({map_name(r.material.specular) for r in mixed if r.map is not None})} maps (materials that share a specular mask share a map).")
    no_map = sorted(r.material.name for r in mixed if r.map is None)
    if no_map:
        out.append("")
        out.append("Mixed with NO map, their textures missing or unreadable: " + ", ".join(no_map) + ".")
    if clashes:
        out.append("")
        out.append("One specular mask under two diffuse textures (the map is the first's): " + "; ".join(clashes) + ".")
    long = sorted({map_name(r.material.specular) for r in mixed if r.map is not None and len(map_name(r.material.specular)) > 32})
    if long:
        out.append("")
        out.append("Map names over the 32 characters an archive's entry has: " + ", ".join(long) + ".")
    out += ["", "## Least sure", "", "| material | class | why | doubt |", "|---|---|---|---:|"]
    bases = [r for n, r in rows.items() if LOD.sub("", n) == n and not r.hand and not (r.cls == "other" and r.doubt >= 0.6)]
    out += [f"| {r.material.name} | {r.cls} | {r.why} | {r.doubt:.2f} |" for r in sorted(bases, key=lambda r: (-r.doubt, r.material.name))[:40]]
    others = sorted((r for n, r in rows.items() if r.cls == "other" and LOD.sub("", n) == n), key=lambda r: (r.why.split(";")[0].split("(")[0], r.material.name))
    out += ["", "## Other", "", f"{len(others)} (level-of-detail copies not listed), by why:", ""]
    groups = collections.defaultdict(list)
    for r in others:
        groups[r.why].append(r.material.name)
    out += [f"- {why}: {', '.join(names)}" for why, names in sorted(groups.items(), key=lambda kv: (-len(kv[1]), kv[0]))]
    return "\n".join(out) + "\n"


TABLE_LINE = re.compile(r"^\s*([^;=\s]+)\s*=\s*([a-z]+)")


def table_classes(path):
    """{material: class} as data\\materials.ini says it."""
    classes = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            found = TABLE_LINE.match(line)
            if found and found.group(2) in CLASSES:
                classes[found.group(1).lower()] = found.group(2)
    return classes


BUMP_PATCHES = 6  # patches of 2 to 64 texels across


def bumps(normal_map):
    """A normal map's width and height, and how far apart its normals are over patches
    of 2, 4, ... 64 texels across, in the mean over the map: for each, twice the larger of the two spreads (the
    larger eigenvalue of the covariance) of the normals' x and y within a patch, which is
    what a pixel covering such a patch adds to the square of its lobe's width (the
    shader's DaoBumps; Tokuyoshi and Kaplanyan 2019 take the same of a surface's turning).
    The game keeps a normal's x in the alpha and its y in the green, each from -1 to 1
    over the byte (static1.psh, read 2026-10-12). A map smaller than a patch has the
    measure of the largest patch it holds."""
    x = normal_map[..., 3].astype(np.float64) * 2 - 1
    y = normal_map[..., 1].astype(np.float64) * 2 - 1
    height, width = x.shape
    spreads = []
    for patch in range(1, BUMP_PATCHES + 1):
        side = 1 << patch
        down, across = height // side, width // side
        if down == 0 or across == 0:
            spreads.append(spreads[-1] if spreads else 0.0)
            continue

        def mean(a, down=down, across=across, side=side):
            return a[: down * side, : across * side].reshape(down, side, across, side).mean(axis=(1, 3))

        mx, my = mean(x), mean(y)
        xx, yy, xy = mean(x * x) - mx * mx, mean(y * y) - my * my, mean(x * y) - mx * my
        larger = (xx + yy) / 2 + np.sqrt(((xx - yy) / 2) ** 2 + xy * xy)
        spreads.append(float(2 * larger.mean()))
    return (width, height), spreads


BLENDED = re.compile(r'(<DefaultSemantic\s+Name\s*=\s*")Alpha(?=[A-Z])')


def cut_out(text):
    """A material object's text with its way of being drawn changed from blended to cut
    out: `Alpha<tint>` to `Punchthrough<tint>`, which the game has for every tint
    (Character.mat's semantics, read 2026-10-12) and uses itself for the same material's
    second level of detail, a few metres off.

    For a material that is a solid thing (is_cut_out). A thing drawn blended is drawn
    after all that is solid, its own parts over one another in the order of its triangles
    and not by which is nearer: a plumed helm, blended whole for the sake of its feathers'
    tips (4 texels in 100 of its texture are part clear), showed its plume through its
    crown and, with the camera close, the shoulders through its bevor, in the unmodified
    game too (the user, 2026-10-12, four times: "helmet looks transparent"). Cut out, each
    texel is there or not, at half clear, and what is there hides what is behind it."""
    return BLENDED.sub(r"\1Punchthrough", text)


SOLID_CLASSES = ("metal", "mixed", "stone", "wood", "cloth", "leather", "skin", "foliage")
PART_CLEAR = (0.02, 0.98)  # an alpha between these is part clear: neither not there nor solid
SEEN_THROUGH = 0.10  # this share of what is there part clear, and the thing is see-through


def part_clear(diffuse):
    """Of a texture's texels that are there at all, the share that is part clear."""
    alpha = diffuse[..., 3]
    there = alpha >= PART_CLEAR[0]
    if not there.any():
        return 0.0
    return float(((alpha < PART_CLEAR[1]) & there).sum() / there.sum())


def is_cut_out(cls, text, diffuse):
    """Whether a material the game draws blended is to be drawn cut out (cut_out).

    Metal, stone, wood, cloth, leather, skin and leaves are not see-through; glass is,
    and what is like it (the user, 2026-10-12: "most materials (leather, wood, etc.)
    shouldn't be see-through. only glass etc. should be"). So: what the table knows for
    one of the solids, unless its own texture says it is seen through over an area and
    not at an edge, more than a tenth of what is there part clear. Counted over the 31
    materials the game draws blended (all Character.mat's): helms, robes, fur, feathers
    and a body's torn edges are 0 to 6 in 100 part clear; a dragon's and the archdemon's
    wings, one dragon's whole body and the Lady of the Forest's hair are 14 to 75, and
    nothing is between. `other`, which is what the table does not know, is left as the
    game draws it: a wraith and a skeleton are among them."""
    if cls not in SOLID_CLASSES or not BLENDED.search(text):
        return False
    return diffuse is None or part_clear(diffuse) <= SEEN_THROUGH


def object_text(material, metal, map_file, bumped, cut=False):
    """A material object's file with what the shaders' model reads of it added
    (shaders\\materials.hlsl): that it is metal, and, where only part of it is, the map
    that says which part; and its normal map's side and spreads (`bumped`, as bumps
    gives them). With `cut`, it is drawn cut out where the game draws it blended
    (cut_out, is_cut_out)."""
    said = ""
    if metal:
        said += '\t<Float Name="mml_fDaoMetal" value="1.00"></Float>\n'
    if map_file:
        said += f'\t<Texture Name="mml_tDaoMetal" ResName="{map_file}"></Texture>\n'
    if bumped:
        (width, height), spreads = bumped
        near = " ".join(f"{v:.5f}" for v in spreads[:2])
        far = " ".join(f"{v:.5f}" for v in spreads[2:6])
        said += f'\t<Vector4f Name="mml_vDaoBumps" value="{width:.2f} {height:.2f} {near}"></Vector4f>\n'
        said += f'\t<Vector4f Name="mml_vDaoBumpsFar" value="{far}"></Vector4f>\n'
    text = cut_out(material.text) if cut else material.text
    end = text.rfind("</MaterialObject>")
    if end < 0:
        return None
    return text[:end] + said + text[end:]


def write_objects(game, classes, folder, log):
    """The table as the game reads it, into a plugin's override folder: for each material
    the table calls metal or mixed, its material object's file with the metal said, and for
    a mixed one its map, made by the rule from its own textures; and for each material
    with a normal map, of whatever class, the map's measure (bumps). A material with
    neither has no file: the shaders take one that says nothing for no metal and smooth.
    Nor has a material whose copies in the game's archives say different things (the game's
    own and Awakening's leather suit `e` and one helmet, 12 in all, counted 2026-10-12): an
    override is one file for both campaigns and can be right for one, and which copy a
    running game reads was not found out.
    Files of an earlier run that the table no longer asks for are taken away."""
    os.makedirs(folder, exist_ok=True)
    wanted = set()
    counts = collections.Counter()
    measured = {}
    for name in sorted(game.materials):
        cls = classes.get(name, "other")
        material = game.materials[name]
        normal_map = material.textures.get("mml_tNormalMap", "")
        if normal_map and normal_map not in measured:
            picture = game.image(normal_map)
            measured[normal_map] = bumps(picture) if picture is not None else None
        bumped = measured.get(normal_map)
        cut = False
        if BLENDED.search(material.text):
            cut = is_cut_out(cls, material.text, game.image(material.diffuse) if material.diffuse else None)
            counts["drawn cut out, not blended" if cut else "left blended"] += 0 if name in game.differing else 1
        if cls not in ("metal", "mixed") and not bumped and not cut:
            continue
        if name in game.differing:
            counts["with copies that differ: left as they are"] += 1
            continue
        if cls not in ("metal", "mixed"):
            text = object_text(material, False, None, bumped, cut)
            if text is not None:
                with open(os.path.join(folder, name + ".mao"), "w", encoding="latin-1", newline="") as f:
                    f.write(text)
                wanted.add(name + ".mao")
                counts["no metal"] += 1
            continue
        map_file = None
        if cls == "mixed":
            specular = game.image(material.specular) if material.specular else None
            diffuse = game.image(material.diffuse) if material.diffuse else None
            if specular is None or diffuse is None:
                counts["mixed with no textures: left no metal"] += 1
                continue
            map_file = map_name(material.specular)
            if map_file not in wanted:
                grey, _ = metal_map(diffuse, specular, is_plate(material))
                with open(os.path.join(folder, map_file), "wb") as f:
                    f.write(dds_dxt1(grey))
                wanted.add(map_file)
        text = object_text(material, True, map_file, bumped, cut)
        if text is None:
            counts["not a material object's file"] += 1
            continue
        with open(os.path.join(folder, name + ".mao"), "w", encoding="latin-1", newline="") as f:
            f.write(text)
        wanted.add(name + ".mao")
        counts[cls] += 1
    listed = os.path.join(folder, "materials.made")
    before = set()
    if os.path.exists(listed):
        with open(listed, encoding="utf-8") as f:
            before = {line.strip() for line in f if line.strip()}
    for old in before - wanted:
        if os.path.exists(os.path.join(folder, old)):
            os.remove(os.path.join(folder, old))
    with open(listed, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(sorted(wanted)) + "\n")
    log("materials: " + ", ".join(f"{count} {what}" for what, count in sorted(counts.items())) + f"; into {folder}")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--game", default=GAME)
    parser.add_argument("--ini", default=os.path.join(REPO, "data", "materials.ini"))
    parser.add_argument("--out", default=os.path.join(REPO, "build", "materials"))
    parser.add_argument("--no-maps", action="store_true", help="write no maps")
    parser.add_argument("--no-review", action="store_true", help="draw no review sheets")
    parser.add_argument("--no-levels", action="store_true", help="do not read the level archives (they have had none of the solid kinds)")
    parser.add_argument("--why", metavar="REGEX", help="print the class and reason of each material whose name matches, and write nothing")
    parser.add_argument("--names", action="store_true", help="print the level's and the props' materials by the word that classed them, and write nothing")
    parser.add_argument("--objects", metavar="FOLDER", help="write the table AS IT STANDS as the game reads it, material objects and maps, into a plugin's override folder; the table itself is not made again")
    args = parser.parse_args(argv)
    if np is None:
        sys.exit("material_table.py needs numpy and Pillow")
    if not os.path.isdir(os.path.join(args.game, "packages")):
        sys.exit(f"no game at {args.game} (DAO_GAME_DIR, or --game)")

    def log(text):
        print(text, flush=True)

    log(f"reading {args.game}")
    game = Game(args.game, levels=not (args.no_levels or args.why or args.names or args.objects))
    if args.objects:
        write_objects(game, table_classes(args.ini), args.objects, log)
        return 0
    if args.why:
        pattern = re.compile(args.why)
        for name in sorted(game.materials):
            if pattern.search(name):
                row = classify(game, game.materials[name], want_map=False)
                log(f"{name:34} {row.cls:8} {row.why}")
        return 0
    if args.names:
        # the classes of the level's and the props' materials by what word gave them: to read the tables against
        groups = collections.defaultdict(list)
        for name, material in sorted(game.materials.items()):
            if material.definition in ("static.mat", "prop.mat"):
                verdict = by_name(name, material.definition, material.diffuse, material.project, material.sound)
                groups[verdict.cls, verdict.why].append(name)
        for (cls, why), names in sorted(groups.items()):
            log(f"{cls:8} {why}: {' '.join(names)}")
        return 0
    hands = hand_edits(args.ini)
    log(f"{len(game.materials)} solid materials, {sum(game.skipped.values())} of other kinds, {len(hands)} lines by hand")
    rows = build_table(game, hands, log)
    os.makedirs(os.path.dirname(args.ini), exist_ok=True)
    with open(args.ini, "w", encoding="utf-8", newline="\n") as f:
        f.write(table_text(rows, hands))
    log(f"wrote {args.ini}")
    os.makedirs(args.out, exist_ok=True)
    made = {}
    clashes = []
    for name in sorted(rows):
        row = rows[name]
        if row.cls != "mixed" or row.map is None:
            continue
        file = map_name(row.material.specular)
        if file in made:
            if made[file].material.diffuse != row.material.diffuse:
                clashes.append(f"{file}: {made[file].material.name} and {name}")
            continue
        made[file] = row
        if not args.no_maps:
            with open(os.path.join(args.out, file), "wb") as f:
                f.write(dds_dxt1(row.map))
    if not args.no_maps:
        for old in os.listdir(args.out):
            if old.endswith("_m.dds") and old not in made:
                os.remove(os.path.join(args.out, old))
        log(f"wrote {len(made)} maps to {args.out}")
    review = os.path.join(args.out, "review")
    sheets = {}
    if not args.no_review:
        sheets = review_sheets(game, rows, review, log)
    os.makedirs(review, exist_ok=True)
    with open(os.path.join(review, "summary.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write(summary_text(game, rows, sheets, clashes))
    counts = collections.Counter(r.cls for r in rows.values())
    log("  ".join(f"{c} {counts[c]}" for c in CLASSES))
    return 0


if __name__ == "__main__":
    sys.exit(main())
