#!/usr/bin/env python3
"""Fails when a source under src/ includes a header from a layer above its own.

Usage: tools/check_layers.py [--update] [repo root]

The layers (docs/plan.md "Architecture", AGENTS.md): a folder of src/ may include headers of its own row and of any
row below, never of a row above.

  app
  nav, plugin, mq
  game, ui, render, audio, texpack
  assets, net, web
  core, platform

The includes that broke this before the check existed are listed, one `<file> <header>` per line, in
tools/data/layer_exceptions.txt. The list only shrinks:
  - an include from a lower row to a higher one that is not listed fails;
  - a listed line whose include is gone fails too, so a fix removes its line. `--update` removes every such line,
    and never adds one.

Only "folder/..." includes of src/'s own folders count; comments are ignored, preprocessor conditions are not.
"""

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_core_names import strip_c  # noqa: E402

ROWS = (
    ("core", "platform"),
    ("assets", "net", "web"),
    ("game", "ui", "render", "audio", "texpack"),
    ("nav", "plugin", "mq"),
    ("app",),
)
LEVEL = {folder: level for level, row in enumerate(ROWS) for folder in row}
SOURCE_EXT = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inc", ".ipp", ".tpp", ".m", ".mm"}
INCLUDE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*[<"]([A-Za-z0-9_]+)/([^">]+)[">]', re.M)
EXCEPTIONS = Path("tools/data/layer_exceptions.txt")
HEADER = """\
# Includes that go up a layer (tools/check_layers.py, docs/plan.md "Architecture"): `<file> <header>` per line.
# Remove a line when its include is fixed (tools/check_layers.py --update does it). Never add one: move the shared
# piece down a layer instead.
"""


def upward_includes(root: Path) -> set[tuple[str, str]]:
    found = set()
    for path in sorted((root / "src").rglob("*")):
        if path.suffix.lower() not in SOURCE_EXT or not path.is_file():
            continue
        rel = path.relative_to(root).as_posix()
        own = path.relative_to(root / "src").parts[0]
        if own not in LEVEL:
            continue
        code = strip_c(path.read_text(encoding="utf-8", errors="replace"))
        for folder, rest in INCLUDE.findall(code):
            if folder in LEVEL and LEVEL[folder] > LEVEL[own]:
                found.add((rel, f"{folder}/{rest}"))
    return found


def read_exceptions(path: Path) -> set[tuple[str, str]]:
    if not path.exists():
        return set()
    entries = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            file, header = line.split()
            entries.add((file, header))
    return entries


def write_exceptions(path: Path, entries: set[tuple[str, str]]) -> None:
    path.write_text(HEADER + "".join(f"{f} {h}\n" for f, h in sorted(entries)), encoding="utf-8")


def unknown_folders(root: Path) -> list[str]:
    return sorted(p.name for p in (root / "src").iterdir() if p.is_dir() and p.name not in LEVEL)


def main() -> int:
    args = [a for a in sys.argv[1:] if a != "--update"]
    update = len(args) != len(sys.argv) - 1
    root = Path(args[0]) if args else Path(__file__).resolve().parent.parent
    status = 0

    for folder in unknown_folders(root):
        print(f"src/{folder}/ has no layer: add it to a row of ROWS in tools/check_layers.py and to docs/plan.md",
              file=sys.stderr)
        status = 1

    found = upward_includes(root)
    listed = read_exceptions(root / EXCEPTIONS)
    new = sorted(found - listed)
    stale = sorted(listed - found)

    for file, header in new:
        print(f"{file}: includes {header}, a layer above its own")
    if new:
        print(f"{len(new)} include(s) go up a layer (docs/plan.md \"Architecture\"): move the shared piece down "
              f"instead of listing it in {EXCEPTIONS}", file=sys.stderr)
        status = 1

    if stale and update:
        write_exceptions(root / EXCEPTIONS, listed - set(stale))
        print(f"{EXCEPTIONS}: removed {len(stale)} fixed line(s)")
    elif stale:
        for file, header in stale:
            print(f"{EXCEPTIONS}: {file} {header} is fixed; remove its line")
        print(f"{len(stale)} line(s) of {EXCEPTIONS} are fixed: run tools/check_layers.py --update",
              file=sys.stderr)
        status = 1
    return status


if __name__ == "__main__":
    sys.exit(main())
