#!/usr/bin/env python3
"""The guide's site (docs/), every version of it: built, or built and served.

    python tools/build_docs.py              # build the site into build/docs-site
    python tools/build_docs.py --serve      # the same, served at http://127.0.0.1:4000/DAOElgaraShaders/

What is on master is "Current", at the site's root; each release tag vX.Y.Z
that has a docs folder is beside it under /X.Y.Z/, its own pages with today's
theme over them (the includes, the styles and the gems, so an old version
has the selector and today's colours). Every version is told the list of
them, which the header's selector shows (docs/_includes/components/
aux_nav.html), and which of the others has the page you are on. DAOTurnBased's
tools/build_docs.py, copied 2026-10-15 (the user: "where's the version
dropdown?").

While writing a page, `bundle exec jekyll serve --livereload` in docs/ is the
quicker loop: one version, the selector reading "Current".
"""

from __future__ import annotations

import argparse
import functools
import http.server
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOCS = ROOT / "docs"
SITE = ROOT / "build" / "docs-site"
BASEURL = "/DAOElgaraShaders"
# Today's theme, laid over a released version's pages.
CHROME = ("_includes", "_sass", "Gemfile", "Gemfile.lock")
LOCAL_OUTPUT = shutil.ignore_patterns("_site", ".jekyll-cache", ".bundle", "vendor")


def git(*args: str) -> bytes:
    return subprocess.check_output(["git", *args], cwd=ROOT)


def release_tags(tags: list[str] | None = None) -> list[str]:
    """vX.Y.Z tags, newest first."""
    if tags is None:
        tags = git("tag", "--list").decode().split()
    found = []
    for tag in tags:
        match = re.fullmatch(r"v(\d+)\.(\d+)\.(\d+)", tag)
        if match:
            found.append((tuple(map(int, match.groups())), tag))
    return [tag for _, tag in sorted(found, reverse=True)]


def has_docs(tag: str) -> bool:
    asked = subprocess.run(
        ["git", "cat-file", "-e", f"{tag}:docs/_config.yml"],
        cwd=ROOT,
        capture_output=True,
        check=False,
    )
    return asked.returncode == 0


def from_tag(tag: str, into: Path) -> Path | None:
    """The tag's docs folder with today's theme over it; nothing for a tag
    from before the guide."""
    if not has_docs(tag):
        return None
    target = into / tag
    with zipfile.ZipFile(
        io.BytesIO(git("archive", "--format=zip", tag, "docs"))
    ) as zipped:
        for entry in zipped.infolist():
            path = target / entry.filename
            if not path.resolve().is_relative_to(target.resolve()):
                raise ValueError(f"unsafe archive entry {entry.filename}")
            if entry.is_dir():
                path.mkdir(parents=True, exist_ok=True)
            else:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(zipped.read(entry))
    docs = target / "docs"
    for name in CHROME:
        source, dest = DOCS / name, docs / name
        if dest.exists():
            shutil.rmtree(dest) if dest.is_dir() else dest.unlink()
        if source.is_dir():
            shutil.copytree(source, dest, ignore=LOCAL_OUTPUT)
        elif source.exists():
            shutil.copyfile(source, dest)
    return docs


def permalinks(source: Path) -> list[str]:
    """The pages a version has, so that the selector keeps you on the page
    you are on where the other version has it."""
    found = set()
    for path in source.rglob("*.md"):
        if "_site" in path.parts or "vendor" in path.parts:
            continue
        match = re.search(
            r"^permalink:\s*[\"']?(/[^\"'\s]*)",
            path.read_text(encoding="utf-8"),
            re.MULTILINE,
        )
        if match:
            found.add(match.group(1))
    return sorted(found)


def versions_of(sources: list[tuple[str, Path, str]]) -> list[dict[str, object]]:
    return [
        {"label": label, "baseurl": url, "pages": permalinks(source)}
        for label, source, url in sources
    ]


def build_site() -> int:
    """Current (the working tree) at the root and each release under
    /X.Y.Z/, each with the list of versions for its selector."""
    out = SITE / BASEURL[1:]
    if SITE.exists():
        shutil.rmtree(SITE)
    out.mkdir(parents=True)
    with tempfile.TemporaryDirectory(
        prefix="docs-versions-", dir=ROOT / "build"
    ) as temporary:
        staging = Path(temporary)
        sources = [("Current", DOCS, BASEURL)]
        for tag in release_tags():
            source = from_tag(tag, staging)
            if source is not None:
                sources.append((tag[1:], source, f"{BASEURL}/{tag[1:]}"))
        versions = versions_of(sources)
        env = {
            **os.environ,
            "BUNDLE_GEMFILE": str(DOCS / "Gemfile"),
            "JEKYLL_ENV": "production",
        }
        for index, (label, source, url) in enumerate(sources):
            # JSON is YAML: Jekyll lays this over the version's own settings.
            config = staging / f"config-{index}.yml"
            config.write_text(
                json.dumps(
                    {"baseurl": url, "docs_version": label, "docs_versions": versions}
                ),
                encoding="utf-8",
            )
            command = [
                "bundle",
                "exec",
                "jekyll",
                "build",
                "--source",
                str(source),
                "--destination",
                str(out if index == 0 else out / label),
                "--config",
                f"{source / '_config.yml'},{config}",
            ]
            done = subprocess.run(
                command, cwd=DOCS, env=env, shell=sys.platform == "win32", check=False
            )
            if done.returncode != 0:
                return done.returncode
    (out / ".nojekyll").touch()
    print(f"built {len(sources)} version(s) into {out}")
    return 0


def serve() -> int:
    """The built site at http://127.0.0.1:4000/DAOElgaraShaders/, every version in it."""
    handler = functools.partial(
        http.server.SimpleHTTPRequestHandler, directory=str(SITE)
    )
    with http.server.ThreadingHTTPServer(("127.0.0.1", 4000), handler) as server:
        print(f"serving http://127.0.0.1:4000{BASEURL}/ (Ctrl+C stops)", flush=True)
        server.serve_forever()
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--serve", action="store_true", help="build every version and serve the site"
    )
    options = parser.parse_args()
    built = build_site()
    if built != 0 or not options.serve:
        return built
    return serve()


if __name__ == "__main__":
    sys.exit(main())
