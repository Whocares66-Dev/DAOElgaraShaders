"""The add-ins' material objects and their textures, out of the running game:
    python dump_addins.py <plugin folder> [out] [--tool <elgarashaders_materials.exe>]
The add-ins' archives are enciphered, so the table's tool cannot read them from disk; the game can, and the plugin's
developer switch (src/ResourceDump.cpp) writes what a list names.

    python dump_addins.py <plugin folder> [out] --types uti,gda

reads files of those types instead: every one the game has that some enciphered archive names (the items and the
data tables issue 24's classes are made from). Otherwise, in three asks of the game, which must be running
with a campaign loaded on the plugin folder given:
  1. every material object's name the game has (`*mao`);
  2. those of them that the add-ins' enciphered archives name: read and written out;
  3. the textures those name (diffuse, specular mask, normal map, tint mask): the same.
What is written is copied into [out], build/addins unless said: the folder elgarashaders_materials reads as the
add-ins'. An add-in whose module is not the loaded campaign's is not the game's to read then (243 of 782 names in
the main campaign, 2026-10-12): its materials stay out of the table until it is read out of its own campaign."""

import os
import shutil
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
arguments = sys.argv[1:]
tool = os.path.join(REPO, "build", "release", "elgarashaders_materials.exe")
if "--tool" in arguments:
    at = arguments.index("--tool")
    tool = arguments[at + 1]
    del arguments[at : at + 2]
types = None
if "--types" in arguments:
    at = arguments.index("--types")
    types = arguments[at + 1].split(",")
    del arguments[at : at + 2]
plugin = arguments[0]
out = arguments[1] if len(arguments) > 1 else os.path.join(REPO, "build", "addins")

# Items and tables are not patched as they are read, so a plugin with answers
# hands them over as they are: the checks below are for material objects.
# What the game hands over is what it reads, and the plugin's answers patch a
# material object as it is read (src/MaterialPatch.cpp), as rewritten objects
# in its override folder once replaced it: a dump taken so is the plugin's
# output, keyed by a hash the game never reads, and its materials drawn as no
# metal (252 of 539, the dump of 2026-10-12; seen 2026-10-15). The plugin the
# game runs on for a dump has neither.
if types is None and os.path.exists(os.path.join(plugin, "materials.answers")):
    raise SystemExit(
        f"{plugin} has materials.answers: run the game for a dump on a plugin folder without it"
    )
override = os.path.join(plugin, "override")
if (
    types is None
    and os.path.isdir(override)
    and any(n.lower().endswith(".mao") for n in os.listdir(override))
):
    raise SystemExit(
        f"{override} has material objects: run the game for a dump on a plugin folder without them"
    )


def tool_lines(*words):
    """What elgarashaders_materials says, a line each (src/materials/TableTool.cpp)."""
    answer = subprocess.run(
        [tool, *words], capture_output=True, text=True, cwd=REPO, check=False
    )
    if answer.returncode != 0:
        raise SystemExit(
            f"{os.path.basename(tool)} {' '.join(words)}: {answer.stderr.strip()}"
        )
    return [line.strip() for line in answer.stdout.splitlines() if line.strip()]


def ask(lines, wait=600):
    """Hand the plugin a list and wait until it has answered: dump.done's lines."""
    done = os.path.join(plugin, "dump.done")
    if os.path.exists(done):
        os.remove(done)
    with open(
        os.path.join(plugin, "dump.txt.new"), "w", encoding="ascii", newline="\n"
    ) as f:
        f.write("\n".join(lines) + "\n")
    os.replace(os.path.join(plugin, "dump.txt.new"), os.path.join(plugin, "dump.txt"))
    started = time.time()
    while os.path.exists(os.path.join(plugin, "dump.txt")) or not os.path.exists(done):
        if time.time() - started > wait:
            raise SystemExit(
                "the game did not answer: is it running on this plugin folder, with a game loaded?"
            )
        time.sleep(1)
    time.sleep(0.5)
    with open(done, encoding="ascii", errors="replace") as f:
        return [line.strip() for line in f if line.strip()]


def dump_types():
    """Every file of the types asked that the game has and an enciphered archive names, into out."""
    os.makedirs(out, exist_ok=True)
    dumped = os.path.join(plugin, "dump")
    for kind in types:
        print(ask([f"*{kind}"]))
        with open(
            os.path.join(plugin, f"names.{kind}.txt"),
            encoding="ascii",
            errors="replace",
        ) as f:
            has = {line.strip() for line in f if line.strip()}
        enciphered = set(tool_lines("enciphered", kind))
        wanted = sorted(has & enciphered)
        print(
            f"{kind}: the game has {len(has)}; the enciphered archives name {len(enciphered)}, {len(wanted)} of them the game's now"
        )
        answer = ask(wanted, wait=1800)
        got = [line for line in answer if not line.startswith(";")]
        print(f"{kind}: written {len(got)} of {len(wanted)}")
        for name in got:
            shutil.copyfile(os.path.join(dumped, name), os.path.join(out, name))


if types is not None:
    dump_types()
    raise SystemExit(0)

print(ask(["*mao"]))
with open(
    os.path.join(plugin, "names.mao.txt"), encoding="ascii", errors="replace"
) as f:
    has = {line.strip() for line in f if line.strip()}
enciphered = set(tool_lines("enciphered"))
wanted = sorted(has & enciphered)
print(
    f"the game has {len(has)} material objects; the enciphered archives name {len(enciphered)}, and {len(wanted)} of those are the game's now"
)
answer = ask(wanted)
got = [line for line in answer if not line.startswith(";")]
print(f"material objects written: {len(got)} of {len(wanted)}")
dumped = os.path.join(plugin, "dump")
need = tool_lines("unreadable", dumped)
print(
    f"they name {len(need)} textures not in the clear on disk, which are asked of the game"
)
answer = ask(need, wait=1800)
got_textures = [line for line in answer if not line.startswith(";")]
print(
    f"textures written: {len(got_textures)} of {len(need)}; none of: {[line for line in answer if line.startswith('; none')][:8]}"
)
os.makedirs(out, exist_ok=True)
for name in got + got_textures:
    shutil.copyfile(os.path.join(dumped, name), os.path.join(out, name))
print(
    f"{len(got) + len(got_textures)} files in {out}: {sum(os.path.getsize(os.path.join(out, n)) for n in os.listdir(out)) / 1e6:.0f} MB"
)
print(
    "The build gives them to the plugin when the table or its tool changes: delete build/<preset>/materials.stamp for the first time."
)
