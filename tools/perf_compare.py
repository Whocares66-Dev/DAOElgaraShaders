"""The frame time of two builds of the plugin on the same saves with the same options, one launch per build per save,
in the game copy `elgara-agent` (dev/RELEASING.md's step 3; written 2026-10-28 after 0.4.0 shipped slower than 0.3.0).

    python -I tools/perf_compare.py stage [OLD] [NEW]   stage the builds: a version (dist/DAOElgaraShaders-<v>.zip) or
                                                        `build` (build/release's plugin); default the last two zips
    python -I tools/perf_compare.py run [rounds]        every save x every stage, the order turned each round
    python -I tools/perf_compare.py report              the mean per save and stage

The stages are build/perf/stage-<name>/plugins: `off` (the new DLL, every option off, no override/: the control), the
old and the new with the options of build/stage's graphics.chosen (the user's), and the new with Day and night off.
Each launch loads a save of Lyna's, pauses, and reads the plugin's frame timer (Graphics.cpp's `shadow-cost` test:
the mean of every 300 frames, logged). A window during which the game was not in front is dropped: leave the machine
alone while it runs. VSync is turned off in the copy's DragonAge.ini for the run and back on after.
"""

import ctypes
import json
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(REPO, "build", "perf")
BRIDGE = r"C:\project\DAOEluvianMCP\tools\eluvian_bridge.py"
GAME_PLUGINS = r"C:\Program Files (x86)\GOG Galaxy\Games\Dragon Age Origins\bin_ship\daolib\plugins"
INSTANCE, OWNER = "elgara-agent", "elgara-agent-perf"
PROFILE = os.path.join(
    os.environ["LOCALAPPDATA"], "DAOLib", "instances", INSTANCE, "BioWare", "Dragon Age"
)
LOG = os.path.join(PROFILE, "daolib", "elgarashaders.log")
INI = os.path.join(PROFILE, "Settings", "DragonAge.ini")
CHOSEN = os.path.join(
    REPO, "build", "stage", "plugins", "elgarashaders", "graphics.chosen"
)
RESULTS = os.path.join(OUT, "results.jsonl")
NAMES = os.path.join(OUT, "stages.json")
TIMER = re.compile(r"shadow cost test: ([\d.]+) ms a frame with (\d+) extra passes")
OFF = (
    "ImprovedMaterials = Off\nImprovedLighting = Off\nAntiAliasing = Off\nAmbientOcclusion = 0\n"
    "AnisotropicFiltering = 0\nExposure = 2\nDayNight = Off\nTimeOfDay = 42\n"
)
# Lyna's saves (CLAUDE.md's list): Denerim's market, Redcliffe's village (day and night), the Brecilian Forest,
# Orzammar's Commons (underground, its lamps), a dungeon.
SAVES = {"market": 86, "redcliffe": 111, "forest": 471, "commons": 347, "dungeon": 35}


def bridge(tool, args):
    out = subprocess.run(
        [
            sys.executable,
            BRIDGE,
            "--instance",
            INSTANCE,
            "--call",
            tool,
            json.dumps(args),
        ],
        capture_output=True,
        text=True,
        check=False,
    ).stdout
    try:
        answer = json.loads(out)
    except json.JSONDecodeError:
        return {"raw": out[:400]}
    return answer.get("answer", answer)


# The running copy's Eluvian address, found at the first call after a launch.
_eluvian = {"url": None}


def call(tool, args, timeout=120):
    if _eluvian["url"] is None:
        eluvian = bridge("game_status", {}).get("eluvian")
        if not isinstance(eluvian, dict) or eluvian.get("owner") != OWNER:
            sys.exit(f"my copy is not answering: {eluvian}")
        _eluvian["url"] = f"http://127.0.0.1:{eluvian['port']}"
    request = urllib.request.Request(
        _eluvian["url"] + f"/api/tool/{tool}",
        data=json.dumps(args).encode(),
        headers={"Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as reply:
            answer = json.loads(reply.read())
            return answer.get("result", answer)
    except urllib.error.HTTPError as error:
        return {"error": error.read().decode()[:300]}


def front():
    user32 = ctypes.windll.user32
    target = bridge("game_status", {})["eluvian"]["pid"]
    found = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    def each(hwnd, _):
        owner = ctypes.c_ulong()
        user32.GetWindowThreadProcessId(ctypes.c_void_p(hwnd), ctypes.byref(owner))
        if owner.value == target and user32.IsWindowVisible(ctypes.c_void_p(hwnd)):
            found.append(hwnd)
        return True

    user32.EnumWindows(each, 0)
    if found:
        hwnd = ctypes.c_void_p(found[0])
        if user32.IsIconic(hwnd):
            user32.ShowWindow(hwnd, 9)
        user32.keybd_event(
            0x12, 0, 0, 0
        )  # an Alt press lets this process hand the foreground on
        user32.keybd_event(0x12, 0, 2, 0)
        user32.SetForegroundWindow(hwnd)
        time.sleep(0.5)


def in_front():
    return bool(call("window", {}).get("foreground"))


def timer_values(start):
    with open(LOG, "rb") as f:
        f.seek(start if start <= os.path.getsize(LOG) else 0)
        return [
            float(m.group(1))
            for m in TIMER.finditer(f.read().decode("utf-8", errors="replace"))
        ]


def measure(label, n=6, settle=6.0):
    """n timer windows after a settle; one during which the game was ever not in front is dropped."""
    front()
    time.sleep(settle)
    start = os.path.getsize(LOG) if os.path.exists(LOG) else 0
    values, dropped, seen, bad, t0 = [], 0, 0, False, time.time()
    while len(values) < n and time.time() - t0 < 120:
        time.sleep(0.3)
        bad = bad or not in_front()
        lines = timer_values(start)
        while seen < len(lines):
            seen += 1
            if seen == 1 or bad:  # the first began before `start`
                dropped += seen != 1
            else:
                values.append(lines[seen - 1])
            bad = not in_front()
        if bad:
            front()
    record = {
        "label": label,
        "values": values,
        "dropped": dropped,
        "time": time.strftime("%Y-%m-%d %H:%M:%S"),
    }
    with open(RESULTS, "a", encoding="utf-8") as f:
        f.write(json.dumps(record) + "\n")
    mean = sum(values) / max(len(values), 1)
    print(
        f"{record['time']} {label}: {mean:.3f} ms over {len(values)} windows, dropped {dropped}",
        flush=True,
    )
    return record


def vsync(on):
    data = open(INI, "rb").read()
    open(INI, "wb").write(
        data.replace(
            b"UseVSync=0" if on else b"UseVSync=1",
            b"UseVSync=1" if on else b"UseVSync=0",
        )
    )


def one(stage, save):
    if bridge("game_status", {}).get("running"):
        sys.exit(f"{INSTANCE} is running: not launching")
    _eluvian["url"] = None
    args = {
        "owner": OWNER,
        "plugins": os.path.join(OUT, "stage-" + stage, "plugins"),
        "seed": "save",
        "load": {"character": "Lyna", "kind": "manual", "number": SAVES[save]},
        "waitSeconds": 300,
    }
    bridge("game_launch", args)
    time.sleep(5)
    front()
    call("pause", {"paused": True})
    # Under Day and night the first minute after a load is slower (Redcliffe, 2026-10-28: 9.8 ms a frame
    # falling to 8.0 at about 55 s, in 0.3.0 and 0.4.0 alike; 5.4 throughout with it off): timed after it.
    time.sleep(75)
    measure(f"{stage}/{save}")
    bridge("game_quit", {"force": True})
    for _ in range(60):
        if not bridge("game_status", {}).get("running"):
            break
        time.sleep(1)
    time.sleep(3)


def zips():
    found = [
        f[len("DAOElgaraShaders-") : -4]
        for f in os.listdir(os.path.join(REPO, "dist"))
        if f.startswith("DAOElgaraShaders-") and f.endswith(".zip")
    ]
    return sorted(found, key=lambda v: [int(p) for p in v.split(".")])


def put(version, root, keep_override):
    """The plugin's folder of a release's zip, or of build/release, at root/elgarashaders."""
    ours = os.path.join(root, "elgarashaders")
    if version == "build":
        shutil.copytree(
            os.path.join(
                REPO, "build", "release", "daolib", "plugins", "elgarashaders"
            ),
            ours,
            ignore=None if keep_override else shutil.ignore_patterns("override"),
        )
        return ours
    prefix = "bin_ship/daolib/plugins/"
    with zipfile.ZipFile(
        os.path.join(REPO, "dist", f"DAOElgaraShaders-{version}.zip")
    ) as z:
        for item in z.namelist():
            if not item.startswith(prefix + "elgarashaders/") or item.endswith("/"):
                continue
            if not keep_override and "/override/" in item:
                continue
            target = os.path.join(root, item[len(prefix) :])
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with open(target, "wb") as f:
                f.write(z.read(item))
    return ours


def stage(old, new):
    user = open(CHOSEN, encoding="utf-8").read()
    stages = {
        "off": (new, OFF, False),
        old: (old, user, True),
        new: (new, user, True),
        new + "-nodn": (new, user.replace("DayNight = On", "DayNight = Off"), True),
    }
    for name, (version, chosen, keep_override) in stages.items():
        root = os.path.join(OUT, "stage-" + name, "plugins")
        shutil.rmtree(os.path.dirname(root), ignore_errors=True)
        os.makedirs(root)
        shutil.copytree(
            os.path.join(GAME_PLUGINS, "eluvian"), os.path.join(root, "eluvian")
        )
        ours = put(version, root, keep_override)
        open(os.path.join(ours, "graphics.chosen"), "w").write(chosen)
        open(os.path.join(ours, "level-shaders"), "w").write(
            "0\n"
        )  # the frame timer runs where any is named
        open(os.path.join(ours, "shadow-cost"), "w").write(
            "0\n"
        )  # and draws no extra passes
        print(name, sorted(os.listdir(ours)))
    json.dump({"old": old, "new": new, "stages": list(stages)}, open(NAMES, "w"))


def run(rounds):
    names = json.load(open(NAMES))["stages"]
    vsync(False)
    try:
        for r in range(rounds):
            for save in SAVES:
                for name in names if r % 2 == 0 else names[::-1]:
                    one(name, save)
    finally:
        vsync(True)


def report():
    info = json.load(open(NAMES))
    names, old, new = info["stages"], info["old"], info["new"]
    data = {}
    for line in open(RESULTS, encoding="utf-8"):
        record = json.loads(line)
        data.setdefault(record["label"], []).extend(record["values"])
    print("save".ljust(10) + "".join(n.rjust(14) for n in names) + f"   {new} - {old}")
    for save in SAVES:
        means = {n: sum(v) / len(v) for n in names if (v := data.get(f"{n}/{save}"))}
        row = save.ljust(10) + "".join(
            (f"{means[n]:.2f} ms" if n in means else "-").rjust(14) for n in names
        )
        if old in means and new in means:
            row += f"   {means[new] - means[old]:+.2f} ms"
        print(row)


if __name__ == "__main__":
    command = sys.argv[1]
    if command == "stage":
        known = zips()
        stage(
            sys.argv[2] if len(sys.argv) > 2 else known[-2],
            sys.argv[3] if len(sys.argv) > 3 else known[-1],
        )
    elif command == "run":
        run(int(sys.argv[2]) if len(sys.argv) > 2 else 2)
    elif command == "report":
        report()
