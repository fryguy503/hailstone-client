#!/usr/bin/env python3
"""Fails when a file under src/ gains a path::string() or path::generic_string() call.

Usage: tools/check_path_strings.py [--update] [repo root]

On Windows those convert to the ANSI code page and throw for a character it lacks. The programs run with UTF-8 as
that code page (cmake/utf8.manifest), but new code uses core/path_utf8.h instead (docs/todo.md "Code health"). The
calls already there are counted per file in tools/data/path_string_counts.txt, and the counts only go down:
  - a file with more calls than its count, or a file not listed that has any, fails;
  - a file with fewer fails too, so a fix lowers its count. `--update` lowers every such count, and never raises one.

Every `.string()` and `.generic_string()` in code counts (std::string has neither), comments do not.
"""

import re
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_core_names import strip_c  # noqa: E402

SOURCE_EXT = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inc", ".ipp", ".tpp", ".m", ".mm"}
CALL = re.compile(r"\.(?:generic_)?string\(\)")
COUNTS = Path("tools/data/path_string_counts.txt")
HEADER = """\
# path::string() and generic_string() calls per file (tools/check_path_strings.py): `<count> <file>` per line.
# Counts only go down (tools/check_path_strings.py --update lowers them). New code uses core/path_utf8.h.
"""


def count_calls(root: Path) -> Counter:
    counts = Counter()
    for path in sorted((root / "src").rglob("*")):
        if path.suffix.lower() not in SOURCE_EXT or not path.is_file():
            continue
        n = len(CALL.findall(strip_c(path.read_text(encoding="utf-8", errors="replace"))))
        if n:
            counts[path.relative_to(root).as_posix()] = n
    return counts


def read_counts(path: Path) -> Counter:
    counts = Counter()
    if path.exists():
        for line in path.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if line and not line.startswith("#"):
                n, file = line.split()
                counts[file] = int(n)
    return counts


def write_counts(path: Path, counts: Counter) -> None:
    rows = "".join(f"{n} {f}\n" for f, n in sorted(counts.items()) if n > 0)
    path.write_text(HEADER + rows, encoding="utf-8")


def main() -> int:
    args = [a for a in sys.argv[1:] if a != "--update"]
    update = len(args) != len(sys.argv) - 1
    root = Path(args[0]) if args else Path(__file__).resolve().parent.parent
    found = count_calls(root)
    allowed = read_counts(root / COUNTS)
    status = 0

    grown = sorted(f for f in found if found[f] > allowed[f])
    for file in grown:
        print(f"{file}: {found[file]} path::string() call(s), {allowed[file]} allowed: use path_utf8() "
              "(core/path_utf8.h)")
    if grown:
        print(f"{len(grown)} file(s) gained path::string() calls", file=sys.stderr)
        status = 1

    lowered = sorted(f for f in allowed if found[f] < allowed[f])
    if lowered and update:
        for file in lowered:
            allowed[file] = found[file]
        write_counts(root / COUNTS, allowed)
        print(f"{COUNTS}: lowered {len(lowered)} count(s)")
    elif lowered:
        for file in lowered:
            print(f"{COUNTS}: {file} has {found[file]} call(s), listed as {allowed[file]}; lower it")
        print(f"{len(lowered)} count(s) of {COUNTS} can go down: run tools/check_path_strings.py --update",
              file=sys.stderr)
        status = 1

    total = sum(found.values())
    print(f"path::string() calls left in src/: {total}")
    return status


if __name__ == "__main__":
    sys.exit(main())
