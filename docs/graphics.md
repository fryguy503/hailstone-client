# Graphics

The client's mechanics follow the RoF2 client; its graphics do not (`docs/plan.md`, section 1). The classic renderer is
a DirectX 9 design from 2003: per-vertex lighting, a hard fog wall, no antialiasing, no shadows. This client keeps
the game's assets and its world data (zones, models, the day clock, weather, the zone's fog values as hints) and
draws them with a modern renderer. The procedural sky's
colour maps, the EQG material classes and the multi-pass light details are not planned work unless a feature needs
them; the classic path that exists stays as the `classic` look.

## Shared GPU asset investigation

GPU allocations remain private by default. The headless `hailstone_gpu_share_probe` tool reports
external buffer, image and semaphore capabilities for each adapter without querying device UUIDs. Its optional
`--smoke --device-index N` verifies multiple buffer aliases on one logical device; it does not enable cross-process
sharing. See [Shared GPU memory](research/shared-gpu-memory.md) for the ownership, driver-compatibility and lifetime
requirements, tested hardware and remaining implementation gates.

`hailstone_gpu_asset_probe` renders real static zone assets in two or six offscreen views on one device, with sharing
OFF/ON pixel comparisons and finite continuously rendered holds for native OS profiling. It is a broker experiment,
not a client GPU-sharing setting. Its simplified material shader and allocation inventory do not establish full-client
visual parity or physical GPU-memory use.

An experimental Windows broker path imports immutable zone and actor textures into separate playable client
processes. Each process keeps its own Vulkan device, UI, mutable buffers and presentation targets. Imported payloads
change instance ownership under a coordinated frame lease; the client completes its release on the GPU before
returning the lease. This preserves separate processes but reduces GPU queue overlap, so memory and rendering cost
must be measured together. Other platforms and unsupported images retain private uploads. See the research page
for runtime validation status and remaining failure/compatibility gates; this is not enabled by default.

For controlled tests, `HAILSTONE_GPU_NAME` requests one exact generic model name. A missing, ambiguous or unusable
request fails rather than choosing a different GPU. Without that variable, automatic GPU selection is unchanged.
`HAILSTONE_GPU_BENCHMARK=1` logs present-submission progress and shared-frame/lease timing for test readiness; those
values are application diagnostics, not native memory measurements or display FPS. `HAILSTONE_BENCHMARK_WORLD`
pins automatic login to one available, unambiguous world name or saved world key, without a first-world fallback.

### Developer quickstart on Windows

This is an experimental **build-tree tool**, not a setting in the client or a broker included in the normal release
package. Build it alongside the client and capability probe from the repository root:

```powershell
cmake --build build-msvc --config Release --target hailstone hailstone_gpu_cache_broker hailstone_gpu_share_probe
```

Keep the client's complete `build-msvc/hailstone/` tree, including `shaders/`, and each executable's runtime DLLs.
The client is `build-msvc/hailstone/hailstone.exe`; the broker/probe normally sit in the build root, or its
`Release/` subfolder with a multi-configuration generator. Set the paths below to the actual build outputs.
The probe's ordinary JSON output supplies generic adapter model names; use an exact, unique name for both broker
and clients. This does not share allocations between different adapters or vendors.

Before running the example, save a development server profile and six separate development accounts through the
ordinary client, including their keyring passwords. Use the same Hailstone data directory for these saved profiles
and for this example. Replace every path/profile/world/account/character placeholder. The game-data directory is
read only; the Hailstone data directory must be separate and writable. CPU sharing is independent: keep
`assets.shared_memory=auto` and its cache budget identical in both GPU comparison modes if CPU sharing is wanted.
See [Shared archive memory](layout.md#shared-archive-memory).

The following **PowerShell 7.4 or later** example generates a fresh local endpoint and 256-bit session token. Only
the owned children's environment receives them; no token or password goes into arguments, console output, a
profile or a script file. Keep this shell open until the clients and broker have closed. Normal client login uses
saved keyring credentials, while the world pin fails if the named development world is unavailable or ambiguous.

```powershell
$ErrorActionPreference = 'Stop'
$clientExe = (Resolve-Path 'build-msvc/hailstone/hailstone.exe').Path
$brokerExe = (Resolve-Path 'build-msvc/hailstone_gpu_cache_broker.exe').Path
$session = @{
    HAILSTONE_GPU_NAME = 'NVIDIA GeForce RTX 3070' # Replace with the exact generic model from the probe.
    HAILSTONE_GPU_IPC_ENDPOINT = '\\.\pipe\hailstone-gpu-' + [Convert]::ToHexString(
        [Security.Cryptography.RandomNumberGenerator]::GetBytes(16)).ToLowerInvariant()
    HAILSTONE_GPU_IPC_TOKEN = [Convert]::ToHexString(
        [Security.Cryptography.RandomNumberGenerator]::GetBytes(32)).ToLowerInvariant()
    HAILSTONE_GPU_BENCHMARK = '1'
    HAILSTONE_BENCHMARK_WORLD = 'Your Development World'
    HAILSTONE_DATA_DIR = 'C:\HailstoneTestData'
}
$gameDir = 'C:\YourGameAssets'
$serverProfile = 'your-dev-profile'
$players = @(
    @('dev-account-1', 'CharacterOne'), @('dev-account-2', 'CharacterTwo'),
    @('dev-account-3', 'CharacterThree'), @('dev-account-4', 'CharacterFour'),
    @('dev-account-5', 'CharacterFive'), @('dev-account-6', 'CharacterSix')
)

function New-GpuStartInfo([string]$Exe, [string[]]$Arguments, [switch]$Capture) {
    $info = [Diagnostics.ProcessStartInfo]::new($Exe)
    $info.UseShellExecute = $false
    $info.WorkingDirectory = Split-Path -Parent $Exe
    foreach ($prefix in @('HAILSTONE_', 'NMSC_')) {
        foreach ($name in @('GPU_IPC_ENDPOINT', 'GPU_IPC_TOKEN', 'GPU_IPC_SERVER_PID',
                            'GPU_NAME', 'GPU_BENCHMARK', 'BENCHMARK_WORLD', 'SERVER_SELECT')) {
            [void]$info.Environment.Remove($prefix + $name)
        }
    }
    foreach ($name in @('VK_INSTANCE_LAYERS', 'VK_LOADER_LAYERS_ENABLE', 'VK_LAYER_PATH', 'VK_LAYER_VALIDATE_SYNC')) {
        [void]$info.Environment.Remove($name)
    }
    foreach ($name in $session.Keys) { $info.Environment[$name] = $session[$name] }
    foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
    $info.RedirectStandardOutput = $info.RedirectStandardError = [bool]$Capture
    $info.CreateNoWindow = [bool]$Capture
    return $info
}
function Invoke-GpuControl([string]$Mode) {
    $control = [Diagnostics.Process]::Start((New-GpuStartInfo $brokerExe @($Mode) -Capture))
    try {
        $output = $control.StandardOutput.ReadToEndAsync()
        $errors = $control.StandardError.ReadToEndAsync()
        if (-not $control.WaitForExit(30000)) { throw 'Broker control timed out.' }
        if ($control.ExitCode -notin @(0, 2)) { throw 'Broker control failed; inspect the broker log.' }
        return [pscustomobject]@{ ExitCode = $control.ExitCode; State = ($output.Result | ConvertFrom-Json) }
    } finally { $control.Dispose() }
}

$clients = [Collections.Generic.List[Diagnostics.Process]]::new()
$broker = $null; $brokerOutput = $null; $brokerErrors = $null
try {
    $broker = [Diagnostics.Process]::Start((New-GpuStartInfo $brokerExe @('--serve', '--hold-seconds', '4320') -Capture))
    [void]$broker.Handle
    $brokerErrors = $broker.StandardError.ReadToEndAsync()
    $line = $broker.StandardOutput.ReadLineAsync().WaitAsync([TimeSpan]::FromSeconds(180)).GetAwaiter().GetResult()
    $ready = $line | ConvertFrom-Json
    $brokerOutput = $broker.StandardOutput.ReadToEndAsync()
    if ($ready.kind -ne 'broker-ready' -or $ready.device_model -cne $session.HAILSTONE_GPU_NAME -or
        $ready.validation_errors -ne 0) { throw 'Broker readiness did not match the requested GPU.' }
    $session.HAILSTONE_GPU_IPC_SERVER_PID = [string]$broker.Id
    foreach ($player in $players) {
        $arguments = @('--game-dir', $gameDir, '--server', $serverProfile, '--account', $player[0],
                       '--character', $player[1], '--window-size', '1280x720', '--max-fps', '30')
        $client = [Diagnostics.Process]::Start((New-GpuStartInfo $clientExe $arguments))
        [void]$client.Handle
        $clients.Add($client)
        Start-Sleep -Seconds 8
    }
    (Invoke-GpuControl '--status').State
    Read-Host 'Verify all six characters and sharing logs; press Enter to close this session' | Out-Null
} finally {
    # Keep all original process handles until every close/exit observation is complete.
    foreach ($client in $clients) { if (-not $client.HasExited) { [void]$client.CloseMainWindow() } }
    foreach ($client in $clients) { if (-not $client.HasExited) { [void]$client.WaitForExit(30000) } }
    $running = @($clients | Where-Object { -not $_.HasExited })
    if ($broker -and -not $broker.HasExited -and $running.Count -eq 0) {
        $stop = Invoke-GpuControl '--stop-if-idle'
        if ($stop.ExitCode -eq 0 -and $stop.State.stopped) { [void]$broker.WaitForExit(30000) }
    }
    if ($broker -and $broker.HasExited) {
        if ($brokerOutput) { $brokerOutput.Result } # Includes broker-complete; check clean=true and exit code 0.
        if ($brokerErrors -and $brokerErrors.Result) { Write-Warning $brokerErrors.Result }
        if ($broker.ExitCode -ne 0 -or @($clients | Where-Object { -not $_.HasExited -or $_.ExitCode -ne 0 }).Count) {
            Write-Warning 'An owned process did not exit cleanly; this session is not an accepted test.'
        }
        $broker.Dispose()
        foreach ($client in $clients) { $client.Dispose() }
        $session.Clear()
    } else {
        Write-Warning 'Session still running: keep this shell open, close its client windows, then retry status/stop-if-idle.'
    }
}
```

The ready message alone does not prove clients use shared textures. Check every client's actual InZone character,
the intended development world/zone, advancing post-zone `GPU benchmark progress` with positive
`shared_images_this_frame`, and positive current-zone `GPU immutable zone textures` sharing. ON should report six
broker consumers, nonzero entries/hits and no poisoned epoch or validation errors. Import/allocation counters alone
can describe an earlier scene. Logs are under the Hailstone data directory; simultaneous clients use separate log
files as described in [The Hailstone folder](layout.md).

Close all six clients before `--stop-if-idle`. The control tool exits 0 when accepted, 2 when busy, and 1 on an error;
never treat busy as a successful shutdown. After shutdown require client exit codes 0 and `broker-complete` with
`clean=true`, no owner/consumers/entries/allocation bytes, balanced grants/releases and broker exit code 0. If the
example leaves a process running, retain the shell/session variables and retry the control commands after closing
its windows. Broker lifetime defaults to 4,320 seconds and accepts at most 7,200; finish the session before its
deadline. There is no reconnect or automatic reconstruction of imported textures after a failed live session.

With no GPU IPC environment, GPU allocations remain private. Unsupported platforms/extensions and initial
connection or explicit adapter/driver mismatch can retain private uploads; individual unsupported images or cache
limits also fall back privately. A malformed/uncertain handshake after connection, import failure, lost ownership
or poisoned session stops the affected client. Changing `HAILSTONE_GPU_NAME` to a missing, ambiguous or unusable
model fails selection instead of silently switching GPUs. Current bounds are Windows local IPC, 16 connections
including controls, 128 MiB input payload per texture, 2,048 textures and a maximum/default 2 GiB broker budget.
Six-client AMD/NVIDIA evidence does not establish Intel, Linux/macOS GPU sharing or 24 clients. Frame leases
serialize GPU work using shared images; memory savings do not establish equivalent rendering performance.

### Native memory comparisons without the private test harness

Use the same frozen client, broker and shader files, six accounts, world/zone, camera, graphics settings, window
visibility and CPU-sharing mode for OFF/ON/ON/OFF. First run both paths with core/synchronization validation; keep
those correctness runs separate from memory captures with validation disabled. To request validation, add
`--validation` to broker and clients and provide the installed layer's `VK_LAYER_PATH` and
`VK_LAYER_VALIDATE_SYNC=1` in the child environment. The example's helper removes inherited forced layer settings
before applying its session values.

For GPU OFF, launch the same clients with both `HAILSTONE_` and legacy `NMSC_` GPU IPC endpoint/token/server-pin
variables absent and without a broker. Keep GPU model, benchmark/world pins and CPU sharing unchanged. For each
cohort, collect native adapter-wide observations before any client/broker starts, after all six actual characters
have settled and continue rendering, and after every client/broker exits. For example, these built-in Windows CIM
sources expose the two native counter families, independently of in-game `/perf`:

```powershell
Get-CimInstance Win32_PerfFormattedData_GPUPerformanceCounters_GPUAdapterMemory |
    Select-Object Name, DedicatedUsage, SharedUsage, TotalCommitted
Get-CimInstance Win32_PerfFormattedData_GPUPerformanceCounters_GPULocalAdapterMemory |
    Select-Object Name, LocalUsage
```

Keep the selected adapter's exact physical/partition row stable across the same boot, and establish its association
with the chosen GPU using native Windows adapter/counter information. On multi-adapter hosts, the generic Vulkan
model name alone is insufficient to identify a counter row. Keep those boot-local mappings private; do not query
or publish hardware UUIDs. If a source is unavailable or rows are ambiguous, report it rather than substituting a
different source. Sample the complete declared window, preserve before/loaded/after ranges and keep host RAM/commit
headroom. Subtract each run's own before baseline; check after-baseline sensitivity and both pair orders. ON's
adapter-wide measurement includes the broker. Never sum per-PID imported-memory charges, add LocalUsage to
DedicatedUsage, or equate these accounting counters with physical residency, capacity or displayed FPS.

## The frame

`render::VkContext` (`src/render/vk_context.h`):

1. `begin_frame_commands` starts the frame; the shadow cascades are drawn (`begin_shadow_cascade` ..
   `end_shadow_cascade`, depth only); `begin_scene` begins rendering into the scene target: RGBA16F colour,
   multisampled when `display.msaa` > 1, with a depth buffer of the same sample count (D32_SFLOAT, else a format with
   stencil, D32_SFLOAT_S8_UINT or D24_UNORM_S8_UINT, whose barriers name the depth aspect alone: the device enables
   `separateDepthStencilLayouts`, which every Vulkan 1.2 driver has and the GPU check requires). Scene pipelines use
   `scene_format()` and `scene_samples()`. (`begin_frame` is the two together, for callers without shadows.)
2. `begin_overlay` ends the scene (the MSAA colour resolves into a single-sampled image and the depth's sample 0
   into a depth image as rendering ends), draws the ambient occlusion (`shaders/ao.frag`, into an R8 image) when the
   frame gave `set_ao_view`, then the bloom in the same frames (`draw_bloom`: six half-size RGBA16F levels, the
   13-tap `shaders/bloom_down.frag` from the scene down, Karis-weighted on the first level, then
   `shaders/bloom_up.frag`'s 3 x 3 tent added back up), then draws `shaders/post.frag` over the swapchain image: AO,
   bloom (mixed in at `display.bloom` x 8 %), exposure, tonemap, sRGB encode, grade, dither. Rendering stays open
   on the swapchain image (one sample, no depth) for the UI; `ui::ImGuiLayer::render` calls it.
3. `end_frame` presents (and calls `begin_overlay` if nothing did).

The 3D view can take a rectangle of the window (/viewport, `VkContext::set_scene_rect`): the scene targets are made at
its size (`FrameInfo::extent`, with its place in `FrameInfo::offset`), the post pass draws into that rectangle (its
push constant's offset gives the scene texel under each pixel) and clears the rest of the image to black; the UI still
covers the whole window, and the app maps the nameplates and mouse picking onto the rectangle
(`ClientApp::view_area_`).

The scene can be drawn below the view's size and upscaled (`display.render_scale`, `SceneOptions::render_scale`,
50 .. 100 %): the scene targets are made at the view's size times the scale (`render::scaled_extent`, each side
rounded), and `FrameInfo::extent` is that size, so the scene's projection, AO, SSR and bloom all work at it; the view's
rectangle in the window (picking, nameplates) is unchanged. Below 100 % `begin_overlay` runs AMD FidelityFX FSR 1
(`third_party/fsr1`, MIT; spatial, no motion vectors): `post.frag` draws into a 10-bit image at the scene's size,
tonemapped and sRGB-encoded as FSR 1 wants it and without the dither; `shaders/fsr_easu.frag` (EASU, edge-adaptive)
brings it up to the view's size; `shaders/fsr_rcas.frag` (RCAS) sharpens that by `display.sharpness` into the
swapchain and adds the dither. The UI is drawn after, at the window's own size. Textures keep their LOD bias (none
yet). A temporal upscaler (FSR 2/3.1, which needs motion vectors and jitter) is the next step (Roadmap).

Screenshots copy the presented swapchain image, so the swapchain asks for `TRANSFER_SRC` when the surface offers it;
where it does not, a screenshot is refused with a log line (and a chat line for the screenshot key) instead of an
invalid copy. The swapchain is made again when the window's framebuffer size changes from the size it was made for,
not whenever the surface's `currentExtent` differs from the window's size.

Uploads do not wait for the GPU. `render::UploadBatch::submit` records its copies (and mip blits) into a command buffer
of their own and submits it on the frame's queue with a fence of its own (`VkContext::submit_upload`); the barriers
it ends with (copy writes to vertex input, index, vertex and fragment shader reads; images to shader-read layout) make
the results visible to every later submission on the queue, so a model, texture or icon sheet put up while a frame is
recorded is drawn by that frame. Its staging buffers go once its fence signals, checked at each frame's start and each
upload; past 256 MB of staging in flight the oldest uploads are waited for. A zone's own uploads are staged in pieces
rather than in one lot: its vertex and index buffers are as big as the zone's geometry, and staging one of them whole
doubled it in host memory while the copy was made. `UploadBatch::buffer_pieces` makes the target once and stages and
copies each piece on its own, and `VkContext::release_finished_uploads` frees the staging of what the GPU has already
read, so a long build (the zone's) holds at most a budget of it (32 MB) rather than all of it until the first frame.
`VkContext::immediate_submit` stays
synchronous, for the context's own one-off work. What a frame in flight (or an upload) may still use is handed to
`VkContext::retire` instead of destroyed after `vkDeviceWaitIdle`: it goes once the frame being recorded, every
upload submitted before and any `frames_ahead` more are done (`render/retire_queue.h`; the UI's textures take one
more frame, as ImGui's draw lists are built before their frame begins). A `ZoneRenderer` is let go this way, so a
neighbour zone freed mid-frame no longer stalls the frame. A recreated swapchain is made with `oldSwapchain`, and the
old one and its present semaphores are retired two frames on: `vkDeviceWaitIdle` does not cover a present's wait.

The ImGui textures (`ui/imgui_vk_textures.h`: the UI art, icon sheets, browser tabs) come from descriptor pools of the
client's own, a new one made when they fill, with a set layout defined as the backend's. `ui::UiTextures` keeps at
most 1024 files (`kMaxFiles`); past that the one asked for least recently is retired.

In the modern lighting the scene is split for the water. The zone's opaque draws (`render::ZonePhase::Opaque`, with
the water held back), the placed objects and the characters go first. `begin_translucent` then ends rendering and
copies the resolved colour and depth (sample 0) into images that `shadow_set()` binds as 1 and 2. Drawing then goes
on into the same targets (stored and loaded, the multisampled ones too): the grass (opaque, but left out of the copies
so the target ring lies on the ground under it), then `ZonePhase::Translucent`: the water first, then the blended and
additive draws.

The scene target holds linear colour. The classic lighting computes in display (sRGB) values as the classic renderer does
and converts at the end of each shader; the modern lighting computes in linear HDR.

The classic lighting lights the characters with the zone's scene light (`render::classic_actor_light`, mirrored in
`shaders/actor.frag`): the region shaders' vertex formula, with the zone's baked light where the player stands (the
modern mode's ambient probe, `render/ambient_probe.h`) as the vertex colour and its alpha as the sky's share; none, black
with the full share. The result stays between `kClassicActorFloor` (0.25) and 1, and a soft key light from above shapes
the body (0.7 .. 1). This is the client's own: how the original lights its skinned models is not traced. Before, the
characters had a fixed key light dimmed by the hour (down to 0.3 at night) whatever the zone looked like, and stood out
nearly black against the bright baked walls of zones such as the Bazaar after dark. The doors and ground objects keep
that hour factor.

In the modern lighting, things that give off light are drawn past white so they bloom: additive particles (spells,
fires) x `render::kParticleGlow` (2.5), lava's own light x 2.5 (`shaders/zone.frag` `emit`), the sun and moon x 4.

World textures (zone, objects, characters, sky, particles) are uploaded as `render::TextureUse::Color`: their mip
chains are built on the CPU by `assets::build_color_mips`. Each level is filtered in linear light with colour weighted
by alpha. Transparent texels take their opaque neighbours' colour, so palette index 0 (often magenta or black) no
longer fringes leaves. Cut-outs keep level 0's coverage on every level, so trees stay full at a distance. DDS files
shipped without mips get a chain too.

In the ordinary private `UploadBatch::texture` path, a fully opaque image whose sides are powers of two needs none
of that: its chain is a plain average, which the GPU builds as the CPU would, so it goes up as it is and the GPU
builds the levels (`render::gpu_builds_mips`, `render::prepare_texture_image`). The image is created `R8G8B8A8_SRGB` with
`VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT` and blitted level by level, so each blit decodes to linear light, filters and
encodes back, as `assets::build_color_mips` does; the view the shaders sample is `R8G8B8A8_UNORM`, so they read the
same values as from a CPU chain. The two agree within a rounding step per level (the GPU stores each level in 8 bits
before making the next; tests/texture_test.cpp checks this against the CPU chain). A device that cannot blit that
format with linear filtering (`DeviceCaps::srgb_mip_blit`) gets the CPU chain. An image of another size keeps the CPU
chain, since a 2:1 linear blit of an odd level reads other texels than the CPU's 2x2 box; so does an image with a cut-out,
as the GPU's own levels would blend the colour key into the edges and bring back the cyan fringe
`assets::remove_color_key` takes out. What this saves is host memory: the chain and its staging copy are never held for
those images (the baked terrain tiles, uncompressed A8R8G8B8 DDS, are opaque where they are a zone's base layer). A
neighbour zone's textures, held to `ZoneRenderer::Options::max_texture_size` (`render::prepare_capped_texture`), always
get the CPU chain, as only a chain has levels to drop.

The experimental shared immutable path instead uses `render::prepare_immutable_texture_image` to finish colour
mips on the CPU before content-keying and export. Its external image is UNORM, so colour chains are never left
for an UNORM blit in the broker. Data images retain their ordinary mip recipe, and unsupported BC formats are
decoded before keying. Disabling GPU sharing keeps the ordinary private path above.

A material that never reads its texture's alpha (opaque, no alpha test: EQG `Opaque_*` classes and the classic opaque
methods, `MaterialState::alpha_unused`) uploads it as `TextureUse::OpaqueColor` in the actor and object renderers: made
opaque first, so the colour stored under alpha 0 is drawn and filtered as the classic renderer does. A DDS's alpha is not
a colour key: the hero's forge plates (`genericplate*.eqg`, `generic_plate_blue_c.dds`) keep everything but their cloth
under alpha 0, drawn by `Opaque_MaxCBSGE1.fx`, and the cloth by `Chroma_MaxCBSGE1.fx` from the same texture.

Some DDS textures were painted on a colour key all the same, which their soft edges are blended into:
potranquility's trees (`dstree308/312/317/319.dds`, DXT5) hold cyan (0, 170, 156) under all of their alpha 0, and
their partly transparent edge texels a share (1 - alpha) of it, which the leaves' alpha blend and bilinear filtering
draw as a cyan rim. `assets::remove_color_key` finds such a key (one saturated colour under at least 90% of the alpha 0
texels; not black or grey, not a normal map's flat blue: 301 of the 61,474 textures in the install, from cyan trees and
magenta character parts to blue spell beams) and gives the edge texels the leaves' own colour: their opaque neighbours'
where mostly clear, their own with the key's share taken out (`(c - (1 - a) key) / a`) where mostly opaque. It runs on
every `TextureUse::Color` texture before its mips, and on a game DDS with its own mips, whose chain is then built
again. The game's file drawn by an opaque material keeps its colour.

Animated textures (a classic SIMPLESPRITEDEF with more than one frame and a delay, `docs/formats/wld.md` 0x04) show
frame `(t / delay) mod count`, `render::texture_frame`, on the one clock the zone renderer's effects use (seconds since
the client started). Every frame is uploaded, in the zone (`ZoneRenderer`) and on the doors and ground objects
(`ObjectRenderer::set_time`, from `WorldObjects`). The classic flames are this: `FIRE1_SPRITE` is fire1..fire4 at
200 ms in every campfire, torch and brazier, whether the zone places it (ecommons' CAMPFIRE) or the server does as a
door (ecommons' GRATE beside Gaddius, the bazaar's IT78 campfire from gequip.s3d). The characters' items (a torch in
a hand) still draw their first frame.

## Fire

In the modern lighting every flame the zone burns is drawn by `render::FireRenderer` instead of the
animated texture or particle cloud (the classic lighting keeps those; `/lighting` switches at once). The flame is
procedural (`shaders/fire.frag`): two layers of noise rising through a teardrop that is narrow at its foot, widest a
fifth of the way up and drawn out into tongues that sway. Its colour runs from deep red at the edges through orange
to a pale yellow core, bright enough to bloom, and a few embers drift up from it. Each flame stands on one quad that
turns about the vertical to face the camera, drawn additively after `VkContext::begin_translucent`: it fades where it
meets the scene (the depth copy) and into the haze. A magic flame (blue, green, violet, red) keeps the fire's
brightness in its own hue. A wide fire (2 units across or more, and wider than half its height: a campfire, a
brazier) burns as a middle flame with three smaller tongues round it (`render::add_fire_sources`).

The flames are found in the zone's data (`assets/zone_fires.h`), in three forms by the zone's era:

- Flame meshes on placed objects and doors (the old world and Velious): the materials `FIRE1`, `BFIRE1`,
  `BLUEFLAME01`, `GFIRE1`, `RFIRE1` and `PFLAME01`. A mesh's flame triangles whose boxes touch are one flame (a
  torch's crossed quads; each candle of a candelabra its own); the flame takes the box's foot, 0.7 of its width and
  1.1 of its height. `ZoneRenderer::set_hide_flames` and `ObjectRenderer::set_hide_flames` leave the meshes out, and
  `ObjectRenderer::prepared_fires` gives the doors' and ground objects' flames each frame (a campfire door).
- Particle clouds on placed objects (Luclin onwards): definitions whose sprite is `GENG*.DDS` or `GENPOP*.DDS`.
  `ZoneParticles` measures each kind of cloud (definition and node rotation, `app::measure_flame_cloud`) by burning a
  fresh emitter of it for one particle lifetime (at least 1 s, at most 8 s), so its oldest particles have risen as
  far as they go, then watching it for a second; the flame takes the bright particles' reach, 0.75 of its width and
  0.85 of its height. (potranquility's pillars are ten 5-second clouds stacked 10 units apart: measured after a
  fixed second each was under 10 units, and the column burnt as separate flames.) The flame
  clouds are drawn only in the classic lighting; the smoke stays.
- An EQG zone's environment emitters, `<zone>_EnvironmentEmitters.txt` (`Name^EmitterDefIdx^X^Y^Z^[AlwaysVisible^]
  Lifespan`, positions in the zone's own frame) over `environmentemittersnew.edd`. `assets::load_zone` adds them to
  the zone's particle fixtures, pointing up, so both lightings draw them (they were not drawn at all before): torches,
  braziers and also their smoke, mist and the like. A few classic zones have the file too (frozenshadow). A
  revamped zone loaded from its RoF2 `.s3d` (`assets::is_revamped_zone_name`: nektulos, arena...) takes none: the
  file beside it is the live `.eqg`'s, and its torches and campfires would burn in mid-air in the old zone.
  Their flames are the definitions with a flame sprite (`FlameD.dds`, `new_flame.dds`, `new_fire.dds`, `fire01.dds`,
  `fire01_gray.dds`, `flamegray.dds`) and no "lava" in the name (nor an `mml_instance_flame` one); their hue comes from the definition's start colour
  (vald_bluetorch is FlameD.dds in 10, 10, 255), or for a grey one from its name (cob_brazier_green).

Not flames, though named like them: logs, coals and holders (`FIREWOOD`, `COALS`, `TORCHTOP`), fireplace stones,
Takish's red crystal torches (`TAKTORCH01`), `YKFLAME01` (black; its fireplace's flame is the cloud on it), smoke, and
`GENE01` sparkles. The flames from the different forms are merged, the bigger kept where one stands in another's flame
(frozenshadow's emitters burn on its torches' meshes). A flame with none of the zone's lights within 30 units gets one
of its own, in its hue (EQG zones such as argath and commonlands have flames and no lights). A flame with a non-finite
place or size, or one more than 1000 units wide or tall (a placement scaled absurdly in a broken zone file), is dropped
before the merge: its neighbour search would walk billions of grid cells.

Not yet: a torch held in a hand (its flame is a gequip particle cloud, which is not loaded) and particle clouds on
server doors (never spawned) still show no flame.

## Particles

`render::ParticleRenderer` draws the particles of the spells, the actors' emitters and the zone's clouds as
quads built on the CPU from `game::ParticleSystem`; the emitters behave the same in both lightings, as the graphics
DLL's update does (docs/formats/particles.md). The classic lighting draws them as the DLL does: emitter by emitter,
additive or alpha-blended, none writing depth, nothing nearer than 2 units or beyond 500 along the view. The modern
lighting draws them past 500 units (Hailstone's: its view reaches further). The modern lighting draws every particle of a renderer in one stream,
sorted back to front by distance from the camera (the nearest kept when there are too many), with premultiplied
blending so the additive ones (alpha 0) and the blended ones mix in that order, and without depth writes. It draws
after `VkContext::begin_translucent`: a particle fades where it meets the scene (the depth copy) over a quarter of its
size, and as it nears the eye. Smoke, dust and other alpha-blended particles are lit per particle by what the objects
get: the sky and ground ambient, 0.3 of the sun's or moon's light, the special ambient and the nearby point lights'
pools, at least the light floor; they fade into the haze's colour with distance. Additive particles are light: they
stay unlit, `render::kParticleGlow` bright, and the haze takes their light away instead of adding its colour. Additive
smoke (a texture with "smoke" in its name, the Luclin chimneys' `CSMOKE.DDS`) is lit as smoke, so it does not glow at
night. Each renderer sorts only its own particles: the zone's clouds, the actors' and the spells' are still drawn one
renderer after the other.

The EDD's billboard mode (+0x70; `render::particle_corners`, as the DLL's quad builder places the corners) turns the
quad: 0 faces the camera, turned clockwise on the screen by a spinning particle's rotation; 1 is a streak along the
particle's axis, its length the particle's height, widened on the screen's x only (rays, teleport columns,
lopingplains' fire pits); 2 lies flat on the world's horizontal plane, turned by the rotation (the gate spells' rune
circles, the ice and crocus paths, gears, water rings), fading into what it lies on over a quarter unit instead of its
size in the modern lighting. The depth offset (+0xd0) brings a quad that much nearer along the view.

An EQG model's emitters started by an animation (`.prt` trigger 1; docs/formats/particles.md) start each time it
starts or loops round on the actor (`ActorRenderer::drawn_playing`), after the entry's delay, and emit for about one
play (at most 1.5 s): the run dust at the feet, swings, casts.

A spell's stages play for every caster: the cast stage when OP_BeginCast arrives, the impact stage when OP_Action 231
lands the spell, on players and NPCs alike. Stock EQEmu sends the landing only to players within `Range:SpellMessages`
(75 units) of the struck spawn, but the begin cast within `Range:BeginCast` (200), so between the two a bystander sees
the caster's cast stage and no impact; the original client gets no more from the server. A server owner who wants
the impact seen from further raises that rule.

The server can also play an effect by its spellsnew.eff number on a spawn (OP_SpellEffect, OP_AddNimbusEffect;
`game/spell_effect_timers.h`): a nimbus, such as an item's or a buff's aura of orbs or flames, is one. Its emitters ask
for a forced 5,000 s, so it stays on the spawn until OP_RemoveNimbusEffect stops it (EQEmu sends that when the buff
fades) or the zone changes. An effect that arrives before its spawn's model has loaded (a server resending the nimbuses
as you zone in) keeps the point each emitter asks for and takes it once the model is drawn, at the spawn's place
until then; the original's spawn always has its points. EQEmu sends a nimbus only to those in the zone when it is cast: a player zoning in later
does not see it, on the original client either.

Hailstone's own: a spell that lands with no OP_BeginCast before it also gets the cast stage at its source, when its
first OP_Action arrives, so an item, weapon or augment proc looks like a cast of the same spell. Stock EQEmu begins
a buff's proc with an OP_BeginCast (cast time 0) but not those (`Mob::TryWeaponProc`); the original plays only the
impact for them (`EQPlayer::TriggerSpellEffect`). An OP_Action counts as a cast's landing for 3 s after its
OP_BeginCast's cast time; a bard song's pulses get no cast stage. Its source plays the spell's casting animation,
or for a spell without one the heal or nuke cast (docs/mechanics.md, option `cast_animation_fallback`). The proc's bolt or comet, in the modern lighting,
leaves at the landing as another's spell's does.

Hailstone's own (the original has one spell density and an "On For" filter, SpellParticleCastFilter, which this client
does not read): the Options window's Particles section has three more Off / Low / Medium / High choices, each scaling
the spell particle density for the stages played from then on (`game::spell_fx_source`, `display.spell_particles_self`,
`_players`, `_npcs`; High when unset). Your spells are the ones you cast and the ones that land on you; other
players' and NPCs' (pets and mercenaries included) go by the caster's kind, or by the struck spawn's when the caster
is not known.

## Radial flora

The EQG heightmap zones' grass, flowers and ferns (`assets/flora.h`, placed at load as docs/formats/eqg.md says) are
drawn by `render::FloraRenderer` while ShowGrass is on (/showgrass, the Options window's Radial Flora): the plants
within `kFloraReach` (400 units) of the camera, each one quad or two crossed ones standing on its foot (instanced, built
in `shaders/flora.vert`), its picture cut out at half alpha, its top swaying by its definition's sway. In the modern
lighting they are lit in the shaders as the zone around them is: per vertex the sky over the ground, the baked light
of the place (the ambient probe) and the zone's point lights, per pixel the sun or moon through its shadow map (one
filtered comparison: grass in a tree's shadow darkens with the ground), a little darker at the roots, then the haze.
In the classic lighting each takes the scene's ambient and half its sun. They take the terrain's colour with
`*INHERITVERTEXCOLOR`, write depth in the opaque part of the scene and shrink into the ground over the last fifth of
the reach. They cast no shadows.

**Bending** (Hailstone's own): a plant's top bends away from the feet of the spawns standing in it, out and down, the
more the nearer (`FloraRenderer::set_pushers`: the eight spawns nearest the camera within 150 units, each round itself
by 0.7 of its size, 3 to 12 units; a spawn more than 10 units above or below a plant does not bend it).

The reach is Hailstone's Flora Distance (`display.flora_distance`, the Options window's slider, the gamepad pack's
Grass distance): 400 to 1,200 units, 400 by default. Past 400 units a share of the plants is kept by the square of
the distance (`render::flora_keep`: a quarter at 800, a ninth at 1,200), the same ones from frame to frame, so the
far ring costs about what the near circle does. The tiles are filled nearest first, so when a frame reaches
`FloraRenderer::kMaxPlants` (262,144) it is far plants that are left out. A plant is 40 bytes to the GPU a frame.

## Zone flora

Hailstone's own, not the original's: the classic zones have no flora, and this gives them grass without changing
their files (`assets/zone_flora.h`, `display.zone_flora`). Their plants join the same `Zone::flora` the EQG zones'
radial flora fills and are drawn the same way (above; /showgrass hides them, Flora Distance sets their reach).

**Which rules.** A zone takes `flora/<zone>.txt` from the install folder (shipped) when there is one, else
`flora/default.txt` when the server calls it outdoor (OP_NewZone's time type 1, 2 or 5; PEQ marks some dungeons so,
which the cover test below keeps bare). A rules file is read line by line, and the first matching ground line wins:
- `donor <archive>`: an EQG archive in the player's game folder whose `RadialFloraDefs.rfd` and textures give the
  plants' shapes and pictures; several donor lines add up. The default takes `commonlands.eqg`'s, the revamped
  Commonlands. Nothing is shipped; without its donors a zone has no flora, and a kind whose donor (or whose picture)
  is missing is left out.
- `ground <glob> <density> [+green|+greenish] <kind>[:<weight>]...`: plants per 100 square units on the ground
  textures the glob matches, and their kinds by weight (a definition of the first donor that has it, or one donor's
  as `<archive stem>/<name>`); 0 keeps a texture bare. The classic ground textures blend
  grass into cliff, sand or a path within one picture, so `+green` keeps a plant only where the game's own texel
  under it is clearly green (`is_green_texel`: not the water's teal, mud's olive or a dungeon's near-black), and
  `+greenish` also takes dry and dark grass, for textures whose name says they are grass.
- `max_slope` (degrees), `scale <min> <max>`, and `include <name>` (another file's lines in place: a zone file puts its
  own exceptions first, then `include default`; East Commonlands adds desert plants on its sand that way).

The shared files turn down built and wet surfaces by name before anything else (`bare.txt`: roofs, walls, cobbles,
tiles, bridges, hedges, ship decks, water, lava), then take grass-named textures with `+greenish`, then turn down
floors, rock, stone and cliffs (`stone.txt`, after the grass lines so a grass and rock blend still grows on its grass),
then take anything else with `+green`. A `+green` texture with next to no green texels (`kMinGreenShare`) grows
nothing at all.

**Families.** Most zones take the shared Commonlands grass alone (`default.txt`). A zone of a climate with plants of
its own includes a family file, the same lines with more kinds from more donors:

| Family | Zones | Added, from |
|---|---|---|
| `forest` | the Faydarks, Kithicor, Nektulos, the Rathe Mountains, Misty Thicket, the Qeynos Hills, Jaggedpine, Toxxulia, Butcherblock, the Wakening Land, the Wakening's woods | ferns and small pines, `oldkithicor.eqg` |
| `swamp` | Innothule, the Swamp of No Hope | ferns, `innothuleb.eqg`; cattails and swamp grass, `moors.eqg` |
| `leafwoods` | the Feerrott | leafy bushes and ferns, `feerrott2.eqg` |
| `jungle` | the Emerald Jungle, Trakanon's Teeth, Firiona Vie, Stonebrunt, the Burning Woods | jungle ferns, `arcstone.eqg`; bushes, `feerrott2.eqg` |
| `plains` | the Karanas, the Commonlands, Lake Rathetear | flowering grass and wheat, `moors.eqg` |
| `kunark` | the Field of Bone, the Overthere, the Frontier Mountains, the Dreadlands, Lake of Ill Omen | flowers and shrubs, `oldfieldofboneb.eqg` |
| `luclin` | the Twilight Sea, the Grimling Forest, Tenebrous, Shadeweaver's, Hollowshade, Dawnshroud, the Maiden's Eye, Shar Vahl, Katta Castellum, Sanctus Seru | shrubs, plants and shade grass, `mistythicket.eqg` |
| `growth` | the Plane of Growth | tall flowering grass and ferns, `arcstone.eqg`; flowering grass, `moors.eqg` |


**Where.** On level ground (45 degrees by default) of a drawn, opaque or cut-out surface: not the invisible collision
surfaces, nor the translucent water. Not in water, lava or a zone line, nor on the water's surface (the region just
under the foot). Not where the zone's own drawn terrain lies overhead: a dungeon's ceiling, a roof, a bridge, an
overhang. The outdoor zones' invisible collision ceiling and cloud layers do not count, and neither do the placed
objects, so grass grows under the trees. Each plant takes the colour of the ground texel it stands on (the classic
outdoor vertices carry none).

**When.** The zone keeps the ground triangles that may grow plants, split into pieces of at most a quarter tile, binned
by 128-unit tile (`assets::ZoneFloraGrower`); the renderer grows the tiles within reach as they come into it, nearest
first and for at most 3 ms a frame, and lets go of those well past it. Each piece's plants come from its own seeded
sequence, so a tile grows the same plants every time and a zone costs memory by the reach, not by its size (South
Karana would hold some 6.5 million plants at once). Setting a zone up takes some 10 to 30 ms at load. A damaged
mesh is bounded: a triangle with a non-finite vertex or one over 100,000 units out (the zones lie within some 20,000)
neither grows plants nor covers any, the ground is cut into at most 2 million pieces, and the cover test holds at most
16 million cell references, a triangle over more than 1,024 of its 64-unit cells (at most 256 such) being tested at
every point instead. A triangle past those bounds is no cover.

They are looks only: collision, line of sight, the navigation mesh and picking do not see them.
`tools/florascan GAME_DIR flora ZONE...` lists, per zone, the rules it takes and the plants on each ground texture
(`--masks DIR` writes each texture with what `+green` turns down darkened, `--sample N` some plants' feet for the
viewer's `--pos`): the way to check a rules change over every zone.

## Spell bolts and comets

In the modern lighting a damaging spell is thrown, not just played on the target (`game/spell_projectiles.h`,
`render/spell_fx_renderer.h`, `app/spell_bolts.h`). Which spells, by the spell file: detrimental, instant direct
damage (a current-HP effect below zero, no duration).
- On one target (target types 1, 5, 9, 10, 11) it is a **bolt** from the caster's hands, or a **comet** that falls
  on the target out of the sky when its name says so (comet, meteor, star, hail...).
- On an area round a target (type 8) it is a **rain**: a comet on each target it hits, and two more round it.
- Heals, buffs, damage over time, lifetaps and point-blank areas have none.

Its **element** is its resist type: fire, cold, magic, poison, disease, chromatic or prismatic, physical (earth),
corruption; a name with lightning, thunder, static, spark and the like is lightning, whatever the resist. Each
element has its own colours and motion: a turbulent fireball shedding rising embers; a hard white-blue core with a
frost star in a misty trail, shedding falling shards; swirling violet rings; a crackling ball at the end of a forked
zigzag arc that jumps 24 times a second; a bubbling green or yellow glob shedding drips; a dark red rim round a dim
heart; colours turning; a glowing rock in grit. It ends in a burst (a flash, a ball swelling and tearing apart,
sparks thrown out); a comet's adds a shockwave racing over the ground. Every piece is light added to the scene,
soft where it meets the scene and fading with the haze, as the fire is; each projectile lights what it passes, and
its burst flashes (up to 6 lights).

**The timing stays the server's.** The damage, the messages and the landing (OP_Action 231) are the server's, when it
sends them; most of these spells have no projectile in the classic client at all.
- The player's own cast is known from its start, with the target it was aimed at: the projectile leaves in time to
  arrive as the cast ends (a bolt flies at 160 units a second, 0.18 to 0.55 s; a comet falls for 0.7 s), and bursts
  when the landing comes. Arriving first, it waits at the target up to 0.7 s, then fizzles (an interrupted cast's
  projectile does not leave at all).
- Another's cast names no target until it lands, so its projectile leaves then, and the spell's own impact
  particles wait for it to arrive (`SpellEffectCue::delay`).
- A spell the server flies itself (target type 1, the magician's bolts: an item type 20 missile, `game/missiles.h`)
  is drawn following that missile, in place of its model, and bursts where the missile ends.

The spell's cast and impact particles still play. Viewer: `--spell-bolt ID` throws a spell's projectile
across the view every 2.5 s (`--lighting modern`).

## The target ring

The ring at the current target's feet (`render/target_ring_renderer.h`, `shaders/target_ring.frag`; its rules in
`game/target_indicator.h`). Which ring shows, its colour and alpha (the default UI's TargetIndicator.ini section for
the target's con) and its radius (unless Max Size caps it, below) follow the original; how it is drawn and how it moves are Hailstone's own (the
original turns four UV-animated circles).

It is a decal on the scene's depth, drawn after `VkContext::begin_translucent` (the classic lighting, which has no
other use for the depth copy, takes it for the ring when there is one). The copy is taken before the grass is drawn,
so the ring lies on the ground and is drawn over the blades; traced over the blades themselves it broke into a coil. A box round the ring, from 1 + 0.6 radii
below the feet to as far above, is drawn without culling or depth test; the fragment shader keeps its far faces and
rebuilds each pixel's scene point from the depth copy. A surface less than 60 degrees from level within that height of
the feet takes the ring, so it follows the ground, slopes and steps, and anything standing in front of it hides it: a
leg or a wall is not level, a head is out of range. Elsewhere the ring is a level one at the feet, shown where it lies
in front of the scene (a target swimming or in the air, the part reaching over a ledge); behind the scene it shows only
with **Target Ring Through Models** (`display.target_ring_xray`, off by default), at a fifth of its strength.

The look: a soft band (half width 6 % of the radius, 0.15 to 0.5 units) with a wider halo, its core past 1 by the
**Glow** (`display.target_ring_glow`, 0 .. 100, default 50: up to 1.4 past, 0.7 by default) so the bloom finds it; dashes on an inner track, about one per unit and a half of radius (12 to 48), turning slowly; a
chevron outside the band where the target faces. With **Target Ring Cues** (`display.target_ring_cues`, on by
default) a second ring sweeps in from 1.6 radii over 0.35 s when the target changes, and while the player's auto
attack is on the dashes turn about four times as fast and the band pulses at 1.25 Hz. The haze fades it with distance
as it does the spell bolts.

Options > Display "Target Ring" also sets the colour for each con, Gray to Red, in the colour picker
(`display.target_ring_color.<Section>`, RRGGBB, the ini's section names; Default Colors erases them and the ini's
colours come back), and the **Opacity**, 25 .. 200 % of the section's alpha (`display.target_ring_opacity`, at most
opaque). The free-target sections are not the player's to colour (`game::apply_ring_colors`).

**Max Size** (`display.target_ring_max_size`, 4 .. 40 units, 0 off, the default) is a deliberate difference: it caps
the radius, so a giant's or a dragon's ring (AvatarHeight times the model's scale, tens of units across) does not fill
the screen. A ring smaller than the cap keeps its size; the band, dashes and chevron follow the capped radius
(`game::cap_ring_radius`). Off, every ring is the original's size.

## Loading screen

The classic client leaves its loading screen only when everything it shows is built. Here the zone, its objects and doors, the sky and the flora are built before the zone-in ends (`load_zone`, on the
main thread while the session's own thread keeps acknowledging), but some things are only built as the world is
first drawn: the spawns' appearances, built on a worker thread and put up a few milliseconds' worth a frame (`ActorRenderer::kBuildBudgetMs`, drawn undressed until then; a look no spawn has worn for five minutes is freed, mesh, GPU buffers and texture slots no surviving model references, so a hub's comings and goings do not keep every look until the zone changes: `render/look_eviction.h`), the
light shadow cubes one a frame, and the UI pack's in-game windows and the data the server sends with the zone-in
(buffs, stats, chat). Leaving the loading screen at the zone-in showed all of them arriving over the first second or
three.

So after the zone-in (`OP_ClientReady` sent; the server's side is done) the game runs and draws as usual behind the
loading screen (`app/zone_settle.h`, `ui::loading_overlay`, an opaque black backdrop and the loading panel over every
window in the foreground draw list) until at least 3 frames have been drawn with the UI pack's frame, no spawn in
view waits for its appearance (100 ms of them a frame meanwhile) and no shadow cube was built that frame; at most 15 s, so
a model that never comes cannot keep the player out. Meanwhile the keys, the pad and the mouse do not reach the world
or the windows, and the zone-in clocks (the input script's, the test drivers') start when it ends. The bar runs over
the network steps to the zone loaded (half), then over the settle. Each zone-in logs `zone-in: loading screen up N ms
(...)`.

Measured on a dev server, zone connect to the first frame shown, before and after: poknowledge 2.2
→ 3.0-3.3 s, qeynos2 1.6 → 2.0 s, commonlands (EQG) 4.6 → 4.9 s. Before, the first frames showed spawns undressed
for up to 3 s in poknowledge, a large EQG object missing for a frame in commonlands and the HUD without the
character's buffs and stats; after, the first frame shown matches those a few seconds later.

The zone build says what it costs as it goes, so a heavy zone can be read from the log. At debug level: `memory
checkpoint: zone load: <phase>: resident <N> MiB` (and private commit on Windows, and the GPU heaps) after the zone
data, the flora, the zone renderer, the flora renderer, the collision, the navigation and the ambient probe; and
`zone build: <phase>: <N> MB resident (+<T>s)` through the zone renderer's own build, the time counted from the start of
that build. At info level, once per zone: `zone renderer prepared: N textures (X MB), N vertices (X MB), N indices (X
MB)` for what the zone's meshes and textures took, and the settled zone-in's `memory checkpoint: zone-in: settled
appearances` lines. The numbers are the log's only; nothing is kept of them.

## Culling

`render::ZoneRenderer` merges the zone into one vertex and index buffer, one draw per material, and cuts each draw
into chunks with a bounding box when it builds them (`Chunk`, `Draw::first_chunk`). An opaque draw's triangles are
sorted by the 128-unit XY cell of their centre, one chunk a cell: their order shows nowhere, the depth test settles
what is in front. A blended or additive draw keeps its triangles' order, which a blended one shows where its
triangles cross, so its chunks are runs of neighbouring triangles that start afresh when the next triangle would
stretch the run's box past 128 units across; a placement appends its triangles together, so a run is mostly one
object. A plant's vertices count with the room the wind can take them (`sway_reach`: at most 2.5 x the bend plus
1.5 x the flutter, from the bounds of `wind_at` and `wind_offset`).

Each pass draws only the chunks whose box can reach its view (`render/culling.h`), and a run of neighbouring kept
chunks is one `vkCmdDrawIndexed`; a draw with nothing to show sets no state. The order of the draws, by pass and
then material, the water's place in `ZonePhase` and the push constants (animated frames, effects, liquid flags) are
as before; skipping chunks keeps the order of the rest.

- **The scene** (`draw`): the camera's frustum, the six planes of `view_proj`'s clip volume (`frustum_from`).
- **The sun's cascades** (`draw_shadow`): each cascade's own clip volume. The cascade's box already reaches
  `kShadowCasterReach` towards the light past its slice, for the casters out there, and the pipeline clips at its
  ends (no depth clamp), so the volume is exactly what can land in the layer.
- **The cover and water maps**: the square of the zone they see round the camera, through their depth range
  (`cover_box`). Their layers keep their depths between frames (nothing else draws into them), so each map is drawn
  only when it has changed (`cover_map_stale`): its view (x and y snap to its texels, its top to `kCoverStep`, 32
  units, with the depth range that much longer so it still reaches `kCoverAbove` over the camera and `kCoverBelow`
  under it), the zone (`ZoneRenderer::id`), the shadow map (`VkContext::shadow_generation`), the doors and objects in
  its box and where they stand (`ObjectRenderer::cover_signature`; the cover map only), or the map being off the
  frame before. Plants with swaying solid parts in its box (`ZoneRenderer::map_moves`, with wind) keep it drawn every
  frame, as before. Standing still in Halas in the snow, 5 of the 44 shadow calls a frame go.
- **The lights' cubes** (`draw_point_shadow`): the opaque chunks in the light's sphere, on the face's side of it.

The shadow passes need a material's texture only where it is alpha-tested (leaves, fences): `zone_shadow.frag` and
`zone_point_shadow.frag` sample it only then. So the opaque draws that are not alpha-tested and not flames
(`casts_untextured`, all the cover map draws) are merged once more at build time, one chunk a cell whatever the
material (`render/shadow_chunks.h`): their indices are copied, cell by cell in the same x-major order, to the end of
the index buffer, and each merged chunk's box holds its materials' boxes. The cascades, the cover map and the lights'
cubes draw these first, a run of neighbouring cells in one call, then the alpha-tested draws by material as before;
the water map draws the water draws as before. A cascade costs about one call per column of cells in view instead of
one per material and column (the Plane of Knowledge from the Library's door: 689 shadow calls a frame before, 84
after). A cell's box is the union of its materials', so a few more triangles go to the GPU and are clipped there; the
depth each texel keeps is the same (the shadow passes take the nearest whatever the order). The copies cost the
opaque draws' indices once more in memory.

The other casters are culled by their bounding spheres (as boxes, `sphere_box`) with the same tests: the spawns
(`ActorRenderer::draw_shadow`: each cascade's clip volume, within the shadow reach; the sphere round the model's origin
that the lights' cubes test them by) and the doors and ground objects (`ObjectRenderer::draw_shadow`: each cascade's
clip volume, the cover map's box). Each drew into every cascade before. In the scene pass a spawn is drawn only
when its sphere can reach the camera's frustum (`ActorRenderer::record`), and it is posed at all (its bones sampled,
blended and composed, `prepare`) only when it can show somewhere: in the view, or within the shadow reach while the
sun's or a light's shadows can take it (`actor_needs_pose`, from `set_pose_view`); its animation runs on either way
(`prepare_spawns`), as the mechanics read it. The particles (`ParticleRenderer::draw`) are
lit, sorted and built only when the sphere a quad may turn in, its half-diagonal round its centre, can reach the
camera's frustum (`particle_in_view`); the modern ones' lighting takes the frame's part once (`ParticleLight`, also the
plants').

The tests are conservative: a box is dropped only when it lies wholly outside one of the volume's planes. Frames
drawn with and without the culling (the viewer's `--frame-dt` holds the clock to the frame count, so the wind, water
and sky match) are identical to the pixel. The viewer's `--no-vsync` with `--frames N` logs the mean frame time and
how many zone triangles and draw calls the last frame made.

## Sky

In the modern lighting the plain skies (sky 1 normal, 2 desert, 4 cottony; `SkyRenderer::has_atmosphere`) draw an
atmosphere instead of their painted dome. `shaders/atmosphere.frag` draws a full-screen triangle at the far plane:
single Rayleigh and Mie scattering, 16 steps along the view ray and 6 towards the sun, the sun along the lighting's
path (`render::Daylight::to_sun`), with more haze in the desert. Ozone's absorption rides with the Rayleigh density
(`kOzone`): without it single scattering left a green-yellow band at the horizon and in the twilight sky. By night
there is a dark blue floor and stars on a cube-face grid (2 % of cells, about a pixel across, twinkling), turning
about the sky's pole. Near and below the horizon the sky goes into the haze, as before.

**The sky's path** (this client's own; the original's is not traced): the sun, moon and stars turn about a pole
standing 35 degrees over the northern horizon (`render::sky_pole`, world +X is north, -Y east; `game/coords.h`). The
sun rises due east at 6:00, stands `render::kSkyNoonElevation` (55 degrees) over the south at noon and sets due
west at 18:00, so the hours of day and night stay the classic ones while noon's shadows fall north instead of
straight down, and the light models shapes all day. The moon keeps its place relative to the sun (below), a little
towards the pole.

**The sky's light** (`render/atmosphere.h` `sky_light`): the same scattering model runs on the CPU (48 directions,
cached while the sun moves less than a thousandth) for the modern lighting's sky ambient (`Daylight::sky`: the
cosine-weighted hemisphere, a quarter desaturated for the clouds and the light scattered more than once, which the
model leaves out) and for the haze's colour (the sky a few degrees over the horizon, below). So the ambient is deep
blue at noon, paler and dimmer with a low sun and gone after dusk, where the night's floor (the stars, the moon's
sky) takes over, and the land fades into the colour of the sky behind it.

The modern cloud layer and the cloud patches thin out towards the horizon (the layer from 0.3 down to 0.03 of the
sky's height, where its painting is stretched into streaks), so the atmosphere's own horizon shows there.

The atmosphere draws its own sun and moon in place of the sky's sprites:

- **Sun:** a 0.9-degree disc, darker towards its edge, as bright as the sunlight that gets through the air (it reddens
  at the horizon and blooms).
- **Moon:** a 1.6-degree disc, grey with darker seas, going through its phases with the server's calendar
  (`game::SkyClock::moon_phase`: twelve months of 28 days, one lunar month a calendar month, new on the 1st and full
  on the 15th). It stands where the sun stood phase x 24 hours earlier, a little off the sun's path
  (`render::moon_direction`), so a waxing moon is in the evening sky, the full moon rises at sunset and a new moon
  passes beside the sun. The shader lights the side of its sphere facing the sun, so the phase comes out of where it
  stands; the dark side is faint with earthshine. It is reddened near the horizon, hides the stars behind it, and by
  day shows pale through the air. A faint halo grows with the lit part. The viewer and the client take `--moon P`
  (0 new, 0.5 full) instead of the calendar.
- **Blood moon:** the full moon of every third month (3, 6, 9, 12; `game::SkyClock::blood_moon`) is deep red with a
  wider red halo, the night sky's floor turns purple-red and the moonlight red (`render::kBloodMoon`). It is wholly
  red for the day and a half about full and fades over the half day either side; from the month of `OP_TimeOfDay`,
  so everyone on a server sees it on the same nights (a game month is 33.6 real hours: one about every four real
  days). When it comes the chat says "The moon turns the color of blood." `--blood-moon B` (0..1) sets it instead.
- **Northern lights:** on some nights (`game::SkyClock::aurora`: each calendar night, noon to noon, draws its own
  strength from its day and month; none on three nights in five, else 0.3..1), curtains of green light, violet at
  their tops, hang over the northern sky (+X, about which the stars turn), folding and drifting slowly, striped by
  rays, brightest at their sharp lower edge. The shader marches the view ray through 16 layers of a sheet of air,
  each pixel starting at its own offset. They show by night only, behind the cloud layer, and fade under a storm.
  `--aurora A` sets them. The value rides in the atmosphere's haze `a` (0 off, else 1 + aurora), the push block
  being full.
- **Shooting stars:** every 3.5 s a 55 % chance of one somewhere in the upper night sky, streaking about 12 degrees
  in 0.7 s, a bright head and a fading tail a pixel or two wide, hidden by the moon and the clouds.

God rays: `VkContext::set_sun_shafts` (`render::sun_shafts_for` places the sun on the screen) lets `shaders/post.frag`
march 40 steps from each pixel toward the sun over the depth. It adds the light of the open sky close to the sun,
fading with each step, so trees and walls in front of the sun leave dark gaps between bright shafts. They are strongest
with the sun on the screen and above the horizon, fade over half a screen past its edge, and go with a storm.

The cloud layer is drawn over it blended. Its clouds are keyed out of its painted sky by the red channel, and lit by
`render::cloud_light` (the sky's ambient and the sun): white by day, warm at sunrise and sunset, dark by night. The
procedural cloud patches take the same light, and the sun and moon sprites stay. The other skies (the planes',
Luclin's, red, grey) keep their painted layers.

## Weather

`OP_Weather` {kind: 0 rain, 1 snow, 0xff stop; intensity} plays out as in the classic client (`game::Weather` over
`game/weather_state.h`). Each frame, in the open (time types 1, 2, 5) with the weather on, the weather start runs first, then
the update starts a pending message, so the message waits for the end of its frame. The amount moves toward
intensity x 0.01 at 0.25 a second, and a different kind replaces the current one at once. `ApplyWeatherState` fades the
weather in over 64 frames (4 a frame to 0x100) or out over 43 (6 a frame); the frame after, it shows the start message
(rain 0x21d, 0x21c in zone 0x48; snow 0x21e) and turns the rain or wind sound on, or turns it off. Once faded in, rain
strikes lightning at once and then every 10 to 20 seconds, with a 30 % chance of a second strike within a second;
snow turns the wind on at those times instead. Each strike plays its thunder with the flash :
thunder1 or thunder2 (sounds 0x8f, 0x90) at random, and a second strike the other one (`Weather::thunder`). The fades
count frames, as in the classic client; their fog band is not applied.

The falling rain and snow are the classic precipitation manager,
implemented in `game/precipitation.h` and driven by `game::Weather` (the effect's kind, wind (0, 0, -1) and
intensity; updated every frame, in every zone type, with or without a sky):

- **Definitions:** `Resources/Precipitation/default.ini` (`<name>.ini` with `[Settings] Valid=1`, else default):
  `[Settings] <Kind>Falling<n>` / `<Kind>Splashing<n>` (n = 1..3; kinds Rain, Snow, Hail, Ice, Blood, Slime, Fire,
  OP_Weather's kind indexing them) name `[Falling<i>]` (MaxSpeed, Acceleration, Width, Height, Rate, Frames,
  Orientation, File) and `[Splashing<i>]` (Life, Chance, Width, Height, Rate, Frames, Orientation, File). The default
  file: snow falls at 8.04 units a second (accelerating at 32.15) as 0.15-unit `snowflake02` flakes tumbling through
  16 frames at 16 a second, facing the camera; rain at 64.3 as 1-unit `rain_drop02` streaks; a tenth of the drops
  leave a flat splash (`snow_hit` 0.12 units, `Rain_Hit` 1 unit) for a second.
- **The particles:** a pool of 7500. Each update (`0x10081860`) adds round(7500 x seconds x p) particles: at most 10
  new ones, the rest restarting from the columns of landed ones (40 to 140 units above their own ground, at their
  first speed). A new one starts 40 + 100 r1 r3 above the camera and 200 r1 away along the camera's heading turned by
  up to 128/512 (three quarters of them) or 128 to 256/512 the rest, at (1 - r1)^2 of MaxSpeed. Its ground is the first
  surface a line from 1200 units above it to 500 below meets (`CollisionMesh::first_hit`: transparent surfaces
  ignored, passable ones hit, back faces skipped), so drops stop on roofs and tree tops and never reach the ground
  under them. Here back faces below the drop are hit too (unlike the classic client): a roof with nothing over it, such
  as the East Commonlands tunnel's under a mountain with no top, would otherwise let the rain through to the floor.
  Only below it: a zone's ceiling facing down over the sky (East Commonlands' at z 719) would otherwise be the ground
  of every drop, which would land as it started, and no rain or snow would be seen at all. A drop that lands on ground whose normal z is at most 0.8 never splashes again. Landed columns farther
  than 200 units from the camera are let go. p is the server's intensity x 0.01 (1..10 gives 0.01..0.10), moving at
  0.25 a second, so intensity 5 is 375 drops a second.
- **Colour:** each particle a random blend of the sky's two precipitation colours for its kind (CSky +0x38: the
  current colour map's rows 4 + 2 kind and 5 + 2 kind, column 31, alpha included; `game::SkyState` from
  `Resources/Sky`), so snow is white by day and grey by night.
- **Drawn** (`render/weather_renderer.h`, `shaders/precipitation.vert`) as the classic renderer builds them: only beyond 2.5 units in front of the camera, at least a pixel wide, orientation 0 facing the camera,
  1 a streak standing up from the drop, 2 flat; 16 frames a 4 x 4 atlas, 4 a 2 x 2, frame trunc(Rate x age);
  texture x colour, alpha blended, depth tested, no depth write. The lightning's flash brightens them.
- **Drawn, modern** (`render::kModernPrecipitation`, not the original's look): the original's rain, 1-unit streaks a
  pixel wide and a few hundred of them within 200 units, can hardly be seen at today's resolutions while the ground
  shows the rain plainly. The modern lighting stretches each upright streak over the distance its drop falls in
  0.08 s (about 5 units at full speed, as a camera's motion blur), draws it at least 1.5 pixels wide, and has the
  precipitation manager start three times as many drops (its spawn rate; the 7500-particle pool and the column reuse
  are the original's). The snowflakes, 0.15 units across and in each frame of their atlas a thin outline (about 8 %
  of it opaque), are a pixel or two a few units off, where the texture's small mips average the outline away: the
  modern style draws a facing particle at least 2.5 pixels across both ways, and below 8 pixels of its own size fades
  it into a soft round dot (fully a dot at 4), so the falling snow shows as specks. The classic lighting keeps the
  original's.

- **The world (modern, `WeatherLook`):**
  - **Transitions:** the original's precipitation reaches the server's intensity within a second; the look eases it.
    The storm (`WeatherLook::storm`, the precipitation x 10 eased: about 36 s to gather, 75 s to clear) holds the
    sky: it dims the sun by up to 70 %, greys the sky light and the atmosphere, spreads the cloud layer over the sky
    and brings in the weather's fog band (Haze, below). The drops drawn thicken and thin over about 18 s either way
    (`WeatherLook::precipitation`, given to the precipitation manager as `Frame::eased_intensity` in place of its own
    p; the classic lighting keeps the original's), and a change from rain to snow takes a few seconds
    (`WeatherLook::snowing`). Viewer: `--weather-age S` shows the weather S seconds after it began,
    `--weather-stopped S` S seconds after it stopped.
  - The ground gets wet over 30 s of rain and dries over 2 minutes: darker, with a Fresnel reflection of the sky,
    most on level surfaces. The doors and ground objects get wet the same way (`shaders/object.frag`); the
    characters get a lighter sheen (`shaders/actor.frag`).
  - Puddles fill the level ground's low spots over 90 s of rain (slow noise picks them; up to about a quarter of the
    ground in a long storm) and dry over 4 minutes: the ground under them darker, the sky in them nearly a mirror's.
    While it rains, drops ring them (`rain_ripples`: jittered cells about a unit across, more drops the harder it
    rains), the rings fading out where a pixel spans more than a tenth of a unit.
  - Snow lies on level surfaces in noisy drifts, covering over 2 minutes and melting over 5: on the ground and on the
    doors' and objects' roofs and ledges, not on the characters (drifts fixed in the world would slide over them).
  - All of it is in `shaders/weather.glsl` (`weather_surface`), fed `SceneLighting::weather` (`render::set_weather`:
    wet, snow, the rain falling, the puddles).
  - The look belongs to the zone: entering another zone starts it over, dry and with a clear sky
    (`WeatherLook::update`'s zone id), so a storm's snow, wet ground and sky do not follow the player in.
  - A lightning strike (`render::lightning_flash`) flickers: a flash, a dimmer one 70 ms on and a third at 190 ms,
    each gone in a few frames, over a faint afterglow of half a second. It lights the sky, the clouds, the ambient
    and the haze (`lightning_haze`).
- **Viewer:** `--weather rain|snow N` shows it already settled, the particles falling for the last 30 s.

Known gaps: without shadows (`--shadows 0`) there is no cover map, and the ground under roofs gets wet and snowy. The
cover map reaches 256 units round the camera; beyond it everything counts as open sky, so the ground under a
roof further away than that gets wet and snowy, and that edge moves with the camera.
Trees give the ground no shelter (the falling drops do stop on them).

## Wind

In the modern lighting the placed plants move in the wind (`render::wind_at`, `SceneLighting::wind`): a breeze
veering slowly about the south-west in fair weather, harder and gustier with the storm. `ZoneRenderer::build` marks a
placed object as a plant when it has a masked material and its name, or a material's or texture's, says what it is
(tree, pine, palm, bush, fern, grass and the like). Each of its vertices gets a sway (`GpuVertex::sway`): how far it
bends, as the square of its height above the plant's foot, up to 4 % of the plant's height, and how much its leaves
(the masked parts) flutter. `wind_offset` in `shaders/lighting.glsl` leans the plant with the wind, rocks it as gusts
roll across the zone in the wind's direction, and shivers the leaves. The zone and shadow vertex shaders both apply
it, so the shadows move with the leaves.

## Zone lights

The zone's lights (lights.wld) light zone geometry per pixel in the modern lighting (`shaders/zone.frag`
`zone_point_lights`). `render::build_light_grid` sorts them into a grid of 64-unit cells over the world's XY plane
(at most 24 lights a cell, the nearest to its centre). The zone renderer binds the lights, cells and indices as
storage buffers (set 1, bindings 1 to 3). Each light gives two terms:

- half the MPL vertex term (`game::point_light_term`, colour x 0.5, each light saturating), added with the baked
  vertex colour in display values as before, so interiors keep the balance their builders gave them (scaled with the
  baked colour's lamp share by night, see Modern lighting);
- a pool in linear light: colour x 3 x facing x `light_pool` (`shaders/lighting.glsl`): the inverse square past a
  core of a fifth of the radius (8 to 16 units), windowed to zero at the radius. Bright at the centre, so a torch
  lights the ground and walls round it, and a big city light (70 to 200 units) pools round its lamp instead of
  lighting a whole square evenly. Characters and objects take the zone's lights with the same falloff.

Warm lights (torches, fires) flicker by up to 12 %. Characters and objects take the nearest lights from the scene's
light list as before.

## Light shadows

Modern scene lighting gives every visible carrier its light (`game::carrier_light_value`). The player's comes from its own inventory (the brightest worn or general-slot item), as before: stock EQEmu sets a client's spawn light only when it builds the spawn packet, ignores the client's own SpawnAppearance type 5 and sends type 5 only for NPCs, corpses, bots and mercenaries. Every other carrier uses its spawn record's light, changed by SpawnAppearance type 5. The existing carried-light palette sets its color and radius; dynamic lights remain switchable. Other carriers' lights follow their displayed body position. The local player's carried light retains its dedicated shadow slot; other carriers join the nearby point-light list.

The zone's lights, the light the player carries and the spells' cast shadows in the modern lighting
(`render/point_shadows.h`), so a torch no longer lights the far side of a wall. The classic renderer has no shadows at all.

- **Which lights:** each frame `PointShadows::update` picks the lights within 360 units of the camera (to their
  sphere) that light the most round it, brightness x reach over distance; up to `display.light_shadows` of them
  (8) less those kept for the lights that move (below). A light keeps its slot unless another scores half again
  better, so neighbours do not trade places. The shadow fades in over a quarter second once drawn and out over the
  last quarter of the range.
- **The cubes:** each slot has a cube of six depth faces (`VkContext::begin_point_shadow`, a second depth array
  image, `display.light_shadow_size` texels a side), holding each caster's distance from the light over its radius.
  The face mapping is this client's own (+x, -x, +y, -y, +z, -z; the other two axes over the distance along the
  face's), covering a little more than 90 degrees so the filter's taps stay on the face; the shaders that draw and
  read it share `point_shadow_face` and `point_shadow_clip` in `shaders/lighting.glsl`. Casters within 3 units of
  the light (its lantern, the torch's head) are left out.
- **Drawn once:** the zone and its lights stay put, so a slot's cube of the zone is drawn when the light gets the
  slot (at most one a frame, `kPointShadowBuildsPerFrame`: a cube is six passes over the zone round the light, the
  frame's longest part when lights come into range; two a frame before, so the last of a group of new lights now
  shows its shadow a few frames later, under the quarter-second fade-in) and kept: `ZoneRenderer::draw_point_shadow` draws the opaque geometry's 128-unit
  chunks ("Culling" above) that reach the light's sphere and the face's side of it, the plants at rest. When characters stand in a light's reach, the cache is copied into the
  slot's live cube each frame and they are drawn over it (`ActorRenderer::draw_point_shadow`); otherwise the
  shaders read the cache.
- **Placed objects:** doors and ground objects swing and come and go, so they are drawn as the characters are: when
  one's bounding sphere (round its vertices, grown by its matrix's largest scale; `render/caster_bounds.h`) reaches
  a shadowed light's sphere, the slot goes live and `ObjectRenderer::draw_point_shadow` draws the opaque and
  alpha-tested parts of those within reach and on the face's side over the cache (`render/point_shadow_pass.h`
  takes the object renderer beside the actor renderer). In a town that keeps most slots near doors live: a cube
  copy and a few small draws each a frame.
- **Reading:** the zone, character and object shaders find a light among the shadowed ones by its position
  (`point_shadow`): a normal offset growing as the surface turns from the light, then 3 x 3 hardware-filtered
  comparisons. In the zone a blocked light loses its pool and its MPL share, and the baked vertex colour, which
  counts every light with nothing in its way, gives back what the light would have added there
  (`game::point_light_term`'s weight), never below a third of it.

- **Lights that move:** the light the player carries and the spells' brightest have slots of their own
  (`render::dynamic_shadow_slots`: one for the carried light from 3 slots up, one for spells from 5, two from 7; so
  the default 8 leaves 5 for the zone's lights, and fewer slots all go to the zone). They are kept for them even when
  nothing shines, so a spell coming and going does not evict a zone light's cache. Each frame
  `PointShadows::set_dynamic` takes them exactly as the scene's light list has them (the shaders match by position):
  the carried light at chest height, then the spells in range scored as the zone's lights are. They have no cache:
  their live cube is drawn whole every frame, the zone chunks in reach and the characters, so they cost the most (the
  zone's chunks in the light's sphere are found once a frame for its six faces, `ZoneRenderer::point_reach`, and each
  face tests only those for its side; a static cube's build does the same). The
  player is left out of its own light's cube (its body surrounds it); a
  spell's caster is not known where the light is made, so a spell's cube leaves out every character whose bounds hold
  the light instead (the caster's hands, the target's body). The zone's dynamic-light loop, characters and objects
  read them like any other shadowed light.

Known gaps: plants' shadows from the lights stand still. Steady cost is about
a quarter millisecond a frame with eight lights (RX 9060 XT, 1280 x 720).

## Water and lava

The modern water (`shaders/zone.frag` `modern_water`) draws on every water surface. In classic zones a level,
passable triangle (WLD 0x10: every classic water and lava surface is one; a beach's sand or a dock lying just under the
water's top is solid and stays ground) counts as one when a water region (WT, SL, VW, W2, W3) lies just under it and
not just over it; and so does one of the same material at the same height where a dry region cuts into the water,
such as a zone line's region over the sea (North Ro's line to Oasis) or the strip by a zone's edge
(`render::classic_liquid_triangles`, by the BSP regions); in EQG zones, the Water effect counts.

- **Colour:** the water's own colour comes from the texture's average (its last mip, with 30 % of the texture itself)
  or the EQG effect's two colours (both black, as bloodfalls' blood: the diffuse map's average).
- **Through the water:** the floor shows through by the depth copy. Each colour is absorbed with depth (more for the
  colours the water is not), and the view is bent by the waves.
- **Reflection:** a screen-space march over the depth copy (40 steps and a 5-step refine), fading at the screen's
  edges. Where it hits nothing: the sky as the frame drew it in the reflected direction, when the screen shows the sky
  there (`screen_sky`: clouds, sun, stars, aurora mirrored), else a sky from the haze's colour at the horizon deepening
  overhead to the sky light's hue, with the sun's glow round it (`sky_reflection`). An EQG water whose environment map
  is a "noswap" cube (the original keeps those instead of its sky cube: a cave's, a volcano's, a day sky; about 100
  zones) reflects that map instead of the sky: `assets::decode_cube_as_octahedral` lays the cube out as one
  octahedral 2D map at load (the game's cubes stand +Y up). The EQG reflection colour tints what is reflected. Fresnel
  (0.02 at normal incidence) mixes it in.
- **Waves:** twelve waves from 18 units down to half a unit move the normal, spread up to about 70 degrees either
  side of the wind (a light breeze without one; the wind's strength raises them), a few running across it, each with
  its own phase and all about as steep, so no one direction rules. The plane they run over is warped up to ten units
  by slow noise, so crests bend and break up instead of running in ruled lines; the short ripples gather in drifting
  gust patches and calm between them; two layers of drifting noise add the fine chop. Each fades once it is under
  four pixels long. The sun's glint is bright enough to bloom, and breaks into glitter rather than a grid.
- **Foam:** within 4 units of depth where the floor rises towards the surface (a beach, a bank, the foot of a
  wall; a shallow pool with a level floor stays clear), broken up by drifting noise, fading out by 160 units away.
- **Falls:** an EQG water surface steeper than about 53 degrees (bloodfalls' falls are RegionWater meshes) is drawn
  as a thin sheet running down (`falling_water`): rippled by the effect's two scrolling normal samples, streaked by
  its diffuse map, reflecting by Fresnel in the material's tint, a little of what is behind showing through.
- **Edges and below:** a soft edge over the last half unit of depth. Seen from below, the sky shows through Snell's
  window overhead (the light from above bent into a cone about 97 degrees across), with the sun bright in it. Outside
  the window the surface mirrors the water itself.

Under water (modern, with shadows on):

- **Water map:** `render/cover_map.h` draws the water surfaces alone from straight above into the shadow map's sixth
  layer (`kWaterLayer`), in the cover map's view, for a zone with water, whenever the view changes ("Culling"). The shaders read its depths
  (the shadow set's binding 3, `water_depth` in `shaders/lighting.glsl`), so any point knows how deep it lies under
  water.
- **Caustics:** under the water the sun's light is absorbed with depth and gathered into a drifting web of bright
  lines (`underwater_light`, two ridged noise fields over a warped plane, taken where the light entered the surface):
  0.3 of it between the lines, up to about 4 times on them. The sky's light fades with depth too. This covers zone
  geometry, characters and objects, and shows through the surface from above.
- **The water round the camera:** with the camera in a water region, the haze becomes the water
  (`render::set_underwater`). Its colour is the daylight's sky and sun in a blue-green medium, as thick at every
  height and reaching 220 units, and on the way red is absorbed first, then green, then blue. The scene clears to
  that colour, and `ZoneRenderer::draw_backdrop` fills the open water behind everything (`shaders/underwater.frag`),
  carrying the surface overhead on past its polygons.
- **Shafts:** `underwater_shafts` marches 8 steps along the view (jittered per pixel). Each step is lit as much as a
  slow pattern on the surface above lets the sun in along the ray to the sun, fading with depth and distance, and
  strongest looking towards the sun. Past the water map's reach the surface is taken to go on level.

Lava (`modern_lava`): the classic LA surfaces; EQG RegionLava (and Lava2) whose texture is fire-coloured or whose
name says lava, magma or molten (the class also draws energy, slime and mist, which keep their own colours and glow);
harbingers' RegionLavaH walls (its Fresnel bias of 1 makes the original opaque; the diffuse map is molten rock). The
heat comes from the texture's brightness, churned by two layers of slow noise drifting with the flow: plates of dark
basalt break and close over molten pools, hot seams run between them, and the whole throbs slowly out of step. The
molten parts glow from deep red through orange to yellow, past white so they bloom; the crust is lit as rock and
warmed by the heat under it. A RegionLava class's crust mask (Diffuse0's alpha) still marks its molten layer.

Waterfalls (RegionWaterFall, modern): the class reads its colour and its alpha at two scroll rates, so the colour was
often read where the texture is clear (black) under a sheet the alpha says is there; the colour is now taken where
the texture shows (each sample weighted by its own alpha, else the texture's average), a little brighter where the
sheet is thick, with a faint sky sheen at grazing angles.

EQG textures with a `<texture>.txt` sidecar (frame count, ms a frame, the frames) animate on the zone's clock as the
classic animated textures do (`eqg::parse_texture_animation`). A heightmap tile with no DAT layers (scorchedwoods has
one) takes the base ecosystem of the nearest painted tile, laid by its own heights and slopes; the original leaves a
hole there.

## EQG surface maps

EQG zone materials of the opaque, Alpha and Chroma classes name a tangent-space normal map (`e_TextureNormal0`,
uncompressed A8R8G8B8 `_n.dds` files) and, by their shader, a specular or reflective term. The classic renderer binds the
normal map only with bump mapping on, and its multi-pass lights are what it bumps; the
modern lighting always uses them (`shaders/zone.frag` `mapped_normal`, `surface_shine`):

- **Normal map:** uploaded as data (mips filtered linearly, not as colour). The meshes carry no tangents, so the frame
  comes from the screen derivatives of the position and the texture coordinates; the map's green runs along +v, as in
  Direct3D's (and 3ds Max's) maps. The bumped normal feeds the whole modern light: the sun and its shadow, the sky
  hemisphere, the zone's and the dynamic lights.
- **Specular** (shaders whose name holds `CBS`, `SB`, `GB` or `Full`): a Blinn-Phong highlight of the sun at
  `e_fShininess0` (40 when unset), as strong as `e_fGloss0` (0.2), in the vertex's share of the open sky.
- **Reflection** (`CE1`, `CBSGE1` with `e_fEnvMapStrength0` > 0, read doubled): the sky
  (`sky_reflection`) in place of the material's environment map, weighted by Fresnel.

The classic lighting draws them as before (the diffuse texture by the lit colour).

Alpha-tested materials (trees, grass, fences, Chroma/Masked) draw in the opaque pass with alpha-to-coverage when the
scene is multisampled: the pipelines set `shaders/coverage.glsl`'s `kAlphaToCoverage`, and the alpha test becomes a
ramp about one pixel wide around the reference, capped at 0.05 of alpha so far minified textures do not leave
specks against the sky.

## Modern lighting

`render/scene_lighting.h` (`daylight`, `make_scene_lighting`) and `shaders/zone.frag`, `actor.frag`, `object.frag`:

- **Daylight**: the sun follows the sky's path (Sky: up at 6:00 in the east, 55 degrees over the south at noon,
  down at 18:00 in the west); its colour
  comes from the elevation through an air-mass model (white overhead, orange to red low), up to 3.2 at noon. By
  night the moon (`render::moon_direction`), `render::kFullMoon` (0.2-0.36) when full and up, falling steeply with
  the phase (0.125 + 0.875 x the lit part squared: an eighth at the new moon) and red under the blood moon. The
  moon also lights the night sky's ambient by as much. With the moon down there is no direct light, and more
  starlight in the ambient (a moonless night stays barely readable). A sky/ground hemisphere ambient: by day the sky
  model's own light (Sky), by night a dark blue floor.
  Indoor zones (time types other than 1, 2, 5): no sun, a dim neutral ambient.
- **Zone geometry**: the WLD vertex colour, the special ambient and the per-vertex lights.wld sum (the classic MPL
  term) add in display values and are converted after the sum, so interiors keep their builders' brightness; the
  vertex alpha (the share of sky light the classic region shader gives the vertex) weighs the hemisphere ambient
  and the sun.
- **Night and the baked colour**: the vertex colour already holds the builders' ambient, sun and lamps, so left whole
  it lights every surface evenly at any hour (the classic renderer does that). The modern light splits it per pixel: its
  lamps' part is what the zone's lights give the pixel as `game::point_light_term` weighs them (less what their
  shadows block); its rest is the remainder, never below a third of it. `Daylight::baked` and `baked_lamps` (the
  scene block's `sky.w` and `ground.w`) scale them: both 1 by day, so the day looks as baked; from the sun 15 degrees
  up to 6 below the horizon they fall to the night's 0.3 (0.45 under a high full moon) and 0.35. The half MPL term
  falls with the lamps' part, so a lamp is not counted three times over (baked, MPL, pool) and the pools carry the
  light round the lamps, the moon and the sky the rest. Indoors, where the baked colour is the only light, 0.5 and
  0.7 at any hour: moody, still playable. A storm takes up to a quarter more off the rest.
- **Also:** alpha-tested plants wrap the sun's light. Water gets a sun highlight. The dynamic lights (the light the
  player carries, the spell effects') are added per pixel, shadowed (Light shadows).
- **Characters and placed objects**: the hemisphere ambient, the sun (wrapped for bodies), the special ambient and
  the zone's lights near the camera per pixel, never below a small floor. And the light of the place they stand in
  (`render/ambient_probe.h`): the zone's vertex colours averaged round the player (24-unit cells, two cells each way,
  nearest first, eased over half a second; `SceneLighting::probe`) give them the zone's baked light as the zone
  keeps it (`Daylight::baked`) and scale their sky light by its share there (at least a quarter). Before, a
  character inside a building or a dungeon took the open sky's light and stood out bright against the walls. One
  probe serves every character and door in view (the player's place); distant ones in another light are in the
  haze. At character select and creation the probe is the model's place, not the camera's.
- **Character select and creation**: the model stands in clz's tree shade at noon and read nearly black, so the
  modern lighting adds a key light for it (`game::char_select_key_light`): a warm point light on the camera's side,
  above the head and to the left, as far out as the race's camera, unshadowed. Not the original's (the sun alone).
- **Tinted baked light** (`baked_to_linear` in `shaders/lighting.glsl`): the zone's vertex colours, special ambient,
  MPL term and lights' colours were tinted by their builders on a display. Converted channel by channel they grow
  far more saturated in linear light (befallen's grey-blue walls went a vivid pure blue), so 70 % of the conversion
  scales the colour by its grey's (the balance as it was seen) and 30 % converts it as is.
- **Uncoloured meshes**: a zone mesh whose vertices are all the loader's opaque white (an EQG placement without
  lighting data) takes no baked light in the modern lighting: it is lit by the sky, the sun and the lights, as the
  doors are; its white had added a full display white, glaring beside the ground.
- **Spell light** (`game::particle_lights`): the glowing spell effects light what is round them, up to 8 lights of
  the 32. An emitter glows when it is additive, unless its texture is smoke, dust, rock, cloud, water or darkness.
  An alpha-blended one glows when its texture is fire or energy (flame, fire, flare, explosion, electric, spark,
  glow and the like). Each glowing emitter is one light at the centre of its particles, weighted by their opacity
  and size, so sparks under 3 units count less than a sheet of flame. Its colour is their tint times its texture's
  average colour (`ParticleRenderer::texture_color`). Its strength follows how much of the effect is alive, and it
  reaches 40 to 100 units as the particles spread. Emitters within 12 units merge.

- **Shadows** (`render/shadows.h`, `render/vk_context.h`): four cascades out to 1200 units (mostly logarithmic
  splits), each an orthographic view of its slice's bounding sphere snapped to whole texels; a depth array image
  (D32), drawn by `ZoneRenderer::draw_shadow` (opaque and alpha-tested draws, no face culling, the chunks in the cascade's volume, slope-scaled bias) and
  `ActorRenderer::draw_shadow` (the spawns `prepare_spawns` posed before the cascades; `draw_prepared` draws them in
  the scene) and `ObjectRenderer::draw_shadow` (the doors and ground objects `prepare` placed, opaque and
  alpha-tested parts, no culling; `shaders/object_shadow.*`);
  sampled with a 1.5-texel normal offset and 3 x 3 hardware-filtered comparisons, fading out over the last 15 %. The
  sun term of the zone, character and object shaders is multiplied by it.

- **Ambient occlusion** (`shaders/ao.frag`, `shaders/ao_kernel.glsl`, `render/vk_context.h` AoView): after McGuire et al.'s scalable AO, on
  the resolved depth: view positions rebuilt from the reverse-Z projection terms, the normal from the nearer
  neighbour on each axis, 12 samples on a spiral within 4 world units (more smears the ground under characters) (at most a tenth of the screen height),
  turned by a 4 x 4 ordered pattern; each sample takes the bounded angular kernel (cubic distance falloff times the
  sample's horizon above the surface), and at close range the kernel's world-space radius shrinks to the pixel cap's
  own world size, so near samples cannot darken out of proportion. An angular bias of 0.1 keeps almost coplanar
  samples from shadowing themselves. Within 12 units of the camera the AO fades out, to none at 4 (`ao_close_fade`):
  there the capped kernel is only about a tenth of the depth across, small enough to find each fold of a low-poly
  face (an erudite's brow groove, the cheeks and eye sockets) that its smoothed normals hide, which drew dark lines
  over a face seen up close. Contact shading within arm's reach of the camera is lost with them. The samples' shade is scaled by AoView's intensity 12 over the 12 samples: the
  bounded kernel weighs a mid-range sample less than the old one (whose weight grew as 1/|v|, which blotched the
  near ones black), and 12 keeps a contact corner's shade near the old depth (tests/lighting_test.cpp). The post pass averages the 4 x 4 block weighted by depth and darkens the scene
  by it (strength 0.85) before the exposure. Applied to everything, not to the ambient alone (there is no depth
  prepass), so it is kept to contact shadows.

- **View distance and haze** (`render::modern_view`, `set_haze_linear`, `apply_haze` in `shaders/lighting.glsl`): the far
  plane is the classic far plane x `display.view_distance` (not under water, in lava or thick-fog regions, which keep the
  classic one), and the fog distances move with it, so characters are drawn out to it. Instead of the exp2 fog wall,
  a haze with optical depth 3.5 (d / far)^2, thinner above the camera (scale 600 units), towards the zone's fog
  colour with a glow around the sun; the sky blends into it within about 7 degrees of the horizon. The post pass
  blurs up to 1.75 pixels from a quarter of the way out to the far plane. Mouse picking keeps the classic yon.
  - **The glow** is the sunlight the haze scatters, so it is no brighter than the haze's own colour allows
    (`render::haze_glow`): full in a day's fog (linear luminance 0.05 and up), less in a darker one. The fog
    colour darkens by the classic hour tables, which lag this sun: at 5:50 nektulos's fog is 19/14/10 while the sun
    is 14 degrees up at 2.8. An undimmed glow turned everything in the haze towards the sun one flat tan, against the
    near-black fog behind it.
  - **Its colour outdoors** (`render::haze_target`, `haze_sky_color`): the zone's fog colour drawn towards the sky's
    own colour a few degrees over the horizon (the sky's light, above), 70 % under a drawn sky and 35 % without one
    (gfaydark, toxxulia), less as the zone's fog thickens (none with the fog ending at 400 units, all at 1200), so
    nektulos's murk stays its builders'. Never more than 2.5 times the fog's brightness, so a zone with a dim fog by
    day gets no glare. Before, every outdoor zone's distance went one flat lilac grey (200, 200, 220) under a blue
    sky, with a hard line where the sky's horizon met it.
  - **Between places** (`render::HazeBlend`): the haze's colour, length and reach (`extend`: the original's far plane
    or the extended one, `modern_view(view, multiplier, extend)` for the part way) ease over about 1.5 s, so walking
    into lava or a thick-fog region, or the weather changing, does not pop. A zone-in starts afresh, and coming out
    of the water the haze is there at once. The original's area fog (AFG region boxes over `weather.ini`
    `[AreaFog-<zone>-<name>]`, `game::area_fog_blend`) is not applied: neither the RoF2 nor the live install has an
    `[AreaFogSettings]` entry.
  - **The weather's fog band** (`render::weather_haze`): the storm pulls the haze in by up to half (more for snow),
    greys it (cooler and a quarter darker under rain, whiter under snow), and the lightning lights it.
  - **Where no sky is drawn** (sky 0, as nektulos, or SetFog's sky flag off, and indoors), the haze also fills the
    background (`ZoneRenderer::draw_backdrop`, `shaders/underwater.frag`): its colour and its glow as at an infinite
    distance, so the gaps between the leaves and the world past the far plane match the hazed trees in front. Before,
    the scene cleared to the plain fog colour, and the leaves' cut-outs showed as hard, stair-stepped edges between it
    and the glowing haze.

Textures are decoded from sRGB in the shaders (after filtering, a small error accepted for now).

## Colour grade

The modern lighting's frame is graded in the post pass (`render::color_grade`, `VkContext::set_color_grade`,
`shaders/post.frag` `mood`): a tint of the linear scene before the tonemap (its luminance kept at 1, so the exposure
does not move) and a saturation after it. Subtle by design:

- outdoors, warm (1.07, 1, 0.9) round sunrise and sunset, cool (0.9, 0.97, 1.1) and a fifth less colourful by night,
  5 % more colourful by day;
- indoors, a little less colour;
- a touch (7 %) of the zone's fog hue: unrest's purple, feerrott's green, lavastorm's red;
- a storm greyer (up to 22 % less colour) and a little cooler.

`display.color_grade` (the modern Options window's Lighting section, Colour Grade) turns it off; the viewer takes
`--no-grade`. The classic lighting is never graded.

## Neighbour zones

In the modern lighting the zone past an outdoor zone line is drawn in the distance, so East Commonlands looks onto
Freeport's walls instead of ending in haze. It is scenery only: nothing collides with it, no spawns stand in it, and
zoning and what the server sees are unchanged. The classic client drew nothing past its zone's own geometry (its zones end
in filler: the ground, hills and walls their builders put past each line, often a stand-in for the neighbour).
`game/neighbour_zones.h` holds the arithmetic, `app/neighbour_zones.h` the loading and drawing.

- **The lines**: the zone-line regions of the zone's BSP (`assets::ZoneRegions::leaf_cells`: each leaf's convex cell,
  its split planes cut from the zone's box) grouped by where they lead: an absolute tag's zone, or a reference tag's
  zone point from OP_SendZonepoints. Each group's seam faces the zone edge it is nearest (+-x or +-y: zones are not
  rotated; a line box at least twice as long across as it is thick faces along its thin axis, as a tunnel mouth near
  a zone's corner does); its face is the cells' inner side, where the player crosses.
- **The offset** (neighbour coordinates + offset = this zone's): first from the targets, per axis 0 where a target
  keeps the coordinate (999999), else the median over the targets of the face (travel axis) or the lines' middle (side
  axis) minus the arrival. Once the neighbour is loaded, its own zone line back (its zone-line cells near the arrival,
  behind it) lines the two faces and middles up when they agree with the first estimate within 120 units, and z
  matches the ground just either side of the seam at nine points across it: of the floors each side finds there,
  the difference the most points agree on within 16 units (ties: the one nearest the first estimate, whose z is only
  the arrival's spawn height), from at least half the points with two and a third of them agreeing. The floors are
  the ground as seen (`CollisionMesh::ground_below`: upward-facing, drawn surfaces; not a tunnel roof's underside nor
  an invisible plane). ecommons -> freportw: (2.3, -2399.4, -26.5); freportw -> ecommons: (0, 2399.4, 26.5); qeynos2
  -> qeytoqrg: (1750.3, 0, 7.8); nro -> oasis: (-4434.7, -36.7, 0.5) (the nearest-estimate pair took North Ro's
  invisible plane at -111 under its shore and drew Oasis 122 units low). Every step goes to the log.
- **Which lines**: the zone has a sky and an outdoor time type (1, 2, 5); the line has open sky over at least half of
  the seam's points (no drawn surface within 400 units over the ground, `CollisionMesh::roof_above`: not a tunnel,
  like the one between East Commonlands and North Ro, or a cave mouth; an invisible lid, as over qeynos2's line to
  the Hills, is no roof); the neighbour's files are in the asset folder; its targets agree within 150 units
  (Freeport's inner city lines do not); and, once loaded, it has a line back, with open sky just past the seam and
  inside that line back, tested as the zone's own line is, so both sides of a pair agree. Doors and teleports have no
  zone-line region and never count. An indoor zone draws none.
- **The cut**: the neighbour is moved into place on the CPU and cut at the plane through the seam's face, keeping what
  lies past it (triangles across it split along it, `ZoneRenderOptions::keep`), so its own filler towards this zone
  goes. Past the seam this zone's filler and the neighbour's real ground overlap as far as this zone's edge, and where
  both have ground this zone's wins:
  - this zone's ground there is sampled on a 24-unit grid (`game::GroundGrid`, bilinear between the samples); the
    neighbour's upward-facing triangles over it are cut along the grid's cells and each piece within 24 units of it
    goes (`game::split_off_ground`, `ZoneRenderOptions::split`), unless it lies in the last cell before this zone's
    ground ends, so no gap opens there; walls, buildings and hills standing higher stay;
  - what still meets this zone's surfaces loses to them: the neighbour is drawn after this zone with a strict depth
    test and a depth bias pushing it back (`ZoneRenderOptions::behind`: 16 steps of the depth's precision plus twice
    its slope; the depth is reversed), so ties and near-ties do not flicker between the two as the camera moves.
- **Loading**: once the zone is entered and the zone points are in, the nearest qualifying neighbour whose line is
  within the view distance plus 1500 units loads on a background thread at a lower priority (`ZoneRenderer::prepare`:
  the geometry, chunks and decoded textures with their mip chains, capped at 128 texels across; no detail passes,
  material maps or multi-pass lights); the frame uploads it when ready. Before the load starts, this zone's ground
  past the seam (the grid of `game::split_off_ground`) is sampled on the frame's thread, about 1 ms of rows a frame
  (`game::GroundSampler`), since the loader must not touch this zone. At most two are loaded; one is no longer drawn
  when the camera is 3500 units past the view distance from its line, and is freed when another needs its room, on
  zoning or with the setting off (freeing one waits for the GPU, which would hitch a frame of play). A load
  cut off by zoning or quitting stops at its next step (after reading the zone, after its collision mesh, and
  between `prepare`'s meshes and materials, `ZoneRenderOptions::cancelled`), so the wait for its thread is short;
  reading the zone's files is not broken off. It is fogged by this zone's haze, as it is drawn with this zone's
  lighting; the sun's shadows do not reach it.
- **Cost** (ecommons by the Freeport line, two neighbours loaded): freportw 81,244 triangles and 28 MB, nektulos
  40,043 and 9 MB; about 300 ms on the loader, 20 to 45 ms of upload on the frame's thread; 72k triangles in 241
  calls a frame, 0.09 ms of CPU, about 0.5 ms a frame on the GPU (7.0 -> 7.5 ms).
- **Not yet**: the seam is a straight cut and the ground can step by a few units where the two zones' builders
  disagree; the two zones' ground textures meet at a hard line; the neighbour's own sky, fog and water colour are
  this zone's; a zone with a short view (freportw's 450) hides its neighbours in the haze; the classic zone-line
  quirks for North Ro and East Freeport (`game::resolve_teleport_index`'s 1044) are not applied to the offset.

## Texture overrides

Replacement textures, such as upscaled packs, go in a pack folder under `textures/` of the Hailstone folder
(`docs/layout.md`): `textures/default/` when `display.texture_folder` is empty, else the pack it names, or any folder
by absolute path (`off` for none; the viewer's `--textures DIR`). `assets/texture_overrides.h`
indexes it at start. A file stands in for the game texture of the same name whatever the extension or case
(`KBARKD.BMP` <- `kbarkd.png`), and a file in a subfolder named after a zone (`textures/gfaydark/kbarkd.png`) is used
in that zone first. Elsewhere a file of that name at the top of the folder wins over one in a subfolder; between
files at the same depth, the first by path wins. Any size works: the UVs are fractions of the texture.

- **Formats:** PNG (`assets/png_read.h`: every colour type and bit depth, tRNS, interlaced; zlib, no new library;
  at most 16384 on a side and 8192 x 8192 pixels in all, and refused when the header asks more than 1032 times the
  compressed data, so a forged header cannot make it allocate gigabytes), DDS, BMP and TGA.
- **Where:** zone geometry and placed objects, characters (including the layered textures), doors and ground items,
  the sky and spell particles. Not icons or the UI.
- **Orientation:** an override is taken as upright, as it looks in an image viewer (`texdump` writes them
  so). `decode_game_texture` turns it the way the game's rows go: a real BMP's bottom-origin rows for the WLD
  meshes, a DDS's top-origin rows. The game's own BMPs are turned the same way, once, for every WLD mesh: zone, sky,
  characters, a zone's placed objects and the equipment archives' items (ground items, and `IT<n>` doors such as the
  bazaar campfire IT78 from gequip.s3d). The EQG models and zones use DDS only, whatever the name; their few real
  BMPs are the terrain's `e_TexturePalette0` maps, which are not drawn.
- **Cut-outs:** a masked material's override keeps its own alpha. A fully opaque one takes the game's
  transparent texels instead, scaled up.
- **Mips:** built from the override as for the game's textures.

An upscale costs video memory by its area: 4x on each side is 16 times the texels, uncompressed. The zone renderer
logs how many textures it replaced.

### Built packs (tools/texpack)

`texpack` and `tools/texpack/upscale.py` build an HD pack of every texture in the player's own asset folder. The
pack is derived from the game's files, so it is built on each machine and never shipped. Hailstone Texture Studio
(projecthailstone/hailstone-texstudio, a tool add-on: docs/addons.md "Tools") does the same in one program with a
window, on any GPU through ncnn's Vulkan compute, with no Python to install; the rules below are its too
(`src/texpack`, which `texpack` and the studio share: the scan, the upscale rules behind `texpack::Model`, the
three-stage build and the pack's files).

1. `texpack export ASSETDIR WORK` writes each distinct texture once, keyed by `texture_content_hash` of its file:
   61,474 in the RoF2 install (from 2,709 archives, in about a minute). A member named `.bmp`, `.dds` or `.tga` is
   read whole; any other is taken only when its first four bytes (`PfsArchive::read_prefix`, which inflates no more)
   are a DDS or BMP magic, so the scan no longer inflates every mesh, WLD and sound. It writes `WORK/src/<hash>.png` upright, with
   `<hash>.m.png`, the masked variant of an 8-bit BMP whose palette index 0 is used, `<hash>.k.png`, the variant
   without its colour key of a texture painted on one (`remove_color_key`), and `WORK/manifest.jsonl`,
   holding the names, archives, size, stored format and alpha kind.
   A work folder exported before the keyed variants (2026-10-26) gets them by exporting again into it (the other
   files come out the same), then running `upscale.py` (it makes only what is missing) and `texpack build`.
2. `upscale.py WORK MODELS` (PyTorch through spandrel, on ROCm or CUDA) produces `WORK/up/`:
   - the colour at 2x: PBRify's 4x SPAN upscaler (CC0, made for 2000s game textures; it also removes DXT1 blocks
     and dithering), its result Lanczos-filtered down to 2x;
   - `<hash>.n.png` (PBRify NormalV3) and `<hash>.r.png` (PBRify RoughnessV2) from that colour.

   Details:
   - Tiling textures are wrapped by 16 texels before every model and cropped after, so no seam shows; character
     skins (`_chr` archives) are reflected instead.
   - The masked and keyed variants' transparent texels get their neighbours' colour first; a DDS's
     colour under alpha 0 is upscaled as stored, since opaque materials draw it. Cut-out alpha is scaled with
     Lanczos and thresholded.
   - Textures of 16 texels or less, the game's own normal maps and EQG data maps (coverage, palettes, masks) are
     only Lanczos-scaled and get no maps.
   - A stopped run resumes.
   - `--redo-alpha STAMP` makes the textures with an alpha channel again where their outputs are older than the file
     STAMP: for a work folder upscaled while a DDS's colour under alpha 0 was still overwritten (packs built before
     2026-10-25's fix show the hero's forge plates, `generic_plate_*_c.dds`, as flat dark colour). Touch STAMP once,
     run, then `texpack build` again; it re-encodes only the changed files.
   - On an RX 9060 XT the whole set takes about 7 to 11 hours (the DAT upscaler, a little sharper, is ten times
     slower).
3. `texpack build WORK PACK` writes `PACK/by-hash/<hash>[.m|.n|.r].dds` as DX10 DDS files with their full mip chains
   (bc7enc_rdo):
   - the colour as BC7, its mips from `build_color_mips` (level 0's colour under alpha 0 grown over only in the
     masked and keyed variants); `decode_game_texture` takes the keyed variant (`<hash>.k.dds`) for a material
     that reads the texture's alpha (`TextureOptions::reads_alpha`: the zone, object, sky and flora renderers), the
     plain file for an opaque one;
   - normals as BC5, the mips averaged and renormalised;
   - roughness as BC4.

   Each file is stored in the rows the WLD meshes read: a BMP source's are flipped, and its normal map's green
   inverted with them, so every pack normal map has green toward +v. PBRify's normals are DirectX's way, green
   down the picture.

Set up once (Linux, AMD):

    uv venv -p 3.12 venv && uv pip install -p venv/bin/python --index-url https://download.pytorch.org/whl/rocm7.2 torch torchvision
    uv pip install -p venv/bin/python spandrel pillow numpy

The models come from github.com/Kim2091/PBRify_Remix: `4x-PBRify_UpscalerSPANV4.pth`, `1x-PBRify_NormalV3.pth` and
`1x-PBRify_RoughnessV2.pth`, from its ComfyUI release zip.

At run time:
- A pack file with its own mips is used as stored (`decode_game_texture`), keeping its compression on the GPU.
- `by-hash/` files win over names: `w1` is a different image in each city. A server's changed file misses its
  hash, so it keeps its own look.
- A masked material takes `.m`.
- The maps (`decode_texture_map`) shade the classic effect of zones, placed objects and characters in the modern
  lighting:
  - the normal map on a cotangent frame from screen derivatives (the meshes have no tangents), with the surface's
    own normal kept for the shadows;
  - the sun's and moon's GGX highlight from the roughness map, taken into 0.5..1.
- `display.material_maps` (on by default) and the viewer's `--no-material-maps` turn the maps off.
- Every viewer, the model viewer too, takes `--textures`.

## The isometric camera's cut

In the isometric camera (`docs/isometric.md`) `zone.frag`, `object.frag` and `flora.frag` drop the pixels of what
stands between the camera and the player: `reveal_discard` in `shaders/lighting.glsl`, from `SceneLighting`'s
`reveal_origin`, `reveal_axis` and `reveal_shape` (`game/iso_occlusion.h`; off, as in every other camera, when
`reveal_origin.w` and `reveal_roof.w` are 0). The test is a cone round the camera-to-player line, above the player's
feet and nearer than them, or (`reveal_roof`) everything over a height within a flat radius of the player, against a
4x4 ordered dither; the shadow passes' own shaders do not have it. The click marker and the hover ring of that camera
are a second and a third `TargetRingRenderer`. The player's silhouette is a fourth actor pipeline
(`shaders/actor_silhouette.frag`: depth test LESS, no depth write, premultiplied blend, the opaque parts only), drawn
before the actors.

## Name plates

NPC names are drawn in the spawn's consider colour (`game::con_rgb` of `game::con_level`, the colour Hailstone's target plate already uses) instead of this client's yellow (255, 220, 120). Mercenaries keep their blue and corpses stay grey, while other players' names stay light blue. This is a deliberate difference (Hailstone option `con_name_colour`, Options > Display, `hailstone.ini` `display.con_name_colour`, default on); turning it off restores the yellow for NPC names. The target keeps its consider colour in both settings.

Neither colour is the original's: PlayerClient::SetNameSpriteTint gives every NPC but a mercenary one fixed tint (0xad, 0xb8, 0x17) and only flashes the target's toward white; the con-coloured target plate is Hailstone's look too (`draw_nameplates`). Which name sprites are drawn, and their parts: docs/mechanics.md "Name sprites".

The plates are a 2D overlay. Each name is drawn through a pipeline of ours (`app/nameplate_layer.h`, `shaders/plate_depth.*`) that compares its pixels with the current frame's scene depth (`VkContext::overlay_depth_view`, the depth the AO reads). Where a surface lies more than 3 units nearer than the plate's head, its text is occluded and not drawn. Walls, objects, doors and characters can occlude a name; a pole or wall edge affects only the covered pixels. The target's name is tested too, unless the option `target_plate_through_walls` (Options > Display "Target Name Through Walls", `display.target_plate_through_walls`, default off) is on: then it is shown behind any geometry, a sight of the target the original does not give. The depth test is Hailstone's own rendering rule. It replaces the earlier CPU test, a line of sight from the camera to the head point through the zone's collision mesh (terrain and placed objects only): that hid a name as a whole, the target's too, when the one point was blocked, and drew it over everything when it was not.

Where geometry hides a name its pixels are dropped; there is no ghost in the client. The viewer's `--plate-test-ghost A` can still draw the test names behind the scene with an alpha A (the shader's `extra.x`). The overlap fade is the option `plate_overlap_fade` (Options > Display "Fade Overlapping Names", `display.plate_overlap_fade`, default on); off, every name is drawn at full alpha, still nearer over farther.

Every name stays at its natural projected head position: 1.5 units above the top of its model, the plate's bottom edge there, so a larger name or more lines under it grow upward, off the head. A rider's head is measured from where it is drawn, its model's top in its mount's seat bone frame (`ActorRenderer::rider_top`), not from its spawn's place on the ground. There is no stacking, distance fade, height timer or runtime tuning command in this version.

How the names' text looks is Hailstone's to choose (the original's name sprites have one fixed font), set in Options > Display "Name Plates" (`game/nameplate_style.h`, hailstone.ini keys `display.plate_*`, kept per character like the other display options). The defaults draw the names as before these options:

| Option | Key | Default | What it does |
|---|---|---|---|
| `plate_font_face` | `display.plate_font_face` | empty | A font family from the system's (as /fontface lists them); empty, or a face not found, is the UI's font. |
| `plate_font_size` | `display.plate_font_size` | 15 | Points at 100 % UI scale, 8 to 48. The names get a font of their own in the atlas at this size, so a large name is not an enlarged small one. |
| `plate_font_bold` | `display.plate_font_bold` | 0 | The face's bold file; a face with none is struck twice, a pixel apart. |
| `plate_edge` | `display.plate_edge` | 0 | 0 a drop shadow one pixel down and right, 1 a dark outline all round (thicker for larger names), 2 none. |
| `plate_far_scale` | `display.plate_far_scale` | 100 | The size of a name at the plates' range (250 units), in % of its size; it falls evenly with the head's distance from the camera (`game::plate_distance_factor`), 25 to 100. 100 keeps every name one size. |
| `plate_tag_scale` | `display.plate_tag_scale` | 100 | The lines under the name (a guild, an NPC's owner line) in % of the name's size, 50 to 100. |

The overlap fade measures each name's box at the size it is drawn. A change rebuilds the font atlas at the next frame, as /fontface does.

Names that overlap each other are drawn from the farthest to the nearest, so the nearer lies over the farther (a deliberate difference; the original's name sprites sort by depth and nothing more). What a nearer box covers fades to `game::kPlateCoveredAlpha` (0.1), by the largest share of its box that one nearer box covers: nothing under `game::kPlateFadeFrom` (10 %), all of it from `game::kPlateFadeTo` (20 %), smoothstep between (`game/nameplate_overlap.h`). The alpha follows its goal smoothly (tau 0.3 s). The target is never faded and stays at its projected head at full alpha.

The target's highlight (the dark plate, the edge in its colour and the bobbing marker) is the option `target_plate_highlight` (Options > Display "Highlight Target Name", `display.target_plate_highlight`, default on); off, the target's name is plain in its colour. If the pipeline cannot be made (a missing shader) the log says so and the names are drawn over the scene. Offline check (the zone viewer, no server; needs a GPU, a display and the game folder, so not in `hailstone_tests`): `tools/probe_test.py --hailstone build/hailstone/hailstone --game-dir DIR` runs the viewer twice in the Bazaar. `--plate-test` draws test names (magenta) through the same `NameplateLayer` as the client, each placed from the CPU's rays into the zone (`app/plate_test.h`) so that it is known what the scene should do to it: in front of everything it covers, behind it all, or in between. `--plate-test-no-depth` draws the same names with no depth test. The script counts the magenta pixels of each name in both screenshots: a front name must keep 95 % of them, a behind name lose all but 2 %, a partial one lose some. The CPU side is in `tests/plate_test_test.cpp`.

## Settings

In `hailstone.ini`, and in the Options window (`ui/modern/options.lua`, this client's own window: its Display and
Graphics tabs; the client's options by name in `app/client_app_options.cpp`, the graphics ones `display_option`); the
viewer (`--zone`, `--model`) takes `--msaa`, `--tonemap`, `--exposure`, `--render-scale` and `--sharpness`.

| Key | Values | Default | |
|---|---|---|---|
| `display.msaa` | 1, 2, 4, 8 | 4 | samples per pixel, cut to what the GPU supports; read at start-up |
| `display.render_scale` | 50 .. 100 | 100 | percent of the 3D view's size the scene is drawn at; below 100 FSR 1 upscales it (The frame); the UI stays sharp |
| `display.sharpness` | 0 .. 1 | 0.5 | FSR 1's sharpening (RCAS) below a render scale of 100; 0 none |
| `display.tonemap` | auto, none, agx, aces | auto | auto: AgX (with a little extra saturation) for modern, none for classic |
| `display.exposure` | EV | 0 | the scene is scaled by 2^EV before the tonemap |
| `display.shadows` | 0, 256 .. 8192 | 2048 | texels per side of each shadow cascade; 0 off; modern only |
| `display.light_shadows` | 0 .. 8 | 8 | how many lights cast shadows (Light shadows: the zone's, the carried light, spells); 0 off; modern only |
| `display.light_shadow_size` | 128 .. 1024 | 512 | texels per side of each face of a light's shadow cube |
| `display.view_distance` | multiplier | 2.5 | the modern mode draws this many times further than the classic client (at most 12000 units), into haze; picking keeps the classic distance |
| `display.ao` | 0, 1 | 1 | screen-space ambient occlusion; modern only; viewer `--no-ao` |
| `display.distance_blur` | 0, 1 | 1 | the far distance's slight blur; modern only |
| `display.neighbour_zones` | 0, 1 | 1 | the zone past an outdoor zone line drawn in the distance (Neighbour zones); modern only; `/neighbourzones [on \| off]` |
| `display.target_ring_cues` | 0, 1 | 1 | the target ring's sweep on a new target and pulse while attacking (Target Ring Cues; The target ring) |
| `display.find_trail_style` | 0, 1 | 0 | Find paths: 0 the gold ground line, 1 rainbow strands; Options > Display (gamepad: Interface), saved per character and changed immediately; docs/navigation.md "The trail" |
| `display.target_ring_xray` | 0, 1 | 0 | the target ring's hidden part showing faintly through what stands in front (Target Ring Through Models) |
| `display.target_ring_color.<Section>` | RRGGBB | (TargetIndicator.ini's) | the target ring's colour for a con, Trivial .. Deadly (Target Ring) |
| `display.target_ring_opacity` | 25 .. 200 | 100 | the target ring's alpha, in % of its con's |
| `display.target_ring_glow` | 0 .. 100 | 50 | how bright the target ring's core shines |
| `display.target_ring_max_size` | 0, 4 .. 40 | 0 | the target ring's largest radius in world units, 0 off (Max Size; a deliberate difference) |
| `display.bloom` | 0 .. 1 | 0.5 | bright light spreading into its surroundings; modern only; viewer `--bloom N` |
| `display.brightness` | -0.2 .. 0.2 | 0 | added after the tonemap, in display values (the UI is not graded) |
| `display.contrast` | 0.5 .. 1.5 | 1 | around mid grey, after the tonemap |
| `display.gamma` | 0.5 .. 1.5 | 1 | after the tonemap; above 1 lifts the shadows |
| `display.color_grade` | 0, 1 | 1 | the modern lighting's colour grade by time of day, weather and zone (Colour grade); viewer `--no-grade` |
| `display.lighting` | modern, classic | modern | `render/scene_lighting.h`; `/lighting` switches; `enhanced` is read as modern |
| `display.window_mode` | windowed, borderless, fullscreen | windowed | borderless: a window over the whole display at the desktop's mode; fullscreen: exclusive at `display.resolution` |
| `display.resolution` | WxH | the desktop's | the fullscreen mode (the closest the display has); in a window, the Options window's Resolution sets the window's size |
| `display.vsync` | off, on, adaptive | on | the present mode: immediate (else mailbox), FIFO, FIFO relaxed; FIFO when the surface lacks one |
| `display.max_fps` | 0, 10 .. 1000 | 0 | frame cap (0 none); `--max-fps` takes precedence |
| `display.background_fps` | -1, 0, 1 .. 1000 | 0 | frame cap while another window has the focus (0: as `display.max_fps`; -1: nothing drawn, the Options window's "0"). Below 10 the loop runs on at 10 a second (the game clock catches up at most 250 ms a frame) and draws only that many frames, or none (`app/frame_pace.h`); a frame not drawn is run as a minimised window's is |
| `display.flora_distance` | units, 400 .. 1200 | 400 | how far the flora is drawn (Radial flora) |
| `display.zone_flora` | on, off | on | Hailstone's flora for zones with a rules file (Zone flora), read as each zone loads |
| `display.extra_shields` | on, off | on | a deliberate difference: shields RoF2 does not list (`game::is_shield_model`, docs/formats/characters.md "Held items") go on the shield point, not in the hand; read at start-up |
| `display.texture_folder` | pack name, absolute folder, off | `textures/default` | replacement textures (Texture overrides): the folder indexed at start, its files read as each zone loads; the Options window's Textures section (from the next start) and Hailstone Texture Studio set it |
| `display.fov` | fov units | none | the camera's field of view in 512ths of a circle (none: the classic 90, 63.3 degrees across below an aspect of 2); the Options window shows degrees, 45 .. 120 |
| `ui.scale` | percent, 0 | 0 | the UI's scale (0: the display's); read at start-up; `--ui-scale` takes precedence |

## Roadmap

1. **Frame foundation** (done): HDR scene target, MSAA with alpha-to-coverage, post pass with tonemap and dither.
2. **Lighting**: (a, done) linear scene, the modern lighting below, AgX by default; (b, done) cascaded shadow maps
   for the sun and moon, cast by the zone's geometry and the characters; (c, done) screen-space ambient occlusion;
   (done) the zone's lights per pixel from a light grid, warm ones flickering; (done) spells' light on everything
   round them; (done) shadows from the zone's lights; (done) placed objects (doors) as casters; (done) world textures' mips built in linear light with alpha-weighted colour and cut-out coverage
   kept; later maybe sRGB formats (filtering between texels in linear).
3. **Sky and atmosphere**: (done) a longer view distance with haze instead of the fog wall, the sky blended into it
   at the horizon, a slight distance blur, a physically based sky with stars and lit clouds; (done) weather (OP_Weather):
   rain, snow, lightning, wet ground and lying snow; (done) the moon's phases, thunder, rain and snow kept off covered
   ground; (done) a tilted sky (the sun rising east, standing south), the ambient and the haze's colour from the
   sky model, the haze eased between places and weathers with a storm fog band, weather transitions over tens of
   seconds, a flickering lightning.
4. **Water and lava** (done): screen-space reflections, refraction, depth colour, shore foam, waves; lava glowing and
   pulsing; (done) under water: caustics, the water's colour and absorption, light shafts, Snell's window; (done) the
   sky as drawn mirrored where the screen holds it, EQG "noswap" environment cubes, falls as sheets, molten lava with
   crust, waterfalls without dark curtains; next maybe planar reflections for what the screen does not hold.
5. **Post and content**: (done) bloom; (done) foliage wind; (done) a texture override folder for upscaled packs;
   (done) built HD packs with normal and roughness maps (tools/texpack);
   (done) colour grading; optional TAA.
6. **Upscaling**: (done) a render scale with FSR 1 (EASU + RCAS); next the temporal groundwork every temporal
   upscaler needs (sub-pixel jitter on the projection, kept off picking and the nameplates; a motion-vector target
   written by every scene pipeline, with last frame's camera, bones, wind time and object matrices; history reset on
   zone-in, teleport and resize; a negative texture LOD bias), then a TAA/TAAU of our own on it, then FSR 2/3.1
   (MIT) as the temporal upscaler. MSAA is off while a temporal upscaler runs, so cut-outs lose alpha-to-coverage;
   particles, spells, water and lava need a reactive mask rather than motion vectors. DLSS, XeSS and frame
   generation are not planned.
