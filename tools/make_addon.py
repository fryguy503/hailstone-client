#!/usr/bin/env python3
"""Makes a Hailstone add-on's packages and its manifest, hailstone-addon.txt (docs/addons.md).

A script is one folder (its init.lua at the top); a plugin is one folder for each platform it is built for, each
with its library (<name>.so, <name>.dll or <name>.dylib) at the top and its windows in ui/<pack>/; a tool (a program
of its own) is one folder for each platform, with its program (<name>, <name>.exe on Windows) and its libraries at
the top and its data anywhere; an upscaler (sdk/hailstone_upscaler.h) is one folder for each platform, with its
provider (<name>.dll, <name>.so, <name>.dylib) and the libraries it loads at the top and data anywhere. UI packs use
one portable folder containing ui/<pack>/ui.ini plus presentation Lua/assets and optional notices; they require no
startup script. Upload everything in --out to a GitHub release of the add-on's repository: the client finds the manifest as the newest
release's asset and the packages beside it.

usage:
  tools/make_addon.py --name autoloot --kind script --version 1.3.0 --caps read,events,actions \\
      --dir path/to/autoloot --out release/
  tools/make_addon.py --name MQ2Hud --kind plugin --version 2.0.0 --caps read,ui \\
      --platform linux-x64=build-linux/MQ2Hud --platform windows-x64=build-win/MQ2Hud --out release/
  tools/make_addon.py --name texstudio --kind tool --version 1.0.0 \
      --platform linux-x64=dist/linux --platform windows-x64=dist/windows --out release/
  tools/make_addon.py --name xess --kind upscaler --version 1.0.0 \
      --platform windows-x64=dist/windows --out release/

Optional: --author, --description, --notes (the release's page), --min-client (the oldest Hailstone it needs).
Then sign it, if you want players' clients to refuse versions signed by anyone else:
  build/hailstone_sign keygen <secret-key-file>          (once; keep the file off GitHub)
  build/hailstone_sign sign <secret-key-file> release/hailstone-addon.txt

Files whose name starts with '.' are left out; a link is stored as the file or folder it points to, which must lie
inside the folder; --out may not lie inside a package's folder. The zips are made
the same way each time (sorted, dated 1980-01-01), so an unchanged package keeps its hash. What a package may hold is checked again by the client.
"""
import argparse
import hashlib
import os
import re
import sys
import zipfile

MANIFEST = "hailstone-addon.txt"
# core/version.cpp parse_version: no leading zeros in the three numbers or a numeric pre-release field.
VERSION = re.compile(r"^v?(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(-(0|[1-9]\d*|\d*[A-Za-z-][0-9A-Za-z-]*)(\.(0|[1-9]\d*|\d*[A-Za-z-][0-9A-Za-z-]*))*)?(\+[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*)?$")
NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_-]{0,31}$")
PLATFORM = re.compile(r"^[a-z0-9_-]{1,32}$")
CAPS = {"read", "commands", "target", "actions", "move", "chat", "events", "ui", "packets", "extend", "login", "net",
        "web", "all"}
PROGRAMS = (".exe", ".dll", ".so", ".dylib", ".com", ".bat", ".cmd", ".ps1", ".sh", ".asi", ".flt", ".py", ".js",
            ".vbs", ".scr", ".msi", ".jar", ".app")
LIBRARY = {"linux": ".so", "windows": ".dll", "macos": ".dylib"}
VERSIONED_SO = re.compile(r"^[^/]+\.so(\.[0-9]+)+$")


def tool_program(name, platform):
    return name + (".exe" if platform.startswith("windows-") else "")


def is_windows_device_name(part):
    """core/windows_names.h is_windows_device_name: CON, PRN, AUX, NUL, CONIN$, CONOUT$, COM1-9, LPT1-9 (and COM / LPT
    with a superscript 1, 2 or 3), in any case, with any extension."""
    stem = "".join(c.lower() if c < "\x80" else c for c in part.split(".", 1)[0]).rstrip(" ")  # ASCII case only
    if stem in ("con", "prn", "aux", "nul", "conin$", "conout$"):
        return True
    if not stem.startswith(("com", "lpt")):
        return False
    digit = stem[3:]
    return (len(digit) == 1 and "1" <= digit <= "9") or digit in ("\u00b9", "\u00b2", "\u00b3")


def is_safe_zip_path(name):
    """core/zip_read.cpp is_safe_zip_path, which the client applies to every name in a package: relative, '/'
    separators, no control character, '\\' or ':', no empty, '.' or '..' part, and no part Windows would store under
    another name or open as a device (a trailing '.' or ' ', aux.lua)."""
    if not name or name.startswith("/"):
        return False
    if any(ord(c) < 0x20 or ord(c) == 0x7F or c in "\\:" for c in name):
        return False
    for part in (name[:-1] if name.endswith("/") else name).split("/"):
        if part in ("", ".", ".."):
            return False
        if part.endswith((".", " ")) or is_windows_device_name(part):
            return False
    return True


def within(path, folder):
    path, folder = os.path.realpath(path), os.path.realpath(folder)
    return path == folder or path.startswith(folder.rstrip(os.sep) + os.sep)


def files_under(root):
    """Every file under root, as '/'-separated names relative to it, sorted; dotfiles and dot-folders left out. A link is stored as the file or folder it points to when that is
    inside root; one that leads out of root, to no file, or back into a folder holding it stops the tool."""
    out = []

    def walk(folder, holding):
        real = os.path.realpath(folder)
        if real in holding:
            sys.exit(f"{folder}: a link to a folder that holds it")
        holding = holding | {real}
        for name in sorted(os.listdir(folder)):
            if name.startswith("."):
                continue
            full = os.path.join(folder, name)
            if os.path.islink(full) and not within(full, root):
                sys.exit(f"{full}: a link out of {root}; copy what it points to in")
            if os.path.isdir(full):
                walk(full, holding)
                continue
            if os.path.islink(full) and not os.path.isfile(full):
                sys.exit(f"{full}: a link to no file")
            out.append(os.path.relpath(full, root).replace(os.sep, "/"))

    walk(root, frozenset())
    return sorted(out)


def check(name, kind, files, platform):
    """The client's own rules (app/addon_install.cpp unpack_addon), so a mistake shows here, not on a player's
    machine."""
    lowered = set()
    for f in files:
        if not is_safe_zip_path(f):
            sys.exit(f"{f}: a name the client refuses (a '\\', ':' or control character, a part ending in '.' or ' ', "
                     "or a Windows device name such as aux or nul)")
        if f.lower() in lowered:
            sys.exit(f"{f}: two files differ only in case")
        lowered.add(f.lower())
    # A versioned shared library (libfoo.so.1) is a library like any other: no script holds one, and a plugin holds
    # only its own library (docs/addons.md "What it holds"). The client takes it for data, since it never loads it
    # and does not make it runnable, so this is stricter than the client: it is a packaging mistake either way.
    def is_program(f):
        low = f.lower()
        return low.endswith(PROGRAMS) or bool(VERSIONED_SO.match(low.rsplit("/", 1)[-1]))

    if kind == "ui":
        packs, manifests = {}, set()
        paths = {}
        for f in files:
            components = f.split("/")
            for count in range(1, len(components) + 1):
                prefix = "/".join(components[:count])
                role = count == len(components)
                previous = paths.setdefault(prefix.lower(), (prefix, role))
                if previous[0] != prefix:
                    sys.exit(f"{f}: UI path components differ only in case")
                if previous[1] != role:
                    sys.exit(f"{f}: a UI path is both a file and a directory")
            if is_program(f):
                sys.exit(f"{f}: a UI package holds presentation Lua and assets, no programs")
            if f in {"LICENSE", "NOTICE", "NOTICE.md", "README.md"}:
                continue
            parts = f.split("/", 2)
            if len(parts) != 3 or parts[0] != "ui" or not NAME.fullmatch(parts[1]):
                sys.exit(f"{f}: UI files go in ui/<pack>/ with a safe pack name")
            key = parts[1].lower()
            if key in packs and packs[key] != parts[1]:
                sys.exit(f"{f}: UI pack folders differ only in case")
            packs[key] = parts[1]
            if parts[2] == "ui.ini":
                manifests.add(key)
        if not packs or manifests != set(packs):
            sys.exit("every UI pack has ui/<pack>/ui.ini")
        return
    if kind == "script":
        if "init.lua" not in files:
            sys.exit("a script has init.lua at its top")
        for f in files:
            if is_program(f):
                sys.exit(f"{f}: a script may hold Lua and data, no programs")
        return
    lib = LIBRARY.get(platform.split("-", 1)[0])
    if not lib:
        sys.exit(f"{platform}: not a platform Hailstone has plugins for")
    if kind == "upscaler":
        provider = f"{name}{lib}"
        if provider not in files:
            sys.exit(f"{platform}: the package has its provider at its top, {provider}")
        for f in files:
            low = f.lower()
            is_lib = low.endswith(lib) or (lib == ".so" and VERSIONED_SO.match(low.rsplit("/", 1)[-1]))
            if is_lib:
                if "/" in f:
                    sys.exit(f"{f}: an upscaler's libraries go at its top, beside its provider")
                continue
            if low.endswith(PROGRAMS):
                sys.exit(f"{f}: an upscaler may hold its provider, the libraries it loads for {platform} and data, "
                         "nothing else that runs")
        return
    if kind == "tool":
        program = tool_program(name, platform)
        if program not in files:
            sys.exit(f"{platform}: the package has its program at its top, {program}")
        for f in files:
            low = f.lower()
            is_lib = low.endswith(lib) or (lib == ".so" and VERSIONED_SO.match(low))
            if f == program:
                continue
            if is_lib:
                if "/" in f:
                    sys.exit(f"{f}: a tool's libraries go at its top, beside its program")
                continue
            if low.endswith(PROGRAMS) or VERSIONED_SO.match(low.rsplit("/", 1)[-1]):
                sys.exit(f"{f}: a tool may hold its one program, its libraries for {platform} and data, nothing "
                         "else that runs")
        return
    libs = [f for f in files if f.lower().endswith(lib)]
    if libs != [f"{name}{lib}"] and libs != [f"lib{name}{lib}"]:
        sys.exit(f"{platform}: the package has exactly one library at its top, {name}{lib} (found {libs or 'none'})")
    for f in files:
        low = f.lower()
        if low.endswith(".lua") and not low.startswith("ui/"):
            sys.exit(f"{f}: a plugin's Lua goes in ui/<pack>/")
        if is_program(f) and f not in libs:
            sys.exit(f"{f}: a plugin may hold its one library, its windows and data, no other programs")


def make_zip(path, root, names, runnable=()):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name in names:
            full = os.path.join(root, *name.split("/"))
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.create_system = 3
            info.external_attr = (0o100000 | (0o755 if name in runnable else 0o644)) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            with open(full, "rb") as f:
                z.writestr(info, f.read(), compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
    if os.path.getsize(path) >= 0xFFFFFFFF:
        sys.exit(f"{path}: 4 GiB or more (the client reads no zip64)")


def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def one_line(text, what):
    if any(ord(c) < 32 or ord(c) == 127 for c in text) or len(text.encode("utf-8")) > 200:
        sys.exit(f"--{what}: one line of at most 200 bytes")
    return text


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--name", required=True)
    ap.add_argument("--kind", required=True, choices=["script", "plugin", "tool", "upscaler", "ui"])
    ap.add_argument("--version", required=True)
    ap.add_argument("--caps", default="", help="what it needs: read,ui,actions ... (docs/plugins.md); tool/upscaler/UI packages none")
    ap.add_argument("--dir", help="a script or UI package's folder")
    ap.add_argument("--platform", action="append", default=[], help="a plugin's, a tool's or an upscaler's PLATFORM=FOLDER, once for each")
    ap.add_argument("--author", default="")
    ap.add_argument("--description", default="")
    ap.add_argument("--notes", default="", help="the release's page")
    ap.add_argument("--min-client", default="", help="the oldest Hailstone it works with")
    ap.add_argument("--autoload", action="store_true", help="managed Lua add-on: activate after installation, controlled in Add-ons")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    if a.autoload and a.kind != "script":
        sys.exit("--autoload is only for a managed Lua add-on (--kind script)")
    if not NAME.match(a.name):
        sys.exit(f"{a.name}: 1 to 32 letters, digits, '-' and '_', starting with a letter or digit")
    for v in (a.version, a.min_client or "0.0.0"):
        if not VERSION.match(v):
            sys.exit(f"{v}: not a version (1.2.3, 1.2.3-beta.1)")
    caps = [c.strip().lower() for c in a.caps.replace(" ", ",").split(",") if c.strip() and c.strip() != "none"]
    if a.kind in ("tool", "ui"):
        if caps:
            sys.exit(f"--caps: a {a.kind} package asks for none")
    elif a.kind == "upscaler":
        if caps:
            sys.exit("--caps: an upscaler asks for none (it sees only the frames it upscales)")
    elif not caps or any(c not in CAPS for c in caps):
        sys.exit(f"--caps: some of {', '.join(sorted(CAPS))}")
    if a.notes and (not a.notes.startswith(("https://", "http://")) or any(c.isspace() for c in a.notes)):
        sys.exit("--notes: an http(s) address")
    if a.notes:
        one_line(a.notes, "notes")

    builds = []  # (platform, folder)
    if a.kind in ("script", "ui"):
        if not a.dir or a.platform:
            sys.exit(f"a {a.kind} takes --dir (and no --platform)")
        builds.append(("any", a.dir))
    else:
        if a.dir or not a.platform:
            sys.exit(f"a {a.kind} takes --platform PLATFORM=FOLDER, once for each platform (and no --dir)")
        for p in a.platform:
            plat, _, folder = p.partition("=")
            if not PLATFORM.match(plat) or plat == "any" or not folder:
                sys.exit(f"--platform {p}: PLATFORM=FOLDER, as linux-x64=build/MQ2Hud")
            builds.append((plat, folder))
    for _, folder in builds:
        if within(a.out, folder):
            sys.exit(f"--out {a.out} is inside {folder}: its zips and manifest would go into the package")
    os.makedirs(a.out, exist_ok=True)

    lines = ["hailstone-addon 1", f"name {a.name}", f"kind {a.kind}", f"version {a.version}"]
    if a.autoload:
        lines.append("autoload 1")
    if a.min_client:
        lines.append(f"min_client {a.min_client}")
    if a.author:
        lines.append(f"author {one_line(a.author, 'author')}")
    if a.description:
        lines.append(f"description {one_line(a.description, 'description')}")
    lines.append(f"caps {','.join(caps) if caps else 'none'}")
    if a.notes:
        lines.append(f"notes {a.notes}")
    for plat, folder in builds:
        files = files_under(folder)
        if not files:
            sys.exit(f"{folder}: empty")
        check(a.name, a.kind, files, plat)
        file = f"{a.name}-{a.version}.zip" if plat == "any" else f"{a.name}-{a.version}-{plat}.zip"
        path = os.path.join(a.out, file)
        make_zip(path, folder, files, [tool_program(a.name, plat)] if a.kind == "tool" else [])
        lines.append(f"package {plat} {os.path.getsize(path)} {digest(path)} {file}")
        print(f"{plat}: {path} ({len(files)} files)")
    manifest = os.path.join(a.out, MANIFEST)
    with open(manifest, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print(f"{manifest}: not signed (optional: build/hailstone_sign sign <secret-key-file> {manifest})")


if __name__ == "__main__":
    main()
