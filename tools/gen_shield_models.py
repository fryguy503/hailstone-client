#!/usr/bin/env python3
"""Writes src/game/shield_models.inc: the item models (IT<n>) that Hailstone
holds on the shield point beyond the original client's list (CDisplay::IsShield
@0x489d70, game::is_shield_model), a deliberate difference behind
display.extra_shields (docs/formats/characters.md "Held items").

RoF2's list stops at the shields of its own time, so it holds about a third of
an EQEmu database's shields in the hand like a weapon (EQEmu client DLLs patch
IsShield with a list of their own for the same reason). A model joins here when
shield items (item type 8) make up more than half of the items using it that are
not augments (type 54: ornaments, which carry the shield's model as well). That
leaves out the placeholder IT63 and the weapon and armour models a few shields
borrow. The retained additions from the separate shield query live in
ADDITIONAL_SHIELDS. HANDHELD_MODELS excludes entries from that query that use
the hand grip, including models shared by shield items and ordinary held items.

Ornaments of later shields often have a model no other item uses (IT13903..13985,
IT100003..100169). The items table does not say which kind of item such an
ornament fits, so with --game-dir a model only augments use joins when its EQG model (it<n>.mod) is shaped like a
shield in every archive that has one. Seen from its broad side, a shield covers
most of the box around it: at least SHIELD_FILL of it, where every shield above
covers 0.45 or more, while the weapon ornaments in the same number ranges (axes,
swords, hammers, a handle with a head) cover 0.42 at most. It must also be no
more than SHIELD_LONG times as long as it is wide and SHIELD_THICK times as
thick (the shields above measure at most 3.0 and 0.43). A model with no EQG
mesh, or none in the folders given, stays out. SEEN_SHIELDS are ornament models
that are shields to the eye but fail the measure: a grip standing off the back
makes them thick, horns or spikes leave the box mostly empty. Every model the
measure lets in was looked at as well (drawn from its broad side).

The input is the items table of an EQEmu (PEQ) database, one "idfile<TAB>itemtype"
row per item, and the game folders to measure ornament models in (read only):

    mariadb -N peq -e "select idfile, itemtype from items" > items.tsv

usage: tools/gen_shield_models.py items.tsv [--game-dir DIR]... > src/game/shield_models.inc
"""
import argparse
import collections
import os
import struct
import zlib

STOCK_SINGLES = {10691, 10729, 10730, 67367, 10738, 10754, 10772, 10775, 10781, 10790, 10826, 10827, 10832,
                 10833, 10849, 10850, 10857, 10858, 11356, 11364, 11372, 11391, 11399, 14000, 11443}
SHIELD = 8
AUGMENT = 54
SHIELD_LONG = 3.0
SHIELD_THICK = 0.45
SHIELD_FILL = 0.44
FILL_GRID = 48
SEEN_SHIELDS = {100005, 100109, 100118, 100119, 100122, 100123, 100169}
ADDITIONAL_SHIELDS = {
    10611, 10989, 11187, 11192, 11441, 11530, 11629, 11651, 11675, 11890,
    12009, 12059, 12257, 12266, 12279, 12293, 12345, 100127, 101060, 101064,
    101068, 101628, 101629, 101630, 102815, 102816, 102914, 102915,
}
HANDHELD_MODELS = {
    13, 46, 47, 48, 51, 56, 65, 67, 68, 72,
    86, 118, 133, 149, 10012, 10015, 10510, 10511, 10512, 10609,
    10641, 10645, 10646, 10686, 10697, 10843, 10942, 11031, 11051, 11220,
    11262, 11588, 11797, 11798, 11838, 11910, 12022, 12431, 12735,
}
PFS_NAMES_CRC = 0x61580AC9


def stock_shield(m):
    return (200 <= m <= 228 or 11001 <= m <= 11020 or 10530 <= m <= 10544 or 10664 <= m <= 10671
            or 11496 <= m <= 11497 or m in STOCK_SINGLES)


def pfs_members(path):
    """The members of a PFS archive (.eqg) as {lower-case name: (offset, size)}."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 12 or data[4:8] != b"PFS ":
        return data, {}
    (dir_off,) = struct.unpack_from("<I", data, 0)
    (count,) = struct.unpack_from("<I", data, dir_off)
    entries = [struct.unpack_from("<III", data, dir_off + 4 + 12 * i) for i in range(count)]
    names_entry = next((e for e in entries if e[0] == PFS_NAMES_CRC), None)
    if names_entry is None:
        return data, {}
    files = sorted((e for e in entries if e[0] != PFS_NAMES_CRC), key=lambda e: e[1])
    blob = pfs_read(data, names_entry[1], names_entry[2])
    (n,) = struct.unpack_from("<I", blob, 0)
    o, names = 4, []
    for _ in range(n):
        (ln,) = struct.unpack_from("<I", blob, o)
        names.append(blob[o + 4:o + 4 + ln].rstrip(b"\0").decode("latin-1").lower())
        o += 4 + ln
    return data, {name: (e[1], e[2]) for name, e in zip(names, files)}


def pfs_read(data, offset, size):
    out = bytearray()
    while len(out) < size:
        deflated, _ = struct.unpack_from("<II", data, offset)
        out += zlib.decompress(data[offset + 8:offset + 8 + deflated])
        offset += 8 + deflated
    return bytes(out)


def mod_shape(mod):
    """(bounding box sides longest first, share of the box the mesh covers seen along its thinnest side)."""
    if mod[:4] != b"EQGM":
        return None
    version, strings, materials, vertices, triangles = struct.unpack_from("<IIIII", mod, 4)
    o = 28 + strings
    for _ in range(materials):
        props = struct.unpack_from("<I", mod, o + 12)[0]
        o += 16 + 12 * props
    stride = 44 if version >= 3 else 32
    if vertices == 0:
        return None
    pts = [struct.unpack_from("<3f", mod, o + stride * i) for i in range(vertices)]
    o += stride * vertices
    tris = [struct.unpack_from("<3I", mod, o + 20 * i) for i in range(triangles)]
    lo = [min(p[k] for p in pts) for k in range(3)]
    side = [max(p[k] for p in pts) - lo[k] for k in range(3)]
    a, b, _ = sorted(range(3), key=lambda k: -side[k])
    if side[b] <= 0:
        return None
    n = FILL_GRID
    flat = [((p[a] - lo[a]) / side[a] * n, (p[b] - lo[b]) / side[b] * n) for p in pts]
    covered = set()
    for t in tris:
        if max(t) >= vertices:
            continue
        (x0, y0), (x1, y1), (x2, y2) = (flat[i] for i in t)
        area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
        if area == 0:
            continue
        for gx in range(max(0, int(min(x0, x1, x2))), min(n, int(max(x0, x1, x2)) + 1)):
            for gy in range(max(0, int(min(y0, y1, y2))), min(n, int(max(y0, y1, y2)) + 1)):
                if (gx, gy) in covered:
                    continue
                cx, cy = gx + 0.5, gy + 0.5
                w0 = ((x1 - cx) * (y2 - cy) - (x2 - cx) * (y1 - cy)) / area
                w1 = ((x2 - cx) * (y0 - cy) - (x0 - cx) * (y2 - cy)) / area
                if w0 >= 0 and w1 >= 0 and w0 + w1 <= 1:
                    covered.add((gx, gy))
    return sorted(side, reverse=True), len(covered) / (n * n)


def shield_shaped(shape):
    (longest, middle, thinnest), fill = shape
    return (middle > 0 and longest <= SHIELD_LONG * middle and thinnest <= SHIELD_THICK * middle
            and fill >= SHIELD_FILL)


def model_shapes(game_dirs, wanted):
    """{model: [mod_shape per archive holding it<n>.mod]} for the models in `wanted`."""
    names = {"it%d.mod" % m: m for m in wanted}
    shapes = collections.defaultdict(list)
    for d in game_dirs:
        for fn in sorted(os.listdir(d)):
            if not fn.lower().endswith(".eqg"):
                continue
            data, members = pfs_members(os.path.join(d, fn))
            for name, (offset, size) in members.items():
                if name in names:
                    shape = mod_shape(pfs_read(data, offset, size))
                    if shape:
                        shapes[names[name]].append(shape)
    return shapes


ap = argparse.ArgumentParser()
ap.add_argument("items")
ap.add_argument("--game-dir", action="append", default=[])
args = ap.parse_args()

uses = collections.defaultdict(collections.Counter)
items = 0
with open(args.items, encoding="utf-8") as f:
    for line in f:
        parts = line.rstrip("\r\n").split("\t")
        if len(parts) != 2:
            continue
        idfile, kind = parts
        if not idfile[:2].upper() == "IT" or not idfile[2:].isdigit() or not kind.lstrip("-").isdigit():
            continue
        items += 1
        uses[int(idfile[2:])][int(kind)] += 1

models = set(ADDITIONAL_SHIELDS)
for m, kinds in uses.items():
    if m == 0 or stock_shield(m) or m in HANDHELD_MODELS or kinds[SHIELD] == 0:
        continue
    held = sum(n for k, n in kinds.items() if k != AUGMENT)
    if kinds[SHIELD] * 2 > held:
        models.add(m)
ornaments = [m for m, kinds in uses.items()
             if m != 0 and not stock_shield(m) and m not in HANDHELD_MODELS and set(kinds) == {AUGMENT}]
shapes = model_shapes(args.game_dir, ornaments) if args.game_dir else {}
for m in ornaments:
    if (shapes.get(m) and all(shield_shaped(e) for e in shapes[m])) or (args.game_dir and m in SEEN_SHIELDS):
        models.add(m)
models = sorted(models)

print("// Generated by tools/gen_shield_models.py from an EQEmu items table (%d items with an IT<n> model)." % items)
print("// Do not edit by hand. Item models of shields that CDisplay::IsShield does not list (game::is_shield_model),")
print("// with ornament models measured in %d game folder%s." % (len(args.game_dir), "" if len(args.game_dir) == 1 else "s"))
for i in range(0, len(models), 10):
    print("    " + " ".join("%d," % m for m in models[i:i + 10]))
