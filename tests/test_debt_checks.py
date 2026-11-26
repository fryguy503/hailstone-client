"""Tests for tools/check_layers.py and tools/check_path_strings.py: the lists they keep only shrink."""

import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import check_layers  # noqa: E402
import check_path_strings  # noqa: E402


def run(module, root: Path, *flags: str) -> tuple[int, str]:
    out = io.StringIO()
    argv = sys.argv
    sys.argv = [module.__file__, *flags, str(root)]
    try:
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            code = module.main()
    finally:
        sys.argv = argv
    return code, out.getvalue()


class Tree:
    def __init__(self):
        self._dir = tempfile.TemporaryDirectory()
        self.root = Path(self._dir.name)
        (self.root / "tools/data").mkdir(parents=True)
        for folder in check_layers.LEVEL:
            (self.root / "src" / folder).mkdir(parents=True)

    def write(self, rel: str, text: str) -> None:
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def read(self, rel: str) -> str:
        return (self.root / rel).read_text(encoding="utf-8")

    def close(self):
        self._dir.cleanup()


class LayerCheckTest(unittest.TestCase):
    def setUp(self):
        self.tree = Tree()
        self.addCleanup(self.tree.close)

    def test_downward_and_same_row_includes_pass(self):
        self.tree.write("src/app/a.cpp", '#include "game/x.h"\n#include <net/y.h>\n')
        self.tree.write("src/game/b.cpp", '#include "ui/z.h"\n#include "assets/w.h"\n')
        self.tree.write("src/core/c.cpp", '#include "platform/p.h"\n')
        self.tree.write("src/mq/d.cpp", '#include "plugin/hooks.h"\n#include "nav/nav_control.h"\n')
        self.assertEqual(run(check_layers, self.tree.root)[0], 0)

    def test_new_upward_include_fails(self):
        self.tree.write("src/net/a.cpp", '#include "game/x.h"\n')
        code, out = run(check_layers, self.tree.root)
        self.assertEqual(code, 1)
        self.assertIn("src/net/a.cpp: includes game/x.h", out)

    def test_commented_include_is_ignored(self):
        self.tree.write("src/net/a.cpp", '// #include "game/x.h"\n/* #include "app/y.h" */\n')
        self.assertEqual(run(check_layers, self.tree.root)[0], 0)

    def test_listed_include_passes_and_fixed_one_must_go(self):
        self.tree.write("src/net/a.cpp", '#include "game/x.h"\n')
        self.tree.write("tools/data/layer_exceptions.txt",
                        "# comment\nsrc/net/a.cpp game/x.h\nsrc/assets/b.cpp game/y.h\n")
        code, out = run(check_layers, self.tree.root)
        self.assertEqual(code, 1)
        self.assertIn("src/assets/b.cpp game/y.h is fixed", out)

        code, _ = run(check_layers, self.tree.root, "--update")
        self.assertEqual(code, 0)
        listed = self.tree.read("tools/data/layer_exceptions.txt")
        self.assertIn("src/net/a.cpp game/x.h", listed)
        self.assertNotIn("src/assets/b.cpp", listed)

    def test_update_never_adds(self):
        self.tree.write("src/net/a.cpp", '#include "game/x.h"\n')
        code, _ = run(check_layers, self.tree.root, "--update")
        self.assertEqual(code, 1)
        self.assertFalse((self.tree.root / "tools/data/layer_exceptions.txt").exists())

    def test_folder_without_a_layer_fails(self):
        (self.tree.root / "src/newthing").mkdir()
        code, out = run(check_layers, self.tree.root)
        self.assertEqual(code, 1)
        self.assertIn("src/newthing/ has no layer", out)

    def test_real_tree_has_every_folder_in_a_row(self):
        root = Path(__file__).resolve().parent.parent
        self.assertEqual(check_layers.unknown_folders(root), [])


class PathStringCheckTest(unittest.TestCase):
    def setUp(self):
        self.tree = Tree()
        self.addCleanup(self.tree.close)

    def test_unlisted_call_fails(self):
        self.tree.write("src/app/a.cpp", "auto s = p.string();\n")
        code, out = run(check_path_strings, self.tree.root)
        self.assertEqual(code, 1)
        self.assertIn("src/app/a.cpp: 1 path::string() call(s), 0 allowed", out)

    def test_comments_and_path_utf8_pass(self):
        self.tree.write("src/app/a.cpp", "// p.string() is not used\nauto s = path_utf8(p); auto t = s.str();\n")
        self.assertEqual(run(check_path_strings, self.tree.root)[0], 0)

    def test_counts_only_go_down(self):
        self.tree.write("src/app/a.cpp", "a(p.string(), q.generic_string());\n")
        self.tree.write("tools/data/path_string_counts.txt", "2 src/app/a.cpp\n3 src/core/b.cpp\n")
        self.tree.write("src/core/b.cpp", "x.string(); y.string();\n")
        code, out = run(check_path_strings, self.tree.root)
        self.assertEqual(code, 1)
        self.assertIn("src/core/b.cpp has 2 call(s), listed as 3", out)

        self.assertEqual(run(check_path_strings, self.tree.root, "--update")[0], 0)
        counts = self.tree.read("tools/data/path_string_counts.txt")
        self.assertIn("2 src/core/b.cpp", counts)

        self.tree.write("src/app/a.cpp", "a(p.string(), q.generic_string(), r.string());\n")
        code, _ = run(check_path_strings, self.tree.root, "--update")
        self.assertEqual(code, 1)
        self.assertIn("2 src/app/a.cpp", self.tree.read("tools/data/path_string_counts.txt"))


if __name__ == "__main__":
    unittest.main()
