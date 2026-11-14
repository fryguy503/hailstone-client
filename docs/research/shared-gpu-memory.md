# Shared GPU memory

Several Hailstone clients on one machine each upload the same zone and character textures to the same GPU. This page
is about sharing that memory: what Vulkan allows, the experiment the client has now (docs/graphics.md "Shared GPU
textures"), what has been measured and what is still open. Sharing CPU-side asset bytes is a separate piece of work
(docs/layout.md "Shared archive memory").

## What Vulkan allows

Vulkan can export memory from one device and import it in another process, but independent Vulkan instances cannot
all sample one imported image at once: an external image is owned by one instance at a time, also with concurrent
sharing mode, and passes between them by queue family ownership transfers to and from `VK_QUEUE_FAMILY_EXTERNAL`.
Leaving it in `GENERAL` does not lift that rule, and plain shader reads are not a portable promise that an external
image's bits or metadata stay unchanged; `VK_EXT_external_memory_acquire_unmodified` is a hint, not permission.
[Resource sharing](https://docs.vulkan.org/spec/latest/chapters/resources.html#resources-sharing),
[ownership transfers](https://docs.vulkan.org/spec/latest/chapters/synchronization.html#synchronization-queue-transfers),
[acquire unmodified](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_external_memory_acquire_unmodified.html)

Opaque handles also need the same adapter and a compatible driver. The client does not read device or driver UUIDs
(AGENTS.md: no real hardware identifiers), so it matches a GPU by its model name (which must be unique on the
machine), API and driver version and a hash of its memory heaps and types, and checks an import's memory type and
size against its own image's requirements. Two cards of the same model are refused rather than guessed between, and
nothing is shared across vendors or between a discrete and an integrated GPU.
[Handle types](https://docs.vulkan.org/refpages/latest/refpages/source/VkExternalMemoryHandleTypeFlagBits.html)

That leaves two designs:

- **Coordinated ownership** (what the client does): every client keeps its own renderer; a broker owns the shared
  images and a lease says which client may use them on the GPU. The holder acquires the images, draws, releases them
  and lets its GPU work finish before giving the lease back. It changes little in the renderer, but serializes the
  GPU work that touches shared images.
- **One rendering process**: clients send their scenes to one process with one device, which can share immutable
  assets inside one instance with ordinary synchronization. A much larger change (windows, input, command transport,
  crash handling), and the stronger route if clients must render in parallel.

## The experiment

What is shared: the textures zone and actor renderers upload through `UploadBatch::immutable_texture`, keyed by a
versioned hash of the final payload and recipe (format, extent, mips, generated-mip policy). Everything else stays
private: geometry buffers, other renderers' textures, per-frame buffers, render targets, shadows, swapchains.

- The broker uploads each texture once into a dedicated, exportable allocation on its own device, finishes the mips,
  releases it to the external queue family and only then hands it out. Its budget is at most 2 GiB and 2,048
  textures, 128 MiB per texture.
- Handles go over a Windows named pipe (current user only, local peers only, the first instance owns the name, a
  128-bit random name and a 256-bit token compared in constant time, at most 16 connections). The broker duplicates
  an NT handle into the client; the client imports it and closes its copy. An NT handle import does not take over
  the handle, and the imported memory keeps its own reference to the payload, so an import stays valid when the
  broker lets go or exits. [Win32 import](https://docs.vulkan.org/refpages/latest/refpages/source/VkImportMemoryWin32HandleInfoKHR.html)
- Imported textures sit in `SHADER_READ_ONLY_OPTIMAL`; each shared frame acquires and releases every live import
  around its own command buffer, under the lease, and waits for its fence before releasing the lease.
- Lifetime follows the private path: `VkContext::retire` waits for frames and uploads before a texture's last owner
  goes, and only then does the client tell the broker it no longer uses it.

Linux (Unix socket, `SCM_RIGHTS`, opaque FDs) and macOS (MoltenVK has no matching route) are not implemented and
keep private textures.

### When something fails

The rule is that sharing never stops the game, and one client's trouble stays its own:

| Event | Broker | That client | Others |
|---|---|---|---|
| Malformed or out-of-turn request | `Invalid` reply, no state change | detaches (below) | carry on |
| Upload fails (out of GPU memory, unsupported) | `Private` reply | uploads that texture itself, keeps sharing | carry on |
| Client crashes or disconnects holding the lease | releases its lease and references | (gone) | the next waiter gets the lease |
| Lease not free within 5 s | `Busy` reply | detaches | carry on |
| Its frame fence does not pass within 2 s | sees the disconnect, releases the lease | detaches | carry on |
| Broker gone, or no reply in time | (gone) | detaches | each detaches the same way |
| Idle for 5 minutes | closes the connection | clients ping every minute, so only a stuck one | carry on |

Detaching closes the connection, so the broker releases everything the client held, and is for the rest of the
session. Before its next frame records, the client copies every live import into an image of its own, frees the
imports and has its renderers rewrite their descriptors (`GpuSharedCache::localize`); new textures are private. A
copy that fails leaves that texture on its import, which still works without the broker.

Two things this does rely on without proof, listed in docs/unknowns.md: the copy reads each import once more after
the client has stopped taking part in the lease, and a client whose frame fence timed out releases the lease while
that frame may still be running. Both only ever read immutable images.

## Measurements

Reported by the PR author, on Windows with six clients in the same zone (1280x720, 4x MSAA, 30 FPS cap, CPU asset
sharing on in both modes), sharing off and on, the broker counted in "on", from the native adapter memory counters
(`GPUAdapterMemory.DedicatedUsage`, above each run's own baseline):

| Adapter | Off | On | Lower |
|---|---:|---:|---:|
| AMD Radeon RX 9070 XT | 4,107 MiB | 3,487 MiB | 620 MiB (15.1%) |
| NVIDIA GeForce RTX 3070 | 4,139 MiB | 3,489 MiB | 650 MiB (15.7%) |

These are adapter-wide accounting figures from two runs per mode, not physical residency. Every client shared all
159 of the zone's texture bindings, with no core or synchronization validation errors in the validation runs. The
lease costs throughput: on the NVIDIA card the clients' median presented frames fell from 16.9 to 15.5 per second.
Private commit including the broker fell by about 660 to 710 MiB, while the private resident set rose by 65 MiB on
AMD and fell by 247 MiB on NVIDIA.

Earlier steps, also by the PR author: `hailstone_gpu_share_probe` found opaque Win32 buffer, semaphore and image
(RGBA8, BC1, BC3, BC7) import and export on both cards and on an integrated AMD GPU, and `hailstone_gpu_asset_probe`
rendered a real zone (Greater Faydark) in two and six views on one device with identical pixels with sharing off
and on. Intel hardware has not been tried.

None of this covers sustained play, zoning and appearance changes under load, the failure paths above on real
hardware, or more than 16 clients.

## Where more could come from

The shared textures are only part of a client's GPU memory. At 1280x720 with 4x MSAA, each client also holds about
67 MiB of scene targets (colour, depth, resolves, water copies, AO, bloom), 64 MiB of sun-shadow cascades, 32 MiB of
cover and water layers and 96 MiB of point-shadow cubes, all private. Because the lease already gives one client the
GPU at a time, a shared pool of transient scene targets would be possible (about 330 MiB less for six clients, in
theory), but it needs exportable depth and MSAA images, a protocol for writable resources and the lease in every
frame. Shadow cascades share an array with cached layers that must stay private. Static vertex and index buffers
could be shared like textures. None of this is implemented.
