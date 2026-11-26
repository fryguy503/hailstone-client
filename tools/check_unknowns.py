#!/usr/bin/env python3
"""Fails when an entry of docs/unknowns.md has no tag.

Usage: tools/check_unknowns.py [repo root]

Every entry (a `- **Name.**` line at the start of a line) carries exactly one of the tags `player`, `server` or
`trace`, in backticks, right after its bold name (docs/unknowns.md's header says what each means). The 1.0
known-issues page is the `player` rows, so an untagged entry would fall off it. CI runs it; it exits 1 when an entry
has no tag, or more than one.
"""

import re
import sys
from pathlib import Path

UNKNOWNS = Path("docs/unknowns.md")
TAGS = ("player", "server", "trace")
ENTRY = re.compile(r"^- \*\*.*?\*\*")
TAG = re.compile(r" `(" + "|".join(TAGS) + r")`(?=\s|$)")
ANY_TAG = re.compile(r"`(?:" + "|".join(TAGS) + r")`")


def untagged(text: str) -> list[str]:
    """Each entry line without exactly one tag right after its bold name, as `<line>: <entry>`. Past the first `##`
    heading (the header's own lists come before it), a top-level `- ` line without a bold name counts too."""
    bad = []
    in_sections = False
    for number, line in enumerate(text.splitlines(), 1):
        in_sections = in_sections or line.startswith("## ")
        entry = ENTRY.match(line)
        if not entry:
            if in_sections and line.startswith("- "):
                bad.append(f"{number}: {line[:60]} (no bold name)")
            continue
        rest = line[entry.end():]
        if not TAG.match(rest) or len(ANY_TAG.findall(rest)) != 1:
            bad.append(f"{number}: {entry.group(0)}")
    return bad


def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent
    bad = untagged((root / UNKNOWNS).read_text(encoding="utf-8"))
    for row in bad:
        print(f"{UNKNOWNS}:{row}: needs exactly one of " + ", ".join(f"`{t}`" for t in TAGS) + " after the name",
              file=sys.stderr)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
