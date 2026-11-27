#!/usr/bin/env python3
"""Names the frames of a Linux crash report's stack (docs/hardening.md "Crash reports").

A crash report (logs/crash-<date>.txt, platform/crash_log.cpp) ends with glibc's backtrace, one frame a line:
"/opt/hailstone/hailstone(+0x1a2b3) [0x5555...]". The released binary carries no debug information; its symbols are
kept aside per version (release-symbols/<version>/<platform>/hailstone.debug, tools/docker/build.sh). This picks the
symbol file whose GNU build ID is the report's "build-id:" line, and prints each frame of the client's binary as a
function and source line, with the functions inlined into it. Frames of other modules (libc, SDL, the driver, a
plugin) are printed as they are.

  tools/symbolize_crash.py logs/crash-20261126-101500.txt --symbols release-symbols/
  tools/symbolize_crash.py hailstone-crash-20261126-101503.zip --symbols release-symbols/0.7.3-beta/linux-x64
  tools/symbolize_crash.py crash.txt --debug-file build-rel/symbols/hailstone.debug

A bug report's zip is read as it is (each crash/crash-*.txt in it). Needs llvm-symbolizer (preferred: it names
inlined functions) or binutils' addr2line on PATH. A Windows crash leaves crash-<date>.dmp instead, which a debugger
reads with the build's hailstone.pdb (docs/hardening.md).
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import zipfile
from pathlib import Path

FRAME = re.compile(r"^(?P<module>.*)\((?P<inside>[^()]*)\) \[(?P<addr>0x[0-9a-fA-F]+)\]\s*$")
ET_EXEC = 2


def elf_info(path):
    """(e_type, build ID in hex or None) of an ELF file; None when it is not one."""
    try:
        with open(path, "rb") as f:
            head = f.read(64)
            if len(head) < 52 or head[:4] != b"\x7fELF":
                return None
            is64 = head[4] == 2
            end = "<" if head[5] == 1 else ">"
            e_type = struct.unpack(end + "H", head[16:18])[0]
            if is64:
                shoff, = struct.unpack(end + "Q", head[40:48])
                shentsize, shnum = struct.unpack(end + "HH", head[58:62])
            else:
                shoff, = struct.unpack(end + "I", head[32:36])
                shentsize, shnum = struct.unpack(end + "HH", head[46:50])
            for i in range(min(shnum, 4096)):
                f.seek(shoff + i * shentsize)
                sh = f.read(shentsize)
                if is64:
                    sh_type, = struct.unpack(end + "I", sh[4:8])
                    offset, size = struct.unpack(end + "QQ", sh[24:40])
                    align, = struct.unpack(end + "Q", sh[48:56])
                else:
                    sh_type, = struct.unpack(end + "I", sh[4:8])
                    offset, size = struct.unpack(end + "II", sh[16:24])
                    align, = struct.unpack(end + "I", sh[32:36])
                if sh_type != 7 or size > 1 << 20:  # SHT_NOTE
                    continue
                f.seek(offset)
                found = gnu_build_id(f.read(size), end, 8 if align == 8 else 4)
                if found:
                    return e_type, found
            return e_type, None
    except (OSError, struct.error):
        return None


def gnu_build_id(notes, end, align):
    """The NT_GNU_BUILD_ID note's descriptor in hex, from a note section's bytes."""
    at = 0
    while len(notes) - at >= 12:
        namesz, descsz, kind = struct.unpack(end + "III", notes[at:at + 12])
        at += 12
        name = notes[at:at + namesz]
        at = (at + namesz + align - 1) // align * align
        desc = notes[at:at + descsz]
        at = (at + descsz + align - 1) // align * align
        if kind == 3 and name == b"GNU\0" and len(desc) == descsz and descsz:
            return desc.hex()
    return None


def parse_report(text):
    """(header fields, stack lines) of a crash report: the fields are its "key: value" lines before the log."""
    fields = {}
    head = text.split("last log lines:", 1)[0]
    for line in head.splitlines():
        key, sep, value = line.partition(": ")
        if sep and key in ("build", "build-id", "time", "what", "log"):
            fields[key] = value.strip()
    at = text.rfind("\nstack:\n")
    stack = text[at + len("\nstack:\n"):].splitlines() if at >= 0 else []
    return fields, [s for s in stack if s.strip()]


def parse_frame(line):
    """(module path, symbol, offset, absolute address) of one backtrace_symbols line, or None."""
    m = FRAME.match(line.strip())
    if not m:
        return None
    symbol, offset = "", None
    inside = m.group("inside")
    if inside:
        sym, plus, off = inside.rpartition("+")
        if plus and re.fullmatch(r"0x[0-9a-fA-F]+", off):
            symbol, offset = sym, int(off, 16)
        else:
            symbol = inside
    return m.group("module"), symbol, offset, int(m.group("addr"), 16)


def find_debug_file(symbols, build_id):
    """The file under `symbols` (a folder searched through, or a file) whose build ID is `build_id`."""
    root = Path(symbols)
    candidates = [root] if root.is_file() else sorted(p for p in root.rglob("*") if p.is_file())
    for p in candidates:
        if p.suffix not in (".debug", "") and root.is_dir():
            continue
        info = elf_info(p)
        if info and info[1] == build_id:
            return p
    return None


def pick_tool(name):
    if name in ("auto", "llvm-symbolizer"):
        found = shutil.which("llvm-symbolizer")
        if found:
            return "llvm-symbolizer", found
        if name != "auto":
            sys.exit("symbolize_crash: llvm-symbolizer is not on PATH")
    found = shutil.which("addr2line")
    if not found:
        sys.exit("symbolize_crash: neither llvm-symbolizer nor addr2line is on PATH")
    return "addr2line", found


def symbolize(tool, debug_file, addresses):
    """For each address, its [(function, location)] list, innermost (inlined) first."""
    if not addresses:
        return []
    kind, exe = tool
    hexes = [hex(a) for a in addresses]
    if kind == "llvm-symbolizer":
        out = subprocess.run([exe, "--obj=" + str(debug_file), "--inlining", "--demangle", "--functions=linkage",
                              *hexes], capture_output=True, text=True, check=True).stdout
        groups = out.strip("\n").split("\n\n")
        result = []
        for g in groups:
            lines = g.splitlines()
            result.append([(lines[i], lines[i + 1] if i + 1 < len(lines) else "??")
                           for i in range(0, len(lines), 2)])
        result += [[("??", "??")]] * (len(addresses) - len(result))
        return result[:len(addresses)]
    out = subprocess.run([exe, "-C", "-f", "-i", "-a", "-e", str(debug_file), *hexes], capture_output=True, text=True,
                         check=True).stdout
    result, current = [], None
    lines = out.splitlines()
    i = 0
    while i < len(lines):
        if re.fullmatch(r"0x[0-9a-fA-F]+", lines[i]):
            current = []
            result.append(current)
            i += 1
            continue
        if current is not None:
            current.append((lines[i], lines[i + 1] if i + 1 < len(lines) else "??"))
        i += 2
    return result


def symbol_addresses(debug_file):
    """name -> address of the debug file's symbols, for a frame given as "(name+0x...)"."""
    nm = shutil.which("nm")
    if not nm:
        return {}
    out = subprocess.run([nm, "--defined-only", str(debug_file)], capture_output=True, text=True).stdout
    table = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            table.setdefault(parts[2], int(parts[0], 16))
    return table


def render(text, debug_file, modules, tool, out):
    """Writes the report's header and its stack, named, to `out`."""
    fields, stack = parse_report(text)
    for key in ("build", "build-id", "time", "what"):
        if key in fields:
            out.write(f"{key}: {fields[key]}\n")
    if not stack:
        out.write("(no stack in this report)\n")
        return
    info = elf_info(debug_file)
    is_exec = bool(info) and info[0] == ET_EXEC
    frames = [parse_frame(line) for line in stack]
    table = None
    # The address within the binary that each of its frames stands for, as the symbol file numbers it.
    wanted = {}
    for i, f in enumerate(frames):
        if not f or os.path.basename(f[0]) not in modules:
            continue
        module, symbol, offset, absolute = f
        if is_exec:
            wanted[i] = absolute
        elif symbol and offset is not None:
            table = symbol_addresses(debug_file) if table is None else table
            if symbol in table:
                wanted[i] = table[symbol] + offset
        elif offset is not None:
            wanted[i] = offset
    # A frame's address is where its call returns to, so the line asked for is one byte before it, inside the call.
    # The frame the signal interrupted is the exception (the faulting instruction itself): it is the one after the
    # signal trampoline that follows the handler's own frame.
    order = sorted(wanted)
    names = dict(zip(order, symbolize(tool, debug_file, [max(wanted[i] - 1, 0) for i in order])))
    for i in order:
        if any("on_signal" in fn for fn, _ in names[i]) and i + 2 in names:
            names[i + 2] = symbolize(tool, debug_file, [wanted[i + 2]])[0]
            break
    for i, line in enumerate(stack):
        if i not in names:
            out.write(f"#{i:<3} {line.strip()}\n")
            continue
        entries = names[i]
        fn, loc = entries[0]
        out.write(f"#{i:<3} {fn} at {loc}  [{os.path.basename(frames[i][0])}+{wanted[i]:#x}]\n")
        for fn, loc in entries[1:]:
            out.write(f"       inlined into {fn} at {loc}\n")


def reports(path):
    """(name, text) of each crash report in a crash-*.txt or a bug report's zip."""
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as z:
            names = [n for n in z.namelist() if re.search(r"(^|/)crash-[^/]*\.txt$", n)]
            dumps = [n for n in z.namelist() if n.endswith(".dmp")]
            for n in dumps:
                print(f"{n}: a Windows minidump; open it in a debugger with the build's hailstone.pdb "
                      "(docs/hardening.md)", file=sys.stderr)
            return [(n, z.read(n).decode("utf-8", "replace")) for n in names]
    return [(str(path), Path(path).read_text(encoding="utf-8", errors="replace"))]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("report", help="a crash-*.txt, or a bug report's zip")
    ap.add_argument("--symbols", help="a folder of symbol files (searched through) to pick from by build ID")
    ap.add_argument("--debug-file", help="the symbol file to use, whatever the report's build ID")
    ap.add_argument("--module", action="append", default=[],
                    help="a module name whose frames the symbol file names (default: its own name less .debug)")
    ap.add_argument("--tool", choices=("auto", "llvm-symbolizer", "addr2line"), default="auto")
    args = ap.parse_args(argv)
    if not args.symbols and not args.debug_file:
        ap.error("give --symbols or --debug-file")
    found = reports(args.report)
    if not found:
        sys.exit(f"symbolize_crash: no crash report in {args.report}")
    tool = pick_tool(args.tool)
    status = 0
    for name, text in found:
        if len(found) > 1:
            print(f"== {name}")
        fields, _ = parse_report(text)
        build_id = fields.get("build-id")
        if args.debug_file:
            debug_file = Path(args.debug_file)
            info = elf_info(debug_file)
            if info is None:
                sys.exit(f"symbolize_crash: {debug_file} is not an ELF file")
            if build_id and info[1] != build_id:
                print(f"warning: {debug_file}'s build ID is {info[1]}, the report's {build_id}: the names may be wrong",
                      file=sys.stderr)
        elif not build_id:
            print(f"{name}: no build-id line (an older build, or not Linux); give --debug-file", file=sys.stderr)
            status = 1
            continue
        else:
            debug_file = find_debug_file(args.symbols, build_id)
            if not debug_file:
                print(f"{name}: no symbol file under {args.symbols} has build ID {build_id}", file=sys.stderr)
                status = 1
                continue
        print(f"symbols: {debug_file}")
        modules = set(args.module) or {debug_file.name[:-len(".debug")] if debug_file.name.endswith(".debug")
                                       else debug_file.name}
        render(text, debug_file, modules, tool, sys.stdout)
    return status


if __name__ == "__main__":
    sys.exit(main())
