"""Set options in a running copy of the game the way a player does (#68): the options screen opened from the Escape
menu with its panel, the tab shown by its own button, each control set as a click sets it, then OK, or Cancel.
Through Eluvian's `options` tool (Eluvian #190, DAOLib's dao::options, 0.8.0): never the screen's movie, whose
`SceneManager.PlayScene` showed a tab's page under the Game tab's highlight and with no panel behind it.

    python -I tools/set_option.py <instance> "Draw distance=2x" "Time of day=18:00" [--tab video] [--cancel]

Needs an Eluvian with `options` in the copy's plugins (Eluvian's main from 7955096); an older one answers that it
has no such tool.

A control is named by its label or its name; a value by its name, true/false or a number, or a choice's
place in its list by # and the place, counting from 0 ("Exposure=#2"): a name of digits is a name. Prints each
control's value before and after, and leaves the game where it was, the Escape menu closed again. Exit code 1 where
a control is not on the tab or does not take the value.
"""

import argparse
import json
import subprocess
import sys
import urllib.error
import urllib.request

BRIDGE = r"C:\project\DAOEluvianMCP\tools\eluvian_bridge.py"


def port_of(instance):
    """The port the running copy's Eluvian serves on."""
    run = subprocess.run(
        [
            sys.executable,
            BRIDGE,
            "--call",
            "game_status",
            json.dumps({"instance": instance}),
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    answer = json.loads(run.stdout or "{}")
    eluvian = answer.get("answer", answer).get("eluvian")
    if not isinstance(eluvian, dict) or "port" not in eluvian:
        sys.exit(
            f"{instance} is not running, or its Eluvian is not answering: {eluvian}"
        )
    return eluvian["port"]


def tool(port, name, args):
    request = urllib.request.Request(
        f"http://127.0.0.1:{port}/api/tool/{name}",
        data=json.dumps(args).encode(),
        headers={"Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(request, timeout=60) as reply:
            answer = json.loads(reply.read())
    except (
        urllib.error.HTTPError
    ) as refused:  # a refusal, said in words: no such control, no such value
        return {"error": json.loads(refused.read() or b"{}").get("error", str(refused))}
    return answer.get("result", answer)


def values(port):
    """Each control of the tab shown, by its label: its value, by its name where it is a choice."""
    look = tool(port, "options", {"action": "look"})
    shown = {}
    for entry in look.get("entries", []):
        value, names = entry.get("value"), entry.get("values")
        if names and isinstance(value, (int, float)) and 0 <= int(value) < len(names):
            value = names[int(value)]
        shown[entry["label"]] = value
    return shown


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument(
        "instance", help="the copy of the game: an Eluvian instance's name"
    )
    parser.add_argument(
        "settings", nargs="+", help='"<label>=<value>", e.g. "Draw distance=2x"'
    )
    parser.add_argument(
        "--tab", default="video", help="the tab the controls are on (default video)"
    )
    parser.add_argument(
        "--cancel", action="store_true", help="close with Cancel, not OK"
    )
    args = parser.parse_args()
    port = port_of(args.instance)

    opened = tool(port, "options", {"action": "open"})
    if not opened.get("open"):
        sys.exit(f"the options screen did not open: {opened}")
    tool(port, "options", {"action": "tab", "tab": args.tab})
    before = values(port)
    failed = False
    try:
        for setting in args.settings:
            label, _, value = setting.partition("=")
            label, value = label.strip(), value.strip()
            asked = (
                int(value[1:])
                if value.startswith("#") and value[1:].isdigit()
                else value
            )
            answer = tool(
                port, "options", {"action": "set", "name": label, "value": asked}
            )
            if answer.get("took"):
                print(f"{label}: {before.get(label)} -> {values(port).get(label)}")
            else:
                failed = True
                print(f"{label}: refused: {answer.get('error', answer)}")
    finally:
        closed = tool(port, "options", {"action": "cancel" if args.cancel else "ok"})
        if closed.get("back"):
            tool(port, "input", {"message": "GUI.HideFullScreenGUI"})
        print(
            "cancel" if args.cancel else "ok",
            "closed" if closed.get("closed") else closed,
        )
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
