import importlib.util
import io
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("symbolize_crash", ROOT / "tools/symbolize_crash.py")
symbolize_crash = importlib.util.module_from_spec(spec)
spec.loader.exec_module(symbolize_crash)

# A report as platform/crash_log.cpp writes it on Linux (tests/layout_extras_test.cpp checks the C++ side).
SAMPLE = """Hailstone crash report
build: 0.7.3-beta (abc1234)
build-id: 0123456789abcdef0123456789abcdef01234567
time: 2026-11-26 10:15:00 UTC
what: signal 11 (SIGSEGV, invalid memory access)
log: /home/x/hailstone/logs/hailstone.log
last log lines:
[info] stack: is a word a chat line could hold
[info] one

stack:
/opt/hailstone/hailstone(+0x1a2b3)[0x5555]
/opt/hailstone/hailstone(+0x1a2b3) [0x55555556e2b3]
/usr/lib/libc.so.6(+0x3e710) [0x7f000003e710]
/opt/hailstone/hailstone(main+0x20) [0x555555570020]
/opt/my (odd) folder/hailstone() [0x401000]
"""


def elf_with_note(build_id, e_type=3, end="<"):
    """A minimal 64-bit ELF: a header, one SHT_NOTE section holding a GNU build ID note, and its section header."""
    desc = bytes.fromhex(build_id)
    note = struct.pack(end + "III", 4, len(desc), 3) + b"GNU\0" + desc
    note += b"\0" * (-len(note) % 4)
    shoff = 64 + len(note)
    header = b"\x7fELF" + bytes([2, 1 if end == "<" else 2, 1]) + b"\0" * 9
    header += struct.pack(end + "HHIQQQIHHHHHH", e_type, 62, 1, 0, 0, shoff, 0, 64, 0, 0, 64, 1, 0)
    section = struct.pack(end + "IIQQQQIIQQ", 0, 7, 2, 0, 64, len(note), 0, 0, 4, 0)
    return header + note + section


class ParseTests(unittest.TestCase):
    def test_report_fields_and_stack(self):
        fields, stack = symbolize_crash.parse_report(SAMPLE)
        self.assertEqual(fields["build-id"], "0123456789abcdef0123456789abcdef01234567")
        self.assertEqual(fields["what"], "signal 11 (SIGSEGV, invalid memory access)")
        self.assertEqual(len(stack), 5)  # the log's own "stack:" word is not the stack

    def test_frames(self):
        _, stack = symbolize_crash.parse_report(SAMPLE)
        p = symbolize_crash.parse_frame
        self.assertIsNone(p(stack[0]))  # no space before the address: not glibc's format
        self.assertEqual(p(stack[1]), ("/opt/hailstone/hailstone", "", 0x1a2b3, 0x55555556e2b3))
        self.assertEqual(p(stack[3]), ("/opt/hailstone/hailstone", "main", 0x20, 0x555555570020))
        self.assertEqual(p(stack[4]), ("/opt/my (odd) folder/hailstone", "", None, 0x401000))

    def test_build_id_from_elf(self):
        with tempfile.TemporaryDirectory() as tmp:
            for end in ("<", ">"):
                path = Path(tmp) / "nested" / f"hailstone{end == '>'}.debug"
                path.parent.mkdir(exist_ok=True)
                path.write_bytes(elf_with_note("00ff10", end=end))
                self.assertEqual(symbolize_crash.elf_info(path), (3, "00ff10"))
            (Path(tmp) / "version.txt").write_text("0.7.3\n")
            (Path(tmp) / "short.debug").write_bytes(b"\x7fELF")
            self.assertEqual(symbolize_crash.find_debug_file(tmp, "00ff10").parent.name, "nested")
            self.assertIsNone(symbolize_crash.find_debug_file(tmp, "abcd"))

    def test_zip_reports(self):
        with tempfile.TemporaryDirectory() as tmp:
            z = Path(tmp) / "hailstone-crash-20261126-101503.zip"
            with zipfile.ZipFile(z, "w") as f:
                f.writestr("summary.md", "x")
                f.writestr("crash/crash-20261126-101500.txt", SAMPLE)
                f.writestr("logs/hailstone.log", "y")
            found = symbolize_crash.reports(z)
            self.assertEqual([n for n, _ in found], ["crash/crash-20261126-101500.txt"])
            self.assertEqual(found[0][1], SAMPLE)


def tools_missing():
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    need = [cc, shutil.which("objcopy"), shutil.which("nm"),
            shutil.which("llvm-symbolizer") or shutil.which("addr2line")]
    return None if all(need) else "needs a C compiler, objcopy, nm and llvm-symbolizer or addr2line"


@unittest.skipIf(tools_missing(), tools_missing() or "")
class EndToEndTests(unittest.TestCase):
    # A small optimised program split as cmake/SeparateSymbols.cmake splits the client, and a report naming one of
    # its functions the way glibc's backtrace does.
    def test_names_a_function_by_build_id(self):
        cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
        with tempfile.TemporaryDirectory() as tmp:
            t = Path(tmp)
            (t / "prog.c").write_text("__attribute__((noinline)) int crashing_function(int *p) { return *p + 1; }\n"
                                      "int main(int argc, char **argv) { (void)argv;\n"
                                      "  return crashing_function(argc > 5 ? &argc : 0); }\n")
            exe, symbols = t / "hailstone", t / "symbols" / "0.0.1" / "linux-x64"
            symbols.mkdir(parents=True)
            debug = symbols / "hailstone.debug"
            subprocess.run([cc, "-O2", "-g1", "-fPIE", "-pie", "-Wl,--build-id", str(t / "prog.c"), "-o", str(exe)],
                           check=True)
            subprocess.run(["objcopy", "--only-keep-debug", "--compress-debug-sections=zlib", str(exe), str(debug)],
                           check=True)
            subprocess.run(["objcopy", "--strip-debug", f"--add-gnu-debuglink={debug}", str(exe)], check=True)
            e_type, build_id = symbolize_crash.elf_info(exe)
            self.assertEqual(symbolize_crash.elf_info(debug), (e_type, build_id))
            address = symbolize_crash.symbol_addresses(debug)["crashing_function"]
            report = SAMPLE.replace("0123456789abcdef0123456789abcdef01234567", build_id)
            report = report.replace("+0x1a2b3) [0x55555556e2b3]", f"+{address + 4:#x}) [0x55555556e2b3]")
            (t / "crash.txt").write_text(report)
            for tool in ("llvm-symbolizer", "addr2line"):
                if not shutil.which(tool):
                    continue
                with self.subTest(tool=tool):
                    out = io.StringIO()
                    real = sys.stdout
                    sys.stdout = out
                    try:
                        status = symbolize_crash.main([str(t / "crash.txt"), "--symbols", str(t / "symbols"),
                                                       "--tool", tool])
                    finally:
                        sys.stdout = real
                    text = out.getvalue()
                    self.assertEqual(status, 0, text)
                    self.assertIn("crashing_function at ", text)
                    self.assertIn("prog.c:1", text)
                    self.assertIn("/usr/lib/libc.so.6(+0x3e710) [0x7f000003e710]", text)  # another module, as it was

if __name__ == "__main__":
    unittest.main()
