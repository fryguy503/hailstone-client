# Shared asset memory

How several Hailstone clients on one computer share immutable asset bytes, what is shared, and what it costs.

- Setup: docs/layout.md "Shared archive memory".
- Code: `core/shared_file_cache.{h,cpp}` (the cache), `platform/shared_file.{h,cpp}` (mappings, locks, publication),
  `assets/wld.h` `TrackFrames` and `assets/character.h` `BoneFrames` (encoded animation keys).
- Tests: `tests/shared_cache_test.cpp` and neighbours; `hailstone_shared_cache_probe` is the helper process the
  cross-process tests start.

## What is shared

For `N` clients holding `S` identical asset bytes and `P` private bytes each, sharing turns `N * (P + S)` into
`N * P + S`. Only bytes that would otherwise sit on each client's heap gain:

| Data | Without the cache | With `assets.shared_memory=auto` |
|---|---|---|
| Game-folder archives (`.s3d`, `.eqg`, `.pfs`) | Mapped read-only (`PfsArchive::Storage::Mapped`): the page cache already shares them | Unchanged; the cache does not copy them |
| Server overlay archives | Read onto each client's heap (the server-files update may replace them) | Snapshot in the cache, mapped |
| Inflated character WLDs | Inflated per client; animation keys expanded to 32-byte `TrackFrame`s | Stored in the cache, mapped; keys stay encoded and are decoded when sampled |
| Zone WLDs, geometry, collision, textures, PCM, GPU memory | Per client | Per client |

With the cache, `TrackFrames` and `BoneFrames` keep a checked view into the mapped WLD and decode a key with
`ByteReader` when sampled (mutation makes an owned copy), so neither the decoded WLD nor the built model holds
private key arrays. The deferred WLD's 128 MiB decode budget is separate from the cache's disk budget; a view stays
valid after its decoded WLD is evicted.

Without a mapping (cache off or failed), `WldFile::parse(ImmutableFileBytes)` decodes private keys as usual, so the
fallback costs the same as no cache.

Independently of the cache, WLD fragments box their rare large payloads (meshes, particle clouds) and the name index
is a sorted vector of fragment indices, which makes every parsed WLD smaller.

## Cache design

- **Content addressed.** A file is `<sha256>.blob` in `cache/assets/v1/pfs`. Overlays with the same name but
  different content get different keys; one file reached by two names is one blob.
- **Written once.** A writer holds `publish.lock`, writes an exclusively created `.tmp-` file, fsyncs it, makes it
  read-only (0400 on POSIX) and publishes it with `link` (POSIX) or `MoveFileEx` without replace (Windows). An
  existing name is never replaced and a blob never rewritten, so a mapping of it cannot fault.
- **Verified once.**
  - A source is hashed once per cache instance while its `FileStamp` (size, device and inode, mtime and ctime from
    the open handle) is unchanged; one modified in the last three seconds is hashed on each load, as git treats its
    index.
  - A blob found on disk is hashed once per instance before mapping (`load`), or compared with the caller's bytes
    (`store`). A blob this instance wrote is trusted.
  - A blob that fails is never replaced (another client may map it); the load is private until a trim removes it.
- **Bounded wait.** A client that finds another publishing waits at most 250 ms, then loads privately.
- **Private by default.** The directory must be owned by the user and private (0700, or a protected DACL naming only
  the user); links, reparse points and non-regular files are refused.

### Eviction

There is no cross-process use lock. The budget counts blobs and temporaries. Whoever holds `publish.lock` may prune:

1. temporaries first (no writer can be active);
2. then blobs by last use (the blob's mtime, refreshed on each instance's first mapping), skipping those the pruning
   instance maps.

A client prunes on first use of the cache (so a lowered budget applies at start) and whenever an insertion would pass
the budget. Removing a blob another client maps is safe: on POSIX the mapping keeps the inode and the space returns
when the last reader lets go; on Windows `ReadOnlyMapping` shares only reading, so the removal fails and the blob is
skipped. A client needing a removed blob builds it again.

## Measurements

Linux (CachyOS), RelWithDebInfo, stock PEQ server, characters idle in North Qeynos, 1280x720; medians of three runs.
Zone-in is the log's `zone-in:` time; memory from `/proc/<pid>/status` (VmRSS) and `/proc/<pid>/smaps_rollup` (Pss,
summed over clients, so shared pages count once). Cold: empty cache; warm: the cache the cold run left.

| Clients | Mode | Zone-in | VmRSS (sum) | Pss (sum) | Pss anonymous (sum) | Cache on disk |
|---|---|---:|---:|---:|---:|---:|
| 1 | off | 1.31 s | 778 MiB | 748 MiB | 620 MiB | - |
| 1 | auto, cold | 1.43 s | 739 MiB | 714 MiB | 535 MiB | 152 MiB, 139 files |
| 1 | auto, warm | 1.38 s | 738 MiB | 709 MiB | 535 MiB | 152 MiB |
| 2 | off | 1.29 s | 1556 MiB | 1382 MiB | 1242 MiB | - |
| 2 | auto, cold | 1.39 s | 1479 MiB | 1256 MiB | 1075 MiB | 152 MiB |
| 2 | auto, warm | 1.44 s | 1484 MiB | 1231 MiB | 1077 MiB | 152 MiB |

With the cache on:

- Each client holds about 85 MiB less private memory (animation keys stay encoded).
- The mapped WLDs, about 45 MiB per client, are shared: two clients' summed Pss falls by 126-151 MiB (9-11%).
- Zone-in takes about 0.1 s longer: a character WLD is still inflated and hashed to find its blob. Differences under
  0.1 s are noise (the machine was busy).

The cache does not copy game-folder archives because doing so cost more than it saved: one client in West Karana took
1.73 s cold / 1.11 s warm to zone in (0.85 s off), held 761-763 MiB resident (628 MiB off) and filled 500 MB of
cache. On Windows with six clients, the original PR (which still copied them) reported 25.8% less private commit,
mostly from animation keys.

## Open

- Windows and macOS: unit and multi-process tests only, no live sessions with this design.
- Sustained play with zoning, more than two clients, and the fallback rate under contention.
- Static collision and character geometry are the next candidates: both are rebuilt per client and need flat,
  byte-backed representations before they can be mapped.
