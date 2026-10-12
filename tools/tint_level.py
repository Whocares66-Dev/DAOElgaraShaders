"""Tint a level's own effects and textures for a level lit again: what tools\\relight.ps1
runs after the relighter, for a level whose light is not the colour its makers drew.

    python tools/tint_level.py <game folder> <level> <out folder> [rule ...]

param=<name>:r,g,b
    A level's room files (<level>_<room>.rml) place each model with a list of named
    colours of its own, its "user parameters": the mage tower's 24 shafts of light through
    its windows (fxe_sunbeam_*_p) each have fxe_glow_color, fxe_startbeam_color and
    fxe_endbeam_color, all white, which the effect multiplies its own gold by. Every such
    colour of that name in the level's rooms is set to r,g,b, and the rooms changed are
    written to the out folder under their own names: a room's file is a level's alone, so
    nothing of another level is touched. (A tower by night has no sunbeam: the user,
    2026-10-11, "moonbeam should be silver, not gold".)

model=<name>:<new name>:<material>:r,g,b
    Not every part of an effect has such a colour: the shafts' drifting dust has none, and
    stayed gold. So the level's rooms are made to place a copy of the model in its stead:
    <new name>, as long a name as <name>, whose emitters of a material with <material> in
    its name have their colour multiplied by r,g,b. The copy (<new name>.mmh) and the rooms
    are written to the out folder; the model itself, which other levels place, is as it
    was.

texture=<name>:r,g,b
    The texture of that name (with no .dds) is written to the out folder with each texel's
    colour replaced by its own brightness times r,g,b, uncompressed, with its alpha kept.
    A texture's name is the whole game's: this is for one that only this kind of level
    has, the Circle tower's window glass.

Put in the plugin's override folder (tools\\deploy.ps1 -Relit <level>), the files are
read by the game in place of its own.
"""

import io
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "extern", "DAOLib", "tools"))
import script_commands  # noqa: E402  (DAOLib's reader of the game's archives)

ONES = struct.pack("<4f", 1, 1, 1, 1)
ZERO = chr(0)
# In a model's file an emitter (the struct nemt) begins with its colour's multiplier, and
# its material's name is referred to 180 bytes on (read in fxe_sunbeam_w_p.mmh and
# fxe_sunbeam_s_p.mmh, 2026-10-11).
EMITTERS_MATERIAL = 180
# In a room's file a user parameter (the struct UPAR) is its four numbers, then its name.
PARAMETERS_NAME = 16


def colour_of(text):
    red, green, blue = (float(v) for v in text.split(","))
    return red, green, blue


def data_offset(body):
    """Where a GFF V4.0's data begins: the word after its count of templates."""
    return struct.unpack_from("<I", body, 24)[0]


def string_at(body, ref):
    """The string a reference in a GFF4's data names: a length, then UTF-16 with its closing zero."""
    at = data_offset(body) + ref
    if ref == 0xFFFFFFFF or at + 4 > len(body):
        return None
    (length,) = struct.unpack_from("<I", body, at)
    if length > 256 or at + 4 + 2 * length > len(body):
        return None
    try:
        return body[at + 4 : at + 4 + 2 * length].decode("utf-16-le").rstrip(ZERO)
    except UnicodeDecodeError:
        return None


def named_whites(body, names_at):
    """Every four numbers that are all 1 in a file, each with the string referred to
    `names_at` bytes after them."""
    at = body.find(ONES)
    while at != -1:
        if at + names_at + 4 <= len(body):
            name = string_at(body, struct.unpack_from("<I", body, at + names_at)[0])
            if name is not None:
                yield at, name.lower()
        at = body.find(ONES, at + 4)


def colours_named(body, name, names_at):
    """Where in a file four numbers stand `names_at` bytes before a reference to the
    string `name`: a user parameter of that name, whatever its colour is now (Orzammar's
    smoke is placed with colours of its own, not white)."""
    start = data_offset(body)
    wanted = struct.pack("<I", len(name) + 1) + (name + ZERO).encode("utf-16-le")
    found = []
    at = body.find(wanted, start)
    while at != -1:
        ref = struct.pack("<I", at - start)
        use = body.find(ref, start)
        while use != -1:
            numbers = use - names_at
            if numbers >= start and use % 4 == 0:
                values = struct.unpack_from("<4f", body, numbers)
                if all(v == v and 0.0 <= v <= 64.0 for v in values):
                    found.append(numbers)
            use = body.find(ref, use + 1)
        at = body.find(wanted, at + 1)
    return sorted(set(found))


def tint_params(body, rules):
    """A room's file with its models' named colours set by the rules, each colour's
    fourth number (how much of it there is) left as it is; and how many."""
    out = bytearray(body)
    changed = 0
    for name, colour in rules.items():
        for at in colours_named(body, name, PARAMETERS_NAME):
            struct.pack_into("<3f", out, at, *colour)
            changed += 1
    return bytes(out), changed


def renamed(body, old, new):
    """A file with every string that is `old` made `new`, of the same length; and how many."""
    assert len(old) == len(new), f"{new} is not as long as {old}"
    was = struct.pack("<I", len(old) + 1) + (old + ZERO).encode("utf-16-le")
    now = struct.pack("<I", len(new) + 1) + (new + ZERO).encode("utf-16-le")
    return body.replace(was, now), body.count(was)


def dds_of(pixels, width, height):
    """An uncompressed A8R8G8B8 DDS with every smaller size, from rows of RGBA floats 0 to 1."""
    import numpy as np

    levels = [pixels]
    while levels[-1].shape[0] > 1 or levels[-1].shape[1] > 1:
        level = levels[-1]
        h, w = max(level.shape[0] // 2, 1), max(level.shape[1] // 2, 1)
        tall = level.shape[0] >= 2
        wide = level.shape[1] >= 2
        level = level.reshape(h, 2 if tall else 1, w, 2 if wide else 1, 4).mean(axis=(1, 3))
        levels.append(level)
    header = bytearray(128)
    header[0:4] = b"DDS "
    struct.pack_into("<7I", header, 4, 124, 0x1 | 0x2 | 0x4 | 0x8 | 0x1000 | 0x20000, height, width, width * 4, 0, len(levels))
    struct.pack_into("<8I", header, 76, 32, 0x41, 0, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
    struct.pack_into("<I", header, 108, 0x1000 | 0x8 | 0x400000)
    body = bytearray(header)
    for level in levels:
        body += np.clip(level[..., [2, 1, 0, 3]] * 255.0 + 0.5, 0, 255).astype(np.uint8).tobytes()
    return bytes(body)


def tint_texture(body, colour):
    import numpy as np
    from PIL import Image

    picture = np.asarray(Image.open(io.BytesIO(body)).convert("RGBA")).astype(np.float64) / 255.0
    bright = picture[..., :3] @ np.array([0.2126, 0.7152, 0.0722])
    tinted = np.empty_like(picture)
    for channel in range(3):
        tinted[..., channel] = np.clip(bright * colour[channel], 0, 1)
    tinted[..., 3] = picture[..., 3]
    return dds_of(tinted, picture.shape[1], picture.shape[0])


def main(argv):
    if len(argv) < 4:
        print(__doc__)
        return 2
    game, level, out = argv[1], argv[2].lower(), argv[3]
    params, models, textures = {}, [], []
    for rule in argv[4:]:
        kind, _, rest = rule.partition("=")
        parts = rest.split(":")
        if kind == "param" and len(parts) == 2:
            params[parts[0]] = colour_of(parts[1])
        elif kind == "model" and len(parts) == 4:
            models.append((parts[0].lower(), parts[1].lower(), parts[2].lower(), colour_of(parts[3])))
        elif kind == "texture" and len(parts) == 2:
            textures.append((parts[0].lower(), colour_of(parts[1])))
        else:
            print(f"tint: not a rule: {rule}")
            return 2
    os.makedirs(out, exist_ok=True)
    data = os.path.join(game, "packages", "core", "data")

    # The copies of models, and the rooms that place them and have the named colours.
    if models:
        hierarchies = script_commands.erf_entries(os.path.join(data, "modelhierarchies.erf"))
    for old, new, material, colour in models:
        body = hierarchies.get(old + ".mmh")
        if body is None:
            print(f"tint: no model {old}.mmh")
            return 1
        body, named = renamed(body, old + ".mmh", new + ".mmh")
        copy = bytearray(body)
        tinted = 0
        for at, name in named_whites(body, EMITTERS_MATERIAL):
            if material in name:
                struct.pack_into("<3f", copy, at, *colour)
                tinted += 1
        with open(os.path.join(out, new + ".mmh"), "wb") as f:
            f.write(bytes(copy))
        print(f"tint: {new}.mmh: a copy of {old} ({named} name), {tinted} emitters of {material} tinted")
    if params or models:
        rooms = script_commands.erf_entries(os.path.join(game, "packages", "core", "env", level, level + ".rim"))
        colours = placed = 0
        for name, body in sorted(rooms.items()):
            if not name.endswith(".rml"):
                continue
            tinted, changed = tint_params(body, params)
            for old, new, _, _ in models:
                tinted, count = renamed(tinted, old, new)
                changed += count
                placed += count
            if changed:
                with open(os.path.join(out, name), "wb") as f:
                    f.write(tinted)
            colours += changed
        print(f"tint: {level}: {colours - placed} colours of its models set, {placed} names of models changed")

    for name, colour in textures:
        found = None
        for archive in (("data", "textures.erf"), ("textures", "high", "texturepack.erf")):
            entries = script_commands.erf_entries(os.path.join(game, "packages", "core", *archive))
            if name + ".dds" in entries:
                found = entries[name + ".dds"]
                break
        if found is None:
            print(f"tint: no texture {name}.dds")
            return 1
        with open(os.path.join(out, name + ".dds"), "wb") as f:
            f.write(tint_texture(found, colour))
        print(f"tint: {name}.dds written, its brightness times {colour}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
