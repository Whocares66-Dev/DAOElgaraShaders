"""Check that a level lit again is lit again whole: every lightmap its rooms name is
among the files made for it.

    python tools/check_relit.py <game folder> <level> <folder of the level's files>

A level's shaders read every lightmap of a relit level as ours are written, the square
of what is stored by eight. One left as the game baked it is then far too bright and
the wrong colour: Orzammar's far halls and its backdrop stood white and red as neon,
because the tiles' far detail (<x>_<y>_1) and the level's own room have lightmaps of
their own that were never made (2026-10-11). So every room file of the level is read,
whatever its kind, and every atlas its models name must be in the folder.

Exit code 1, with the rooms and atlases named, if any is missing.
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "extern", "DAOLib", "tools"))
import gff4  # noqa: E402
import script_commands  # noqa: E402


# How far a level's mean light may be from vanilla's in green or blue against red before
# it is remarked on. A level lit again is to look as vanilla does with its light come from
# its sources, not another colour (the user, 2026-10-11: "can we not make things look like
# in vanilla, just w/ more / better lighting, w/out ... up colors?"): Aeducan Thaig was
# 1 : 0.55 : 0.19 against vanilla's 1 : 0.77 : 0.59 and nobody had measured it.
COLOUR_APART = 0.15


def colour_against_vanilla(game, level, folder):
    """The mean colour of a level's light as made (green and blue against red), and of
    vanilla's bake over the same texels; None where pictures cannot be read here."""
    try:
        import io

        import numpy as np
        from PIL import Image
    except ImportError:
        return None
    vanilla = {}
    env = os.path.join(game, "packages", "core", "env", level)
    for archive in os.listdir(env):
        try:
            entries = script_commands.erf_entries(os.path.join(env, archive))
        except Exception:  # noqa: BLE001  (not an archive)
            continue
        vanilla.update({name: body for name, body in entries.items() if name.endswith(".dds")})
    ours_sum = np.zeros(3)
    vanilla_sum = np.zeros(3)
    for name in os.listdir(folder):
        if not name.lower().endswith(".dds") or name.lower() not in vanilla:
            continue
        stored = np.asarray(Image.open(os.path.join(folder, name)).convert("RGB")).astype(float) / 255
        light = stored * stored * 8  # ours is the square root of an eighth of the light
        theirs = Image.open(io.BytesIO(vanilla[name.lower()])).convert("RGB").resize((light.shape[1], light.shape[0]))
        theirs = np.asarray(theirs).astype(float) / 255
        lit = light.sum(axis=2) > 0
        ours_sum += light[lit].sum(axis=0)
        vanilla_sum += theirs[lit].sum(axis=0)
    if ours_sum[0] <= 0 or vanilla_sum[0] <= 0:
        return None
    return (ours_sum[1] / ours_sum[0], ours_sum[2] / ours_sum[0]), (vanilla_sum[1] / vanilla_sum[0], vanilla_sum[2] / vanilla_sum[0])


def main(argv):
    if len(argv) != 4:
        print(__doc__)
        return 2
    game, level, folder = argv[1], argv[2].lower(), argv[3]
    names = {name: label for label, name in gff4.field_names().items()}
    rooms = script_commands.erf_entries(os.path.join(game, "packages", "core", "env", level, level + ".rim"))
    made = {name.lower() for name in os.listdir(folder)}
    wanted = {}
    for room, body in sorted(rooms.items()):
        if not room.endswith(".rml"):
            continue
        root = gff4.Gff4(body).walk(depth=64)
        for model in root.get(names["EnvRoomModelList"]) or []:
            for atlas in model.get(names["EnvModelLightmapAtlasList"]) or []:
                for value in atlas.values():
                    if isinstance(value, str) and value.lower().endswith(".dds"):
                        wanted.setdefault(value.lower(), set()).add(room)
    missing = {atlas: sorted(where) for atlas, where in wanted.items() if atlas not in made}
    print(f"check: {level}: its rooms name {len(wanted)} lightmaps, {len(wanted) - len(missing)} of them made")
    colour = colour_against_vanilla(game, level, folder)
    if colour is not None:
        ours, vanillas = colour
        print(
            f"check: {level}: its light is red:green:blue 1 : {ours[0]:.2f} : {ours[1]:.2f}; "
            f"vanilla's bake is 1 : {vanillas[0]:.2f} : {vanillas[1]:.2f}"
        )
        if max(abs(ours[0] - vanillas[0]), abs(ours[1] - vanillas[1])) > COLOUR_APART:
            print(f"check: {level}: NOTE its colour is far from vanilla's: look at the recipe's colours before it is shown")
    if missing:
        by_room = {}
        for atlas, where in missing.items():
            for room in where:
                by_room.setdefault(room, []).append(atlas)
        for room, atlases in sorted(by_room.items())[:12]:
            print(f"check:   {room}: {len(atlases)} not made, as {sorted(atlases)[0]}")
        print(f"check: {level} is NOT lit again whole: {len(missing)} lightmaps are the game's own still")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
