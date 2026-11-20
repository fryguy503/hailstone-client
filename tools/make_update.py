#!/usr/bin/env python3
"""Makes a release's download for one platform and adds it to the release's update manifest (docs/updater.md).

Run once for each platform's installed folder (cmake --install ... --prefix dist/hailstone, with the server
plugins' components installed into the same folder), with the same --out for every platform of a release; then
sign the manifest once with tools/hailstone_sign and upload everything in --out to the release.

usage: tools/make_update.py --version 0.2.0-alpha --platform linux-x64 --dist dist/hailstone [--notes URL] --out release/

The whole folder, the browser and the plugins included, becomes one zip, hailstone-<version>-<platform>.zip: the
download a player unpacks into a folder of their own, and the manifest's one package (`core`) the updater installs.
Its names have no top folder, as the updater wants them. Files and folders whose name starts with '.' are left out.
A symbolic link is stored as the file or folder it points to, which must lie inside --dist (a link out of it, or to a
folder holding it, stops the tool). --out may not lie inside --dist. The zip is made the same way each time (sorted, dated
1980-01-01, the files' Unix permissions).
Existing package lines of other platforms in --out/hailstone-update.txt are kept; this platform's are replaced.
"""
import argparse
import hashlib
import os
import re
import sys
import zipfile

MANIFEST = "hailstone-update.txt"
# core/version.cpp parse_version: no leading zeros in the three numbers or a numeric pre-release field.
VERSION = re.compile(r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(-(0|[1-9]\d*|\d*[A-Za-z-][0-9A-Za-z-]*)(\.(0|[1-9]\d*|\d*[A-Za-z-][0-9A-Za-z-]*))*)?$")
WORD = re.compile(r"^[a-z0-9_-]{1,64}$")
# What an update may never write (app/update_install.cpp is_player_path); kept out of packages here too.
PLAYER_FOLDERS = {"update", "servers", "logs", "cache", "screenshots", "bug-reports", "maps", "scripts", "textures",
                  "web", "addons", "tools", "ui-data", "ui-addons"}
PLAYER_FILES = {"hailstone.ini", "hailstone-windows.ini", "notes.txt"}


def is_player_path(name):
    p = name.lower()
    if p in ("ui-data", "ui-addons"):
        return True
    first = p.split("/", 1)[0]
    return ("/" in p and first in PLAYER_FOLDERS) or p in PLAYER_FILES or \
        p.startswith("ui/settings/") or p.startswith("ui/loadingbackgrounds/")


def is_windows_device_name(part):
    """core/windows_names.h is_windows_device_name (tools/make_addon.py has the same)."""
    stem = "".join(c.lower() if c < "\x80" else c for c in part.split(".", 1)[0]).rstrip(" ")  # ASCII case only
    if stem in ("con", "prn", "aux", "nul", "conin$", "conout$"):
        return True
    if not stem.startswith(("com", "lpt")):
        return False
    digit = stem[3:]
    return (len(digit) == 1 and "1" <= digit <= "9") or digit in ("\u00b9", "\u00b2", "\u00b3")


def is_safe_zip_path(name):
    """core/zip_read.cpp is_safe_zip_path, which the updater applies to every name in the download."""
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
    """Every file under root, as '/'-separated names relative to it, sorted; dotfiles and dot-folders left out. A link
    is stored as the file or folder it points to when that is inside root; one that leads out of root, to no file, or
    back into a folder holding it stops the tool."""
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


def make_zip(path, root, names):
    lowered = set()
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name in names:
            if not is_safe_zip_path(name):
                sys.exit(f"{name}: a name the updater refuses (a '\\', ':' or control character, a part ending in "
                         "'.' or ' ', or a Windows device name such as aux or nul)")
            if is_player_path(name):
                sys.exit(f"{name}: one of the player's paths, which an update may not write")
            if name.lower() in lowered:
                sys.exit(f"{name}: two files differ only in case")
            lowered.add(name.lower())
            full = os.path.join(root, *name.split("/"))
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.create_system = 3  # Unix: the client takes the permission bits from external_attr
            mode = os.stat(full).st_mode & 0o777
            if sys.platform == "win32":
                mode = 0o755 if name.lower().endswith((".exe", ".dll")) else 0o644
            info.external_attr = (0o100000 | mode) << 16
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


def check_notes(url):
    """The release page: an http(s) address on one line of the manifest, no blanks or control characters."""
    if (not url.startswith(("https://", "http://")) or any(c.isspace() or ord(c) < 32 or ord(c) == 127 for c in url)
            or len(url.encode("utf-8")) > 200):
        sys.exit("--notes: an http(s) address, one line of at most 200 bytes without blanks")
    return url


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", required=True)
    ap.add_argument("--platform", required=True, help="linux-x64, linux-arm64, windows-x64, macos-arm64 ...")
    ap.add_argument("--dist", required=True, help="the installed client folder, its plugins included")
    ap.add_argument("--notes", default="", help="the release's page")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    if not VERSION.match(a.version):
        sys.exit(f"{a.version}: not a version")
    if not WORD.match(a.platform):
        sys.exit(f"{a.platform}: not a platform name")
    if a.notes:
        check_notes(a.notes)
    if within(a.out, a.dist):
        sys.exit(f"--out {a.out} is inside --dist {a.dist}: its zips and manifest would go into the download")
    os.makedirs(a.out, exist_ok=True)

    everything = files_under(a.dist)
    if not any(n in ("hailstone", "hailstone.exe") for n in everything):
        sys.exit(f"{a.dist}: no hailstone binary at its top")
    file = f"hailstone-{a.version}-{a.platform}.zip"
    make_zip(os.path.join(a.out, file), a.dist, everything)
    packages = [("core", file, "")]  # (component, file, needs)

    manifest = os.path.join(a.out, MANIFEST)
    kept, notes = [], a.notes
    if os.path.exists(manifest):
        for line in open(manifest, encoding="utf-8"):
            w = line.split()
            if not w or w[0] in ("hailstone-update", "signature"):
                continue
            if w[0] == "version":
                if len(w) != 2:
                    sys.exit(f"{manifest}: \"{line.strip()}\" is not a version line (version <version>)")
                if w[1:] != [a.version]:
                    sys.exit(f"{manifest} is for {' '.join(w[1:])}, not {a.version}")
                continue
            if w[0] == "notes":
                notes = notes or (w[1] if len(w) > 1 else "")
                continue
            if w[0] == "package" and len(w) > 1 and w[1] == a.platform:
                continue
            kept.append(line.rstrip("\n"))
    lines = ["hailstone-update 1", f"version {a.version}"]
    if notes:
        lines.append(f"notes {notes}")
    lines += kept
    for component, file, needs in packages:
        path = os.path.join(a.out, file)
        line = f"package {a.platform} {component} {os.path.getsize(path)} {digest(path)} {file}"
        lines.append(line + (f" {needs}" if needs else ""))
    with open(manifest, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    for component, file, _ in packages:
        print(f"{component}: {os.path.join(a.out, file)}")
    print(f"{manifest}: not signed yet (tools/hailstone_sign sign <key> {manifest})")


if __name__ == "__main__":
    main()
