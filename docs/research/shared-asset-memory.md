# Shared asset memory

Investigation dated 2026-11-12, against public `main` at `9f6226a86982380440da561cf80dbc2e86868674`.
The archive-storage and encoded classic-animation stages are implemented on `improvement/shared-asset-memory`.
The later geometry/collision stages remain proposals. Validation results are recorded below as they become available.

## Implemented archive stage

The playable client accepts the opt-in `assets.shared_memory=auto` setting, default off, and a
`assets.shared_memory_cache_mb` disk budget, default 1024 MiB. See docs/layout.md "Shared archive memory" for setup.
`GameDir` carries the cache service explicitly to archive loaders; no process-global cache is required. Participating
production loaders preserve the normal cache-off policy: read-only mappings for base/classic game archives and
owned bytes for replaceable server-overlay archives. Tools using the default heap-storage overload and
`PfsArchive::from_memory` fixtures retain owned bytes. With `auto` selected, a zero cache budget takes the owned
fallback path; it is distinct from disabling the snapshot cache and using the normal mapping policy.

The implementation stores exact archive snapshots as `<SHA-256>.blob`, verifies their size and digest before
mapping, and leaves existing PFS parser validation in place. There is no new native-struct cache header. Every
acquisition hashes the current opened source; a cache miss copies it in bounded chunks and verifies the second
digest and source-handle stamp before publication. Cache snapshots are never changed after publication.

One global publication lock serializes insertion and budget accounting in this first stage. Waiters recheck for
the winning file for at most 250 ms, then load privately. This is simpler than the per-entry coordination proposed
for future derived caches and can produce more fallbacks under contention for unrelated files. Finished snapshots
and orphan temporary bytes count toward the budget. Only an exclusive cache-use lease permits eviction or explicit
trimming; every live mapping and builder retains a shared lease. Windows uses protected current-user cache ACLs,
and POSIX requires a user-owned private cache leaf. Unsafe paths, links or permissions fall back privately.

Mappings in one cache instance share one native use-lease object. The service retains only a weak reference, so
the last mapping/active load releases it and permits cleanup. POSIX closes the source descriptor immediately after
successful `mmap`; the mapping retains its backing independently. Windows retains the file handle to deny writes.
Thus retaining many POSIX mappings does not consume two file descriptors per archive.

The developer performance window reports live mapped backing plus hit/build/fallback counts. It does not report
physical-memory savings. Tests include actual helper processes for concurrent cold acquisition, process termination,
native lock release and live-reader cleanup refusal. Static collision, geometry, PCM and GPU sharing are unchanged.

## Encoded animation and compact metadata stage

Character archive loading also publishes inflated WLD members by content digest through `SharedFileCache::store`.
They use the same bounded disk budget, immutable publication, native mapping and lease protocol as archive snapshots.
A cache failure retains the supplied decoded bytes privately. The parser's byte framing and validation still apply;
no compiler-native structures or GLM arrays are mapped onto file contents.

Character WLDs keep a validated resource index and defer full fragment decoding until a scoped consumer needs it.
The existing process-wide 128 MiB LRU retains released decoded payloads within its own budget, separate from the
snapshot disk budget. Shared immutable loaders retain encoded byte owners through payload eviction, so built
animation views can remain valid after the full decoded WLD is released.

`TrackFrames` retains the immutable WLD owner and checked packed/unpacked key ranges. Const access returns decoded
values through `ByteReader`; mutable access materializes an independent vector. `BoneFrames` lets built WLD model
clips and their aliases retain those same bytes, applying the original normalization and borrowed-scale rules when
sampled. EQG animation data keeps owned vectors. Thus decoded global keys and copies in built clips no longer need
separate retained private arrays when their immutable source is mapped.

Clips retain the normal `AnimationTrack` copy-on-write behavior, model-local key deduplication and constant-track
compaction. Constant tracks keep their logical length and sampling period; mapped tracks remain encoded instead
of materializing private poses to compact them. Builder mutation detaches only the modified track. Private key
storage and encoded owners are counted once across aliases in model diagnostics, with mapped payload bytes kept
separate from private storage.

WLD fragments box only rare oversized mesh/particle-cloud payloads, preserving value-copy behavior. The common
small payloads remain inline. A sorted vector of fragment indices replaces the owning hash name index and preserves
case-sensitive, first-definition lookup semantics. These metadata reductions apply with shared memory off as well;
expanded OFF/ON measurements must be distinguished from the original archive-only binary's results.

Character track and resource-level tables borrow exact names from stable deferred resource-index entries, or
retained fragment names in eagerly parsed WLDs, avoiding two additional owning key copies per track. Full decoded
payloads can therefore leave the LRU without invalidating those names. Other tables keep owning keys where names
are transformed.
Copy/move assignment swaps archive owners and their dependent tables together, so even replaced keys remain valid
through table destruction. Recorded lookup dependencies still own their strings.

## Recommendation

Multiple Hailstone processes on one computer can share physical RAM used by identical, immutable asset bytes.
Use a persistent asset cache under `platform::cache_dir()`, with every client mapping the same finished files
read-only. Start with compressed archive storage, measure it, then extend the design to retained static collision,
character geometry and animation data. Decoded sound samples are another relatively small integration.

This needs a platform mapping abstraction and changes to payload ownership. A shared C++ allocator, or putting the
existing `SharedCharacterCache` into a mapped region, would not work: its objects contain pointers, containers,
reference counts and synchronization state that belong to one process.

The first scope is simultaneous clients using one Hailstone data folder on a local filesystem, under the same user.
Clients may play on different servers or in different zones; only matching resolved content is reused. Different
portable installs have different caches unless they already use the same data folder. Identical copies of a cache
file do not guarantee shared physical pages. Do not change `HAILSTONE_DATA_DIR` automatically to obtain sharing:
that would also combine settings, servers and other data. A cache-only root spanning independent installs would
need a separate, explicit folder-layout design.

## What the investigated baseline client duplicates

This table describes the original `main` snapshot named above, before the implemented stages.

| Area | Current behavior and evidence | Value of mapping |
|---|---|---|
| Compressed archives | [`PfsArchive::open`](../../src/assets/pfs.cpp), lines 112-121, calls `read_file` and retains a full private byte vector; [`PfsArchive`](../../src/assets/pfs.h), line 54, owns that vector. [`read_file`](../../src/core/game_dir.cpp), line 307, allocates and reads the whole file. | A focused first change can replace the retained archive vector with mapped immutable bytes. Global, zone and on-demand loaders use this boundary. Directory indexes stay private. |
| Inflated archive members | `PfsArchive::read` and `inflate_entry` return fresh vectors. | Mapping compressed archives does not remove decompression or parsed-object allocations. A later immutable member cache could avoid repeated inflation. |
| Character models and animations | [`CharacterModel`](../../src/assets/character.h), line 139, contains nested containers. The cache at line 554 uses pointers and shared ownership; [`CharacterArchiveCache`](../../src/assets/character.cpp), around line 745, reuses global data between zones in one process. | Potential retained-memory savings, but geometry and animation payloads need a flat representation and view-based consumers first. |
| Static collision | [`CollisionMesh`](../../src/game/collision.h), lines 151-156, owns triangles, lighting, cell vectors and large-triangle references. [`collision.cpp`](../../src/game/collision.cpp) builds them per client. | A promising next target for clients in the same zone. Keep ladder settings and query scratch private. |
| Decoded textures | [`Image`](../../src/assets/texture.h), lines 32-38, owns mip vectors. [`ZoneRenderer::upload`](../../src/render/zone_renderer.cpp), lines 277-283, releases CPU images after staging. | Primarily less repeated decoding/mip work and lower loading peaks. Many of these CPU images are temporary already. |
| GPU textures and buffers | [`gpu.cpp`](../../src/render/gpu.cpp), lines 69-79 and 148-167, creates per-process VMA resources. Upload staging also copies bytes. | CPU file mapping does not share these GPU allocations. |
| Decoded sound | [`Pcm`](../../src/audio/pcm.h), lines 16-20, retains sample vectors. [`SoundLibrary`](../../src/audio/sound_library.cpp), lines 284-291, publishes immutable PCM; [`Mixer`](../../src/audio/mixer.cpp), lines 351-390, reads samples with private voice state. | A small, useful retained-payload consumer for the same storage abstraction. |
| Navigation | [`ZoneNav`](../../src/nav/zone_nav.cpp), lines 35-45, reads and copies its existing disk cache; [`NavMesh::add_tile`](../../src/nav/nav_mesh.cpp), lines 662-675, allocates another tile copy, and `mark_areas` mutates it. | Coordinate cached build output initially. The live Detour mesh is not a drop-in read-only mapping. |

The OS already caches file reads, but `read_file` copies those bytes into each process's heap. Read-only mappings
avoid those private archive copies. Preserving that benefit requires consumers to retain views into mapped data;
copying a mapped payload into a vector again preserves the duplication.

SoundFont storage also needs separate work. [`xmi_player.cpp`](../../src/audio/xmi_player.cpp), lines 22-28 and
158-166, loads TinySoundFont's heap representation and copies synth instances. Mapping the original SF2 file does
not make its decoded sample heap cross-process shared.

## OS implementation

| OS | Read-only mapping | Scoped build lock |
|---|---|---|
| Windows | `CreateFileW(GENERIC_READ)` -> `CreateFileMappingW(PAGE_READONLY)` -> `MapViewOfFile(FILE_MAP_READ)` | `LockFileEx` on a separate stable lock file |
| Linux | `open(O_RDONLY | O_CLOEXEC)` -> `mmap(PROT_READ, MAP_SHARED)` | `flock` on a separate stable lock file |
| macOS | The same POSIX mapping backend; requires native Apple Clang/macOS validation | `flock` |

Windows supports independent mapping objects backed by the same file, so a named `Global\\` mapping is unnecessary.
Neither backend needs identical virtual addresses in different clients. Avoid writable mappings and Windows
`FILE_MAP_COPY`; the latter permits private copies and charges potential commit for the whole view.
See Microsoft's [CreateFileMappingW](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-createfilemappingw)
and [MapViewOfFile](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-mapviewoffile),
the Linux [mmap manual](https://man7.org/linux/man-pages/man2/mmap.2.html), and Apple's
[mmap manual](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/mmap.2.html).

All native calls stay in `src/platform/`. A move-only RAII mapping owns the view and native handles. A core immutable
byte owner can hold either a mapping or owned memory and return `std::span<const std::byte>`. Shared ownership of
that owner is local to each process and keeps views valid through asynchronous loading, zoning and uploads.
Keep `PfsArchive::from_memory` for fixtures and callers that supply owned bytes.

Map whole blobs from offset zero initially, using the OS's actual page/allocation granularity. Validate lengths
before conversion to `size_t`, handle empty files explicitly, and keep bounded owned-memory fallback for mapping
failures or address-space pressure. This supports x64 and arm64 without baking pointer size into the cache.
Source-only architectures and 32-bit address spaces still need their own build/size-limit validation.

## Immutable snapshots and concurrent launches

Mapping the original game files directly is a smaller edit, but the files are only read-only to Hailstone.
An external patcher can rewrite or truncate them. Unix mappings can fault after truncation; Windows deny-write
handles can obstruct the patcher for the life of the client. Prefer cache snapshots that Hailstone controls.

Proposed acquisition protocol:

1. Acquire a shared cache-use lease before opening payloads. Resolve the actual source through `GameDir` and
   identify its content generation. A valid finished cache hit can be mapped immediately once freshness is known.
2. On a miss, acquire the scoped build lock for the final content key, then recheck for another client's completed result.
   Bound waiting and do all copying, hashing and decoding on loading workers. On timeout, load privately.
3. Create a unique temporary file exclusively in the cache directory. Stream the source into it while hashing,
   validate the snapshot and its expected digest, close it, then publish it atomically without replacing an existing
   content key. Map the existing winner if another builder published first. Never expose a partial entry. If the
   snapshot's digest changed since acquisition began, retry under the new key rather than publishing under the old one.
4. Map the published file read-only. Every published payload is immutable: new contents or incompatible schemas
   get new files. Never truncate or rewrite a live entry. A corrupt existing key uses private fallback until exclusive
   cleanup can remove it; an ordinary atomic replace is insufficient for this no-clobber publication protocol.
5. Release the per-entry build lock. Each view retains its mapping owner and cache-use lease until its last user
   finishes. A producer crash releases native locks; later clients ignore incomplete temporary files and rebuild.

Before/after source-handle identity, size and timestamp checks detect ordinary patch races and allow bounded retries.
They cannot guarantee a coherent source generation against an uncooperative in-place writer. The existing whole-file
read also has this limitation. Content hashes provide identity for the bytes actually captured; freshness must not
depend only on size and mtime, which can stay unchanged after an edit. The initial implementation should hash current
resolved inputs when establishing a generation, and measure that extra startup I/O before adding a faster manifest.

Use stable lock-file objects; deleting and recreating a lock file while clients run can split coordination between
different file objects. The existing [`hold_lock_file`](../../src/platform/process.cpp), lines 76-95, is for process
lifetime log selection and treats some lock failures as success. It needs a separate scoped, explicit-error API for
cache coordination. Windows byte locks do not prevent access through mapped views, so a build lock does not make
in-place payload updates safe. See [LockFileEx](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-lockfileex)
and the [flock manual](https://man7.org/linux/man-pages/man2/flock.2.html).

For the first version, trim the cache only while holding an exclusive cache-use lease, which cannot coexist with
clients' shared leases. Skip cleanup when clients are active. This avoids cross-platform active-file deletion
differences and avoids unmapping/recreating a content key while existing readers still use an older file object.
Windows can initially open payloads with `FILE_SHARE_READ` and defer deletion until all mappings close. Never delete
the lease or build-lock files as part of trimming. Later live eviction requires its own acquisition/GC protocol.

Use a configurable disk budget and skip cache insertion when it cannot be met. An unavailable, full, corrupt or
unsupported cache must leave the ordinary private loader usable. This optimization should not require the cache
to succeed before a player can log in.

## Identity, format and trust

- Cache under Hailstone's data folder, never in the player's game directory. Proposed location:
  `cache/assets/v1/<kind>/<content-key>.blob`. Keep locally derived game data local; do not ship it in releases.
- Resolve server overlays and classic-preference rules first. [`GameDir`](../../src/core/game_dir.h), lines 20-28,
  permits a server's archive to replace the whole lower-layer archive. A filename, zone name or model code alone
  is not a cache identity. Different server files with the same name must produce different keys.
- Use the existing `Sha256` implementation in `core/sha256.h` for source/dependency digests.
  For derived data include the ordered input digests, schema/decoder version and every output-affecting option.
  Hashes establish identity/integrity, not trust in an arbitrary writer.
- Character output depends on more than one archive: include animation donors, resource/global-load ordering,
  appearance dependencies and Luclin selections. Reuse existing reload boundaries when asset layers change.
- For texture outputs include resolved overrides, masking/alpha rules, Color/OpaqueColor/Data use, mip rules,
  size caps, and BC capability if conversion is device-dependent. The texture pack's existing lookup hash is not
  the integrity boundary for this cache.
- Validate magic, schema, payload kind, digest, counts, offset/length arithmetic and resource limits before use.
  Encode metadata through `ByteWriter` and read with `ByteReader`. No pointers, STL objects, `glm` layouts, native
  `long`, or casts of structs onto cache bytes. Static geometry needs byte-backed accessors to flat arrays.
- PCM float payloads need an explicit validated representation and lifetime rules, too. A native sample view
  requires documented IEEE-754, endian, alignment and C++ object-lifetime handling; do not casually cast file bytes
  to `float*`. Preserve a portable decode fallback if a target cannot consume the representation safely.
- Restrict the cache to cooperating processes of the same user; use a private directory, restrictive creation
  permissions/ACLs, exclusive temporary-file creation, and checks for unexpected file types or links. Shared
  writable directories and network filesystems need separate qualification. POSIX locks are advisory.

## Proposed implementation stages

1. **Portable storage and archive integration.** Add the platform mapping/scoped-lock APIs and the core immutable
   byte owner/cache protocol. Replace `PfsArchive`'s retained input vector with that owner. Preserve parser limits,
   memory-backed fixtures and private fallback. Add explicit mapping/cache byte counters and an experimental
   `assets.shared_memory` off/auto setting, initially off. Register new sources and tests in CMake.
2. **Measure retained-memory consumers.** Compare mapped and private archives, then select static collision or
   decoded PCM based on real retained bytes. Collision can flatten triangles/light, sorted cell descriptors,
   contiguous cell references and the large-triangle list. Query marks, ladder options and dynamic objects stay
   private. PCM can retain mapped samples while each mixer voice keeps its own position, loops and gain.
3. **Geometry/animation and decode reuse.** Introduce immutable model views over flat geometry/animation data,
   keeping dressing changes, poses, indices and ownership machinery private as appropriate. Add inflated-member
   or prepared-texture caches where profiling justifies them. Raw mappings alone do not share parsed models.

The archive phase is a focused storage/ownership change. Collision and character sharing are larger representation
changes with query/rendering regression risk, so they should be reviewed and measured independently.

Gameplay state, network connections, character settings, Lua/plugin state, moving doors/boats, animation poses,
audio playback cursors and Vulkan resources remain per process.

GPU sharing is possible only through a separate external-memory design with capability checks, handle transfer,
synchronization and resource-lifetime coordination. Vulkan and MoltenVK support different external-handle paths;
do not promise a uniform VRAM reduction from this proposal. See the
[Vulkan external memory guide](https://docs.vulkan.org/guide/latest/extensions/external.html) and
[MoltenVK runtime guide](https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Runtime_UserGuide.md).

## Validation and expected benefit

For `N` clients with `S` identical resident asset bytes and `P` private resident bytes per client, the ideal asset
portion changes from `N * (P + S)` to `N * P + S`, saving approximately `(N - 1) * S` before mapping/index overhead.
`S` must be measured; this is not an estimate of a percentage of today's complete client footprint.

Run 1, 2, 4 and 6 clients in the same zone, then in different zones, with cold and warm caches. Measure private
memory, shared resident mappings, total system memory/commit, load time, repeated zoning, disk growth and VRAM
separately. Keep graphics, Luclin models, asset layers and texture packs recorded and comparable.

Do not sum clients' RSS/working sets to claim total physical use: shared pages can appear in every process.
The current [`resident_memory`](../../src/platform/process.cpp), line 42, and performance display report that kind
of per-process figure. On Linux use aggregate PSS/private fields from `smaps_rollup`; on Windows inspect private
bytes/private working set and mapped-page sharing; on macOS inspect clean mapped residency separately from
dirty/compressed footprint. See the [Linux proc documentation](https://www.kernel.org/doc/html/latest/filesystems/proc.html),
[Windows working-set documentation](https://learn.microsoft.com/en-us/windows/win32/procthread/process-working-set),
and [Apple memory profiling documentation](https://developer.apple.com/documentation/xcode/analyzing-the-memory-usage-of-your-metal-app).

Required implementation coverage includes simultaneous cold builders, producer termination, bounded lock failure,
view lifetime across zoning, source changes and truncation, same-name server overrides, differing loader options,
corrupt/oversized caches, disk-full and disabled-cache fallback, exclusive cleanup, and mapped/private behavioral
parity. Use actual subprocesses for cross-process ownership and locking tests; a shared pointer inside one process
does not demonstrate cross-process sharing.

Before calling the implementation complete, build and run the full suite with GCC/Linux, MSVC/Windows and
Apple Clang/macOS, including native multi-process checks. Run asset tests with `EQ_GAME_DIR` set and sanitizer
coverage for new cache parsers. Headless builds cannot establish rendering or live multi-client acceptance.

## Local validation

Windows x64, MSVC 19.50: the complete all-target build and final incremental rebuild passed. The one initial
command-line warning is the existing audio codec target overriding `/W4` with its intentional `/W0`; new sources
produced no compiler diagnostics. The final full base suite passed 2,504 cases and 190,107 assertions, with 58 explicit
skips (game files and external vectors unset; additional vector cases return without comparisons). Focused archive,
mapping and cache tests passed 45 cases / 1,526 assertions. A final stock-focused run passed 49 cases / 2,131 assertions,
including mapped/warm/private member parity for three stock S3D archives and an available EQG.

The stock-focused run excludes one unchanged stale-name fixture after its missing custom archive was reproduced
in the baseline binary. Windows could not create symlinks with the available user privileges, so that subcase
explicitly reports its link-specific omissions. Base, focused, stock and baseline-fixture logs remain under this
branch's ignored `build-msvc/`. No unrelated fixture or socket behavior was changed to obtain these results.

Linux x64, GCC 13.3, RelWithDebInfo with ASan/UBSan enabled and browser off: the playable client, test executable
and helper built and linked successfully. The final full base suite passed 2,510 cases / 190,535 assertions, with
58 explicit skips and game files/external vectors unset. Focused tests passed 28 cases / 1,273 assertions; the
stock-focused run passed 31 cases / 1,360 assertions, including real mapped/private archive parity and excluding
the same unavailable custom fixture. Final test and resource-limit runs emitted no sanitizer diagnostics.

A controlled Linux child with `RLIMIT_NOFILE=256` exposed the original two-descriptors-per-archive cost: a
160-source probe stopped after 125 finished snapshots. That deliberately exhausted pre-fix probe also reported a
UBSan diagnostic in the stream fallback. After sharing the native lease and closing mapped POSIX descriptors,
the final probe retained all 160 distinct sources (187,520 bytes), with 160 builds, zero fallbacks and exit 0.
The 160-archive unit regression also passed under that limit (649 assertions). These results are separate from
the ordinary full suite. Cold-cohort tests allow the documented private fallback; subsequent warm cohorts require
mapped sharing and verify that reader termination cannot invalidate other readers or permit premature cleanup.
Raw Linux logs remain under this branch's ignored `build-linux-logs/`; the native build is in
`/tmp/hailstone-shared-asset-memory-20261112`.

Native macOS execution and sustained gameplay acceptance remain open; reviewing the POSIX implementation is not a
substitute for running it on a Mac. Windows six-client live zone-entry and idle-memory measurements are recorded below.

A Windows subprocess experiment retained and touched every byte of a 64 MiB synthetic file per helper. Mapping
cohorts used a warm shared cache; all helpers reported the expected mapped size/checksum and zero private fallbacks.
These are aggregate `PrivateMemorySize64` measurements in MiB:

| Helpers | Ordinary private buffers | Read-only mapped snapshots |
|---:|---:|---:|
| 1 | 65.72 | 1.82 |
| 2 | 131.43 | 3.62 |
| 4 | 262.88 | 7.20 |
| 6 | 394.28 | 10.73 |

The six-helper cohort used 383.55 MiB less private commit. Summed working sets were 436.80 MiB with private buffers
and 438.46 MiB with mappings: those sums count shared resident pages in each reader and cannot establish unique
physical residency. This verifies the retained-file storage mechanism, not whole-client RAM savings, zone-load
performance or VRAM sharing. Cold-launch concurrency is covered separately by helper-process tests.

The experiment ran from this branch's ignored `build-msvc/measure-shared-cache-memory.ps1`; raw aggregate evidence
is in `build-msvc/shared-cache-memory-windows.json` and `.log`. All helper processes exited and the disposable
source/cache directories were removed. No game files or user settings were changed by the experiment.

## Expanded CPU storage validation, 2026-11-13

The expanded Windows all-target build passed without compiler diagnostics. Final focused tests passed 64 cases /
3,151 assertions; the full base suite passed 2,527 cases / 191,787 assertions with 59 explicit game/vector skips.
Recorded vectors were not configured; additional vector cases return without comparisons, beyond the explicit skip
count. Stock asset results below used the real game folder.
The stock asset subset passed 53 cases / 14,925,141 assertions, including private/mapped character clips across
real HUM/HUF/ERM/ELM models and retained keys after library destruction. It excludes the same missing custom fixture
identified in the archive-stage baseline. The Windows symlink-specific privilege omission remains explicit.

The fresh Linux GCC 13.3 ASan/UBSan build compiled the playable app, tests and probe. Focused tests passed 56 cases /
3,029 assertions; the full base suite passed 2,533 cases / 192,215 assertions with 59 explicit skips. Stock parity
passed 61 cases / 14,926,415 assertions with the same unavailable fixture excluded. The controlled 256-descriptor
regression passed 649 assertions while retaining 160 unique archives. No ASan, UBSan or leak-sanitizer diagnostics
were emitted in these final runs. Original third-party/GCC warnings remain documented in the build logs.

Synthetic tests cover packed/unpacked keys, malformed ranges, quaternion normalization/sign/nonfinite values,
borrowed hidden-bone scales, Luclin aliases, independent mutation, concurrent const reads and ownership beyond
cache/library lifetimes. Assignment tests replace unrelated archive/library owners and exercise copy, move and
self-assignment while resolving heap-backed borrowed track names. This validation does not establish macOS or
Linux live gameplay acceptance.

A separate native Windows CPU-only audit loaded exactly the same 114 global character archives, 114 WLD files,
860,154 fragments and 4,781,299 packed keys, before constructing model clips or a renderer:

| Global-library stage | Native private commit, MiB |
|---|---:|
| Original private storage | 881.39 |
| Expanded compact metadata, sharing off | 609.93 |
| Expanded mapped archive and animation storage | 314.23 |

The mapped audit retained 256.42 MiB of addressable shared backing (135.02 MiB compressed archives and 121.40 MiB
inflated WLD members), with 228 builds and no fallbacks. Fragment records shrank from 280 to 128 bytes on this
MSVC build, reducing their allocation from 229.69 to 105.00 MiB; expanded decoded key capacity fell from 145.91 MiB
to zero in the mapped case. These are native process commit and structural measurements, not whole-client unique
resident RAM or GPU savings. Built clips and rendering need the live measurements below.

The CPU audit used intermediate expanded client `167c7720f2656b18d24703b1d85e7e72d8b2e5c8270d63a0dd51008dcbeda91a`.
The later assignment-lifetime fix does not change that audited load path or layout. Final validated client
`3d6a9b5fbe5b873bed24c59a554713f22d56498f7a391f3379b761e4236f0125` is used for the expanded live runs.
Machine-readable local evidence is `build-msvc/wld-cpu-audit-expanded-summary.json`, final native build/test logs
are under `build-msvc/`, and sanitizer logs use `build-linux-logs/new-expanded-*`. No proprietary assets are included
in the repository.

## Earlier mixed-zone archive-only experiment, 2026-11-13

This historical experiment used the archive-only implementation and a shorter protocol. It is excluded from the
final reset study below; its references to a primary comparison apply only within this earlier experiment.

Four cohorts used six existing development accounts, real assets and the playable Vulkan client, without moving
characters: one in arena2, one in poknowledge, one in cshome and three in gfaydark. The executable SHA-256 was
`b908b3c12df096d2e889fde3bcc06b8f5513c58e76694e2bd41a7d150dd3f7c4`. Each cohort had a fresh isolated data folder
shared by its six clients, 1280 x 720 windows, foreground/background 30 FPS caps, vsync off and a 4096 MiB cache budget.
The assets and ordinary user data were read-only. Root configurations differed only in shared-memory mode;
paired character settings and generated navigation caches matched. The GPU was an AMD Radeon RX 9070 XT.

The sequence was OFF pilot, ON 1, OFF repeat, ON repeat. The pilot included a different initial character-selection
flow and retained more private memory, so it is excluded from the primary comparison. Its cause was not established.
The primary comparison uses the explicit-character OFF repeat and both explicit-character ON cohorts. Each capture
waited for all six expected zone-entry completions, then another 60 seconds, then sampled every 5 seconds for
60 seconds. All 36 primary samples passed process identity, readiness and page-query checks. These repeated samples
are observations of three cohorts, not independent randomized trials. ON repeat private commit rose from 12,641.7
to 12,733.6 MiB during its sample window; the graph preserves that ramp rather than claiming stationary memory.

| Aggregate metric | OFF median [observed range], MiB | ON pooled median [observed range], MiB |
|---|---:|---:|
| Private commit | 13,936.5 [13,936.0-13,936.5] | 12,772.7 [12,641.7-12,812.3] |
| Private resident pages | 9,516.8 [9,516.3-9,516.9] | 8,355.1 [8,227.8-8,393.4] |
| Unique resident mapped archive pages | 0 | 278.7 [278.7-278.7] |
| Private resident pages plus unique archive pages | 9,516.8 [9,516.3-9,516.9] | 8,633.8 [8,506.5-8,672.1] |

Private commit decreased by 1,163.8 MiB between pooled medians. Adding the unique archive pages back to private
residency gives an 883.0 MiB reduction in that measured resident subset. It excludes other shared mappings and
does not establish whole-client or whole-system unique physical RAM. Separately, the ON cohorts' 1,124.1-1,124.8 MiB
of resident archive views represented only 278.7 MiB of unique cache file-pages: about 845 MiB of duplicate page
accounting. The observer deduplicated immutable blob paths plus file-page offsets and checked native read-only
sharing, with no private/COW cache pages or page-query failures. This does not mean every archive page was used by
all six clients.

The native performance window (`/perf`, also Ctrl+Shift+P) was checked in the same arena2 client in both modes.
Enabled runs showed 163.5 MiB of live mapped backing, 14 hits, 180 builds and zero private fallbacks for that client.
Those are backing/counter values, not unique physical residency. Its GPU usage/allocation values matched between
modes, as did the per-PID dedicated GPU accounting in the observer; GPU resources remain per process. Summed process
working sets count shared pages repeatedly and are diagnostic only.

The snapshot caches began empty, but the OS/source-file cache was warmed by previous runs. Live-world conditions,
day/night and time since initial login were not fully controlled. These results establish live mixed-zone memory
sharing; they do not establish loading-speed, FPS, combat, repeated-zoning or native Linux/macOS gameplay results.
Every benchmark client was closed and temporary session credential aliases removed; cleanup targeted only the
benchmark processes and tagged aliases, leaving preexisting credentials unchanged.

The ignored local evidence is under `build-msvc/real-world-six-20261113-003913/`: raw captures in each
`*-measurements/`, and the generic-label SVG, per-client/time-series CSVs and numeric report in `report/`.
Raw profiles, logs and account-bearing manifests must remain local and uncommitted. The graph uses OFF repeat
`off-r2` versus `on-r1` and `on-r2`; observed ranges are not confidence intervals.

## Reset study: six same-zone Windows clients, 2026-11-13

This study supersedes the earlier mixed-zone experiment. Each valid cohort used the same six development
characters, all in gfaydark, with no movement, combat or inventory actions. The local development server was
confirmed running. The archive-only OFF/ON pair used binary `b908b3c12df096d2e889fde3bcc06b8f5513c58e76694e2bd41a7d150dd3f7c4`
on boot `2026-11-13T06:22:49.5Z`; the expanded OFF/ON pair used final binary
`3d6a9b5fbe5b873bed24c59a554713f22d56498f7a391f3379b761e4236f0125` on boot
`2026-11-13T07:47:27.5Z`. Exact boot ages and capture timestamps are retained. A reboot does not prove that unrelated
applications have no leaks; these measurements target the six identified client processes and their mapped files.

Each cohort had an isolated data folder and initially empty shared-asset cache, shared among its six clients.
Both modes used 1280 x 720 windows, 30 FPS foreground/background caps, vsync off and a 4096 MiB cache budget.
Assets, characters, zone and graphics settings matched. Plugins, scripts and optional update/news activity were
disabled. The clients used the existing game folder read-only. No compilers or asset-audit loads ran during capture.

The protocol waited for all six zone-entry completions, warmed up for 300 seconds, then collected 60 native
observations at approximately 10-second intervals over 600 seconds. The primary summary uses the last 300 seconds
by actual timestamps, declared before the first OFF capture finished. The full time series, medians, ranges,
endpoint changes and slopes remain in the report. Validation requires the expected six process identities and
zone, complete mapping inventories, no private/COW asset pages, a 575-605 second actual sample span and no gap
over 30 seconds or burst under one second. Samples are repeated observations, not independent trial replicates.

Windows `GetProcessMemoryInfo`, `QueryWorkingSet`, `VirtualQueryEx`, `GetMappedFileName` and working-set sharing
fields supply the CPU measurements. The observer counts each immutable cache file-page once across clients;
summed working sets are diagnostic only. Private resident pages plus the unique resident asset-cache pages form
the measured resident subset below. It excludes other shared mappings and is not whole-system physical RAM.
Private commit is separate from residency. The in-game `/perf` feature supplies none of these measurements.

The expanded pair also has three native address-space/residency inventories, at the start, middle and end of its
sampling window. These separate private, mapped and image memory and record large private allocations. Anonymous
heap arenas are not attributed to source subsystems without allocation-stack evidence. GPU baselines use native
WDDM per-process and adapter/local-memory counters; overlapping sources are kept separate and never added to RAM.

Both pairs verify the intended development world from their session logs. The expanded pair additionally held
server selection, explicitly selected the development world and enforced the expected world in the collector.
Failed attempts with an offline server, boot mismatch or a different selected world were invalidated and excluded.
The existing saved-world fallback is recorded in docs/unknowns.md. The archive-only pair used automatic saved-world
selection, so cross-version end-to-end differences also cross boots and startup flows. The expanded OFF/ON pair
uses one binary, boot and startup flow and is the controlled comparison for enabling sharing.

All four captures completed 60/60 valid observations (240 total), with actual spans of 589.860-590.000 seconds
and individual intervals of 9.868-10.133 seconds. Each final native process snapshot completed all six mapping
inventories with zero unmatched resident pages. The measured medians were:

| Six-client cohort | Private commit, MiB | Private resident, MiB | Unique resident asset cache, MiB | Measured resident subset, MiB |
|---|---:|---:|---:|---:|
| Archive-only, OFF | 13,611.87 | 9,453.88 | 0.00 | 9,453.88 |
| Archive-only, ON | 12,435.86 | 8,277.28 | 203.66 | 8,480.94 |
| Expanded, OFF | 12,039.89 | 7,910.70 | 0.00 | 7,910.70 |
| Expanded, ON | 8,928.24 | 4,828.46 | 324.20 | 5,152.66 |

Within the expanded binary, enabling sharing saved 2,758.04 MiB (34.86%) of the measured resident subset and
3,111.65 MiB (25.84%) of private commit. About 1,914.6 MiB of resident mapped asset views across the six processes
represented 324.2 MiB of unique resident cache file-pages, with no private/COW cache pages. Other private copies
were eliminated by keeping built clips on encoded keys. Compact metadata also reduces the OFF footprint.

The original private-loading baseline to final expanded ON observation fell from 9.23 to 5.03 GiB in the measured
resident subset: 4.20 GiB (45.50%) less. Private commit fell 4,683.63 MiB (34.41%). This combines representation
changes and sharing across different boots/startup flows; it is not an isolated estimate of any single change.
The original archive-only same-binary OFF/ON pair saved 972.94 MiB of the measured resident subset.

The expanded ON primary window still grew: private commit slope +14.40 MiB/min and endpoint change +84.59 MiB;
private resident slope +14.63 MiB/min and endpoint change +86.28 MiB. The full graph retains this behavior. The
window does not establish a plateau, a memory leak, or absence of leaks. Repeated zoning and longer sessions remain
necessary to characterize long-term retention.

Dedicated WDDM charges remained about 724.4 MiB per client, or 4,346.7 MiB (4.24 GiB) across all six, in every mode.
They are process charges, not unique physical VRAM. Separate native host counters on the active adapter in the
expanded pair provide the baseline for later GPU-sharing work:

| Native host counter, MiB | Before OFF, zero clients | OFF, six clients | Before ON, zero clients | ON, six clients |
|---|---:|---:|---:|---:|
| Adapter DedicatedUsage | 1,944.3 | 6,093.0 | 1,990.4 | 6,106.1 |
| Adapter SharedUsage | 199.7 | 434.4 | 199.0 | 434.9 |
| Local-adapter LocalUsage | 1,658.6 | 5,817.8 | 1,837.6 | 5,811.9 |

These are separate overlapping counter sources and must not be summed. Host totals include desktop/driver activity,
which was not fully controlled. GPU allocations are still per process; CPU mappings have not reduced GPU memory.

All benchmark clients were closed after their final profiles and readiness checks. No client disconnected during
capture. The five temporary credential aliases were removed, original credentials preserved, and unrelated
applications and development services left running. Native Windows live acceptance covers zone entry and idle
rendering only, not sustained combat, repeated zoning, loading speed, native Linux gameplay or native macOS.

Ignored local evidence uses `build-msvc/real-world-six-20261113-013300/` for the archive-only pair and
`build-msvc/real-world-six-20261113-063253/` for the expanded pair. The latter's `report-native-final/` contains the
sanitized SVG/PNG graph, Markdown/JSON report, full time-series CSVs, allocation categories and GPU baselines.
`independent-final-median-audit.json` independently reproduces the four primary medians from the raw native CSVs.
Raw logs, account-bearing manifests and process profiles remain local and uncommitted.

## Scaling to 24 readers, 2026-11-13

There is no coded limit of six readers. A native Windows synthetic test started 24 helper processes, each of which
retained, touched and checksummed the same disposable 64 MiB payload. Private-buffer readers consumed 1,575.73 MiB
of aggregate private commit and 1,559.84 MiB private residency. Warm mapped readers consumed 41.51 MiB private commit,
25.46 MiB private residency and 64.00 MiB of unique resident cache pages. Their summed mapped views were 1,536 MiB.
All 24 mappings succeeded, with complete native inventories, no fallback, no private/COW cache pages and no query
failures. Every helper exited and its disposable files were removed. This tests the storage mechanism at 24 readers;
it is not 24 logged-in, rendered clients.

Windows' three-bit working-set ShareCount field saturated at 7 while all 24 readers were active. That is a telemetry
limit, not a seven-reader mapping limit. File-page identity remains the deduplication key. See Microsoft's
[PSAPI_WORKING_SET_EX_BLOCK documentation](https://learn.microsoft.com/en-us/windows/win32/api/psapi/ns-psapi-psapi_working_set_ex_block).

For a conditional projection from the expanded six-client live pair, let `P` be its private residency per client
and `S(N)` the unique asset-cache residency for N clients. The measured subset is `N * P + S(N)`, not N times a
working-set sum. Keeping the same scene, graphics, per-client behavior and 324.20 MiB shared asset union gives:

| Projected 24-client metric | Expanded OFF, GiB | Expanded ON, GiB |
|---|---:|---:|
| Private commit | 47.03 | 34.88 |
| Private resident plus unique asset-cache pages | 30.90 | 19.18 |

If unique asset residency grows fourfold instead, the same private-memory assumption gives 20.13 GiB for ON.
These are short-window projections, not whole-system RAM, confidence bounds or a capacity guarantee. Different
zones, characters, retained models, paging and GPU spill can change them. The observed ON memory slope was positive;
linear scaling of that observed slope would be about 57.6 MiB/min of private commit for 24 clients, not a leak diagnosis.

The same content uses one disk-cache entry regardless of reader count. POSIX closes each mapped file descriptor;
Windows retains a file and section handle per blob in each process. Each service shares one active cache-use lease.
The main scaling risks are cold-start work and retained cache generations: every client hashes/verifies sources,
inflates character members transiently, and waits only 250 ms for the global publication lock before loading privately.
That fallback stays private for its retained lifetime. Warming the cache or staggering launches reduces that risk.
Eviction requires all live leases to end, so many different zones/asset revisions can fill the budget while clients
remain open and force newer loads private.

With CPU sharing alone, GPU memory and rendering work still grow per client. The six-client dedicated GPU-charge baseline of about 724 MiB
per client would scale to roughly 17 GiB of charges at 24, before desktop activity. Charges are not unique physical
VRAM and do not predict residency or frame rate under pressure. GPU sharing needs its own synchronization and
ownership design; the subsequent experimental Windows texture path and its separate six-client native results are
documented in [Shared GPU memory](shared-gpu-memory.md).

Ignored evidence is `build-msvc/shared-cache-memory-windows-24.json` and `.log`; the live-data projection and formulas
are in `build-msvc/cpu-memory-24-projection.json`. No 24-account live test was performed.
