# Parser hardening

The client parses untrusted bytes from the network (session, login, world and zone packets) and from game files
(PFS/S3D/EQG archives, WLD, TER/MOD/ZON, heightmap DAT, DDS/BMP/TGA, XMI, sound and effect tables, ini). This page
lists what is checked and what was found. Every input is synthetic.

## Sanitizer build

```sh
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DHAILSTONE_SANITIZE=ON
cmake --build build-asan --target hailstone_tests
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build-asan --output-on-failure
```

`HAILSTONE_SANITIZE` adds `-fsanitize=address,undefined -fno-sanitize-recover=undefined` to every target (GCC or Clang), so
a finding aborts the test with a stack trace. The whole existing suite (561 test cases, 44 skipped for missing
`EQ_GAME_DIR` / vector files) ran clean under it before any fix; the findings below all came from the new fuzz tests.

The lookup collector's per-thread pointer is declared and defined `constinit thread_local`: every thread starts
with a null collector, and other translation units need no dynamic initialization wrapper. This also avoids an
observed GCC 13.3/ELF TLS-relaxation interaction that made an optimized sanitizer null check reuse flags from an
ADD after the linker replaced it with LEA. Sanitizer checks remain enabled; lookup tests cover nested scopes and
a fresh worker thread's independent state.

## Fuzz-style tests (always on, a few seconds)

`tests/fuzz_net_test.cpp`, `tests/fuzz_assets_test.cpp`, helpers in `tests/fuzz_support.h`. Inputs are deterministic
(SplitMix64 with fixed seeds): known-good synthetic fixtures cut at every length up to 700 bytes (sparser beyond),
then seeded mutations (bit flips, boundary bytes, 32-bit fields overwritten with `0xffffffff`/`0x80000000`/small
counts, deleted/duplicated runs, appended tails), then random data behind a valid magic.
`tests/fuzz_alloc_guard.cpp` replaces `operator new` so a single request over 64 MB fails the test (a parser
trusting a bogus length). A call over 2 s also fails.

| Area | What is run | Must |
|---|---|---|
| 296 typed decoders `decode_*(const AppPacket&)`, and the game-side decoders of server data (`tests/fuzz_decoders.inc`; a test checks the list covers every such declaration in `src/net/*.h` and `src/game/*.h`) | 5 patterns (zeros, 0xff, counting, random, text) of 24 KB cut at ~900 lengths each, 300 mutations, and an opcode sweep for the decoders that look at the opcode | never throw, never over-allocate |
| Session codec | `decode_datagram` for all pass/CRC combinations (also mutated plain packets re-encoded with a valid CRC), `decompress_body` (including a zlib bomb), `split_combined`/`split_app_combined`, `decode_app`, `parse_session_response`, `InboundStream` (fragments, out-of-order, wraparound, 20 000 random packets) | never throw |
| Live `Session` | a real session over loopback UDP receiving 3 000 corrupt datagrams | keep running |
| Login replies | `parse_handshake_reply`, `parse_login_reply`, `parse_server_list`, `parse_play_reply`, `reply_body`, with compressed/bomb seeds | error by exception only (the caller catches `std::exception`) |
| Shared GPU textures | the transport's frame header (`gpu_ipc_wire::decode_header`), the broker's request parser (`gpu_cache::decode_request`, with the texture recipe it carries) and the recipe decoder (`gpu_cache::decode_texture`) | errors by `std::exception` |
| Assets | PFS (multi-block), WLD (mesh fixtures plus every fragment type 0x03-0x37, `build_mesh` with its parts checked against its indices and vertices, `build_skeleton_pose`), EQG TER/MOD, ZON, heightmap ZON/DAT, ECO, TOG, water, EQGS/EQGM meshes, animations, DDS/BMP (all depths, RLE)/TGA, XMI, `_sounds.eff`, spellsnew, EDD, sound-bank/asset/emt/race text, ini, zone-line tags | no crash/hang/huge allocation; errors by `std::exception` |

Optional libFuzzer targets (`-DHAILSTONE_LIBFUZZER=ON`, Clang): `fuzz_pfs`, `fuzz_wld`, `fuzz_texture`, `fuzz_soe`, `fuzz_decoders` (every typed decoder of `tests/fuzz_decoders.inc`, byte 0 choosing it),
`fuzz_packets`, `fuzz_zip` (the updater's package zip and manifest), `fuzz_gpucache` (the shared GPU texture transport's frame header, broker requests and texture recipes) from `tests/fuzz_libfuzzer.cpp`. These were only syntax-checked here (no Clang build of the
dependencies was run); the deterministic tests are the verified part.

## Findings and fixes

| Where | Problem | Fix |
|---|---|---|
| `sdk/hailstone_byte_io.h` (`core/byte_io.h`) `ByteReader::strnlen_bounded` | `memchr(nullptr, 0, 0)` on an empty buffer: UB (null pointer passed to a nonnull parameter), hit by `fixed_string`/`cstring` on empty input | return 0 for a zero length |
| `src/assets/pfs.cpp` `inflate_entry` | the directory's file size was used for `std::vector(size)` before any check: a 12-byte directory entry could request 4 GB | reject a size above 1032 x the archive size (zlib's maximum expansion) |
| `src/assets/pfs.cpp` `inflate_entry` | within that ratio, ~4 MB of junk deflate could still declare 4 GB, zero-filled before zlib saw a byte | the output grows with what zlib writes (64 KB, then doubling) |
| `src/assets/eqg_character.cpp` `build_model` | EQGA key times 0 and 0xFFFFFFFF resampled to 21 million poses per bone | a clip is cut at 4096 frames (13.6 minutes at 200 ms) |
| `src/assets/eqg_character.cpp` `EqgCharacterStore::part_mesh` | a Resources/Layers chain of distinct names recursed once per link (stack overflow at ~100k) | a layered part follows at most 8 rows to its base |
| `src/assets/zone_fires.cpp` `merge_fires` | a non-finite or absurdly scaled flame cast NaN to int64 or walked (1e14)^2 grid cells | drop non-finite flames and those over 1000 units; grid cells saturate |
| `src/assets/eqg.cpp` `light_definition_color` | `*COLOR 1e400` in a light .def cast inf to int64 (UB) | `saturate_cast`; a non-finite `*INTENSITY` is 1 |
| `src/assets/texture.cpp` `decode_bmp` | `biClrUsed` larger than the bit depth can index (e.g. 256 colours at 1 bit) wrote past the palette array: heap-buffer-overflow | clamp the entry count to `1 << bits` |
| `src/assets/eqg.cpp` `parse_heightmap` | tile longitude/latitude `i32 - 100000` overflowed a signed int for values near `INT_MIN` | subtract as `uint32_t` and wrap |
| `src/assets/eqg.cpp` `HeightmapTile::height_at` | a non-finite object position (corrupt record) reached float-to-unsigned casts | treat non-finite coordinates as 0 |
| `src/net/movement_packets.cpp` `decode_find_person_reply`, `src/game/player_path.cpp` `handle_reply` | a reply whose points were all NaN left the path empty, then read its last point (UB) | non-finite points are dropped in the decoder; a reply that leaves no point is a failed find |
| `src/game/player_path.cpp` `load_path_settings`, `trim`, `walk` | a `SegmentLength` or `ControlSegmentLength` of 0 or less, a negative or tiny `LODFactor`, or a NaN `MergeDistance` in Path.ini looped forever (or added points every frame without end) | such values take the defaults; trim adds at most the reply's 10000 points, and a drawn segment at most 256 |
| `src/game/collision.cpp` `CollisionMesh::build`, `object_collision.cpp` `ObjectShape::from_mesh`, `zone_ladders.cpp` | a mesh part whose first index and count reach past the index list read out of bounds | the part ends at the list's end |
| `src/assets/zone_flora.cpp` `CoverGrid` | ~256 zone-sized roof triangles wrapped the 32-bit cell totals, so the cell index was allocated too small and written past its end | a triangle over 1,024 cells goes to a list tested at every point (at most 256); the cells hold at most 2^24 references; the rest are no cover |
| `src/assets/zone_flora.cpp` `ZoneFloraGrower`, `CoverGrid` | a NaN or infinite vertex reached float-to-int casts; a finite 1e9-unit triangle split into 4^10 pieces, each binned over millions of tiles | triangles with a non-finite vertex or one past 100,000 units are left out; at most 2^21 pieces; the cell lookup clamps as a float |
| `src/assets/wld.cpp` `string_at`, `src/assets/eqg.cpp` `string_at`, `src/assets/eqg_character.cpp` `name_at` | every reference into one long run without a NUL copied all of it: a 5 MB run named by a million fragments is terabytes | a name ends after 256 bytes (the longest in the RoF2 files is 59); the EQGS/EQGA reader, which refuses an unterminated name, refuses a longer one |
| `src/assets/eqg.cpp` `parse_heightmap` | the tile vector was reserved by a 12-byte-per-tile bound, for a ~160-byte record: 3.4 GB for a 256 MB DAT | reserved by the smallest record a tile can have (its vertices, flags and seven words) |
| `src/assets/eqg.cpp` `nearest_painted_tile` | `lng + dx` overflowed a signed int for a tile index near the int32 limits | in 64 bits; a neighbour past the limits is none |
| `src/assets/png_read.cpp` `decode_png` | the whole filtered stream (up to 1032 x the IDAT) was inflated, then unfiltered into a second copy, besides the image: 1.3 GB peak from 500 KB | inflated a row at a time into two row buffers, written straight into the image; the stream must still end where the image does |
| `src/assets/texture.cpp` `decode_bmp` (RLE) | a 130 KB RLE8 could declare 8192 x 8192 (end of bitmap at once), decoded into a 64 MB index buffer and the 256 MB image | refused when the size is over 128 pixels per byte of data (+512 bytes), what runs can fill; decoded straight into the image |
| `src/assets/character_dress.cpp` `layered_texture` | Luclin layers were composited at the widest layer's width by the tallest's height: a 16384 x 1 and a 1 x 16384 layer made 1 GiB | composited at the size of the layer with the most texels |
| `src/assets/wld_zone.cpp`, `src/assets/eqg_zone.cpp` (lights) | a non-finite light position, colour or radius from `lights.wld`, an EQG zone's DAT or ZON reached the light grid | left out at load (`finite_light`) |

Behaviour for valid input is unchanged. The typed packet decoders, the session layer, the login parsers and the WLD, ZON,
model, DDS, TGA, XMI and table parsers produced no findings.

## Not covered

- Real game files: the synthetic fixtures reach the parsers' normal paths but cannot reproduce every real-world quirk.
- Packet decoders get structure only from the patterns above; deep fields behind several valid prefixes (e.g. the
  later parts of the player profile) are reached only with all-zero or counting data.
- `ReliableChannel` (timing logic, no parser) and the game-connection state machines beyond the `Session`.
- Windows/macOS: the sanitizer build was run on Linux with GCC only.

## Mapped game archives

`PfsArchive::open` maps an archive read-only (`platform::MappedFile`) when the caller asks for it, which every caller
that finds a file through `GameDir` does for a file in the asset folder or the classic folder
(`PfsArchive::storage_for`). Those folders are never written by Hailstone. A file in a server's overlay
(`servers/<id>/assets`, which the server files sync replaces while the client runs) is read into the heap once, as every
archive was before: on Windows a rename over a file with a mapped view is typically refused, and the sync would fail.
When a mapping cannot be made (no address space, a file system that cannot map, an empty file), the file is read into
the heap instead.

The cost: bytes are no longer read once at open. A game file that another program truncates or rewrites in place while
it is mapped (a patcher or launcher run alongside the client, a sync tool writing into the game folder) can crash the
client with SIGBUS on Linux and macOS or `EXCEPTION_IN_PAGE_ERROR` on Windows when a member past the new end is read,
or hand the parsers bytes that change under them. The parsers bound every read against the mapping's length as they did
against a heap buffer, so changed bytes are treated like any malformed file; a fault from a shrunk file is not caught.
It is accepted because Hailstone never writes the game folder (docs/layout.md) and nothing else is expected to while
the client runs, a patcher that does is expected to replace files by rename (which leaves the old mapping intact on
POSIX) rather than truncate them, and the gain is the archives' size in private memory. Overlay files, the ones that do
change under a running client, are never mapped. A file replaced by rename is picked up the next time the archive is opened
(another zone, `CharacterArchiveCache::clear()` after an asset-folder or overlay change).

## Windows allocation profiling

The memory checkpoints distinguish resident memory, Windows private commit, GPU allocations and estimated character payloads. When the remaining private commit has no measured owner, use allocation stacks instead of assuming that freed objects leaked. WPR's Heap profile records heap allocation/free activity; VirtualAllocation records virtual allocation activity. The installed recorder's `wpr -profiles` and `wpr -help heap` show which commands it supports. See Microsoft's [Recording for Heap Analysis](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/recording-for-heap-analysis) and [WPR command-line options](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/wpr-command-line-options).

Use one client, starting before login and ending after the repeated zoning route. Build with symbols first (`cmake --build build-msvc --config RelWithDebInfo --target hailstone` for a Visual Studio build), and retain its matching `hailstone.exe` and linker `hailstone.pdb`. A Release build without the matching PDB can identify modules but cannot reliably resolve Hailstone allocation stacks to functions. Put traces under the ignored `build-msvc/` folder.

In an **elevated PowerShell** at the repository root, with Hailstone closed:

```powershell
wpr -status
wpr -HeapTracingConfig hailstone.exe
```

If another recording is active, finish that recording before starting this one; do not cancel someone else's session. Note the current heap-tracing configuration so it can be restored afterward. When it was initially disabled:

```powershell
New-Item -ItemType Directory -Force .\build-msvc\memory-profile | Out-Null
$trace = Join-Path (Resolve-Path .\build-msvc\memory-profile) ("route-{0:yyyyMMdd-HHmmss}.etl" -f (Get-Date))
wpr -HeapTracingConfig hailstone.exe enable
wpr -start Heap -start VirtualAllocation -filemode
```

Check each command succeeds. Then launch `build-msvc/hailstone/hailstone.exe`, log in, run the route and return to the first zone. Stop the recording **while the client remains in that zone**, so outstanding allocations can be examined before shutdown frees them:

```powershell
wpr -stop $trace
wpr -HeapTracingConfig hailstone.exe disable
```

Always restore the initial tracing configuration after collecting or abandoning the run (leave it enabled if it was already enabled before this procedure). Heap tracing configuration affects newly launched clients, which is why the client must be restarted after enabling it. Trace-enabled measurements have instrumentation overhead; compare normal runtime memory using the client logs as well.

Open the ETL in Windows Performance Analyzer and load the matching executable/PDB symbols. Filter to the Hailstone PID. In Heap Allocations, compare outstanding allocations at first settled Kael and return-to-Kael, grouped by allocation stack. Inspect VirtualAlloc Commit LifeTimes for large committed regions outside those heap allocations. Keep the associated `hailstone.log` for the route; native heap allocations, allocator-retained commit and graphics-driver regions are distinct categories, and the same bytes must not be added twice. Trace collection itself does not establish which category explains a gap.
