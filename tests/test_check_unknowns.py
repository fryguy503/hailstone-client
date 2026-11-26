"""Tests for tools/check_unknowns.py: every entry of docs/unknowns.md has one tag."""

import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import check_unknowns  # noqa: E402


def run(text: str) -> tuple[int, str]:
    with tempfile.TemporaryDirectory() as folder:
        root = Path(folder)
        (root / "docs").mkdir()
        (root / "docs/unknowns.md").write_text(text, encoding="utf-8")
        out = io.StringIO()
        argv = sys.argv
        sys.argv = [check_unknowns.__file__, str(root)]
        try:
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
                code = check_unknowns.main()
        finally:
            sys.argv = argv
    return code, out.getvalue()


class CheckUnknownsTest(unittest.TestCase):
    def test_tagged_entries_pass(self):
        text = (
            "# Known gaps\n\n- Unknown: a label, not an entry.\n- `player`: a tag's definition.\n\n## Area\n\n"
            "- **Alone.** `trace`\n"
            "- **With text.** `server` Some words after it.\n"
            "- **Parenthesis** `player` (docs/x.md).\n"
            "  - **A sub-bullet** needs no tag.\n"
        )
        self.assertEqual(run(text), (0, ""))

    def test_missing_tag_fails(self):
        code, out = run("- **Tagged.** `player`\n- **Untagged.** Text.\n")
        self.assertEqual(code, 1)
        self.assertIn("docs/unknowns.md:2: - **Untagged.**", out)
        self.assertEqual(len(out.splitlines()), 1)

    def test_entry_without_a_bold_name_fails_in_a_section(self):
        code, out = run("# Known gaps\n\n- Unknown: a label.\n\n## Area\n\n- **Tagged.** `player`\n- Plain words.\n")
        self.assertEqual(code, 1)
        self.assertIn("docs/unknowns.md:8: - Plain words. (no bold name)", out)

    def test_unknown_tag_fails(self):
        self.assertEqual(run("- **Name.** `maybe`\n")[0], 1)

    def test_tag_not_right_after_the_name_fails(self):
        self.assertEqual(run("- **Name.** Text first `player`.\n")[0], 1)
        self.assertEqual(run("- **Name.**`player`\n")[0], 1)
        self.assertEqual(run("- **Name.** `player`text\n")[0], 1)

    def test_two_tags_fail(self):
        self.assertEqual(run("- **Name.** `player` `trace`\n")[0], 1)
        self.assertEqual(run("- **Name.** `server` and later `player`.\n")[0], 1)

    def test_real_file_is_tagged(self):
        root = Path(__file__).resolve().parent.parent
        text = (root / check_unknowns.UNKNOWNS).read_text(encoding="utf-8")
        self.assertEqual(check_unknowns.untagged(text), [])


if __name__ == "__main__":
    unittest.main()
