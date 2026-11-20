# Parser hardening

The client parses untrusted bytes from the network (session, login, world and zone packets) and from game files
(PFS/S3D/EQG archives, WLD, TER/MOD/ZON, heightmap DAT, DDS/BMP/TGA, XMI, sound and effect tables, ini). This page
lists what is checked, with synthetic inputs only, and what was found.

## Sanitizer build

```sh
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DHAILSTONE_SANITIZE=ON
cmake --build build-asan --target hailstone_tests
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build-asan --output-on-failure
```

`HAILSTONE_SANITIZE` adds `-fsanitize=address,undefined -fno-sanitize-recover=undefined` to every target (GCC or
Clang), so a finding aborts the test with a stack trace. The findings below came from the fuzz tests; the rest of the
suite ran clean.

The lookup collector's per-thread pointer is `constinit thread_local` (null in every new thread, no dynamic
initialization wrapper). Keep it so: it avoids a GCC 13.3/ELF TLS-relaxation bug where an optimized sanitizer null
check reused flags from an ADD the linker had replaced with LEA.

## Fuzz-style tests (always on, a few seconds)

The tests are `tests/fuzz_net_test.cpp`, `tests/fuzz_assets_test.cpp` and `tests/fuzz_server_cfg_test.cpp`, with
helpers in `tests/fuzz_support.h`. Inputs are deterministic (SplitMix64, fixed seeds):

1. known-good synthetic fixtures cut at every length up to 700 bytes (sparser beyond);
2. seeded mutations: bit flips, boundary bytes, 32-bit fields overwritten with `0xffffffff`/`0x80000000`/small
   counts, deleted/duplicated runs, appended tails;
3. random data behind a valid magic.

`tests/fuzz_alloc_guard.cpp` replaces `operator new` so a single request over 64 MB (a parser trusting a bogus
length) fails the test, as does a call over 2 s.

| Area | What is run | Must |
|---|---|---|
| 338 typed decoders `decode_*(const AppPacket&)`, and the game-side decoders of server data (`tests/fuzz_decoders.inc`; a test checks the list covers every such declaration in `src/net/*.h` and `src/game/*.h`) | 5 patterns (zeros, 0xff, counting, random, text) of 24 KB cut at ~900 lengths each, 300 mutations, and an opcode sweep for the decoders that look at the opcode | never throw, never over-allocate |
| Session codec | `decode_datagram` for all pass/CRC combinations (also mutated plain packets re-encoded with a valid CRC), `decompress_body` (including a zlib bomb), `split_combined`/`split_app_combined`, `decode_app`, `parse_session_response`, `InboundStream` (fragments, out-of-order, wraparound, 20 000 random packets) | never throw |
| Live `Session` | a real session over loopback UDP receiving 3 000 corrupt datagrams | keep running |
| Login replies | `parse_handshake_reply`, `parse_login_reply`, `parse_server_list`, `parse_play_reply`, `reply_body`, with compressed/bomb seeds | error by exception only (the caller catches `std::exception`) |
| Server configuration | `plugin::parse_server_cfg`, with enabled/disabled policies, malformed headers/lines, identity lists, embedded NULs, binary input, truncations and seeded mutations, up to 16 KiB per input (`tests/fuzz_server_cfg.h`) | never throw or over-allocate; every add-on identity kept is a normalized owner/repo; disabled add-on rules remain inactive, invalid enabled policies refuse installed add-ons' files but never the server's own, explicit blocks win |
| Shared GPU textures | the transport's frame header (`gpu_ipc_wire::decode_header`), the broker's request parser (`gpu_cache::decode_request`, with the texture recipe it carries) and the recipe decoder (`gpu_cache::decode_texture`) | errors by `std::exception` |
| Assets | PFS (multi-block), WLD (mesh fixtures plus every fragment type 0x03-0x37, `build_mesh` with its parts checked against its indices and vertices, `build_skeleton_pose`), EQG TER/MOD, ZON, heightmap ZON/DAT, ECO, TOG, water, EQGS/EQGM meshes, animations, EQGL layers, PTS/PRT points and emitters, texture animation lists, radial flora (ECO flora layers, RFD definitions), `Resources/OnDemandResources.txt`, DDS/BMP (all depths, RLE)/TGA, DDS cube maps made octahedral, WLD detail palette maps, JPEG, XMI, `_sounds.eff`, spellsnew, the old `spells.eff` (switch particles), EDD, sound-bank/asset/emt/race text, ini, zone-line tags | no crash/hang/huge allocation; errors by `std::exception` |

#### libFuzzer targets

Optional libFuzzer targets (`-DHAILSTONE_LIBFUZZER=ON`, Clang) come from `tests/fuzz_libfuzzer.cpp`:

- `fuzz_pfs`, `fuzz_wld`, `fuzz_soe`, `fuzz_packets`;
- `fuzz_texture`, which also runs the cube map, palette map and JPEG decoders;
- `fuzz_decoders`: every typed decoder of `tests/fuzz_decoders.inc`, byte 0 choosing it;
- `fuzz_zip`: the updater's package zip and manifest;
- `fuzz_server_cfg`: server configuration and opt-in policy invariants, up to 16 KiB of text;
- `fuzz_gpucache`: the shared GPU texture transport's frame header, broker requests and texture recipes;
- `fuzz_models`: EQGL layers, PTS, PRT, texture animations, ECO flora, RFD and the old `spells.eff`, byte 0
  choosing.

These are only syntax-checked (no Clang build of the dependencies has been run); the deterministic tests are the
verified part.

## Findings and fixes

| Where | Problem | Fix |
|---|---|---|
| `sdk/hailstone_byte_io.h` (`core/byte_io.h`) `ByteReader::strnlen_bounded` | `memchr(nullptr, 0, 0)` on an empty buffer (UB), via `fixed_string`/`cstring` | return 0 for a zero length |
| `src/assets/pfs.cpp` `inflate_entry` | the directory's file size sized a `std::vector` unchecked: a 12-byte entry could request 4 GB | reject a size above 1032 x the archive size (zlib's maximum expansion) |
| `src/assets/pfs.cpp` `inflate_entry` | within that ratio, ~4 MB of junk could still declare 4 GB, zero-filled up front | the output grows with what zlib writes (64 KB, then doubling) |
| `src/assets/eqg_character.cpp` `build_model` | EQGA key times 0 and 0xFFFFFFFF resampled to 21 million poses per bone | a clip is cut at 4096 frames (13.6 minutes at 200 ms) |
| `src/assets/eqg_character.cpp` `EqgCharacterStore::part_mesh` | a long Resources/Layers chain recursed once per link (stack overflow) | a layered part follows at most 8 rows to its base |
| `src/assets/zone_fires.cpp` `merge_fires` | a non-finite or absurdly scaled flame cast NaN to int64 or walked (1e14)^2 grid cells | drop non-finite flames and those over 1000 units; grid cells saturate |
| `src/assets/eqg.cpp` `light_definition_color` | `*COLOR 1e400` in a light .def cast inf to int64 (UB) | `saturate_cast`; a non-finite `*INTENSITY` is 1 |
| `src/assets/texture.cpp` `decode_bmp` | `biClrUsed` over what the bit depth indexes wrote past the palette (heap overflow) | clamp the entry count to `1 << bits` |
| `src/assets/eqg.cpp` `parse_heightmap` | tile longitude/latitude `i32 - 100000` overflowed near `INT_MIN` | subtract as `uint32_t` and wrap |
| `src/assets/eqg.cpp` `HeightmapTile::height_at` | a non-finite position reached float-to-unsigned casts | treat non-finite coordinates as 0 |
| `src/net/movement_packets.cpp` `decode_find_person_reply`, `src/game/player_path.cpp` `handle_reply` | an all-NaN reply left the path empty, then read its last point (UB) | non-finite points are dropped in the decoder; a reply that leaves no point is a failed find |
| `src/game/player_path.cpp` `load_path_settings`, `trim`, `walk` | a `SegmentLength` or `ControlSegmentLength` of 0 or less, a negative or tiny `LODFactor`, or a NaN `MergeDistance` in Path.ini looped forever or grew without end | such values take the defaults; trim adds at most the reply's 10000 points, and a drawn segment at most 256 |
| `src/game/collision.cpp` `CollisionMesh::build`, `object_collision.cpp` `ObjectShape::from_mesh`, `zone_ladders.cpp` | a mesh part reaching past the index list read out of bounds | the part ends at the list's end |
| `src/assets/zone_flora.cpp` `CoverGrid` | ~256 zone-sized roof triangles wrapped the 32-bit cell totals: the index was under-allocated and overrun | a triangle over 1,024 cells goes to a list tested at every point (at most 256); the cells hold at most 2^24 references; the rest are no cover |
| `src/assets/zone_flora.cpp` `ZoneFloraGrower`, `CoverGrid` | a non-finite vertex reached float-to-int casts; a 1e9-unit triangle split into 4^10 pieces over millions of tiles | triangles with a non-finite vertex or one past 100,000 units are left out; at most 2^21 pieces; the cell lookup clamps as a float |
| `src/assets/wld.cpp` `string_at`, `src/assets/eqg.cpp` `string_at`, `src/assets/eqg_character.cpp` `name_at` | each reference into a long run without a NUL copied all of it (terabytes for a 5 MB run and a million fragments) | a name ends after 256 bytes (the longest in the RoF2 files is 59); the EQGS/EQGA reader, which refuses an unterminated name, refuses a longer one |
| `src/assets/eqg.cpp` `parse_heightmap` | tiles were reserved at 12 bytes per tile for a ~160-byte record: 3.4 GB for a 256 MB DAT | reserved by the smallest record a tile can have (its vertices, flags and seven words) |
| `src/assets/eqg.cpp` `nearest_painted_tile` | `lng + dx` overflowed near the int32 limits | in 64 bits; a neighbour past the limits is none |
| `src/assets/png_read.cpp` `decode_png` | the whole stream was inflated, then unfiltered into a second copy: 1.3 GB peak from 500 KB | inflated a row at a time into two row buffers, written straight into the image; the stream must still end where the image does |
| `src/assets/texture.cpp` `decode_bmp` (RLE) | a 130 KB RLE8 could declare 8192 x 8192, decoded via a 64 MB index buffer into a 256 MB image | refused when the size is over 128 pixels per byte of data (+512 bytes), what runs can fill; decoded straight into the image |
| `src/assets/character_dress.cpp` `layered_texture` | layers were composited at the widest width by the tallest height: 16384 x 1 plus 1 x 16384 made 1 GiB | composited at the size of the layer with the most texels |
| `src/assets/wld_zone.cpp`, `src/assets/eqg_zone.cpp` (lights) | a non-finite light position, colour or radius (`lights.wld`, EQG DAT or ZON) reached the light grid | left out at load (`finite_light`) |
| `src/assets/wld.cpp` header and 0x03 | the fragment count and a bitmap's name count were checked only against the bytes left, at 8 and 2 bytes each, while a parsed fragment is well over 100 bytes and a name a `std::string`: a small archive whose member inflates to its 256 MB limit asked for gigabytes | absolute caps far above the RoF2 files (`wld::kMaxFragments` 524288 against 55572; 4096 names against 12) |
| `src/nav/nav_mesh.cpp` `InputGeometry::finish` | the tile grid was sized from the raw vertex bounds: one vertex at 1e30 asked for billions of tiles, and one past `int`'s range made the tile cast undefined | triangles past 100000 units are left out (as ground_refine and zone_flora take them), and geometry needing more than 2^20 tiles builds no mesh |

Valid input behaves as before. The packet decoders, session layer, login parsers and the WLD, ZON, model, DDS, TGA,
XMI and table parsers had no findings.

## Not covered

- Real game files: the synthetic fixtures reach the parsers' normal paths but cannot reproduce every real-world quirk.
- Packet decoders get structure only from the patterns above; deep fields behind several valid prefixes (e.g. the
  later parts of the player profile) are reached only with all-zero or counting data.
- `ReliableChannel` (timing logic, no parser) and the game-connection state machines beyond the `Session`.
- Windows/macOS: the sanitizer build was run on Linux with GCC only.

## Mapped game archives

`PfsArchive::open` can map an archive read-only (`platform::MappedFile`). Callers going through `GameDir` map files
in the asset folder or the classic folder (`PfsArchive::storage_for`), which Hailstone never writes.

A file in a server's overlay (`servers/<id>/assets`) is read into the heap: the server files sync replaces overlay
files while the client runs, and Windows refuses a rename over a mapped file. A file that cannot be mapped (no
address space, a file system that cannot map, an empty file) is also read into the heap.

#### The cost of mapping

A game file that another program truncates or rewrites in place while it is mapped can:

- crash the client with SIGBUS on Linux and macOS, or `EXCEPTION_IN_PAGE_ERROR` on Windows, when a member past the
  new end is read;
- hand the parsers bytes that change under them.

(a patcher, launcher or sync tool writing into the game folder alongside the client). The parsers bound every read
against the mapping's length, so changed bytes are treated like any malformed file; a fault from a shrunk file is
not caught.

It is accepted because:

- Hailstone never writes the game folder (docs/layout.md), and nothing else is expected to while the client runs;
- a patcher that does is expected to replace files by rename (which leaves the old mapping intact on POSIX) rather
  than truncate them;
- the gain is the archives' size in private memory.

A file replaced by rename is picked up the next time the archive is opened (another zone,
`CharacterArchiveCache::clear()` after an asset-folder or overlay change).

## Windows allocation profiling

The memory checkpoints split resident memory, Windows private commit, GPU allocations and estimated character
payloads. When private commit has no measured owner, record allocation stacks with WPR (its Heap and
VirtualAllocation profiles) rather than assuming a leak. `wpr -profiles` and `wpr -help heap` show what the installed
recorder supports.

See Microsoft's [Recording for Heap Analysis](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/recording-for-heap-analysis)
and [WPR command-line options](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/wpr-command-line-options).

#### Preparing the run

Record one client from before login to after the repeated zoning route. Build with symbols
(`cmake --build build-msvc --config RelWithDebInfo --target hailstone` for a Visual Studio build) and keep the
matching `hailstone.exe` and `hailstone.pdb`: without the PDB, stacks do not resolve to functions. Put traces under
the ignored `build-msvc/` folder.

In an **elevated PowerShell** at the repository root, with Hailstone closed:

```powershell
wpr -status
wpr -HeapTracingConfig hailstone.exe
```

Do not cancel someone else's active recording. Note the heap-tracing configuration to restore it afterwards. When it
was disabled:

```powershell
New-Item -ItemType Directory -Force .\build-msvc\memory-profile | Out-Null
$trace = Join-Path (Resolve-Path .\build-msvc\memory-profile) ("route-{0:yyyyMMdd-HHmmss}.etl" -f (Get-Date))
wpr -HeapTracingConfig hailstone.exe enable
wpr -start Heap -start VirtualAllocation -filemode
```

#### Recording the route

Check each command succeeds. Launch `build-msvc/hailstone/hailstone.exe`, log in, run the route and return to the
first zone. Stop **while the client is still in that zone**, before shutdown frees the outstanding allocations:

```powershell
wpr -stop $trace
wpr -HeapTracingConfig hailstone.exe disable
```

Always restore the initial configuration afterwards (leave it enabled if it was). It affects only newly launched
clients. Tracing adds overhead, so compare normal memory from the client logs too.

#### Reading the trace

1. Open the ETL in Windows Performance Analyzer and load the matching executable/PDB symbols.
2. Filter to the Hailstone PID.
3. In Heap Allocations, compare outstanding allocations at first settled Kael and return-to-Kael, grouped by
   allocation stack.
4. Inspect VirtualAlloc Commit LifeTimes for large committed regions outside those heap allocations.

Keep the route's `hailstone.log`. Native heap allocations, allocator-retained commit and graphics-driver regions are
distinct categories: do not count the same bytes twice. The trace alone does not say which one explains a gap.
