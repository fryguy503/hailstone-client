# Shared GPU memory: feasibility and integration

GPU asset sharing needs a different design from the shared CPU file cache. Vulkan can export and import memory, but six independent Vulkan instances cannot simply sample one imported allocation at the same time. The specification requires exclusive ownership by one instance, including for resources created with concurrent sharing mode. A valid design must coordinate ownership or move rendering into one Vulkan instance. [Vulkan external resource sharing](https://docs.vulkan.org/spec/latest/chapters/resources.html#resources-sharing)

An experimental Windows external-texture broker now connects separate playable client processes. It shares
immutable zone and actor texture payloads while each client retains its own Vulkan instance/device, network
session, renderer and window. It is opt-in through a private test session, with no normal client setting. Controlled
six-client OFF/ON correctness and repeated native Windows observations now pass on the RX 9070 XT and RTX 3070;
the accepted scope and method limits are recorded below. The existing CPU memory work remains independent.

Three kinds of evidence are kept separate below: historical single-device probes, two-process procedural-image
handoffs, and the current six-process live-game validation. The historical six-view percentages do not describe
six logged-in clients. The current live comparison observes about 15% lower dedicated-memory accounting on the
tested adapters; it does not establish physical residency, sustained play or a throughput improvement.

## What can be shared

The current renderer provides a narrow asset-upload boundary in `src/render/gpu.cpp`:

| Resource | Upload boundary | Current experimental scope |
|---|---|---|
| Prepared zone and actor textures | Explicit `UploadBatch::immutable_texture` calls in `ZoneRenderer` and `ActorRenderer` | Shared when the final pixels, format and mip recipe match and external support/budget checks pass. Each process owns local image/view/import handles, samplers and descriptors. |
| Other textures, including mutable/UI uploads | Ordinary `UploadBatch::texture` | Private. Merely using the uploader does not opt a resource into sharing. |
| Zone vertices, indices and static light-grid buffers | `ZoneRenderer::upload` | Private; candidates when the final bytes and creation parameters match. |
| Object and actor vertices and indices | `ObjectRenderer::add_model` and `ActorRenderer::install` | Private; candidates by final uploaded content, including appearance-dependent vertices. |
| Bones, instances, per-frame lighting and particles | Host-written, frame-specific buffers | Private to each client. |
| Scene color/depth, AO, bloom, water copies, directional and point shadows | `VkContext::create_scene_targets`, `create_shadow_map`, `create_point_shadow_map` | Private: each client writes its own view and frame. |
| Swapchain, screenshot/readback, browser and changing UI images | Window or changing-content resources | Private in the initial scope. |

The private `UploadBatch::submit` copies into device-local memory and generates missing RGBA mips on the GPU;
eligible opaque colour textures use the private sRGB image/blit path to preserve linear-light filtering.
Texture uploads end in `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`. Its submission returns before GPU completion;
same-queue ordering makes this safe locally. Shared textures use the broker's separate upload path, which waits for
the upload and external ownership release to complete before publishing the exported allocation. Shared immutable
colour textures first finish their mip payloads on the CPU, so the broker's UNORM image does not generate colour
mips with gamma-space filtering. Data textures retain their ordinary generated-mip recipe.

There is already delayed local destruction: `VkContext::retire` waits for the relevant frames and uploads, while `retire_after_upload` frees staging after its upload fence. Zone destruction, appearance eviction and texture-slot reuse depend on this lifetime. A shared resource must retain its local memory import and broker lease through that same completion point.

## Historical CPU-sharing GPU baseline

The six-client native retest measured about 724 MiB of WDDM dedicated charged memory per client, with little change from CPU sharing. This is process accounting, not an allocation inventory or proof that all of it can be shared. The native host adapter counters are recorded separately in the CPU-sharing research; overlapping GPU counters are not added. See [Shared asset memory](shared-asset-memory.md).

Some private GPU storage can be estimated from existing CPU metadata without another live session. The recorded renderer configuration was a 1280×720 scene, 4× MSAA, 2048 directional shadows and eight 512-point-shadow slots. The source allocates six directional layers and 96 point-shadow layers (cached and live faces). Their nominal pixel storage is 96 MiB with 16-bit shadow depth or 192 MiB with 32-bit depth. Scene, depth, AO, water and bloom add about 66.5 MiB if scene depth occupies four bytes per pixel. These are calculations from dimensions and formats, not measured allocation sizes; they exclude tiling, alignment, swapchain images and driver storage. The selected shadow format must be recorded before choosing one end of that range.

The existing `VkContext::heap_use` uses VMA heap budgets and allocation totals. It does not classify bytes by owner. `ZoneRenderer::prepared_bytes` describes CPU payload and is also not a GPU allocation measure. Saved native process counters cannot retrospectively recover the missing texture/static-buffer breakdown.

Useful instrumentation would record, at allocation and delayed destruction:

- Category: immutable texture, static vertex/index/storage buffer, staging, dynamic buffer, render target or shadow.
- Logical payload size, VMA allocation size, memory type, dimensions, format, mip/sample/layer counts and live allocation count.
- Current and peak bytes, with retired-but-not-yet-freed objects included until actual release.

VMA allocation size is still not a physical-residency census. Imported allocations and broker-held allocations would need separate counters so their accounting is not accidentally summed as unique memory. No content, file paths, character names or device UUIDs are needed for these counters.

## Architecture choices

### Coordinated external ownership

Keep each client renderer and add a session broker plus an exclusive GPU ownership token. Only the token holder
may submit work that accesses the common asset set. Before passing it, that client releases every shared image
subresource to `VK_QUEUE_FAMILY_EXTERNAL` and establishes GPU completion. The next client acquires those
subresources before drawing. An exported semaphore can establish that completion; the current experiment instead
waits for the submitting client's frame fence before returning its host lease. The fence completion is essential:
a host message or mutex alone does not finish previous GPU accesses.
[Vulkan ownership transfers](https://docs.vulkan.org/spec/latest/chapters/synchronization.html#synchronization-queue-transfers)

This changes less renderer architecture, but serializes GPU work that touches common assets. Barrier count, frame pacing, aggregate throughput and minimized-client behavior are unmeasured. Six clients at 30 FPS require the scheduled work and handoffs to fit their frame deadlines. A per-resource token system could permit more overlap for disjoint assets, but adds scheduling and deadlock complexity.

Leaving an image in `GENERAL` does not remove the ownership requirement. Shader reads are not a portable guarantee that external image bits or metadata stay unchanged. `VK_EXT_external_memory_acquire_unmodified` is an optimization hint, not permission for concurrent independent-instance access. [Khronos explanation](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_external_memory_acquire_unmodified.html)

Opaque external handles also require compatible physical devices and drivers. [AGENTS.md](../../AGENTS.md) says,
"Never read or send a real hardware identifier." This work does not query device or driver UUIDs. The current
local experiment requires the requested generic model name to occur exactly once in each process's Vulkan
enumeration, and compares that name, API version, driver version and a hash of memory heap/type properties during
the handshake. Two identical-model adapters are rejected instead of guessed. Import also checks the exporter's
memory type and allocation size against the local image requirements, then uses those exporter values verbatim.
These are conservative guards for the tested local setup, not a guarantee that arbitrary adapter/driver pairs can
exchange opaque handles. The `maintenance9` ownership relaxation is limited to the same logical device and does
not solve independent-instance ownership.
[Handle compatibility](https://docs.vulkan.org/refpages/latest/refpages/source/VkExternalMemoryHandleTypeFlagBits.html),
[Sharing modes](https://docs.vulkan.org/refpages/latest/refpages/source/VkSharingMode.html)

The design is vendor-neutral, not a promise that every AMD, NVIDIA or Intel driver supports every resource tuple. Sharing is limited to the **same compatible adapter**. It does not share one allocation between AMD and NVIDIA cards, between discrete and integrated GPUs, or between two different cards of the same model. A driver change invalidates the broker's allocation generation and compatibility state. Handles and GPU allocations are never a persistent disk cache.

| Vendor | Native evidence so far | Remaining validation |
|---|---|---|
| AMD | RX 9070 XT passes the historical probes, validated two-process image proof and six live OFF/ON clients. Repeated native GPU accounting passes descriptive gates. Integrated AMD has capability/alias evidence only. | Rendering performance, sustained play and broader lifecycle/failure coverage remain open; the first shader-hash record was during the first OFF run. |
| NVIDIA | RTX 3070 passes the historical probes, validated two-process image proof, six live OFF/ON clients and repeated native GPU-accounting gates. | Rendering performance and sustained lifecycle/failure acceptance remain open; Intel and other drivers are not covered. |
| Intel | Native GPU support has not been tested. | An actual Intel adapter/driver is required; vendor-neutral policy tests do not supply hardware evidence. |

Capability decisions must use each selected adapter's actual handle, buffer and image-format results, rather than a vendor allowlist. Unsupported BC formats, missing import/export bits, unsupported semaphore handles or an incompatible image usage/tiling combination keep that resource private. Policy tests should exercise those negative combinations independently of vendor identity.

### One rendering broker

Move GPU rendering into one process with one `VkInstance` and `VkDevice`. Client processes send scene/render-state commands to it. A device-owned content cache then supplies one immutable image or buffer to multiple client views, while their frame targets, descriptors and simulation remain separate.

This is the larger architectural change: input/window presentation, command transport, render-state lifetime, crash handling and frame scheduling need a defined boundary. It avoids cross-process opaque GPU imports and can share immutable assets within the ordinary one-device synchronization model. It is the stronger long-term route when parallel client rendering is required. Moving only asset creation into a broker while leaving rendering in independent Vulkan instances does not remove external ownership constraints.

## Allocation and publication rules

The current coordinated path covers explicit immutable textures, with bounded dedicated allocations and private
fallback before an unsafe state exists. Static buffers remain private. The design does not make every VMA
allocation exportable.

1. Key a texture by a versioned hash of its final upload payload and recipe: format, extent, mip count, layers, usage, generated-mip policy and content-transform version. A filename is insufficient: color-key removal, opaque-alpha conversion, replacement packs, mip limits and BC support affect the result. Key buffers by final bytes, logical size and usage. Publish immutable generations rather than overwriting memory in use.
2. Query external support for the exact image/buffer creation tuple and intended handle type. Respect import/export and dedicated-only requirements. Create the resource with its external-memory creation structure; existing images cannot be made exportable afterward. [External image creation](https://docs.vulkan.org/refpages/latest/refpages/source/VkExternalMemoryImageCreateInfo.html)
3. Use dedicated allocations initially to avoid exporting a VMA block containing unrelated allocations. Account for allocation-count limits and per-allocation overhead; thousands of small textures are unsuitable for a naive dedicated-allocation policy.
4. Upload once, finish mip generation and release external ownership. Publish only after the GPU release is complete. The broker retains a strong payload reference plus validated creation metadata and an epoch. Readers create local resource handles and views, import compatible memory, then acquire ownership through the token protocol. They must not run the ordinary fresh-upload `UNDEFINED` transition against published content.
5. Keep immutable images in an agreed supported layout between handoffs, with matching release/acquire layout parameters. Current descriptors expect `SHADER_READ_ONLY_OPTIMAL`; using `GENERAL` would require descriptor and barrier changes throughout the texture consumers and still would not allow fan-out.
6. Treat loss of the lease holder, malformed session state or a failed import after publication as a fatal session
   failure. A timeout must never grant ownership over possibly running GPU work. The experiment stops affected
   clients rather than claiming automatic recovery. Rebuilding a new generation or safely recreating private
   resources remains future work; never steal the old generation's token or reuse uncertain storage.

VMA is pinned to **3.3.0**, commit `1d8f600fd424278486eade7ed3e877c99f0846b1`, in `cmake/Dependencies.cmake`. That version supports export chains through custom pools, dedicated allocations and `vmaGetMemoryWin32Handle`; the returned duplicated NT handle belongs to the caller. Newer online examples use additional APIs that this pin does not provide. The current broker and consumer use explicit `vkAllocateMemory` and separate owners for external allocations, independent of VMA pool reuse. `Texture::destroy` releases an imported texture's shared owner rather than passing its image to `vmaDestroyImage`; private textures retain their existing VMA path. Both kinds wait for GPU retirement. [Pinned VMA source and documentation](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/blob/1d8f600fd424278486eade7ed3e877c99f0846b1/include/vk_mem_alloc.h)

## Native handle transport and fallback

| Platform | Transport status | Lifetime rule / current behavior |
|---|---|---|
| Windows | Implemented current-user-DACL named pipe, local peers only, random session token and OS-verified peer PID; duplicate `OPAQUE_WIN32` NT handles into that process | Import does not consume the NT handle; RAII closes the duplicate after import. NT payload references avoid the weak lifetime of KMT handles. Handle numbers are not persisted. |
| Linux | Not implemented; private allocations. A future route needs a user-private Unix-domain socket, `SCM_RIGHTS` and verified peer credentials | Successful Vulkan FD import consumes that descriptor. A future implementation must close retained descriptors on failed import and shutdown. |
| macOS / MoltenVK | Private allocation fallback in this scope | Win32 and opaque-FD transports do not establish a Metal/IOSurface sharing path. Any native Metal route needs its own capability, layout, ownership and lifetime proof. |

Absent/unsupported transport or extension, initial connection failure and an explicit compatibility rejection use
the existing private uploader. A valid per-texture unsupported-capability/budget response also keeps that texture
private. Once session state or an import becomes uncertain, the client fails closed; it does not continue rendering
with stale handles or silently recover privately. Native IPC operations live in `src/platform/gpu_ipc.*`, with
versioned metadata encoded using the project's byte I/O helpers. The broker receives final asset payloads and
generic rendering metadata, never login credentials or game-server state. Endpoint/token values are private child
environment configuration and must not appear in command lines, logs or committed reports.
[Win32 import lifetime](https://docs.vulkan.org/refpages/latest/refpages/source/VkImportMemoryWin32HandleInfoKHR.html), [FD import lifetime](https://docs.vulkan.org/refpages/latest/refpages/source/VkImportMemoryFdInfoKHR.html)

## Historical capability and buffer-alias groundwork

The portable capability groundwork lives in `render/gpu_share_capabilities.{h,cpp}`, `tools/gpu_share_probe.cpp` and `tests/gpu_share_capabilities_test.cpp`. It queries support without enabling sharing or querying UUIDs. Local Windows capability observations report opaque-memory buffer import/export, binary-semaphore import/export and dedicated-only sampled/transfer image support for the tested RGBA8 and BC formats. Capability support is not a client-sharing benchmark.

The tool has no vendor whitelist. A successful capability query is a statement about the tested adapter, driver and
resource tuple, not every product from that vendor. Native Windows observations on 2026-11-13 were:

| Adapter | Vulkan API | Buffer and binary-semaphore import/export | RGBA8 / BC1 / BC3 / BC7 images | Image dedicated allocation required | Same-device 24-view readback |
|---|---|---|---|---|---|
| AMD Radeon RX 9070 XT | 1.4.359 | Supported | All four import/export | Yes | Passed |
| NVIDIA GeForce RTX 3070 | 1.4.357 | Supported | All four import/export | No | Passed |
| AMD Radeon integrated graphics | 1.4.315 | Supported | All four import/export | Yes | Passed |
| Intel | Not tested | Unknown | Unknown | Unknown | Not tested |

Each smoke test filled one alias and verified all 393,216 words copied through 24 buffer handles bound to the same
65,536-byte device-local allocation. Copies used a separate readback allocation. Global memory dependencies cover
the aliases; a completed fence and, where necessary, cache invalidation precede host verification. This is a
single-logical-device primitive suitable for investigating a broker. It does **not** prove cross-process import,
simultaneous independent client access, texture sampling or reduced six-client VRAM use.

At this initial stage the installed Windows Vulkan runtime had no Khronos validation layer. Plain runs passed checksum verification;
they are not validation-layer-clean acceptance. `--validation` fails clearly when the layer is unavailable instead
of treating zero uncollected messages as success. Its debug callback counts warnings/errors and emits only static
validation message identifiers, without device/object details. The normal fence wait is five seconds; safe cleanup
of submitted work may still wait longer if the driver stalls.

The portable module, headless tool and three focused tests built with Linux GCC using ASan/UBSan; all 38 assertions
passed without sanitizer diagnostics. That is compiler/unit coverage, not a native Linux GPU or Intel test. Windows
all-target MSVC builds also passed. The full Windows base suite passed 2,530 cases / 191,824 assertions, with 59
explicit skips and recorded/asset fixture limits described in the CPU validation notes. An initial harness run
created an empty-but-present `EQ_LIVE_DIR`, causing two live-fixture setup exceptions; the successful rerun removed
fixture variables from the child environment entirely, without source changes. Both logs are preserved. Eight
CLI help/rejection checks and explicit missing-validation-layer rejection passed. AMD and NVIDIA also passed
minimum (one view/four bytes) and maximum (64 views/one MiB per view) smoke bounds; each maximum checked
16,777,216 words.

Usage (no assets or server needed):

```sh
hailstone_gpu_share_probe
hailstone_gpu_share_probe --smoke --device-index 0
hailstone_gpu_share_probe --smoke --device-index 0 --validation
```

Adapter indices are runtime ordinals; read the enumeration before selecting one. The probe enumerates every adapter,
reports each tested format separately, and never queries device UUIDs or exports/imports external handles. Smoke
bounds are 1-64 views and 4-1,048,576 bytes per view in multiples of four; actual device/readback allocations are
also limited. Unknown arguments and invalid bounds are rejected before GPU initialization.

Ignored Windows evidence is under `build-msvc/gpu-start-validation/`, including per-adapter JSONL and
`native-smoke-summary.json`. Linux compiler/unit logs use `build-linux-logs/gpu-share-*`. The old CPU-test executable
and its exact measured hash were preserved in `build-msvc/final-cpu-validation-20261113/` before rebuilding.

The implementation described later adds image imports and production texture bindings. These historical buffer
results alone established none of that. Acceptance still requires production lifecycle/failure coverage, native
host GPU accounting and rendering-cost comparisons. A single successful import or checksum is insufficient;
there is no normal client GPU-sharing switch.

## Historical real-asset, single-device rendering prototype, 2026-11-13

`hailstone_gpu_asset_probe` implements the first rendering-broker primitive: one process and Vulkan device render
two or six offscreen views of actual static zone assets. It does not log in to a server, accept commands from game
clients or share allocations between independent client processes. This prototype did not change the playable
client renderer. The later separate-process integration is a different execution path.

The scene factory in `assets/gpu_probe_scene.*` prepares terrain and transformed placed objects. It uses the current
zone renderer's base-texture preparation and retains native formats/mips when preparation needs no conversion.
Unsupported BC sampling falls back per device. Geometry is a diagnostic position/UV/color layout with placements
flattened into world space; its byte totals are probe payloads, not the client's existing vertex allocations.
Auxiliary material textures, character assets, sky, lighting, vertex-color shading, blend effects and gameplay are outside
this rendering workload. Base animation frames are retained, with the first frame sampled. Omitted effects and
missing resources are counted explicitly. The measured GFay factory output contains 225,203 vertices, 553,689
indices, 9,001 draw batches and 159 texture slots (156 unique contents). All prepared textures in this scene are
RGBA8. These real-asset runs therefore do not establish native compressed-format sampling or fallback acceptance.

Sharing OFF creates a separate immutable geometry/texture set for every view. ON reuses content-identical immutable
images and buffers. Both modes retain separate color/depth targets, readback buffers and descriptors for every view,
with the same shaders, cameras and draw calls. Upload staging is freed only after GPU completion and is zero at
readiness. The owning renderer waits for submitted work before resource destruction. No external handles or device
UUIDs are used.

Every view has a different camera. GFay contains geometry extending far below the playable surface, so the camera
recipe uses indexed XY extents and median drawn-vertex height rather than the full vertical bounding-box midpoint.
All geometry emitted by this factory remains uploaded. Material alpha cutoffs are applied per draw; opaque textures do not discard their
authored zero-alpha pixels. The simplified opaque/cutout shader does not establish original-client visual parity.

Correctness runs compare complete RGBA readbacks, view by view, between OFF and ON. They reject blank views, less
than 5% coverage and identical first/second cameras. Final AMD RX 9070 XT and NVIDIA RTX 3070 two- and six-view runs
matched exactly within each adapter, with roughly 26% scene coverage. Cross-vendor pixel equality is not required.
Visual inspection of private local PNGs confirmed the intended forest views. Reports bind source inventory, camera
matrices, dimensions and both SPIR-V payloads by semantic hashes, and pin the selected adapter's generic model/API.

An official Khronos nightly validation package was downloaded into the ignored build directory, verified against
its published SHA-256 and kept isolated from global driver settings. Its source commit is
`37ec2baf9aac8c0f55fe2b9beec1d818b70d7841`; archive SHA-256 is
`cfcb5fe29f248c295256366dfbf399511af7d8b5f03b14dd7c9563840d9cd39a`. With core and synchronization validation enabled,
all four native comparisons emitted zero API errors and zero validation-rule messages. Each emitted one general
loader message about an existing duplicate AMD switchable-graphics layer registration. No driver registry changes
were made. This supersedes the earlier stage's lack of an available validation-layer run.

The tool can hold completed GPU resources while rendering continuously at a 30 FPS cap, then stop on its finite
deadline or stdin EOF. Both lifecycle paths passed native checks. Compare-only PNG capture requires a new private
directory outside canonical asset/texture roots and publishes files without replacement. Captures and game-derived
payloads are never committed.

```sh
hailstone_gpu_asset_probe --game-dir <game-folder> --zone gfaydark --views 2 --device-index 0 --compare --validation
hailstone_gpu_asset_probe --game-dir <game-folder> --zone gfaydark --views 6 --device-index 0 --sharing on --hold-seconds 60
```

The Windows all-target build passed. Its full base suite passed 2,538 cases / 191,889 assertions with 60 explicit
fixture skips; additional unset-vector cases retain the limitations described earlier. The focused stock-asset
subset passed 12 cases / 592,025 assertions, including repeated real GFay factory construction and complete index,
draw, mip, digest and inventory checks. Native physical GPU-memory evidence is collected separately from these
allocation and pixel checks; compiler or harness smoke runs cannot establish memory savings.

The Linux GCC 13.3 RelWithDebInfo build with AddressSanitizer and UndefinedBehaviorSanitizer also passed all targets.
Its full suite passed 2,544 cases / 192,317 assertions with 60 explicit fixture skips; the focused GPU suite passed
11 cases / 102 assertions, and the real-stock scene-factory subset passed 5 cases / 591,954 assertions. No Linux GPU
rendering or presentation acceptance is implied by those CPU-side sanitizer checks.

### Native GPU-memory experiment protocol

Use the same pinned executable, shaders, source inventory and per-view cameras for every run. Each adapter runs
two views and six views in OFF/ON/ON/OFF order, with at least 10 seconds settling and six native samples at least
five seconds apart. Primary measurements disable validation layers; the separate correctness runs above retain
core and synchronization validation. Upload staging must be gone, rendering must keep completing GPU work, and
each owned process must exit normally after complete device/instance teardown.

Unrelated brief Windows compiler activity prevented a sustained quiet window. Interrupted strict-quiet attempts
are retained as excluded evidence. The replacement protocol is GPU-memory-only: record compiler activity without
waiting for it or filtering samples. CPU memory and fenced wall timing remain diagnostic; they do not establish
CPU performance, frame-time or FPS acceptance.

Before interpreting a result as conclusive, require available physical RAM and commit headroom at every native
before/loaded/after snapshot to each be at least the larger of 2 GiB or 10% of its total/limit. Require each selected
GPU counter's before-run triplet range to stay within the larger of 16 MiB or 2% of its median. Keep every raw
observation and report failed interpretation gates. Post-exit recovery, across-run baseline movement and both
reversed-pair differences remain visible; conflicting pair signs or baseline variation as large as the effect
make attribution inconclusive. No absence of GPU memory pressure is inferred without GPU-budget telemetry.

The primary source is Windows `GPULocalAdapterMemory.LocalUsage`, plotted as loaded minus before-run baseline.
`GPUAdapterMemory.DedicatedUsage` is an independent accounting source and is reported separately. Never add them.
Adapter association uses an exact generic-model match to DXGI's boot-local adapter mapping, then matches that
adapter to the recorded Windows counter rows. This replaces charge-size dominance, which was ambiguous for the
NVIDIA process's incidental contexts. Session identifiers remain private; reports expose model/API and aliases.
[DXGI adapter descriptions](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/ns-dxgi-dxgi_adapter_desc1)
provide the name and local adapter identifier. Per-PID GPU charges remain diagnostics rather than unique VRAM.
Allocation inventory bytes are also separate from native residency. Six serial observations in one run are not
six independent trials; graph ranges describe the two run medians for each mode.

### Historical single-device native Windows observations, 2026-11-13

Both adapters completed eight valid runs: 16 runs, 96 process-memory samples and 192 native host snapshots in all.
Every snapshot passed the RAM/commit-headroom thresholds. Exact DXGI model/session association matched both
captures and every counter phase; AMD's mapping also agreed with the original charge-based association.

These are six offscreen views in one process/device, not six logged-in client processes. Results for the separate
`GPUAdapterMemory.DedicatedUsage` accounting source are below. Values are
MiB added above each run's before baseline, summarized by the median of the two runs for each mode.

| Tested GPU | Sharing OFF | Sharing ON | Lower accounting | Reduction |
|---|---:|---:|---:|---:|
| AMD Radeon RX 9070 XT | 531.37 | 102.01 | 429.36 | 80.8% |
| NVIDIA GeForce RTX 3070 | 392.90 | 83.10 | 309.80 | 78.8% |

Both orders and before/after-baseline sensitivity checks support lower dedicated-memory accounting on both GPUs.
These are observations of this static-asset prototype, not whole-client percentages or proof that every accounted
byte was physically resident. Allocation inventory and native accounting include different resource/driver costs
and must not be equated.

The predeclared primary `GPULocalAdapterMemory.LocalUsage` source fell by 304.53 MiB for NVIDIA's six views and
passed the direction, headroom and baseline interpretation checks. AMD's primary-source attribution is
inconclusive: the two-view after-baseline comparison reversed direction, and the six-view ON increment was
negative despite a populated scene. That is evidence of unrelated host-counter movement, so its apparent
624.82 MiB reduction must not be presented as asset residency saved. Negative increments also invalidate
attribution even when a range threshold alone passes. Keep this uncertainty alongside the accounting result.

The frozen measured executable has SHA-256
`854a9894e9beb801b5014557ac616485b2b96da2f26c96b8f8bd0877aefee249`.
The full local report retains both sources, raw before/loaded/after medians and ranges, reversed pairs, sampling
times, compiler observations and interpretation flags. Only sanitized numeric reports and graphs are shareable;
session mappings, process identifiers, asset paths and game-derived captures remain private.

## Experimental separate-process production texture path, 2026-11-13

`render/gpu_shared_cache.*`, `platform/gpu_ipc.*` and `hailstone_gpu_cache_broker` implement the coordinated
external-ownership option. Six game processes connect independently to their ordinary development server; the
asset broker is an additional process, not a replacement game client or a renderer for their windows. Each game
client runs the actual zone and actor renderers. No per-view command stream or gameplay simulation moves into the
broker.

### Upload, frame ownership and retirement

1. Explicit zone/actor calls enter `UploadBatch::immutable_texture` after the normal texture preparation,
   color/alpha conversion and per-device BC fallback. `prepare_immutable_texture_image` finalizes colour mip chains
   on the CPU before keying, rather than leaving the private path's sRGB GPU blit to the external UNORM image.
   The versioned content key includes final mip payloads,
   dimensions, format and generated-mip policy; the remaining image creation tuple is fixed by this protocol.
   A filename or original decoded image is not the key. Within a client, repeated keys reuse a canonical texture
   owner; this local reuse is distinct from sharing the exported allocation across processes.
2. On the first broker miss, the broker creates a dedicated exportable image/allocation, uploads the final bytes,
   generates requested mips and releases to `VK_QUEUE_FAMILY_EXTERNAL` in `SHADER_READ_ONLY_OPTIMAL`. It waits for
   its upload fence before publishing. Hits reuse that same immutable allocation. Consumers create their own
   image/view and raw memory import with matching metadata, so their Vulkan handles differ while the underlying
   exported payload is shared.
3. Each client finishes CPU frame recording, then snapshots every live imported texture owner, including imports
   added while recording a newly loaded zone or appearance. It acquires the global frame lease only at submission.
   One submit executes acquire barriers, the normal frame commands and external release barriers in that order.
   The client waits for **that submission's frame fence** before explicitly releasing its lease. CPU recording
   remains independent, but GPU work using shared assets is serialized across clients. No simultaneous
   independent-instance sampling is assumed.
4. The snapshot keeps imports alive through GPU completion. Existing frame/upload retirement retains textures
   removed by zoning or appearance eviction. Imported ownership destroys the local view/image/raw memory exactly
   once, then sends its retirement reference to the broker. The broker drops an allocation when its last consumer
   reference retires. An unfinished lease destructor closes the connection rather than falsely releasing ownership;
   loss of the owner poisons the epoch. Reconnect and automatic private-resource reconstruction are not implemented.

The current bounds are 128 MiB of input mip payload per texture, 2,048 broker entries and a maximum/default 2 GiB
broker allocation budget. Limit or unsupported-format responses fall back per texture. Dedicated allocations can
be costly for small textures, and the cache does not yet share static geometry. IPC has a hard maximum of 16
connection workers, including control connections: **24-client scale is neither supported by this transport limit
nor demonstrated by six-client behavior**. Raising it also requires queue scheduling, headroom and rendering-cost
validation.

The private test environment uses `HAILSTONE_GPU_IPC_ENDPOINT`, `HAILSTONE_GPU_IPC_TOKEN` and optional
`HAILSTONE_GPU_IPC_SERVER_PID`; a session supervisor supplies their values without logging them. `HAILSTONE_GPU_NAME`
selects one exact, unambiguous generic model. `HAILSTONE_GPU_BENCHMARK=1` emits successful present-submission counts,
shared-frame counts, current imported-image snapshot size and lease/fence wait totals in both OFF and ON runs.
These markers establish continued application activity and coverage; they are not display FPS, GPU timestamps or
native memory counters. Import/allocation-byte counters are cumulative, not live residency. Broker live entries
and allocation bytes are separate inventory counters and must not be added to per-process import totals as unique
physical VRAM.

### Two-process procedural proof

The RX 9070 XT and RTX 3070 each passed a separate broker with **two independent consumer processes**, using the
same production image-import, ownership and retirement implementation. Each consumer completed 2,048 actual GPU
readback iterations: 4,096 readbacks per adapter. Both consumers imported/reused the allocation with no private
fallback; their readbacks matched the expected generated-pixel checksum. Overlapping registered consumers and
interleaved exclusive lease serials prevented a merely sequential two-process test from counting as acceptance.
Each adapter finished with 4,096 matched grants/releases, no owner, no remaining references/images and zero core
or synchronization-validation errors. Stop acknowledgement and normal teardown were also checked.

This proves exported image payloads can reach two separate processes on those tested adapters with the enforced
lease model. It does not establish six-client gameplay correctness, native GPU-memory savings, Intel support,
compressed real-asset coverage or long-running crash/zoning behavior. It also does not imply AMD and NVIDIA can
share one allocation with each other.

### Integrated build and unit validation

The final Windows MSVC suite passed 2,755 cases / 365,710 assertions; the Linux ASan/UBSan suite passed 2,761 cases /
366,124 assertions. Each reported 62 explicit optional-fixture skips. Focused cache/WLD/animation/texture checks
passed 122 cases / 6,722 assertions on Windows. Selected stock-asset checks passed 31 cases / 15,519,259 assertions
on each platform with the game folder read-only. Core-name, wiki and diff checks also passed.
These are compiler, sanitizer and offline regression results; they do not replace the native gameplay and memory
gates below or remove fixture-related coverage limits.

### Six live clients on AMD/NVIDIA: OFF/ON correctness

Six separate production clients have reached the same development zone and recorded actual shared-texture frame
activity. The first such attempt exposed two existing ordinary-frame synchronization hazards, so it was excluded
from acceptance and no native memory samples were accepted. Depth resolve writes require
`COLOR_ATTACHMENT_OUTPUT`/`COLOR_ATTACHMENT_WRITE`, and reads from the multisample depth source also require
`COLOR_ATTACHMENT_READ`; depth-test access alone is insufficient. The corrected typed policy now covers initial
layouts, water copy/restore, resumed rendering and AO/post sampling for both single-sample and MSAA depth. The
presentation semaphore now signals at `ALL_COMMANDS` in both OFF and ON, covering the final layout transition.
Pure 1x/4x/all-MSAA policy regressions accompany these production fixes.
[Fixed-function resolves](https://docs.vulkan.org/spec/latest/chapters/renderpass.html),
[Presentation synchronization example](https://docs.vulkan.org/guide/latest/synchronization_examples.html)

The corrected RX 9070 XT and RTX 3070 both passed this controlled six-process idle-zone correctness gate in
**OFF and ON**. Every client was in the pinned development world and `gfaydark`, with advancing present-submission
progress after zone entry. ON shared all 159/159 current-zone texture bindings in every client, with advancing
shared-frame counts and nonzero current imported-image snapshots. OFF recorded zero imports, shared frames and
current shared images. There were zero Vulkan errors under core and synchronization validation. Each cohort's six
clients exited normally with code 0, without forced cleanup.

| Adapter | Accepted live correctness | Current-zone bindings in each ON client | ON broker grants/releases | Final broker state |
|---|---|---|---:|---|
| AMD Radeon RX 9070 XT | Six separate clients, OFF and ON | 159/159 shared | 28,246 / 28,246 | Clean exit 0; no owner/consumers/entries/allocation bytes |
| NVIDIA GeForce RTX 3070 | Six separate clients, OFF and ON | 159/159 shared | 7,521 / 7,521 | Clean exit 0; no owner/consumers/entries/allocation bytes |

Both ON brokers reported zero validation errors and no poisoned epoch. The final playable executable's SHA-256
is `EEA9430E0A8917C7E864FEF40C54588F047B798706BAEAA0863E9A0B168350F0` across these cohorts. The closed local ledgers
and logs preserve individual readiness, progress, sample validity and normal teardown; private account/session
details are not part of this report.

Each of the four validation-enabled cohorts produced three valid native diagnostic samples. All twelve are
excluded from the primary memory comparison because validation was enabled. These correctness runs establish
production texture use and normal six-process retirement on the tested AMD/NVIDIA setups; the separate primary
native series below supplies the memory observations. Neither establishes sustained combat/zoning, failure
recovery, Intel support or cross-adapter allocation sharing.

The primary native series required all six clients to remain logged in and rendering the same world
and zone, with current shared bindings/import snapshots in ON and comparable private workloads in OFF. Run both
the ordinary private path and shared path under core/synchronization validation first. A world disconnect, stale
progress marker, zero zone coverage or any Vulkan error excludes the cohort rather than being hidden by a
successful login or cumulative import count.

The separate same-binary OFF/ON/ON/OFF native OS series held CPU asset sharing, cameras, 1280x720
windows, 4x MSAA, visible/non-minimized state and the 30 FPS cap constant. Process diagnostics include the broker
and each client; baseline-subtracted native adapter counters supply aggregate GPU accounting, with different
counter sources kept separate. Application logs establish readiness and coverage, while memory measurements come
from native OS tools. Present-submission progress and lease/fence waits remain diagnostics rather than displayed
FPS or GPU execution measurements.

### Primary live native Windows observations, 2026-11-13

Each adapter completed OFF/ON/ON/OFF with validation disabled: eight closed cohorts and 288 valid loaded native
observations in total. Every cohort retained all 36 declared observations. The two OFF run medians and two ON run
medians are summarized separately, after subtracting each run's own before baseline. ON includes the shared-texture
broker's full native cost; no per-PID GPU charges or logical allocation totals are added together as unique VRAM.

| Adapter | DedicatedUsage OFF, MiB above baseline | DedicatedUsage ON, MiB above baseline | Lower accounting, MiB | Reduction |
|---|---:|---:|---:|---:|
| AMD Radeon RX 9070 XT | 4,106.99 | 3,486.93 | 620.06 | 15.10% |
| NVIDIA GeForce RTX 3070 | 4,138.82 | 3,488.79 | 650.04 | 15.71% |

The predeclared `GPULocalAdapterMemory.LocalUsage` source is retained separately. AMD changed from 4,048.63 to
3,463.99 MiB above baseline, a 584.64 MiB / 14.44% reduction. NVIDIA reported the same 4,138.82 to 3,488.79 MiB
values as its DedicatedUsage source, a 650.04 MiB / 15.71% reduction. These sources are never added. Both sources
on both adapters passed the descriptive headroom, baseline-stability, reversed-pair direction and baseline-sensitivity
gates; observed variation was smaller than the mode difference. Two runs per mode are descriptive observations,
not a confidence interval or a causal/physical-residency estimate. No GPU-budget telemetry was available to prove
absence of memory pressure.

All 48 client exits across the eight primary cohorts were normal with code 0. The four ON brokers shut down cleanly
with balanced leases and zero consumers/resources, and temporary credential aliases were removed. One earlier
NVIDIA attempt failed six-client readiness after one world session closed; it captured zero native observations
and was explicitly excluded. The complete NVIDIA ABBA series was restarted, without inserting partial failed-run
data into the accepted groups.

The nominal plan was 120 seconds of warmup, 180 seconds of capture and a minimum five-second interval. In practice,
36 native observations plus acquisition/loop overhead took about 215 seconds per capture; first-to-last recorded
sample timestamps span approximately 208–212 seconds. The full observed window is retained, rather than selecting
a favorable nominal 180-second subsection. The NVIDIA restart declared and recorded an eight-second client launch
interval. AMD used the older controller's two-second default, but no per-series launch interval was recorded in
its immutable plan; the public report therefore leaves that field unrecorded instead of inventing a declaration.

Shader provenance also differs. NVIDIA has a prelaunch manifest of all 49 shader files, matching the later checked
files. AMD's first 49-file hash manifest was recorded **during the first OFF capture**, and matches the later
checked inventory. It cannot establish hashes before that first launch. This limitation is recorded in the report
and simple graph; it is not silently replaced by the later manifest or treated as an invented prelaunch record.

The simplified graph uses only native `GPUAdapterMemory.DedicatedUsage`, in GiB above each run's baseline, with
the broker included in ON. The detailed report keeps both sources, all before/loaded/after observations, reversed
pairs, host headroom and native CPU diagnostics. Working artifacts remain under ignored
`build-msvc/live-gpu-primary-final-report-20261113/`. A sanitized **local export** is available in the
`hailstone-gpu-live-proof` artifact leaf: ten report/CSV/JSON/SVG/PNG/README artifacts plus a SHA-256 manifest.
The final simple PNG was visually reviewed. This is a local export, with no remote or website publication.
Historical single-device reductions near 80% describe the earlier stripped-down static prototype and are not mixed
with these roughly 15% live-client observations.

### Native CPU-memory tradeoff and rendering limits

The native CPU-memory tradeoff interpretation is complete for this primary series. CPU-sharing mode stayed `auto`
in both GPU modes. The following separate native process diagnostics include the broker in ON. Values are ON
minus OFF, summarized from the same primary series; negative means lower accounting.

| Adapter | Private commit change, MiB | Private working set + unique CPU asset-cache resident subset change, MiB |
|---|---:|---:|
| AMD Radeon RX 9070 XT | -659.25 | +64.94 |
| NVIDIA GeForce RTX 3070 | -710.53 | -246.50 |

Private commit is a charge, while the resident subset includes process-private working sets and deduplicated
CPU asset-cache pages. Other shared resident mappings are outside that subset, so neither number establishes
whole-machine physical RAM savings. In particular, AMD's measured resident subset increased despite lower private
commit. Do not add commit, GPU dedicated accounting or `SharedUsage` to infer a combined physical-memory saving.

Performance equivalence is unproven. The global GPU lease serializes work that accesses the shared textures.
NVIDIA's sparse application present-submission intervals had median rates of 16.86 per second in OFF and 15.48
in ON. These are application progress diagnostics, not native OS/display FPS, GPU execution time or per-frame
latency. A configured 30 FPS cap is not evidence that either mode maintained 30 FPS. Native frame-time/throughput
tests are still needed to judge the rendering cost alongside the memory reduction.

Zoning, appearance changes, reload/eviction, descriptor reuse, final-consumer retirement, producer/consumer loss,
device loss and sustained play still need broader coverage. Native Intel hardware, Linux/macOS transports and
presentation, and 24-client scheduling remain separate gates. Keep the private renderer as the default while these
compatibility, lifecycle and rendering-cost gates remain open.

### Next gains: source estimates, not measured savings

The current path shares immutable zone/actor textures. It leaves render targets, geometry buffers, other
renderers' textures, mapped per-frame buffers, swapchains and driver resources private. Whole-client savings
therefore cannot be inferred from the percentage of texture bindings imported.

For a **full 1280x720 scene at render scale 1, 4x MSAA and four-byte scene/shadow depth**, the following are nominal
pixel-storage calculations. Actual view size, selected formats, tiling and allocation requirements can change
them. These are **theoretical inventories and opportunities, not native GPU-memory measurements**.
The closed AMD exploratory log confirms those dimensions, MSAA and shadow settings; depth formats were not logged.

| Private category | Nominal storage per client | Lifetime and sharing limit |
|---|---:|---|
| Scene MSAA color/depth, resolves, water copies, AO and six bloom levels | 66.503 MiB | Contents belong to one frame; no temporal-history image is retained here. |
| Four 2048x2048 sun-shadow cascades | 64 MiB | Redrawn each frame using sun shadows; camera-specific contents. |
| Two 2048x2048 cover/water layers | 32 MiB | Cached between frames; current client state deliberately skips unchanged maps. |
| Eight 512x512 point-shadow slots, six faces and two sets | 96 MiB | Mixes cached static-light cubes and current-frame character/dynamic-light cubes. |
| Swapchain, if four-byte color | 3.516 MiB per image | Actual image count is surface-dependent; each window presents independently. |

The first row follows `VkContext::create_scene_targets`: 1280x720 times 73 bytes, plus 2,456,800 bytes for bloom.
`create_target`, `create_shadow_map` and `create_point_shadow_map` force dedicated allocations. These images
are not unused space in a general VMA block. Shadow depth can instead be two bytes per texel, halving the three
shadow rows. Render targets already serve both frame slots within each client; do not multiply them by two.
See [scene resources](../../src/render/vk_context.cpp), [shadow layers](../../src/render/shadows.h) and
[point-shadow caching](../../src/render/point_shadow_pass.cpp).

The existing whole-frame exclusive lease makes a **shared transient scene-target pool** a plausible next step:
each client can overwrite the same compatible targets during its own turn, then finish its GPU work before
another client uses them. Presentation continues from each window's private swapchain. Replacing six compatible
66.503 MiB sets with one has a **theoretical 332.516 MiB reduction** before allocation/driver overhead. This is
not implemented or demonstrated. It requires a new writable-resource protocol/API, matching extent/format/usage/
sample-count recipes, depth/attachment access barriers, correct external ownership/layout transitions and lease
participation even in a frame with no shared immutable textures. Existing sampled-color export support does not
establish depth or MSAA export support: query each exact tuple and its returned sample counts, with private
fallback for unsupported resources. See [external image properties](https://docs.vulkan.org/refpages/latest/refpages/source/VkExternalImageFormatProperties.html)
and [image-format limits](https://docs.vulkan.org/refpages/latest/refpages/source/VkImageFormatProperties.html).

Cascades could provide a further theoretical 320 MiB reduction across six at the stated four-byte profile, but
they share an array with persistent cover/water layers. Those layers must remain private, be split into separate
resources, or be rebuilt before reuse. Point-shadow caches likewise cannot be overwritten while retaining each
client's existing "built" state. At eight slots, three dynamic slots never use their cache cubes: packing those
unused cubes out could remove a nominal 18 MiB per client at four-byte depth, with index/shader changes and new
validation. Rebuilding all cached shadows every frame would exchange memory for additional rendering work.

Two other candidates need an allocation inventory before choosing an implementation:

- Immutable zone/actor/object vertex/index data and zone light-grid buffers still use private
  `UploadBatch::buffer` allocations. Six identical eligible buffer sets of unique size B offer at most 5B payload
  savings before sharing overhead. Mutable bones, instance data and per-frame lighting remain private; their
  mapped allocations are not necessarily dedicated VRAM.
- The allocator leaves VMA's preferred large block size at its default: 256 MiB for heaps larger than 1 GiB,
  with smaller first blocks and retained-empty-block hysteresis. Lowering that preference may reduce pool slack,
  but does not save 256 MiB per resource or reclaim dedicated targets. Settled block bytes minus allocation bytes
  bounds allocator slack; native OS before/after measurements must establish any physical/accounting benefit.
  Raw shared imports bypass VMA, so remaining private allocations can keep partially occupied blocks alive.

## Current branch validation

After integrating upstream archive mapping, deferred WLD retention, animation compaction and private sRGB mip
generation, the rebuilt client passed fresh six-process correctness checks on the RX 9070 XT and RTX 3070,
with sharing OFF and ON. Each cohort reached the pinned development zone, retained advancing post-entry frames
and produced three valid diagnostic samples after settling. These validation-enabled samples are excluded from
the primary memory comparison. All 24 client exits were normal with code 0, with no forced cleanup or Vulkan
core/synchronization errors. The ON brokers ended with zero owners, consumers, entries and allocation bytes;
AMD grants/releases balanced at 17,768/17,768 and NVIDIA at 10,211/10,211. All 50 shader files matched the launch
manifest before and after capture. The playable executable SHA-256 was
`34F5F544C8A8D3BAB947C740866F1C7322E5057DC915CA6BBC2D75676502FF0C`.

The first integrated live attempt found a constant-animation interning error: a multi-key constant adopting a
single-key encoded owner lost the flag needed to preserve its logical timing. The fix retains either track's
constant representation flag. A regression samples both interning orders, aliases, loop/clamp timing and
copy-on-write mutation; the subsequent full suites and fresh live cohorts use the corrected build.
