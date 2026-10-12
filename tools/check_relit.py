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
