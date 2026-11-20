# Shared GPU memory

Several Hailstone clients on one machine each upload the same zone and character textures to the same GPU. This page
covers sharing that memory: what Vulkan allows, the client's experiment (docs/graphics.md "Shared GPU textures"),
what has been measured and what is open.

Sharing CPU-side asset bytes is separate (docs/layout.md "Shared archive memory").

## What Vulkan allows

Vulkan can export memory from one device and import it in another process, but independent instances cannot all
sample one imported image at once:

- An external image is owned by one instance at a time, even with concurrent sharing mode.
- It passes between them by queue family ownership transfers to and from `VK_QUEUE_FAMILY_EXTERNAL`; `GENERAL`
  layout does not lift that rule.
- Plain shader reads are not a portable promise that an external image's bits or metadata stay unchanged.
  `VK_EXT_external_memory_acquire_unmodified` is a hint, not permission.

Sources: [Resource sharing](https://docs.vulkan.org/spec/latest/chapters/resources.html#resources-sharing),
[ownership transfers](https://docs.vulkan.org/spec/latest/chapters/synchronization.html#synchronization-queue-transfers),
[acquire unmodified](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_external_memory_acquire_unmodified.html)

Opaque handles also need the same adapter and a compatible driver. The client does not read device or driver UUIDs
(AGENTS.md: no real hardware identifiers); it matches a GPU by model name (unique on the machine), API and driver
version, and a hash of its memory heaps and types, and checks an import's memory type and size against its own
image's requirements. Two cards of the same model are refused, not guessed between. Nothing is shared across vendors
or between a discrete and an integrated GPU.
[Handle types](https://docs.vulkan.org/refpages/latest/refpages/source/VkExternalMemoryHandleTypeFlagBits.html)

Two designs follow:

- **Coordinated ownership** (what the client does): each client keeps its own renderer; a broker owns the shared
  images and a lease says which client may use them. The holder acquires the images, draws, releases them and lets
  its GPU work finish before returning the lease. Small renderer change, but GPU work on shared images is serialized.
- **One rendering process**: clients send their scenes to one process with one device, which shares immutable assets
  with ordinary synchronization. A much larger change (windows, input, command transport, crash handling), and the
  stronger route if clients must render in parallel.

## The experiment

Shared: textures the zone and actor renderers upload through `UploadBatch::immutable_texture`, keyed by a versioned
hash of payload and recipe (format, extent, mips, generated-mip policy). Everything else stays private: geometry,
other renderers' textures, per-frame buffers, render targets, shadows, swapchains.

- The broker uploads each texture once into a dedicated exportable allocation on its own device, finishes the mips
  and releases it to the external queue family before handing it out. Budget: at most 2 GiB, 2,048 textures,
  128 MiB per texture.
- Handles go over a Windows named pipe: current user and local peers only, the first instance owns the name, a
  128-bit random name and a 256-bit token compared in constant time, at most 16 connections.
- The broker duplicates an NT handle into the client, which imports it and closes its copy. The imported memory keeps
  its own reference, so an import stays valid when the broker lets go or exits.
  [Win32 import](https://docs.vulkan.org/refpages/latest/refpages/source/VkImportMemoryWin32HandleInfoKHR.html)
- Imports sit in `SHADER_READ_ONLY_OPTIMAL`; each shared frame acquires and releases every live import around its
  command buffer, under the lease, and waits for its fence before releasing the lease.
- Lifetime follows the private path: `VkContext::retire` waits for frames and uploads before a texture's last owner
  goes, then the client tells the broker.

Linux (Unix socket, `SCM_RIGHTS`, opaque FDs) and macOS (MoltenVK has no matching route) are not implemented and
keep private textures.

### When something fails

Sharing never stops the game, and one client's trouble stays its own:

| Event | Broker | That client | Others |
|---|---|---|---|
| Malformed or out-of-turn request | `Invalid` reply, no state change | detaches (below) | carry on |
| Upload fails (out of GPU memory, unsupported) | `Private` reply | uploads that texture itself, keeps sharing | carry on |
| Client crashes or disconnects holding the lease | releases its lease and references | (gone) | the next waiter gets the lease |
| Lease not free within 5 s | `Busy` reply | detaches | carry on |
| Its frame fence does not pass within 2 s | sees the disconnect, releases the lease | detaches | carry on |
| Broker gone, or no reply in time | (gone) | detaches | each detaches the same way |
| Idle for 5 minutes | closes the connection | clients ping every minute, so only a stuck one | carry on |

Detaching closes the connection, so the broker releases everything the client held, for the rest of the session.
Before its next frame the client copies every live import into its own image, frees the imports and rewrites its
descriptors (`GpuSharedCache::localize`). New textures are private. A failed copy leaves that texture on its import,
which still works without the broker.

Two unproven assumptions, listed in docs/unknowns.md (both only read immutable images):

- the copy reads each import once more after the client has left the lease;
- a client whose frame fence timed out releases the lease while that frame may still be running.

## Measurements

Reported by the PR author: Windows, six clients in one zone, 1280x720, 4x MSAA, 30 FPS cap, CPU asset sharing on in
both modes, broker counted in "on". Native adapter counters (`GPUAdapterMemory.DedicatedUsage`, above each run's
baseline):

| Adapter | Off | On | Lower |
|---|---:|---:|---:|
| AMD Radeon RX 9070 XT | 4,107 MiB | 3,487 MiB | 620 MiB (15.1%) |
| NVIDIA GeForce RTX 3070 | 4,139 MiB | 3,489 MiB | 650 MiB (15.7%) |

- Adapter-wide accounting from two runs per mode, not physical residency.
- Every client shared all 159 of the zone's texture bindings, with no core or synchronization validation errors.
- The lease costs throughput: on the NVIDIA card median presented frames fell from 16.9 to 15.5 per second.
- Private commit including the broker fell by about 660 to 710 MiB; private resident set rose 65 MiB on AMD and fell
  247 MiB on NVIDIA.

Earlier probes, also by the PR author:

- `hailstone_gpu_share_probe`: opaque Win32 buffer, semaphore and image (RGBA8, BC1, BC3, BC7) import and export
  work on both cards and an integrated AMD GPU.
- `hailstone_gpu_asset_probe`: Greater Faydark in two and six views on one device, identical pixels with sharing off
  and on.
- Intel hardware has not been tried.

Not covered: sustained play, zoning and appearance changes under load, the failure paths on real hardware, more than
16 clients.

## Where more could come from

Each client also holds private GPU memory at 1280x720 with 4x MSAA:

| Resource | Size |
|---|---:|
| Scene targets (colour, depth, resolves, water copies, AO, bloom) | about 67 MiB |
| Sun-shadow cascades | 64 MiB |
| Cover and water layers | 32 MiB |
| Point-shadow cubes | 96 MiB |

Ideas, none implemented:

- The lease already gives one client the GPU at a time, so transient scene targets could be pooled (in theory about
  330 MiB less for six clients). Needs exportable depth and MSAA images, a protocol for writable resources and the
  lease in every frame.
- Shadow cascades share an array with cached layers that must stay private.
- Static vertex and index buffers could be shared like textures.
