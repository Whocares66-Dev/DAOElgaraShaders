"""The game's visual-effect models, each classed GLOW or LIT, written to data/effects.ini.

    python tools/effect_list.py            rewrite data/effects.ini
    python tools/effect_list.py --check    print the counts and the UNSURE ones, write nothing

An effect model is a model hierarchy (.mmh) with particle emitters. Its emitters
are drawn with VFX.mat materials, whose colour carries no lighting, so left
alone they glow at midnight: a waterfall, a cloud of dust. The plugin dims the
LIT ones with the sky's light as the hour has it and leaves the GLOW ones (fire,
magic, lightning...) as they are (EffectLights.cpp); a model this file does not
name is drawn as vanilla.

The tool reads the installed game (GOG's, or DAO_GAME_DIR) and every archive
of it that it can read: packages\\core\\data and patch, Awakening's, the
add-ins' and the offers', through DAOLib's dao_files (DAO_FILES, else the
release build's), its one reader of the game's archives and GFF4 files. An
archive whose models and materials are all enciphered or compressed (most of
the add-ins' version 2.2 ones: Blowfish under a password the tool does not
have) is skipped and counted. Per
model it records the emitters' and meshes' material objects (.mao: material,
default semantic, textures) and whether a node is a point light (an nplt node,
GFF4 field MMHNodePointLightColor). Read 2026-10-21: the torches
(fxe_fire_trch_*) have no light of their own, their glow is a lamp in the
level's layout, so no light does not mean LIT.

The class is proposed from that evidence by RULES below. data/effects.ini's
lines ending `; by hand` are the decision and a rerun keeps them as they are.
"""

import argparse
import glob
import json
import os
import re
import subprocess
import sys
import tempfile
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

GAME = Path(
    os.environ.get(
        "DAO_GAME_DIR",
        "C:/Program Files (x86)/GOG Galaxy/Games/Dragon Age Origins",
    )
)
OUT = ROOT / "data" / "effects.ini"
HAND = "; by hand"

# ---------------------------------------------------------------------------
# The proposal. These tables are the tool's guess at a class from the names in
# an effect; data/effects.ini's `; by hand` lines decide, and are kept.
#
# A rule is (pattern, class, what it names). The pattern is searched in each
# lower-case word of a name (names are split at everything that is not a
# letter: fxe_fire_trch_ost_p is fxe, fire, trch, ost, p). A word counts once
# for a class, at the weight of the place it is most heavily found in: the
# model's own name 3, an emitter's name or user parameter 2, a material
# object's or a texture's 1. A point light in the model is GLOW with no
# further question. Otherwise the class with the greater score wins if it is
# at least MINIMUM and twice the other; both present and close is UNSURE, and so is
# none. The words are those the game's 764 effect models use (read 2026-10-21):
# the names are abbreviated, "trch" for torch, "lit" for lightning, "fb" for
# flipbook.
# ---------------------------------------------------------------------------
GLOW, LIT, UNSURE = "glow", "lit", "unsure"
RULES = [
    # emits light
    (
        r"fire(?!ring)|flame|flaming|ember|blaze|inferno|brazier|torch|trch|bonfire|campfire|cinder|ignit|immolat|candle|cndl|lantern|lamp|pyre",
        GLOW,
        "fire",
    ),
    (r"lava|magma|molten", GLOW, "lava"),
    (r"rune|glyph|explo", GLOW, "runes, explosions"),
    (
        r"^(magic|mag|spell|arcane|mystic\w*|enchant\w*|ritual|sigil|aura|conjur\w*|summon|portal|vortex|eluvian|unholy|devine|divine|holy|smite|wisps?|mana|lyrium|taint\w*|sparkae)$",
        GLOW,
        "magic",
    ),
    (
        r"glow|(?<!sun)shine|shining|radian|luminous|flare|halo|sparkl|twinkl|glint|flash|flsh|plasma|puzzle|^stars?$",
        GLOW,
        "glow",
    ),
    (
        r"lightn|lghtn|^lit$|^litarc|^bolt|volt|electr|^shock$|spark|sprk|zap|thunder",
        GLOW,
        "lightning, sparks",
    ),
    # scatters the light around it
    (
        r"water|wtr|^falls?$|fall$|splash|spray|rapids|foam|puddle|rain|fountain|geyser|cascade|drizzle|mist|burble|bubbl|stream|river|^pool$|^(ft|fs|footstep\w*)$",
        LIT,
        "water, footsteps",
    ),
    (
        r"smok|smk|dust|fog|steam|vapo|haze|cloud|snow|flurr|blizz|sleet|hail|sand|dirt|mud|^ashe?s?$|soot|pollen|^webs?$|spore|fluff|feather|^gas$|sunbeam|sunshine|sunlight|moonbm|moonbeam",
        LIT,
        "smoke, dust, fog, snow, light shafts",
    ),
    (r"blood|gore|splatter|^splat$|^bile$", LIT, "blood"),
    (
        r"leaf|leave|leaves|petal|foliage|grass|debris|rubble|rock|stone|chunk|shard|splinter|woodbit|gravel|pebble|wallchip|boulder|bird|raven|seagull|bugs|insect|moth",
        LIT,
        "leaves, debris, critters",
    ),
]
# By the model's name alone: (pattern, class, weight, what it names). The
# ability effects (fxa_) are spells and talents cast by the party and the
# enemy, which light their surroundings; the rest of the prefixes mix fire and
# smoke, so they say nothing.
PREFIXES = [
    (r"^fxa_(blood|bl_pool|bld_splatter)", LIT, 6, "blood effect"),
    (r"^fxa_", GLOW, 6, "ability effect"),
    (r"^fxe_.*(beam|moonbm)", LIT, 6, "light shaft"),
]
MINIMUM = 2
WEIGHTS = {"model": 3, "emitter": 2, "material": 1}

# ---------------------------------------------------------------------------
# The archives
# ---------------------------------------------------------------------------


def archives():
    """[Path] of every .erf of the install, the earlier-read first so a patch
    or an add-in wins over the copy it replaces."""
    found = [Path(p) for p in glob.glob(str(GAME / "**" / "*.erf"), recursive=True)]

    def order(p):
        s = p.as_posix().lower()
        for i, part in enumerate(
            ("packages/core/data/", "packages/core_ep1/data/", "packages/core/patch/")
        ):
            if part in s:
                return (i, s)
        return (3, s)

    return sorted(found, key=order)


def dao_files():
    """DAOLib's dao_files, its one reader of the game's archives and GFF4 files:
    DAO_FILES, else the release build's."""
    tool = Path(
        os.environ.get(
            "DAO_FILES",
            ROOT / "build" / "release" / "extern" / "DAOLib" / "dao_files.exe",
        )
    )
    if not tool.is_file():
        sys.exit(
            f"no dao_files at {tool}: build the plugin (tools/build.ps1) or set DAO_FILES"
        )
    return tool


FIELD_NAMES = ROOT / "extern" / "DAOLib" / "dev" / "gff4_fields.csv"


def extract(archive, folder, *patterns):
    """(the names written, the names not read) of an archive's files matching the
    patterns, written into `folder` by dao_files; None if it is no archive it
    reads."""
    done = subprocess.run(
        [dao_files(), "extract", archive, folder, *patterns],
        capture_output=True,
        text=True,
        check=False,
    )
    if done.returncode != 0:
        return None
    written, unread = [], []
    for line in done.stdout.splitlines():
        if "\t" in line:
            unread.append(line.split("\t", 1)[0])
        elif line and not line.endswith(" files written"):
            written.append(line)
    return written, unread


def trees(folder, *inputs):
    """{file name: GFF4 tree} of the inputs (files, or `archive|pattern`), as
    dao_files reads them into JSON in `folder`; a file that is no GFF4 is left
    out."""
    subprocess.run(
        [dao_files(), "json", "--names", FIELD_NAMES, folder, *inputs],
        capture_output=True,
        text=True,
        check=False,
    )
    out = {}
    for path in sorted(Path(folder).glob("*.json")):
        with open(path, encoding="utf-8") as f:
            out[path.stem] = json.load(f)
        path.unlink()
    return out


# ---------------------------------------------------------------------------
# The models
# ---------------------------------------------------------------------------

L_NAME = "MMHName"
L_CHILDREN = "MMHChildren"
L_MATERIAL = "MMHMaterialObject"
L_LIGHT = "MMHNodePointLightColor"
L_USERPARAM = "MMHNodeEmitUserParamName"
# Either marks an emitter node; a template without both holds none.
L_EMIT = ("MMHNodeEmitBirthRate", "MMHNodeEmitType")


def nodes(tree):
    """Every node (a struct with children or a name) of a hierarchy, depth first."""
    stack = [tree]
    while stack:
        node = stack.pop()
        if not isinstance(node, dict):
            continue
        yield node
        stack.extend(node.get(L_CHILDREN) or [])


def read_model(tree):
    """The facts of one .mmh's tree, or None if it has no emitter: {light, emitters, materials}."""
    light = False
    emitters = []
    materials = set()
    for node in nodes(tree):
        if L_LIGHT in node:
            light = True
        material = node.get(L_MATERIAL) or ""
        if material:
            materials.add(material.lower())
        if any(label in node for label in L_EMIT):
            emitters.append(
                (
                    (node.get(L_NAME) or "").lower(),
                    (node.get(L_USERPARAM) or "").lower(),
                    material.lower(),
                )
            )
    if not emitters:
        return None
    return {"light": light, "emitters": emitters, "materials": materials}


def read_mao(data):
    """(material, semantic, [textures]) of a .mao's XML."""
    text = data.decode("utf-8", errors="replace")
    material = re.search(r'<Material\s+Name="([^"]*)"', text)
    semantic = re.search(r'<DefaultSemantic\s+Name="([^"]*)"', text)
    textures = re.findall(r'<Texture\s+[^>]*ResName="([^"]*)"', text)
    return (
        material.group(1) if material else "",
        semantic.group(1) if semantic else "",
        [t.lower() for t in textures],
    )


def scan(deciphered=None):
    """({model: facts}, {material object: (material, semantic, textures)}, notes)
    of every readable archive."""
    models, maos = {}, {}
    read, skipped = 0, Counter()
    skipped_models = 0
    sources = Counter()
    with tempfile.TemporaryDirectory() as work:
        files = Path(work) / "files"
        jsons = Path(work) / "json"
        files.mkdir()
        jsons.mkdir()
        for path in archives():
            got = extract(path, files, "*.mao", "*.mmh")
            if got is None:
                skipped["not an archive dao_files reads"] += 1
                continue
            written, unread = got
            if unread and not written:
                skipped["enciphered or compressed"] += 1
                if "modelhierarchies" in path.name:
                    skipped_models += 1
                continue
            read += 1
            for name in written:
                if name.lower().endswith(".mao"):
                    maos[name.lower()[:-4]] = read_mao((files / name).read_bytes())
                (files / name).unlink()
            if not any(name.lower().endswith(".mmh") for name in written):
                continue
            source = "core" if "packages" in path.as_posix().lower() else "addin"
            for name, tree in trees(jsons, f"{path}|*.mmh").items():
                facts = read_model(tree)
                if facts:
                    facts["source"] = source
                    models[name.lower()[:-4]] = facts
                    sources[source] += 1
        overridden = 0
        if deciphered:
            # Files loose under the folder stand for the add-ins' enciphered
            # archives. They are read after the core's, so one that has the name of
            # a core model replaces it: the add-ins come after the core in the
            # game's order, and a copy that differs is what the add-in's level
            # draws. The folder is only read, never copied.
            loose = sorted(
                path
                for path in Path(deciphered).rglob("*")
                if path.suffix.lower() in (".mmh", ".mao") and path.is_file()
            )
            for path in loose:
                if path.suffix.lower() == ".mao":
                    maos[path.stem.lower()] = read_mao(path.read_bytes())
            mmhs = [path for path in loose if path.suffix.lower() == ".mmh"]
            # In handfuls, to keep each command line short.
            for start in range(0, len(mmhs), 64):
                handful = mmhs[start : start + 64]
                read_trees = trees(jsons, *handful)
                for path in handful:
                    tree = read_trees.get(path.name)
                    facts = read_model(tree) if tree is not None else None
                    if facts:
                        name = path.stem.lower()
                        facts["source"] = "addin"
                        overridden += (
                            name in models and models[name]["source"] == "core"
                        )
                        models[name] = facts
    notes = {
        "overridden": overridden,
        "read": read,
        "skipped": skipped,
        "skipped_models": skipped_models,
    }
    return models, maos, notes


# ---------------------------------------------------------------------------
# The proposal
# ---------------------------------------------------------------------------


def words(text):
    return [w for w in re.split(r"[^a-z]+", text.lower()) if w]


def evidence(name, facts, maos):
    """[(place, text)] the names a class is proposed from."""
    out = [("model", name)]
    for emitter, param, material in facts["emitters"]:
        out += [("emitter", emitter), ("emitter", param)]
        out.append(("material", material))
    for material in sorted(facts["materials"]):
        out.append(("material", material))
        _, _, textures = maos.get(material, ("", "", []))
        out += [("material", t.rsplit(".", 1)[0]) for t in textures]
    return out


def propose(name, facts, maos):
    """(class, short evidence) for one model."""
    if facts["light"]:
        return GLOW, "point light"
    score = {GLOW: 0, LIT: 0}
    hits = {GLOW: {}, LIT: {}}
    for pattern, cls, weight, what in PREFIXES:
        if re.search(pattern, name):
            hits[cls][what] = weight
            break
    for place, text in evidence(name, facts, maos):
        for word in words(text):
            for pattern, cls, _ in RULES:
                if re.search(pattern, word):
                    hits[cls][word] = max(hits[cls].get(word, 0), WEIGHTS[place])
    for cls in score:
        score[cls] = sum(hits[cls].values())
    g, w = score[GLOW], score[LIT]

    def top(cls):
        return ",".join(sorted(hits[cls], key=hits[cls].get, reverse=True)[:4])

    if g >= MINIMUM and g >= 2 * w:
        return GLOW, "names: " + top(GLOW)
    if w >= MINIMUM and w >= 2 * g:
        return LIT, "names: " + top(LIT)
    if g or w:
        return UNSURE, f"conflict: glow {top(GLOW)} / lit {top(LIT)}"
    return UNSURE, "no evidence"


def describe(facts, maos):
    """A short account of an effect's materials for the line's comment."""
    semantics = Counter()
    textures = []
    for material in sorted(facts["materials"]):
        mat, semantic, tex = maos.get(material, ("", "", []))
        if semantic:
            semantics[semantic] += 1
        textures += [t.rsplit(".", 1)[0] for t in tex[:1]]
    sem = "/".join(sorted(semantics)[:2])
    return sem, textures[:2]


# ---------------------------------------------------------------------------
# The file
# ---------------------------------------------------------------------------

HEADER = """\
; The game's visual-effect models (model hierarchies with particle emitters) and
; whether each GLOWS (emits light: fire, magic, lightning) or is LIT (scatters the
; light around it: water, smoke, dust). With the day and night, the plugin dims
; the LIT ones with the sky's light as the hour has it and leaves the GLOW ones;
; a model not named here, and one marked unsure, is drawn as vanilla.
;
; Written by `python tools/effect_list.py`, which proposes a class from each
; model's point light and the names of its model, emitters, materials and
; textures (the RULES table at the top of the script). A line ending `; by hand`
; is the decision and is kept as it is by every run, by model name; the other
; lines are rewritten. To decide one, change its class and end it with `; by hand`.
;
; <model name, lower case, no extension> = glow|lit|unsure ; <evidence>
;
; A lightless fire may be given the light of a lit twin, by hand, as
; `= glow <red> <green> <blue> <reach> <height above its place>`.
; A model a flame stands in may give the flame its own light, by hand, as
; `= holds <red> <green> <blue> <reach> <height above its place>`: a flame the
; level places within half a metre of one is lit so, at it.
"""


def hand_lines(path):
    """{model: line} of the `; by hand` lines of an existing file."""
    kept = {}
    if path.exists():
        for line in path.read_text(encoding="utf-8").splitlines():
            if (
                line.rstrip().endswith(HAND)
                and "=" in line
                and not line.startswith(";")
            ):
                kept[line.split("=", 1)[0].strip().lower()] = line.rstrip()
    return kept


def class_of(line):
    """The class a line of the file gives its model."""
    return line.split("=", 1)[1].split(";")[0].split()[0]


def build(models, maos):
    """{model: (class, line)} proposed for every model."""
    out = {}
    for name in sorted(models):
        facts = models[name]
        cls, why = propose(name, facts, maos)
        sem, textures = describe(facts, maos)
        extra = " ".join(x for x in (sem, ",".join(textures)) if x)
        out[name] = (cls, f"{name} = {cls} ; {why}" + (f" [{extra}]" if extra else ""))
    return out


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument(
        "--check",
        action="store_true",
        help="print counts and the UNSURE ones, write nothing",
    )
    ap.add_argument(
        "--deciphered",
        default=os.environ.get("DAO_DECIPHERED"),
        help="a folder of the add-ins' deciphered files (default: DAO_DECIPHERED)",
    )
    args = ap.parse_args(argv)
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    models, maos, notes = scan(args.deciphered)
    proposed = build(models, maos)
    kept = hand_lines(OUT)
    # A hand line decides over the proposal. One for a model no longer found
    # stays too: a mod's may be absent here.
    lines = {name: line for name, (_, line) in proposed.items()}
    lines.update(kept)
    classes = Counter(class_of(line) for line in lines.values())

    cores = sum(1 for f in models.values() if f["source"] == "core")
    print(
        f"{len(models)} effect models ({cores} core, {len(models) - cores} add-in); "
        f"{notes['read']} archives read, {sum(notes['skipped'].values())} skipped "
        f"({dict(notes['skipped'])}, {notes['skipped_models']} of them modelhierarchies); "
        f"{notes['overridden']} core models replaced by an add-in's"
    )
    print(
        ", ".join(f"{k} {classes[k]}" for k in (GLOW, LIT, UNSURE)),
        f"; by hand {len(kept)}",
    )
    if args.check:
        for name in sorted(lines):
            if class_of(lines[name]) == UNSURE:
                print(lines[name])
        return 0
    text = HEADER + "\n" + "\n".join(lines[k] for k in sorted(lines)) + "\n"
    OUT.parent.mkdir(exist_ok=True)
    OUT.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
