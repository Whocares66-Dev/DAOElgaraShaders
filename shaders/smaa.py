"""Anti-aliasing for Elgara Shaders: SMAA's three passes as the game's
colour-grade chain and material take them, and the files they read.

The passes are our own pixel shaders (smaa_edges.hlsl, smaa_weights.hlsl,
smaa_blend.hlsl over smaa/SMAA.hlsl, which is its authors' file as published,
github.com/iryoku/smaa at 71c806a, under smaa/LICENSE.txt). Nothing here
touches the engine: build_shaders.py puts this module's text into the game's
da_hsvmatrix.ffx.xml and hslmatrix.mat.xml, compiles OURS, and calls
write_files for the rest. The grade then reads PICTURE in place of the
game's BackBufferCopy.

What the chain is asked for that the game's own chains show it has
(read 2026-10-10 in postprocesseffects.erf): a target's size given to a pass
(da_dof's `fWindowWidth = DOFBlurredA.width`); and a target cleared every
frame, which the first pass needs because it writes edges only: the
target's own `autoclear` (da_distortion's DistortionMask), which the engine
clears at the end of the world's drawing, before the chain runs. A filter's
`bind ... clear R0G0B0A0`, as da_bssrdf has it, clears nothing: the bind's
parser skips it (DAOLib's Framebuffer.cpp, read in the exe 2026-10-15), and
with it SMAA switched off on the options screen kept blending by the last
frame's edges (1.71% of the tavern's pixels, a debug view of the blend).
"""

import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))

# The target the last pass writes: the picture, anti-aliased.
PICTURE = "DaoSmaaPicture"

# The chain's value that turns the anti-aliasing on, 1, or leaves the picture
# as it came, 0, which it is until something sets it: a plugin, by this name on
# da_hsvmatrix (dao/Framebuffer.h). The first pass reads it and writes no edge
# at 0, so the second finds no weights and the last hands the picture through.
SWITCH = "DaoSmaa"

# For the chain's <resources>: the switch, and the edges, the weights and the
# picture.
CHAIN_RESOURCES = f"""
        <float id="{SWITCH}" value="0.0" range="0.0 1.0" expose="true" />
        <target id="DaoSmaaEdges" width="BACKBUFFER_WIDTH" height="BACKBUFFER_HEIGHT" format="A8R8G8B8" shared="true" autoclear="0 0 0 0"/>
        <target id="DaoSmaaWeights" width="BACKBUFFER_WIDTH" height="BACKBUFFER_HEIGHT" format="A8R8G8B8" shared="true"/>
        <target id="DaoSmaaPicture" width="BACKBUFFER_WIDTH" height="BACKBUFFER_HEIGHT" format="A8R8G8B8" shared="true"/>"""

# The three passes, to go before any filter that reads PICTURE.
CHAIN_FILTERS = f"""<filter id="Dao Smaa Edges" library="DAEffects" material="HSLMatrix">
                Input   = BackBufferCopy
                f{SWITCH} = {SWITCH}
                fWidth  = DaoSmaaEdges.width
                fHeight = DaoSmaaEdges.height
                bind DaoSmaaEdges at 0
                run semantic DaoSmaaEdges
            </filter>

            <filter id="Dao Smaa Weights" library="DAEffects" material="HSLMatrix">
                Edges   = DaoSmaaEdges
                fWidth  = DaoSmaaWeights.width
                fHeight = DaoSmaaWeights.height
                bind DaoSmaaWeights at 0
                run semantic DaoSmaaWeights
            </filter>

            <filter id="Dao Smaa Blend" library="DAEffects" material="HSLMatrix">
                Input   = BackBufferCopy
                Weights = DaoSmaaWeights
                fWidth  = DaoSmaaPicture.width
                fHeight = DaoSmaaPicture.height
                bind DaoSmaaPicture at 0
                run semantic DaoSmaaBlend
            </filter>

            """

# The state the three passes are drawn with, as a material names it.
STATE = "DaoSmaa.sta"

# The lookup textures as the material names them: a parameter no filter sets
# is given its Default, a texture found by name like any of the game's
# (Default_Missing is textures.erf's default_missing.dds).
AREA = "daosmaaarea"
SEARCH = "daosmaasearch"

_SIZE = [
    '<Parameter Type="Float" Name="fWidth" Default="1280.0"/>',
    '<Parameter Type="Float" Name="fHeight" Default="720.0"/>',
]

# The semantics hslmatrix.mat.xml is given, each (name, its parameters): the
# pixel shader is <name>.psh, the state STATE.
SEMANTICS = [
    (
        "DaoSmaaEdges",
        [
            '<Parameter Type="Texture" Name="Input" SamplerName="smpPicture" />',
            f'<Parameter Type="Float" Name="f{SWITCH}" Default="0.0"/>',
            *_SIZE,
        ],
    ),
    (
        "DaoSmaaWeights",
        [
            '<Parameter Type="Texture" Name="Edges" SamplerName="smpEdges" />',
            f'<Parameter Type="Texture" Name="Area" SamplerName="smpArea" Default="{AREA}" />',
            f'<Parameter Type="Texture" Name="Search" SamplerName="smpSearch" Default="{SEARCH}" />',
            *_SIZE,
        ],
    ),
    (
        "DaoSmaaBlend",
        [
            '<Parameter Type="Texture" Name="Input" SamplerName="smpPicture" />',
            '<Parameter Type="Texture" Name="Weights" SamplerName="smpWeights" />',
            *_SIZE,
        ],
    ),
]

# The pixel shaders: the file here, and the name the material asks for.
OURS = [
    ("smaa_edges.hlsl", "daosmaaedges.psh.fxo"),
    ("smaa_weights.hlsl", "daosmaaweights.psh.fxo"),
    ("smaa_blend.hlsl", "daosmaablend.psh.fxo"),
]

# The passes' state. The game's own for a pass over the screen
# (hsvmatrixblend.sta.xml) filters sampler 0 alone; the second pass reads two
# textures between their texels, the edges and the areas, and one at them.
STATE_FILE = """<?xml version="1.0" encoding="UTF-8"?>
<MaterialState Name="DaoSmaa">
  <DeviceState State="DS_CULLMODE" Value="CULL_NONE" />
  <DeviceState State="DS_ZENABLE" Value="ZBUFFERENABLE_FALSE" />
  <DeviceState State="DS_ZWRITEENABLE" Value="FALSE" />
  <DeviceState State="DS_ALPHABLENDENABLE" Value="FALSE" />
  <SamplerState Stage="0" State="SAMP_MINFILTER" Value="TEXFILT_LINEAR" />
  <SamplerState Stage="0" State="SAMP_MAGFILTER" Value="TEXFILT_LINEAR" />
  <SamplerState Stage="0" State="SAMP_ADDRESSU"  Value="TEXADD_CLAMP" />
  <SamplerState Stage="0" State="SAMP_ADDRESSV"  Value="TEXADD_CLAMP" />
  <SamplerState Stage="1" State="SAMP_MINFILTER" Value="TEXFILT_LINEAR" />
  <SamplerState Stage="1" State="SAMP_MAGFILTER" Value="TEXFILT_LINEAR" />
  <SamplerState Stage="1" State="SAMP_ADDRESSU"  Value="TEXADD_CLAMP" />
  <SamplerState Stage="1" State="SAMP_ADDRESSV"  Value="TEXADD_CLAMP" />
  <SamplerState Stage="2" State="SAMP_MINFILTER" Value="TEXFILT_POINT" />
  <SamplerState Stage="2" State="SAMP_MAGFILTER" Value="TEXFILT_POINT" />
  <SamplerState Stage="2" State="SAMP_ADDRESSU"  Value="TEXADD_CLAMP" />
  <SamplerState Stage="2" State="SAMP_ADDRESSV"  Value="TEXADD_CLAMP" />
</MaterialState>
"""

# SMAA's two lookup textures as published for Direct3D 9, each (file, width,
# height, bytes a pixel): the areas as luminance and alpha, the search as
# luminance.
_LOOKUPS = {
    AREA: ("AreaTexDX9.dds", 160, 560, 2),
    SEARCH: ("SearchTex.dds", 64, 16, 1),
}

_DDS_HEADER = 128


def as_a8r8g8b8(dds, width, height, channels):
    """A DDS file of `channels` bytes a pixel, uncompressed and without
    mipmaps, as one of four: the first channel in red, the second in green.
    The game's own uncompressed textures are A8R8G8B8 (default_missing.dds);
    whether it reads luminance formats is not known. A file that is not
    `width` by `height` of `channels` bytes is refused."""
    if (
        dds[:4] != b"DDS "
        or struct.unpack_from("<II", dds, 12) != (height, width)
        or struct.unpack_from("<I", dds, 88)[0] != 8 * channels
        or len(dds) != _DDS_HEADER + width * height * channels
    ):
        raise ValueError(f"not a {width}x{height} texture of {channels} bytes a pixel")
    pixels = bytearray(width * height * 4)
    pixels[2::4] = dds[_DDS_HEADER::channels]  # red
    if channels > 1:
        pixels[1::4] = dds[_DDS_HEADER + 1 :: channels]  # green
    pixels[3::4] = b"\xff" * (width * height)
    header = struct.pack(
        "<4s7I44x8I5I",
        b"DDS ",
        124,
        0x100F,  # caps, height, width, pixel format; pitch
        height,
        width,
        width * 4,
        0,
        0,
        32,
        0x41,  # RGB with alpha
        0,
        32,
        0x00FF0000,
        0x0000FF00,
        0x000000FF,
        0xFF000000,
        0x1000,  # a texture
        0,
        0,
        0,
        0,
    )
    return header + bytes(pixels)


def write_files(out):
    """Write into `out`, a plugin's override folder, what the passes read
    besides their shaders: their state and the two lookup textures."""
    with open(
        os.path.join(out, STATE.lower() + ".xml"), "w", encoding="utf-8", newline=""
    ) as f:
        f.write(STATE_FILE)
    for name, (source, width, height, channels) in _LOOKUPS.items():
        with open(os.path.join(HERE, "smaa", source), "rb") as f:
            texture = as_a8r8g8b8(f.read(), width, height, channels)
        with open(os.path.join(out, name + ".dds"), "wb") as f:
            f.write(texture)
