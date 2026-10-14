"""The add-ins' material objects and their textures, out of the running game: python dump_addins.py <plugin folder> [out]
The add-ins' archives are enciphered, so the table's tool cannot read them from disk; the game can, and the plugin's
developer switch (src/ResourceDump.cpp) writes what a list names. In three asks of the game, which must be running
with a campaign loaded on the plugin folder given:
  1. every material object's name the game has (`*mao`);
  2. those of them that the add-ins' enciphered archives name: read and written out;
  3. the textures those name (diffuse, specular mask, normal map, tint mask): the same.
What is written is copied into [out], build/addins unless said: the folder tools/material_table.py reads as the
add-ins'. An add-in whose module is not the loaded campaign's is not the game's to read then (243 of 782 names in
the main campaign, 2026-10-12): its materials stay out of the table until it is read out of its own campaign."""

import os
import re
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import material_table as mt

plugin, out = sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else mt.ADDINS


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


print(ask(["*mao"]))
with open(
    os.path.join(plugin, "names.mao.txt"), encoding="ascii", errors="replace"
) as f:
    has = {line.strip() for line in f if line.strip()}
game = mt.Game(mt.GAME, addins=None)
enciphered = {name + ".mao" for names in game.unread.values() for name in names}
wanted = sorted(has & enciphered)
print(
    f"the game has {len(has)} material objects; the enciphered archives name {len(enciphered)}, and {len(wanted)} of those are the game's now"
)
answer = ask(wanted)
got = [line for line in answer if not line.startswith(";")]
print(f"material objects written: {len(got)} of {len(wanted)}")
dumped = os.path.join(plugin, "dump")
textures = set()
for name in got:
    with open(os.path.join(dumped, name), encoding="latin-1") as f:
        text = f.read()
    for slot, resource in re.findall(
        r'<Texture Name="([^"]+)" ResName="([^"]*)"', text
    ):
        if (
            slot
            in ("mml_tDiffuse", "mml_tSpecularMask", "mml_tNormalMap", "mml_tTintMask")
            and resource
        ):
            textures.add(resource.lower())
on_disk = set(game.textures)
need = sorted(t for t in textures if t not in on_disk)
print(
    f"they name {len(textures)} textures; {len(textures) - len(need)} are in the clear on disk; {len(need)} are asked of the game"
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
    "The build writes them as the game reads them when the table or its tool changes: delete build/<preset>/materials.stamp for the first time."
)
