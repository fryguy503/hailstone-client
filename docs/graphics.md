# Graphics

The client's mechanics follow the RoF2 client; its graphics do not (`docs/plan.md`, section 1). The classic renderer is
a DirectX 9 design from 2003: per-vertex lighting, a hard fog wall, no antialiasing, no shadows. This client keeps
the game's assets and its world data (zones, models, the day clock, weather, the zone's fog values as hints) and
draws them with a modern renderer. The procedural sky's
colour maps, the EQG material classes and the multi-pass light details are not planned work unless a feature needs
them; the classic path that exists stays as the `classic` look.

## Shared GPU textures (experimental, Windows)

Several clients on one machine can share the GPU memory of their immutable zone and actor textures instead of each
uploading its own copy. It is off unless a separately started broker is configured, it is not in the release package,
and every client still has its own Vulkan device, renderer, UI and window: only the texture payloads are shared.
The design and its limits are in [Shared GPU memory](research/shared-gpu-memory.md).

- **The broker** (`hailstone_gpu_cache_broker`, `tools/gpu_lab/`) owns one copy of each texture on its own device and
  hands each client an opaque Win32 handle to it over a local named pipe (`platform/gpu_ipc.h`: current user only,
  local peers only, a random endpoint and 256-bit token passed in `HAILSTONE_GPU_IPC_ENDPOINT` and
  `HAILSTONE_GPU_IPC_TOKEN`, never in arguments or logs). Clients and broker must use the same GPU model, driver and
  memory layout (checked in the handshake); `HAILSTONE_GPU_NAME` picks the GPU by its exact model name.
- **The client** (`render/gpu_shared_cache.h`) asks for a texture by its final content (after colour preparation and
  mip building, `render::prepare_immutable_texture_image`, so the broker's UNORM image never builds a colour chain)
  and imports the handle (`platform/gpu_external_memory.h`). A texture the broker declines (unsupported format, its
  budget full, an upload failure) is uploaded privately; sharing carries on.
- **Frames:** Vulkan allows one instance to own an external image at a time, so a frame that samples imports takes a
  machine-wide lease, acquires the imports from `VK_QUEUE_FAMILY_EXTERNAL`, submits, waits for its own fence and
  releases them. The barriers cover every live import, not only those the frame samples (the renderers bind whole
  texture arrays). GPU work that touches shared textures is therefore serialized across clients, and each such
  frame waits for its own fence: memory is saved at a cost in frame overlap.
- **Failure never stops the game.** A bad request fails alone; a client that disconnects or crashes gives back its
  lease and references and the others carry on. A client that loses the broker, waits more than 5 seconds for the
  lease, does not see its frame finish within 2 seconds or gets any reply it cannot use stops sharing for good: it
  closes its connection, copies its imports into its own images before the next frame (they stay valid without the
  broker) and plays on with private textures (`render/gpu_share_link.h`). A connection idle for 5 minutes is closed
  by the broker; clients ping well within that.

The tools beside it, also only in the build tree: `hailstone_gpu_share_probe` reports each adapter's external memory
capabilities (`--smoke --device-index N` checks buffer aliases on one device), and `hailstone_gpu_asset_probe` renders
a real zone's static assets in several offscreen views on one device with sharing on and off and compares the pixels.

To try it, build `hailstone_gpu_cache_broker`, start it with `--serve` and the session variables, then start each
client with the same variables (and `HAILSTONE_GPU_IPC_SERVER_PID` set to the broker's process id to pin it).
`--status` and `--stop-if-idle` query and stop the broker. Without the variables nothing changes: every texture is
private, as before. `HAILSTONE_GPU_BENCHMARK=1` logs the client's presented frames and the time spent waiting for
the lease and for shared frames' fences.

## Transient plugin geometry

The generic world geometry service (`sdk/hailstone_world_geometry.h`) accepts complete, transient batches of
colored triangles or one-pixel lines. A producer supplies the final world positions and compositing order;
`render/WorldGeometryRenderer` never invents terrain heights. It depth-tests against the current scene with
reverse Z, blends premultiplied linear-light color without writing depth, and writes the temporal reactive mask
through the same scene attachments as the target ring. Thick lines, outlines and glyphs are authored triangles.
The per-frame budget admits at most 256 batches and 98,304 vertices; invalid inputs and repeated owner/tag pairs
cannot consume unbounded validation work. Buffers belong to fenced frame slots.

The app offers a current scene frame only when its connection, zone, instance and scene epoch match the loaded
collision scene. Effect floor queries choose the highest upward-facing drawn opaque/masked surface at or below
the caller's ceiling: terrain, static props (passable parts included), current animated scenery, doors and placed
scenery. A separate visual mesh uses the same selected smoothed terrain and replacement objects as the renderer;
movement keeps its original mesh. It is rebuilt with the scene and unavailable during a scene rebuild or failed
upload. Invisible collision, transparent materials, undersides and movement-only ladder slabs are excluded. These
are visual surfaces, separate from movement floor policy; NPC/player/pet meshes, dropped items, missiles and
placement ghosts are excluded. The producer bounds the ceiling to the desired layer; missing surfaces remain
missing. Actor anchors use the model transform prepared for this render attempt, including the displayed position,
model origin and mount seat, with a separate plausibility-checked floor sample when available. A hidden or clipped
actor can have a prepared model pose: this is not evidence of visibility. Connection retirement, zoning, skipped
render attempts and renderer failure withdraw the service, and changed scenery transforms or animation time
advance its collision revision. Masked surfaces use their mesh footprint; this CPU query does not sample texture alpha.
Animated visual surfaces use the renderer's exact clock, independently of the fixed-step movement pose. The
optional surface-patch query clips current upward scene faces against a producer triangle, proves coplanar union
coverage by polygon subtraction, and returns an interior witness and its highest source face for a mismatch.
Off-grid or thin props cannot be certified flat just because point samples missed them. Coverage holes, incomplete
enumeration, numeric uncertainty and exhausted shared work never produce a planar certificate.

A recorded receipt means all admitted vertices reached `vkCmdDraw` for the exact batch identity. A submitted
receipt additionally means the real frame reached a successful queue submission. A presented receipt means
`vkQueuePresentKHR` accepted that real frame (`VK_SUCCESS` or `VK_SUBOPTIMAL_KHR`); an optional generated frame
cannot confer a receipt on it. None of these receipts proves visible pixels, lack of occlusion or physical display.
`world_geometry_probe` checks actual recording, frame submission/presentation and PNG readback for winding,
alpha blending, depth occlusion, foreground geometry and lines without game assets.

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
(`ClientApp::view_area_`). A move of the rectangle is taken at once; a new size makes the scene targets again (a
device wait and every target rebuilt), so it waits until the size has held for 4 frames, or at most 30 frames after
it first changed (`render::SceneResizeDebounce`, render/swapchain_size.h): dragging an edge does not stall every
frame, and the old rectangle is drawn meanwhile.

The scene can be drawn below the view's size and upscaled (`display.render_scale`, `SceneOptions::render_scale`,
33 .. 100 %, `render::kMinRenderScalePercent`): the scene targets are made at the view's size times the scale (`render::scaled_extent`, each side
rounded), and `FrameInfo::extent` is that size, so the scene's projection, AO, SSR and bloom all work at it; the view's
rectangle in the window (picking, nameplates) is unchanged. Below 100 % `begin_overlay` runs AMD FidelityFX FSR 1
(`third_party/fsr1`, MIT; spatial, no motion vectors): `post.frag` draws into a 10-bit image at the scene's size,
tonemapped and sRGB-encoded as FSR 1 wants it and without the dither; `shaders/fsr_easu.frag` (EASU, edge-adaptive)
brings it up to the view's size; `shaders/fsr_rcas.frag` (RCAS) sharpens that by `display.sharpness` into the
swapchain and adds the dither. The UI is drawn after, at the window's own size. Textures keep their LOD bias (none
yet). With `display.upscaler = taa` the temporal upscaler takes FSR 1's place (Temporal upscaling, below).

### Worker threads

The frame hands some of its own loops to a small pool of worker threads (`core/worker_pool.h`): one fewer than the
machine's hardware threads, at most 6 (`core::default_worker_count`), or `display.worker_threads` (0: none, every loop
on the main thread). `core::parallel_for` splits a loop into chunks that the workers and the main thread take in turn;
without a pool (the tests, the tools) or from inside a worker it runs the loop inline. What runs there:

- The characters' posing and skinning (`render::ActorRenderer::prepare`): each drawn instance's slice of the frame's
  bones is given out on the main thread (the culling and the bone budget, in order), the instances are posed and
  skinned on the workers, each into its own slice (`ActorRenderer::pose_instances`), and the motion history is kept on
  the main thread after. An actor's bones as drawn (`drawn_bone`: its effects, its name plate, spells on it) are posed
  once a frame and kept until the spawn is drawn again (`ActorRenderer::cached_bone_world`).
- The particle quads (`render::ParticleRenderer::draw`). Modern: after the sort, each particle has its own six
  vertices in the buffer. Classic: the particles each emitter shows are counted on the workers, the emitters' places
  are given out in order up to the buffer's size, then their quads are built on the workers. A particle that turns
  out to show nothing after its place was given out (a quad past its depth offset) is written as six vertices at one
  point, which draw no pixel.
- The actor effects' step (`app::ActorEmitters::simulate_async`): once the frame is recorded, each attached emitter's
  place is read from the poses as drawn (on the main thread), then both particle systems step on a worker while the
  frame is submitted and the next one begins; every other call on them waits for the step. The next frame draws the
  result, from the same poses the step used before (those drawn the frame before), the particles' own time one frame
  behind. The item previews' emitters still step inline; the tests take both ways (the constructor's `async`).
- The scene's command recording (`render::SceneRecorder`, `display.parallel_record`). Each draw between `begin_scene`
  and the scene's end is a job in a lane: a renderer's own draws share its lane, and the characters, the spawn boxes,
  the target ring and the isometric markers share one (the markers ask the character renderer's caches, and the
  spawn boxes take the set the characters drew). At `begin_translucent` and before the name plates, the lanes are
  recorded at once, a lane's jobs in order on one thread (`render::SceneLanes`), each job into a secondary command buffer of its own
  (`VkCommandBufferInheritanceRenderingInfo` from `VkContext::scene_inheritance`, the viewport and scissor from
  `set_scene_state`), and the buffers are executed in the order the jobs came, so the draws and their order are as
  they were. The scene's renderings then begin with `VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT`
  (`VkContext::set_scene_secondary`). Each frame slot keeps a command pool per lane, reset when the slot comes round.
  Off (or without workers), the jobs run at once into the frame's own command buffer. A queued draw runs at the
  flush, so what it reads must not change between its `add` and the flush: the frame sets the zone renderer's distant
  land (`set_distant`, `set_distant_hidden`) before it queues the first draw.
- The shadow layers' command recording (a second `render::SceneRecorder`, made without `scene`, under the same
  `display.parallel_record`): the sun's cascades, then the cover and water maps when they are drawn. Each layer is a
  target of its own (`VkContext::shadow_inheritance`, its viewport, scissor and depth bias from `set_shadow_state`),
  and each renderer (the zone, the characters, the doors and objects) a lane, so a renderer's layers are recorded in
  order on one thread while the others record theirs (`render::SceneLanes` keeps each job's lane and target). Every
  layer's rendering then begins with secondary contents (`begin_shadow_cascade`'s `secondary`) and executes its
  buffers in the inline order. The point-light cubes are still
  drawn inline.
- The other spawns' movement (`game::WorldState::smooth`): the vehicles' riders are stepped first on the main thread
  (each reads its vehicle), then every other spawn on the workers, each wholly on one thread, so the result does not
  depend on which. Its floor, region and contact probes only read the zone and its doors; the contact probe sweeps
  doors into a buffer of each thread's own (`ZoneMoveWorld::set_contact_probe_per_thread`).
- A zone's load (`ClientApp::load_zone`): once the zone's data is final, its collision mesh, moving parts, ladders
  and ambient probe are built as background tasks while the main thread makes the zone's GPU objects, and each is
  waited for where it was built before.

A character's skeleton is posed at most once a frame for the bones its effects, spells and name plate ask for
(`ActorRenderer::drawn_bone`); the poses of spawns no longer drawn are dropped once a second, not each frame.

### Temporal upscaling

`display.upscaler = taa` (read at start-up, as `display.msaa`; the viewer's `--upscaler taa`) draws the scene jittered
and resolves it over the frames into a picture at the view's size: anti-aliasing at a render scale of 100 and
upscaling below it. It is the groundwork a temporal upscaler from outside (FSR 3.1) needs too, run through one of our
own first (Roadmap 6). `render/temporal.h` holds the maths, `VkContext::begin_overlay` the passes:

1. **One sample.** The scene targets are single-sampled whatever `display.msaa` says: the resolve smooths the edges,
   and the motion and depth it reads are taken per pixel. Alpha-tested cut-outs lose MSAA's alpha-to-coverage.
2. **Jitter.** Each frame the scene's projection is moved by a sub-pixel offset (`VkContext::projection_jitter`,
   `render::jittered`): Halton (2, 3) from its second point, in a cycle of 8 x (view / scene)^2 frames (8 at a
   render scale of 100, 32 at 50; FSR 2's count), so every view pixel sees samples near it. Only the scene's draws
   take it: picking, the name plates, the map overlay, the sun's place for the shafts and the culling of spawns to
   pose keep the plain projection. The app gives that plain camera to `VkContext::set_temporal_view`; a frame that
   does not (no 3D scene) is drawn without the temporal path and drops the history.
3. **Prep** (`shaders/temporal_prep.frag`, at the scene's size, two targets): the scene with its AO applied (the AO is
   lighting, so it goes in before the resolve, which then smooths its pattern too; the post pass does not apply it
   again), and each pixel's motion in UV to where it was a frame ago (RG16F). The motion is rebuilt from the depth
   with `render::reprojection`, last frame's view-projection times this frame's inverse, worked in double through
   the cameras' relative motion so that zone coordinates in the tens of thousands keep sub-pixel precision, wherever
   no draw wrote a motion of its own (step 6); where one did, that is taken. The reactive mask (step 6) goes on in
   the colour's alpha.
4. **Resolve** (`shaders/taa_resolve.frag`, at the view's size): each view pixel gathers the 3 x 3 scene samples round
   it, weighted by a Gaussian fit of Blackman-Harris by their jittered distance; takes the nearest surface's motion
   among them (edges move with what is in front); reads last frame's history there through a 5-tap Catmull-Rom;
   clips it in YCoCg to the samples' mean +- 1.25 standard deviations (inside their extremes); and blends. Colours are
   compressed by their exposed luma while mixed, so one bright pixel cannot dominate. The history's alpha holds the
   weight it has gathered: this frame adds how near a sample fell to the pixel's centre in view pixels (0 .. 1), the
   history keeps at most 12 (`kHistoryMaxWeight`; each frame is then at least 1/12 of the result), less the further
   the clip pulled it, and at most 4 once the motion passes 32 view pixels a frame (resampling softens it). A new,
   dropped or off-screen history takes the frame as it is. Two histories (RGBA16F) are written in turn.
5. **Post** at the view's size: `post.frag` reads the history in the scene's place (`post_taa_sets_`); the distance
   blur and the shafts read the scene-sized depth through `depth_texel` and UVs. The bloom is still made from the
   scene at its own size. With `display.sharpness` above 0, the post pass draws into a 10-bit image and RCAS
   sharpens it into the swapchain, as under FSR 1; at 0 it draws straight into the swapchain.

6. **The draws' own motion and the reactive mask.** Under a temporal upscaler (and with `independentBlend`, which
   every desktop driver has; without it the camera's motion is all there is) the scene has three colour
   attachments: the colour, the object motion (RGBA16F: the motion in red and green, cleared to `kMotionUnset`;
   blue below) and the reactive mask (R8, cleared to 0). Every scene pipeline is made for them through `VkContext::scene_rendering` and `scene_blends`, which mask
   off what its shader does not write (`SceneOutput`); the item previews, drawn into targets of their own, keep the
   colour alone. The scene shaders read the temporal block (the shared scene set's binding 5,
   `shaders/motion.glsl`: this frame's and last frame's plain view-projection), which `begin_scene` writes into the
   frame's commands, and write each vertex's place now and a frame ago as `o_motion`:
   - the zone: its plants' wind sway at last frame's wind clock (`SceneLighting::time.y`, kept per zone renderer);
     the rest of it stands still, so its motion is the camera's, drawn exactly;
   - characters: their bones a frame ago, kept per spawn (`ActorDraw::motion_key`) and written after this frame's at
     `kMaxBones` in the same buffer; a spawn new this frame, or with another number of bones, moves with the camera.
     They are kept on the CPU (`render::BoneHistory`), and both halves are built there and copied into the buffer at
     once: it is write-only memory to the CPU, so nothing is read back from it;
   - doors, objects, placed items, the placement ghost and missiles: their transforms a frame ago, kept per thing
     (`ObjectRenderer::Instance::motion_key`), written after this frame's at `kMaxInstances`;
   - flora and the lawn: their sway and the wind at last frame's clock (`Push::time.y`); the pushing spawns' bend
     is taken as it is now.
   The opaque passes write the motion; the characters' blended parts (a see-through shield, hair: `MotionReactive`)
   write their motion too, over what shows through them, with their depth in blue, as they write none (frame
   generation takes the nearer of it and the depth buffer, or a glass shield's pixels follow the background and it
   breaks up; temporal_prep.frag passes blue on in `motion_`); the characters' additive parts (an ice shield's
   glowing rim, a glow: `MotionReactiveWhereSeen`) do the same only where they add something to see (more than 0.02
   in a channel: the shader's alpha, 0 or 1, blends the motion in), so a faint glow spread past the body leaves what
   is behind it alone, while the rim no longer stays behind with the background as the character turns, flung off in
   pieces in the generated frame; the blended and additive draws (the zone's water,
   glass and glows, the characters' additive glows and the objects' blended parts, particles, fire, spell bolts, rain
   and snow, the target ring, the isometric silhouette) write the reactive mask, `shaders/reactive.glsl`: the larger of their alpha and
   brightness, at most 0.9, blended to the largest over the pixel (water a third of that, or its waves would keep no
   history). The resolve keeps that much less of the history. The sky and the spawns' stand-in boxes write neither.
   Under FSR 1 the same shaders run with the colour alone; the validation layer then warns once that locations 1 and
   2 are not drawn into, which is harmless.

The history is dropped when the scene targets are made again (a resize, a new render scale), on zone-in
(`VkContext::reset_history`), on a frame without the temporal path, and when the camera jumps between two frames
(`render::camera_cut`: more than 200 units, or a turn of more than 0.5 radians: a teleport, a camera switch; not the
chase camera pushed in by a tree and out again, tens of units in a frame, which the motion follows exactly).

Open (docs/todo.md): a negative texture LOD bias below a render scale of 100; the bloom from the resolved picture;
the held items and other attachments drawn as characters without a motion key.

### AMD FSR 3.1

`display.upscaler = fsr3` (the viewer's `--upscaler fsr3`) runs AMD FidelityFX FSR 3.1's upscaler in the resolve's
place, on the same groundwork: one sample, the jitter, `temporal_prep.frag`'s colour (with the AO) and motion, the
history dropped as above. The post pass then reads its output at the view's size (`post_fsr3_set_`, as it reads
the history through `post_taa_sets_` under `taa`); `display.sharpness` drives FSR's own RCAS (0 skips it) and our RCAS pass does not run.

- **What is vendored**: only the upscaler's GLSL (`third_party/fidelityfx`, FidelityFX SDK tag v1.1.4, the upscaler
  3.1.4, MIT; its README lists the source paths and two local changes). SDK 2.x ships the Vulkan upscaler only as
  signed prebuilt DLLs under a binary-only licence, so it is not used. Nor are the SDK's runtime and Vulkan backend:
  `render::Fsr3Upscaler` (`src/render/fsr3_upscaler.cpp`) is Hailstone's own host side, following the SDK's
  `ffx_fsr3upscaler.cpp`: the internal images (the pairs that alternate by frame, the luminance pyramid, the
  reactive and lock masks), the constants, the clears and the dispatch order (prepare inputs, luma pyramid, shading
  change pyramid, shading change, prepare reactivity, luma instability, accumulate, RCAS). The binding tables are the
  passes' own `FSR3UPSCALER_BIND_*` numbers; descriptor sets are written each frame, one set per pass per frame in
  flight.
- **Shaders**: `cmake/Shaders.cmake` (`hailstone_add_fsr3_shaders`) compiles each pass once as compute with glslc or
  glslangValidator, no AMD tool: HDR colour, reverse depth, motion vectors at the render size without jitter, 32-bit maths,
  the history reprojected with FSR's Lanczos table (the permutation AMD's runtime picks on its own GPUs; the reference
  Lanczos, which it keeps elsewhere, computes a sin() per tap and doubled the accumulate pass on an Intel Iris Xe); accumulate twice, with and without sharpening (`fsr3_*.spv`).
- **Inputs as FSR wants them**: our jitter (`jitter_px_`) is FSR's `jitterOffset` as it is (a texel's unjittered
  place is `t + 0.5 - jitter` in both), and its cycle length is ours (FSR's count rounded up: 15 against its 14 at a
  render scale of 75, the same at 50 and 100); our motion (UV to last frame's place) goes in with a scale of 1; near,
  far and the vertical field of view are read back from the plain projection. Exposure is FSR's automatic one (its
  frame info), so nothing else is passed; the frame time is measured between FSR frames.
- **Device**: storage images in FSR's formats (R8, R16F, RG16F: `shaderStorageImageExtendedFormats`, enabled when
  present) and quad subgroup operations in compute (`render::Fsr3Upscaler::supported`, `DeviceCaps::fsr3`). Without
  them `fsr3` falls back to `taa` with a log line.
- **Why 32-bit**: AMD's runtime picks a 16-bit permutation (`FFX_HALF=1`) where the device has 16-bit floats, but
  in the vendored 3.1.4 it has nothing to give. The accumulate pass, most of the cost, has no 16-bit code (compiled
  with `FFX_HALF=1` it holds no 16-bit float), and the luma and shading change pyramids' 16-bit SPD callbacks are
  stubs (`SpdLoadSourceImageH` and `SpdLoadH` return 0, `SpdStoreH` stores nothing). Tried on an Intel Iris Xe, the
  pyramids looked 13 times faster because they did nothing: the picture came out about 9 % darker (the exposure from
  an empty pyramid) and softer. Writing those callbacks would save at most about 0.3 ms there.
- **Timing**: with `HAILSTONE_FSR3_TIMING` set, GPU timestamps round the clears and each pass
  (`Fsr3Upscaler::timing_summary`); the viewer's `--frames` logs their means ("FSR 3.1 GPU time"). On an Intel Iris
  Xe (1280x720 from 858x482) FSR 3.1 takes 3.9 ms, the accumulate pass 2.0 of them; on an RX 9060 XT 0.26 ms.
- **Motion and reactive mask**: the same as the resolve's (Temporal upscaling 6): the draws' own motion in
  `motion_`, and the scene's reactive mask as FSR's (`Fsr3Upscaler::Inputs::reactive`; none, FSR's default of 0,
  without `independentBlend`).

### Upscaler providers

`display.upscaler = <name>` (the viewer's `--upscaler <name>`), where `<name>` is an upscaler add-on in
`upscalers/<name>/` (docs/addons.md "Upscalers"), runs that add-on's temporal upscaler in FSR 3.1's place
(`render::Upscaler::External`). This is how upscalers the client cannot ship come in: Intel XeSS and AMD FSR 4 are
closed binaries, so each is an add-on of its own repository, built against `sdk/hailstone_upscaler.h` (MIT;
docs/plugins.md "Upscaler providers" has the interface) and NOTICE.md's add-on exception. The client never names,
links or ships any of them.

- **Loading** (`render/upscaler_provider.*`): at the start, before the Vulkan instance, from a copy of its folder in
  `cache/upscalers/<name>-<key>/` (the key from its files' names, sizes and times; a box set's windows share one), so
  the add-on manager can replace the folder while the copy is loaded. No server sends one; a server's opted-in
  add-on policy can require, allow or block one, checked against the loaded copy as the character enters
  (docs/plugins.md "Opt-in add-on policies").
- **The device**: the provider names the instance and device extensions and the Vulkan 1.0 to 1.3 features it needs,
  and may chain feature structs of extensions of its own (`gather_extra_features`); `VkContext` turns them on beside
  its own when the loader and the GPU have the extensions and the core features (`missing_names`,
  `missing_features`, `add_features`), and otherwise goes on without the provider. The provider checks its extension
  structs' features itself, and says no for another vendor's GPU.
- **The frame**: the same inputs as FSR 3.1 (`temporal_prep.frag`'s colour with the AO, the depth, the motion and the
  reactive mask, at the scene's size, `SHADER_READ_ONLY_OPTIMAL`), the jitter in FSR's convention, the camera's planes
  and field of view, the exposure, the frame time and a reset on a cut; it writes `fsr3_output_` (RGBA16F at the
  view's size, in `GENERAL`), which the post pass reads as under FSR 3.1. A memory barrier before the call makes the
  inputs readable by any stage, and one after makes the output readable by the post pass. The colour carries
  `TRANSFER_SRC` and the output `TRANSFER_DST` as well, for a provider that copies. Frame generation follows the same
  motion as under the other temporal upscalers. A provider that sharpens (`HAILSTONE_UPSCALER_SHARPENS`) gets
  `display.sharpness`; for any other the client's RCAS runs after it, as under `taa`.
- **Falling back**: a provider that is not installed, does not load, says no at any step, or lacks something on this
  GPU is dropped with a line in the log; the client then runs FSR 3.1, or `taa` where the GPU cannot run FSR 3.1. One
  that fails a frame is dropped at the next frame (the scene targets are made again, the device idle), and `taa` runs
  from then on.
- **The example** (`plugins/example/upscaler/example.c`, built into `build/hailstone/upscalers/example/`) stretches the
  colour with a linear blit: no accumulation, only the plumbing. `--upscaler example` runs it.

### Frame generation

`display.frame_gen = true` (Options > Frame Generation, shown with a temporal upscaler; the viewer's `--frame-gen`)
presents a frame made between each two real ones. It needs `taa` or `fsr3`: their motion (`motion_`, the draws' own
and the camera's) and depth are what it follows; under `fsr1` it stays off with a log line. It is Hailstone's own,
fed by the engine, not a layer guessing motion from finished images; the choices of what to show where the two frames
disagree follow kettle-framegen (the same author's BSD-3 Vulkan layer; no code taken). `render/frame_gen_logic.h`
holds the decisions (unit tested), `render::FrameGen` (`src/render/frame_gen.cpp`) the GPU side.

1. **Pictures without the UI.** The post pass draws into one of two pictures (`pictures_`, `kUpscaleFormat`, the
   view's size; written in turn) instead of the swapchain, and the UI draws into an image of its own (`overlay_`,
   the swapchain's format, cleared transparent, premultiplied). The in-between frame is made from pictures alone, so
   text and windows never warp; each presented image is the picture with the UI laid over it (`fg_composite.frag`,
   which also encodes sRGB for an sRGB swapchain).
2. **Motion at the in-between time** (`fg_scatter.comp`, two runs): each pixel of this frame is moved back along its
   motion to where it stood at t = 0.5 (`kFrameGenTime`); the nearest depth that lands on each place wins (reverse
   Z, atomic max on the depth's bits), and the winner's motion is written there. What moves in front covers what is
   behind, as in the frame. The first run also marks where each pixel was a frame ago, with its depth (the keys at
   the last frame's places), so step 4 can tell a last-frame sample that was hidden behind something nearer.
3. **Holes** (`fg_fill.comp`): a place nothing landed on shows something this frame does not, because it is being
   covered between the two frames. It takes the motion of the farthest of the marks bordering it, looked for in eight
   directions out to 32 pixels (the background's: a hole is half as wide as the background moves against what
   covers it, tens of pixels as the camera turns past a still character), with that mark's depth, and is flagged, so
   the synthesis takes it from the last frame alone; with no mark near, it is flagged with no motion. A crack is
   not a hole: a place with the same surface (depths within 5 %) marked a few pixels away on two opposite sides
   (four pixels each way, along a row, a column or a diagonal) is a surface whose pixels spread apart between the
   frames (a character turning, or nearing the camera: close up the whole body is a grid of them). It takes the
   nearer side's motion and the synthesis blends it as usual, unless this frame shows something nearer along that
   motion (then it is the background behind a thin figure, a hole after all). Taken for holes, the cracks read the
   last frame along the background's motion, tens of pixels away while the camera turns, and sprinkled the
   character with specks of whatever was there (its shield, its gloves).
4. **The in-between frame** (`fg_synth.frag`, the view's size): each pixel samples this frame (back along the motion)
   and the last one (forward). The motion is at the scene's size and the pixel at the view's, so of the 3 x 3 motion
   texels round it the one along which the frames agree best is taken, nearer ones favoured and farther ones held
   back (the background's motion agrees through a figure thinner than it moves, a sword, and would cut it out). Where
   they agree it blends them by the time; a flagged pixel takes the last frame, or, where the last frame had
   something nearer at that place, this frame along the motion when nothing nearer covers it there; and in neither
   frame (the background between a character's legs, behind the other leg in each, or a thin sword the background
   passes behind) the nearest sample of either frame round its own place that shows background, out to 64 pixels in
   eight directions. Before, only the last frame was looked in, out to 16 pixels, and the gap between the legs
   took the other leg's colour, a second leg. What is seen through a glass shield counts as covered: its colour is
   the shield's too, and would lay a second shield beside it; a pixel whose last-frame sample lands where something nearer
   was then (the keys at the last frame's places, the 3 x 3 round it) came out from behind it and takes this frame,
   however alike the colours (a dark character against a dark world turning behind it); one whose sample of this
   frame lies at the edge of something nearer (this frame's depth round it: the picture's anti-aliased outline of a
   character, carried along the background's motion, would draw the character's outline beside it) takes the last
   frame; anything else that disagrees was uncovered and takes this frame; a sample off the screen takes the other
   frame. The reactive mask's pull towards this frame is left out in a hole, where this frame shows what covers the
   place (a see-through copy of the character over water, rain or a spell beside it otherwise).
   Where the reactive mask is set (particles, spells, rain: no motion of their own) it takes this frame along the
   motion, sharp, rather than two copies a little apart.
5. **Presenting**: a second swapchain image is acquired (`Frame::generated_available`) and both are submitted
   together, the in-between frame presented first. FIFO is forced whatever `display.vsync` says, and the swapchain
   gets one image more (`swapchain_image_count`, min + 2): waiting for the second image is what holds the game to
   half the refresh, so the two presents alternate evenly. With fewer images (a surface's maximum) the second is
   taken only when one is free. No frame is made across a cut, a reset, a resize or the first frame after any of them,
   nor across a camera snap of more than 8 units (`kFrameGenMaxStep`: the chase camera pushed in by a tree, or
   zoomed; running moves it a unit or two a frame, and a frame half way through a snap uncovers half the scene at
   once): `frame_gen_ready`. The real frame is then shown twice (`frame_gen_repeat`, as kettle-framegen shows the
   nearer real frame), keeping the pace of two images a frame; alone, it was a hitch on top of the cut.

Cost and limits: two small compute passes at the scene's size and two full-screen draws at the view's per real frame,
plus one composite more. The game reacts up to one real frame later. A moving thing the depth does not hold (glass,
water's surface) follows what is behind it; thin edges can leave a pixel-wide trace where they were about to cover the
background, and a thin, fast-swinging part (a sword) can break up as the camera turns.

Measuring it: the model viewer stands an animated character in a zone and `--orbit D` circles the camera round it at
D degrees a second (a still character against a turning world, as the player sees their own). With `--frame-dt`
both viewers' clocks step exactly, and a timed run (`--frames`) takes no input, so two runs agree to the pixel: a run
at half the step renders the frame the in-between one stands for, the truth to compare it with. At a 60 fps base the
mean error round the character against that truth went from 2.37 to 1.32 at 360 degrees a second (two runs of the
same frame: 0.98), from 1.74 to 1.02 at 180 (0.84), and at a 30 fps base from 2.29 to 1.44 at 180 (1.00), against
4.8 .. 6 for a plain blend or the nearer real frame. Crack filling and the search of both frames then took it to
1.28 at 360 (floor 0.96), 1.35 at 180 on a 30 fps base (1.01) and 1.19 at 90 (1.14); the larger gain was live, as
Hailtest (an ice shield) in the arena at 3076 x 1366 with the camera turning, where the specks and the second leg
were plain to see. `--spin D` turns the models on the spot instead (the player turning with the camera fixed); at
20 degrees a frame (800 a second at a 40 fps base, FSR 3.1) an ice shield's rim, drawn additively, had the
background's motion and left a second outline behind: 2.42 round the character (floor 0.86), 1.97 once the
additive parts carried their motion. `--screenshot-generated`
saves the in-between frame with the real one after it (`FILE_real`) for a side-by-side look: the viewer's last frame
(with the one before as `FILE_prev`), and in the client every screenshot, the input script's `shot` too; the
viewer's `--frames` log counts the in-between frames presented. The
performance window shows its rates, skips, pacing and GPU time (`VkContext::frame_gen_stats`, docs/dev-ui.md); the
generated GPU time is only a frame's that synthesised an in-between frame, not one that showed the real frame twice.

Screenshots copy the presented swapchain image, so the swapchain asks for `TRANSFER_SRC` when the surface offers it;
where it does not, a screenshot is refused with a log line (and a chat line for the screenshot key) instead of an
invalid copy. The readback is 4 bytes a pixel, so it is also refused when the swapchain is not 8-bit RGBA or BGRA
(`render::screenshot_format_supported`): a surface that offers no such format with sRGB colour space gets its first
format, which may be FP16 or 10-bit HDR. A surface that reports no format at all fails the swapchain's creation. The swapchain is made again when the window's framebuffer size changes from the size it was made for,
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
more frame, as ImGui's draw lists are built before their frame begins). A `ZoneRenderer` is let go this way, so freeing it
does not stall the frame. A recreated swapchain is made with `oldSwapchain`, and the
old one and its present semaphores are retired two frames on: `vkDeviceWaitIdle` does not cover a present's wait.

The ImGui textures (`ui/imgui_vk_textures.h`: the UI art, icon sheets, browser tabs) come from descriptor pools of the
client's own, a new one made when they fill, with a set layout defined as the backend's. `ui::UiTextures` keeps at
most 1024 files (`kMaxFiles`); past that the one asked for least recently is retired.

In the modern lighting the scene is split for the water. The zone's opaque draws (`render::ZonePhase::Opaque`, with
the water held back), the placed objects and the characters' opaque and alpha-tested parts (`render::ActorPhase::Opaque`)
go first. `begin_translucent` then ends rendering and
copies the resolved colour and depth (sample 0) into images that `shadow_set()` binds as 1 and 2. Drawing then goes
on into the same targets (stored and loaded, the multisampled ones too): the grass (opaque, but left out of the copies
so the target ring lies on the ground under it), then the characters' blended and additive parts
(`ActorPhase::Translucent`: they write no depth, so grass drawn after them would cover a glass shield or a glow; in
every lighting), then `ZonePhase::Translucent`: the water first, then the blended and additive draws. Being after the
copies, a character's blended parts are missing from what the water shows through it.

With `display.depth_prepass` (off by default; the Options window's Depth Pre-pass, in either UI pack, switches it
from the next frame, to compare; the viewer's `--depth-prepass`) the zone's opaque and alpha-tested
geometry is drawn depth-only first (`ZoneRenderer::draw_depth`, `shaders/zone_depth.frag`), then coloured with the
depth test equal and no depth write: a pixel covered by several surfaces is shaded once. The alpha test is the colour
pass's, so an alpha-tested edge covers the same samples in both passes, and `zone.vert`'s position is `invariant`, so
both passes give a surface the same depth. The pre-pass discards every pixel `zone.frag` discards, the isometric
camera's cut-away included, or the cut walls would keep their depth and hide what stands behind them. The water stays
out of the pre-pass (it is drawn blended after `begin_translucent`). The pre-pass covers the zone's geometry only: the objects, the flora and the characters keep their single pass.
A zone builds the pre-pass's two pipelines whether it is on or not, so the switch needs no reload. The pre-pass's
shader is optional: without it the zone draws the single pass, with a warning in the log.

The scene target holds linear colour. The classic lighting computes in display (sRGB) values as the classic renderer does
and converts at the end of each shader; the modern lighting computes in linear HDR.

### Smooth classic lighting

The lighting has three levels, low to high (`render::LightingMode`, the Options window's Lighting choices): Classic,
the original's look; Smooth Classic (`display.lighting = smooth`, `/lighting smooth`, the viewer's `--lighting
smooth`); and Modern. Smooth Classic keeps the classic lighting's values and changes only where two of them are worked
out. It is Hailstone's own; Classic is the original's as before. Its maths is in
`shaders/classic_light.glsl`, which the CPU tests compile too.

- **The region light per pixel.** The original runs the region effects' light (`classic_region_light`: the vertex
  colour, the ambient, the special ambient and the sun weighed by the vertex alpha, saturated) per vertex and blends it
  across the triangle. `zone.frag` runs the same formula per pixel from the interpolated colour and normal, then adds
  the lights.wld term (`v_point`) as before. Outdoors the sun shades by the pixel's own normal (the classic trees'
  leaves show their sunlit and shaded sides); and where a face's corners saturate, the middle no longer dims between
  them (Befallen's red-lit walls by about 3 %). A mesh without normals takes no sun, as per vertex.
- **Soft highlights** (`render::Tonemap::Soft`, Smooth Classic's automatic tone curve). The original's frame buffer
  clips, and what is drawn past white turns flat: at night in the cities it is the braziers' and torches' flames
  (North Qeynos and North Freeport, thousands of pixels a view), seldom a wall. Display values to 0.8 stay exactly as
  they were; above, each channel rolls off towards white (`soft_highlight`: display 1 to about 0.93, 1.5 to 0.99),
  which keeps colour in a flame's core and cuts the clipped pixels by about 40 %. The cost: whites the original showed
  at full white are about 7 % dimmer.

The fog was tried per pixel too and left per vertex: at no view tried (Qeynos Hills with a near fog, with and without
the smoothed ground) did it change more than a few hundred pixels by a few levels.

Nothing is added to a frame but a few instructions per pixel: no pass, texture or buffer. The ZoneRenderer carries the
switch in `SceneLighting::ambient.w` (`ZoneRenderer::set_classic_smooth`); everything else treats Smooth Classic as
Classic.

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
those images (the baked terrain tiles, uncompressed A8R8G8B8 DDS, are opaque where they are a zone's base layer).

A texture shared through the broker ("Shared GPU textures" above) instead gets its colour chain on the CPU
(`render::prepare_immutable_texture_image`) before it is keyed and exported, since the broker's image is UNORM. Data
images keep their ordinary mip recipe, and BC formats the device lacks are decoded first.

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
(`ObjectRenderer::set_time`, from `WorldObjects`) and on characters and their equipment (`ActorRenderer::set_time`).
Every frame remains referenced through texture eviction and zone handoff, and shadow alpha tests select the same
frame as the colour pass. The classic flames are this: `FIRE1_SPRITE` is fire1..fire4 at
200 ms in every campfire, torch and brazier, whether the zone places it (ecommons' CAMPFIRE) or the server does as a
door (ecommons' GRATE beside Gaddius, the bazaar's IT78 campfire from gequip.s3d).

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

Held torches and equipment-backed server objects keep their authored animated materials or WLD particle clouds.
These moving clouds are drawn by `ActorEmitters`, rather than replaced by the zone's procedural flame geometry.
Server doors resolved only from the zone's flattened object meshes still lack per-actor cloud metadata.

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
A timed entry's elapsed time stops at 2^32 − 1 ms and the entry is then dropped (a deliberate change: the original's
32-bit sum wraps, so an entry whose last stage starts near 2^32 stays for good).

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
far ring costs about what the near circle does. How thick the lawn grows is Hailstone's Grass Density
(`display.grass_density`, the Options window's slider, the gamepad pack's Grass density): 25 to 300 %, 100 by
default. It scales the blades each of the lawn's three draws gives a patch (`render::lawn_lod_blades`: 49, 16 and 9
at 100 %, 147, 48 and 27 at 300 %; at least one), the blades keeping their width, so less is sparser and cheaper and
more thicker and dearer: the blades are made in the vertex shader, so the plants' memory is the same at any density
and the lawn's GPU time grows with it (the flora pass in South Karana at 1280 x 720 on a Radeon RX 9060 XT, RADV: 0.54 ms at 100 %, 1.11 ms at 300 %); the other plants are not thinned. The tiles are filled nearest first, so when a frame reaches
`FloraRenderer::kMaxPlants` (262,144) it is far plants that are left out. A plant is 40 bytes to the GPU a frame.

## Zone flora

Hailstone's own, not the original's: the classic zones have no flora, and this gives them grass without changing
their files (`assets/zone_flora.h`, `display.zone_flora`). Their plants join the same `Zone::flora` the EQG zones'
radial flora fills (/showgrass hides them, Flora Distance sets their reach). The grass is a lawn the client builds
itself, no game asset (below); ferns, flowers and bushes from donor EQG archives are scattered over it and drawn as
the radial flora is (above).

**Which rules.** A zone takes `flora/<zone>.txt` from the install folder (shipped) when there is one, else
`flora/default.txt` when the server calls it outdoor (OP_NewZone's time type 1, 2 or 5; PEQ marks some dungeons so,
which the cover test below keeps bare). A rules file is read line by line, and the first matching ground line wins:
- `donor <archive>`: an EQG archive in the player's game folder whose `RadialFloraDefs.rfd` and textures give the
  scattered plants' shapes and pictures; several donor lines add up. Nothing is shipped; a kind whose donor (or
  whose picture) is missing is left out, and the lawn needs none (the default names no donor).
- `ground <glob> <density> [+green|+greenish] [lawn] [<kind>[:<weight>]...]`: on the ground textures the glob
  matches, the lawn when the line says `lawn`, and `<density>` plants per 100 square units of the kinds by weight (a
  definition of the first donor that has it, or one donor's as `<archive stem>/<name>`); a density of 0 and no lawn
  keeps a texture bare. The classic ground textures blend
  grass into cliff, sand or a path within one picture, so `+green` keeps a plant only where the game's own texel
  under it is clearly green (`is_green_texel`: not the water's teal, mud's olive or a dungeon's near-black), and
  `+greenish` also takes dry and dark grass, for textures whose name says they are grass.
- `max_slope` (degrees), `scale <min> <max>` (the scattered plants' sizes), `lawn_height <min> <max>` (the lawn's
  blades, 1.8 to 3 units by default; the grasslands' grow 2.4 to 4), and `include <name>` (another file's lines in
  place: a zone file puts its own exceptions first, then `include default`; East Commonlands adds desert plants from
  `commonlands.eqg` on its sand that way).

The shared files turn down built and wet surfaces by name before anything else (`bare.txt`: roofs, walls, cobbles,
tiles, bridges, hedges, ship decks, water, lava), then take grass-named textures with `+greenish`, then turn down
floors, rock, stone and cliffs (`stone.txt`, after the grass lines so a grass and rock blend still grows on its grass),
then take anything else with `+green`. A `+green` texture with next to no green texels (`kMinGreenShare`) grows
nothing at all.

**Families.** Most zones take the lawn alone (`default.txt`). A zone of a climate with plants of its own includes a
family file, the same lines with kinds from donors scattered sparsely over the lawn (about 0.4 to 1 plant per 100
square units):

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


**The lawn.** An even carpet rather than plants scattered at random: the ground is cut into world-aligned squares
of `kLawnCell` (3.2 units, 40 to a tile), and each square whose centre lies over ground that grows a lawn (the
tests below, at that centre; `+green` by the majority of five texels, the centre's and four half a square out, so a
texture blending grass into dirt texel by texel grows no speckle) holds one patch of blades, at the ground's height
and slope there. Each square goes to
exactly one ground triangle, so the patches meet without gaps or overlaps. A patch takes the colour of the ground
texel at its centre; its height wanders slowly over the zone between `lawn_height`'s two values (smooth value noise
on a 48-unit lattice, `assets::lawn_height_noise`), not patch by patch. Where the lawn meets bare ground (a path, a
road, a texture `+green` turns down) it tapers over three squares (`lawn_edge_factor`: 0.55 of the height beside it),
so it does not end in a wall along a texture's straight border, and it frays: a patch there keeps only a share of its
blades, a draw of its own between a quarter and 85% beside bare ground, all of them three squares in
(`lawn_edge_cover`, carried to `lawn.vert` in the instance's flags). A patch with fewer than three of its eight
neighbours grown is dropped (a lone tuft); a tile grows its neighbours' squares within three of its edge to see them,
then drops them. Under cover (a platform, a bridge, a roof) a square asks whether it is covered not at its centre but
at a spot moved off it by smooth noise of two scales, up to three squares either way (`lawn_cover_offset`), on the
ground's plane there and a little over it, so the bare ground under a straight platform wobbles in and out rather
than tracing its outline.

The blades are built in `shaders/lawn.vert`, without a texture (`render::FloraRenderer`'s second pipeline): a
patch's square holds a grid of blades, one jittered in each cell, so the cover is even. A blade is a tapered strip
of a few pieces ending in a point, standing on the ground across the patch (its slope), curving over as it rises, a
little taller or shorter, combed: it leans 0.4 to 0.65 of its height the way a slowly turning field over the zone
points (broad swathes lying together), a little off it, its flat side turned a way of its own (so the wide faces do
not all line up); leaning
with the scene's wind as gusts roll across the zone (the zone's plants' wind, `SceneLighting::wind`: gentle in fair
weather, hard in a storm) and fluttering a little, and bent away from the spawns walking through it like the other
plants. `shaders/lawn.frag` gives it the ground's hue with its brightness held within a band (a pale or dark texel
makes no pale or dark patch), darker at the roots and brighter and warmer towards the tip; in the modern lighting it
is lit as the radial flora is, with its face's normal turned most of the way up (the lawn is lit like the ground
under it, not blade by blade) and some light through it when the sun is behind it. It casts no shadow.

Nothing about the lawn changes at a line as the camera moves (`render::kLawnLods`, `lawn_blades`). A patch's blades
come in a fixed order, placed by a low-discrepancy sequence shifted per patch, so any first few of them cover its
square evenly. All 49 stand within 40 units; past that the last of them shrink away one by one (each over three
places in the order) to 16 at 90 units and 9 at 250, while the rest widen by the square root of how many fewer there
are, so a patch keeps its cover. The three draws (49 blades of three pieces within 90 units, 16 of three to 250, 9
single triangles past that) each take the first blades of the one before, so a draw's switch removes only blades
already gone. Past 400 units every second patch each way fades out over the 120 units before it while the patches
kept widen to take their squares, past 800 every fourth (`lawn_patch_fade`, `lawn_patch_side`); a grown tile keeps
its patches by the coarsest stride that keeps them, so a far tile walks only those it draws. The lawn shrinks into
the ground over the last fifth of the reach with the other plants. A blade is narrow at the root (0.55 of its width),
widest a third of the way up, and comes to a point. In South Karana, on the AMD card (RADV) it was tuned on, the lawn
costs about 0.4 ms a frame at the default reach.

**Where.** On level ground (45 degrees by default) of a drawn, opaque or cut-out surface: not the invisible collision
surfaces, nor the translucent water. Not in water or lava, nor on the water's surface (the region just under the
foot); in a zone line's region it grows (up to 0.7 it did not, and North Qeynos's line to Qeynos Hills is a box over
the whole field north of the city, which was left bare). Not where the zone's own drawn terrain lies more than a
quarter unit overhead (a dungeon's ceiling, a roof, a bridge, a path's tiles laid over the grass; the ground's own
triangle is not cover); the outdoor zones' invisible collision ceiling and cloud layers do not count. In a zone the
server calls outdoor the terrain covers only within 20 units overhead (`assets::kOutdoorTerrainCover`, set with the
rules by `load_zone_flora_rules`, so florascan places grass as the client does): a hut's roof or a low bridge keeps
the ground bare, a cliff's overhang, an arch or Kelethin's platforms high over Greater Faydark do not. A known cost
of that: the classic towns' thick walls are hollow boxes over the ground, so grass grows unseen inside them where
their tops lie over 20 units up (North Qeynos's city wall some 21 units, the wall in Misty Thicket some 33), and
under raised floors 20 to 40 units up (the Plane of Knowledge). A lawn square asks about the terrain at its spot off
the centre (above) and also at its centre for a floor lower than its blades are tall (`lawn_max`), so a floor laid a
unit over the grass (the Plane of Knowledge's brick tiles over its grass texture) does not show blades through it.

Placed objects keep the grass off the ground under them, the zone's own and the server's alike: its doors, ground
objects (dropped items, tradeskill containers) and placed real estate items (`WorldObjects::flora_cover`), and the
campfire spawns (race 567). An object covers a spot with its opaque surfaces 0.25 to 16 units over it
(`assets::kObjectCoverClearance`, `kObjectCoverReach`), so grass grows up to a boulder's base, under a tree's crown
and its cut-out leaves; a plant (`assets::is_plant`: cut-out leaves and a plant's name) covers nothing. A fire (a
flame material, or fire, camp or coal in its name; `assets::is_fire_object`) small and low enough (within 12 units
of its middle and 16 tall) covers a disc over its whole footprint and half a lawn square round it, so the middle of
a ring of logs is bare too; a campfire spawn covers a disc of its size (3 to 10 units, and the half square) round its feet. A lawn square
asks about objects at its centre, not the spot off it, so a small object's bare patch is not scattered. The server's
objects are asked as each tile grows (`FloraRenderer::set_cover`), and the tiles round one that came, went or moved
grow again (`FloraRenderer::regrow`; a door swinging is not counted). Each plant takes the colour of the ground texel
it stands on (the classic outdoor vertices carry none).

**Past the zone's edge.** In the modern lighting the land round an outdoor zone (Horizon hills) grows the zone's
flora too, out to 300 units past the zone's terrain (`render::horizon_lawn_ground`: its ground there, not rock, sand,
the sea, nor what lies tucked under the zone), so the lawn does not stop in a line where the zone's ground ends; the
fog past the edge takes it from there. Its triangles are the zone's ground of the land's ground texture, their
texture coordinates the land's own (`assets::FloraExtraGround`). The land is built after the zone has loaded, so when
it is ready the zone's flora is set up again with it (`assets::add_zone_flora_ground`, some 25 ms on the frame's
thread once a zone) and the grass renderer lets go of the tiles it has grown (`FloraRenderer::restart`); only a zone
whose flora Hailstone's rules grow, not an EQG zone's own.

**When.** The zone keeps the ground triangles that may grow plants, split into pieces of at most a quarter tile,
binned by 128-unit tile (`assets::ZoneFloraGrower`); the renderer grows the tiles within reach as they come into it,
nearest first, on a background thread of its own (`render::FloraGrowth`: a few tiles queued at a time, so the queue
follows the camera; a tile keeps its old plants while it grows again; a tile whose growing fails is logged once and
not tried again until the cover there changes or it leaves reach), and lets go of those well past it. Each piece's plants come from its
own seeded sequence, so a tile grows the same plants every time and a zone costs memory by the reach, not by its size
(South Karana would hold some 6.5 million plants at once). Setting a zone up takes some 10 to 30 ms at load. A damaged
mesh is bounded: a triangle with a non-finite vertex or one over 100,000 units out (the zones lie within some 20,000)
neither grows plants nor covers any, the ground is cut into at most 2 million pieces, and the cover test holds at most
16 million cell references, a triangle over more than 1,024 of its 64-unit cells (at most 256 such) being tested at
every point instead. A triangle past those bounds is no cover. The split bounds a piece's area, not its length, so the
tiles are bounded as well: a piece whose bounds touch more than 1,024 tiles (only a damaged mesh's sliver) grows
nothing, and the tiles hold at most 16 million piece references in all.

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

While the server plugins' consent question waits, the foreground list would hide it too, so the loading screen
(the zone-in's and the settling's alike) is drawn instead as a window of its own over the viewport, after every other
window of the frame, and the question's windows (the client's prompt, or the built-in pack's that asked
`hs.PluginConsent`) are raised above it: the world, its names and the other windows stay hidden beneath it, and it
takes their mouse presses. During zone settling the windows get the mouse again for that question, unless `/safelock`
is on; the world gets none either way. Whether the question waits is read once as the frame's UI begins, so the
answer does not leave a frame without the loading screen; from the next frame the foreground overlay is back.

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

The sky stack (including its blended clouds and additive bodies) and the underwater/haze backdrop test
equal against the scene's reverse-Z clear depth, 0, without writing depth. This tests the original
multisampled depth attachment, so a cut-out or geometry edge keeps the sky in every uncovered sample.
The modern zone viewer draws the stack after the opaque zone and flora, before
the water colour/depth copy and all translucent draws. The client moves it after the opaque zone
(including the distant land), but still before the objects: their single call includes
blended parts. Its object/actor/silhouette order and the grass-after-copy rule stay unchanged, so those
later opaque draws do not yet save sky shading. Classic lighting keeps the background first because
the zone draws its opaque and blended parts together. A zone or active neighbour with WLD detail
layers also keeps the background first: these blend in the opaque group, potentially over a base
that leaves depth clear. Painted skies keep their internal layer order;
the model viewer has no sky/backdrop call and is unchanged.

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

### Volumetric clouds

Over the atmosphere (the plain skies in the modern lighting) the clouds are a layer of air 1.5 to 4 km up over a
round planet, marched along each view ray (`render/cloud_renderer.h`, `shaders/clouds.comp`), in place of the painted
cloud layer and the `weather.ini` cloud patches. This is Hailstone's own look; the original has only the painted
clouds. `display.volumetric_clouds` (Options, Volumetric Clouds; off on the Low tier) turns it off, and the viewer
takes `--no-clouds`. The patches keep moving while they are hidden, so turning the option off shows them in place.

- **Shape** (after Schneider, "The Real-time Volumetric Cloudscapes of Horizon: Zero Dawn", SIGGRAPH 2015): a weather
  map (30 km across) says where cloud grows and of what type. A height profile keeps flat clouds to the layer's
  bottom third and lets towering ones fill it. Perlin-Worley noise (7 km across) gives the billows, and finer Worley
  noise (0.9 km) wears the edges, wispy at the bottom and billowing higher up. The noise is drawn on the GPU when
  the clouds are first shown (`shaders/cloud_noise.comp`: a 128³ shape, a 32³ detail and a 512² weather map, every
  one tiling). No noise file ships.
- **Cover** (`render::cloud_cover`): fair-weather cumulus over the normal sky (0.36 of the map), a few over the desert
  (0.18), heaped ones over the cottony sky (0.55). A storm (`WeatherLook::storm`) eases every sky to overcast, thicker
  (2.2 times), flatter and dark at its base, as it gathers and clears (about 36 s in, 75 s out).
- **Wind** (`render::cloud_wind_step`): the field drifts from the west-south-west at 12 m/s, twice that in a full
  storm, added up frame by frame (so a storm speeds the clouds up without a jump) and wrapped every 600 km, a whole
  number of every texture's tiles (a wrap throws the history away once).
- **Light** (after Hillaire, "Physically Based Sky, Atmosphere and Cloud Rendering in Frostbite", SIGGRAPH 2016):
  the sun by day and the moon by night (`Daylight::to_light`, `light`, after `apply_storm`), marched six steps
  towards the light from each sample. The light scattered more than once is approximated by three octaves of a
  dimmer, deeper-reaching and wider lobe (Wrenninge et al. 2013), so a cloud's shadowed side is not black and its
  edge towards the sun glows. Ambient comes from the open sky (`Daylight::sky`), less at the cloud's base. Each step
  adds its light in Hillaire's energy-conserving form, and the march stops once less than 1 % of the sky shows.
- **Cost:** the clouds are marched at half the scene's size on each side (`render::cloud_extent`). Each frame marches
  one pixel of every 2x2 block (`render::cloud_update_slot`, in a Bayer order) at 40 steps overhead to 64 near the
  horizon, each start jittered (interleaved gradient noise). The other three are carried over from the last frame
  turned to this one's view: the clouds are far enough away that the camera's turn alone moves them on the screen.
  A freshly marched pixel is blended in at a fifth over what was there, so the jitter averages out over about a
  second. A pixel whose last place was off the screen is marched at once. The atmosphere reads the image through a
  3x3 tent filter (four bilinear taps), so the 2x2 blocks do not show at the clouds' edges as the view turns. The history is thrown away (`render::cloud_history_stale`) when
  the sun or moon jumps more than 0.02 radians, the storm by more than 0.1, or the wind by more than 0.5 km.
- **Into the sky:** the compute pass runs before the scene begins (`SkyRenderer::prepare`). `shaders/atmosphere.frag`
  reads its image (binding 2: rgb the clouds' light, a the sky still showing) and puts the clouds in front of the sky,
  the stars, the sun and the moon. This happens after the storm greys the sky and before the haze at the horizon.
  Far clouds thin out (exp(-distance / 45 km)), so the atmosphere's own horizon shows under them. The lightning lights
  the clouds twice as much as the clear air. Without the clouds the atmosphere reads a clear 1x1 image, and the sky is
  as it was.
- Measured on an RX 9060 XT (RADV) at 1280x720, in the viewer over Qeynos Hills: about 0.5 ms a frame over the
  painted clouds (10.6 against 10.1 ms). The viewer's `--pan D` turns the camera D degrees a second, to watch the
  history carried over as the view turns.
- Not yet: the clouds cast no shadows on the ground, and the god rays do not see them (a storm already turns the god
  rays off). With the isometric camera's orthographic view there is no history to carry over, so every pixel is
  marched each frame.

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
  - Puddles fill the flat ground's low spots over 90 s of rain (warped noise picks them, a finer one frays their
    shores; up to about a quarter of the flat ground in a long storm) and dry over 4 minutes. Only flat ground holds
    water: both the smoothed normal and the triangle's own normal (screen derivatives of the position; the smoothed
    one reads level at a slope's foot or on a rounded crest) must be within about 11 degrees of level, full puddles
    within about 6, and ground nearer that limit needs a deeper spot to hold water. At the shore the water is
    shallow and shows the soaked ground through it; further in, its reflection of the sky takes over by Schlick's
    Fresnel term (water's 2 %), so it reads dark from above and a mirror at a low angle; a damp, darker rim rings
    it, and the sun or moon glints in it, shadowed (`puddle_surface`; the cover is `shaders/puddle_cover.glsl`, which
    `tests/lighting_test.cpp` also compiles as C++). While it rains, drops ring them
    (`rain_ripples`: jittered cells about a unit across, more drops the harder it rains), the rings fading out where
    a pixel spans more than a tenth of a unit.
  - Snow lies on level surfaces in noisy drifts, covering over 2 minutes and melting over 5: on the ground and on the
    doors' and objects' roofs and ledges, not on the characters (drifts fixed in the world would slide over them).
  - A cut-out surface of the zone (alpha-tested: leaves, fronds, grates) gets neither puddles nor snow, and a third
    of the wet look: its normals are a crown's or a card's, mostly facing up, and would paint a generated tree's
    crown with the sky's reflection or turn it white.
  - All of it is in `shaders/weather.glsl` (`weather_surface`, and `puddle_surface` on the zone's ground), fed `SceneLighting::weather` (`render::set_weather`:
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
it, so the shadows move with the leaves. An object that plays its own animation (below) is not given this sway: its
animation is how it moves.

## Animated zone objects

A placed object whose skeleton plays its own animation (docs/formats/wld.md, "Placed-object animation") is drawn
where the animation has it. `ZoneRenderer::build` draws its still triangles with every placement as any object's,
and keeps its moving triangles as one instanced model per definition (the instancing of "Model overrides"): each
vertex names its node in `GpuVertex::point_light.x` (its index among the definition's moving nodes + 1; 0 still;
point lights reach only regions, so a placed object has no other use for it), and each placement's instance names
the definition's first motion in `GpuInstance::normal_row[1].w`. A motion is one moving node's matrix: every frame
`animate_instances` poses each definition once on the zone's animation clock (`set_animation_time`, milliseconds
since the zone loaded, the clock the collision uses) and writes every motion's rows, now and a frame ago, into the
frame slot's motion buffer (set 1, binding 4). `zone.vert` and `zone_shadow.vert` put a vertex through its motion
before its placement, and the motion pass's previous place uses the rows a frame ago, so the temporal upscaler and
frame generation see the parts move. The upload is a few matrices whatever the number of placements: Emerald Jungle's
22,174 animated trees draw in fewer calls than their per-placement copies did (0.43 ms against 0.58 ms a frame
classic, 2.36 against 2.60 modern, `--frames` at the same view). The instances' boxes reach as far as the animation
takes the parts. The model's vertices carry the first placement's baked light; another placement's instance tints
them by its own over that one's, channel by channel, so its gradient is the first one's. The point lights' shadow
cubes (drawn once and kept) and the cover map keep the parts in their first pose.

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
- **Reading:** the shaders find a light's shadow without searching (`point_shadow_at` in `shaders/lighting.glsl`).
  Each scene light carries its entry among the shadowed lights in `color.w` (`render::tag_shadowed_lights`, run by
  `PointShadows::apply`, matching on position and reach within a tenth of a unit and a hundredth). Each zone light
  carries a shadow key (`render::point_shadow_keys`: 1 + the first light in the same place, so stacked lights share a
  cube), which its entry also holds (`slot.z`). Only the current zone's renderer stores keys
  (`ZoneRenderOptions::light_shadow_keys`). The
  shadow is looked up only where the light reaches the pixel and faces it, since every term it scales is zero
  elsewhere. The lookup itself is a normal offset that grows as the surface turns from the light, then the 3 x 3
  hardware-filtered comparisons in four lookups (see Shadows under Modern lighting). In the zone a blocked light loses its pool and its MPL share, and the baked vertex colour, which
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

Held EQG equipment retains its authored normal, specular and environment maps in modern lighting, through the same
actor surface path as armour. For example, the cleric epic shield's pearl environment map and strength travel with
its mesh instead of being discarded when the item is loaded. `AddAlpha` actor and object materials retain their
authored unlit, unfogged state in both lighting modes.

EQG zone materials of the opaque, Alpha and Chroma classes name a tangent-space normal map (`e_TextureNormal0`,
uncompressed A8R8G8B8 `_n.dds` files) and, by their shader, a specular or reflective term. The classic renderer binds the
normal map only with bump mapping on, and its multi-pass lights are what it bumps; the
modern lighting always uses them (`shaders/zone.frag` `mapped_normal`, `surface_shine`):

- **Normal map:** uploaded as data (mips filtered linearly, not as colour). The meshes carry no tangents, so the frame
  comes from the screen derivatives of the position and the texture coordinates; the map's green runs along +v, as in
  Direct3D's (and 3ds Max's) maps. The bumped normal feeds the whole modern light: the sun and its shadow, the sky
  hemisphere, the zone's and the dynamic lights.
- **Specular** (shaders whose name holds `CBS`, `SB`, `GB` or `Full`, and the packed `CBGG1` class when its map
  loads): a Blinn-Phong highlight of the sun at `e_fShininess0` (40 when unset), as strong as `e_fGloss0` (0.2), in
  the vertex's share of the open sky.
- **Reflection** (`CE1`, `CBSGE1` with `e_fEnvMapStrength0` > 0, read doubled): the sky
  (`sky_reflection`) in place of the material's environment map, weighted by Fresnel.

The `CBGG1` / `CBSG1` classes use a packed surface map, with environment reflection added by `CBSGE1`.
The player's compiled `SkinMeshCBSG1` and `SkinMeshCBSGE1` shaders confirm RG as the tangent normal's direction,
with Z fixed at 0.9 before normalization, B added to ambient illumination, and A replacing `e_fGloss0` for the
specular term. The actor and zone renderers use those channels separately. The diffuse colour times the glow mask
is added after the weather, independent of baked light, sky share and shadows; modern lighting gives it the same
HDR headroom as additive particles (`render::kParticleGlow`, 2.5), while classic uses intensity 1. A missing surface
map supplies no packed normal, glow or gloss. The native `CB1` and `CBS1` shaders retain full RGB normals, even when
an item shares one map across packed and ordinary materials.

The legacy `CBGGE1`, `CBE1` and `CBSE1` names select the same native render class as `CBSGE1`, so they retain the
same packed illumination and environment path. The original name is preserved in the material metadata.

Equipment environment maps may be cubemaps or ordinary 2D sphere maps. Both become octahedral textures for the
actor renderer. Sphere-map coordinates follow the native `SkinMeshCBSGE1` vertex shader: reflect the surface-to-eye
direction, project its X/Z axes around the +Y pole, then scale into texture coordinates. The cleric epic shield's
pearl map is a 2D sphere map. A malformed cubemap is rejected without treating its first face as a sphere map.

Held MPL materials retain their separately scaled coverage texture and modulate diffuse RGB component by component
(`saturate(2 * diffuse * coverage)`), while their material's alpha remains separate. WaterFall materials scroll
colour and alpha independently. Lava materials retain both scrolling diffuse layers and the shared normal map
sampled at both coordinates; the primary alpha blends its lit colour against the bump-lit secondary. Lava2 is drawn
as Lava, as the zone draws it (its own formula is not settled, docs/unknowns.md), and a lava material keeps its
second layer over an MPL coverage map, the two sharing one slot (`render::actor_lava_surface`,
`actor_takes_coverage`). In the classic lighting the native surface map is read only for a glowing part. These actor
effects use the display clock, wrapped at 100 seconds as the native preshader does. Hailstone's world light,
shadows and colour space still determine the final lighting. A missing optional coverage texture keeps the
material's diffuse colour.

The crystal's beam and radial rays in Forgotten Halls are additive model triangles. In modern lighting every EQG
`AddAlpha` zone surface, in every EQG zone, is multiplied by the same 2.5 for bloom; classic keeps their texture-direct
additive intensity. The full-bright view (`--lighting none`, `VertexLighting::None`) skips the gain: the zone renderer clears its bit there,
since the shader sees every surface unlit in that view and `AddAlpha` surfaces always are. Opaque EQG materials and
WLD additive materials are not brightened by this rule. The zone's geode particle fixtures supply the floating motes,
not the rays.

Apart from that packed glow mask, classic lighting keeps its diffuse-by-lit-colour surface path.

Hero's Forge and held attachments carry their own EQG surface properties into the actor renderer, including supplied layer
overrides. Native maps use the RGB or packed tangent interpretation selected by their material class, with the same +v convention as EQG zones;
texture-pack normals keep their separate BC5 RG interpretation and remain the fallback when a native map is absent.
Reflection uses the material's environment DDS, converted from its cube or sphere representation;
an absent or invalid map supplies no reflection. The base shader's known specular/reflective classes select the
effect, and a layer's parameters do not invent a new shader class. Native surface records share the actor's lighting
buffer, with one bounded table per frame shared by instances of a mesh. Push constants remain 128 bytes and no
descriptor bindings are added. Ordinary WLD and Luclin surfaces keep their diffuse path, animated texture frames
and attached WLD clouds.

Alpha-tested materials (trees, grass, fences, Chroma/Masked) draw in the opaque pass with alpha-to-coverage when the
scene is multisampled: the pipelines set `shaders/coverage.glsl`'s `kAlphaToCoverage`, and the alpha test becomes a
ramp about one pixel wide around the reference, capped at 0.05 of alpha so far minified textures do not leave
specks against the sky.

The zone colour and depth shaders turn that ramp into a sample mask and intersect it with the LOD fade's mask.
Their pipelines leave fixed-function alpha-to-coverage off: combining it with shader sample-mask output and
fragment discard made terrain and buildings disappear on Radeon RX 9070 XT. Both passes use the same pixel noise
and coverage helper, so cut-out edges keep the same samples when the depth pre-pass is enabled. Actor and object
world pipelines continue to use hardware alpha-to-coverage. Item previews compile a separate actor shader using
the shared explicit coverage mask and alpha 1 on accepted samples; this avoids squaring coverage when the
transparent preview is resolved and unpremultiplied for ImGui. The world shader contains no sample-mask output.

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
  Binary EQG terrain also takes its mesh's `.lit` sidecar, including that alpha. Previously only object placements'
  sidecars were read, leaving older TER meshes opaque white: their authored indoor light was lost and they admitted
  the full sun wherever its shadow map allowed.
  This is an asset-loading correction in both lighting modes, with no change to the daylight or shadow formulas.
  Older outdoor EQG zones with terrain sidecars also regain their authored lighting; zones without those sidecars
  keep their existing terrain colours.
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
- **Indoor follow-up**: restoring EQG terrain lighting is the first correction identified by the Forgotten Halls
  comparison. Fixed-camera offline captures confirm that restoring the terrain file removes the broad sunlit bands
  and restores localized lamp pools, but the hall becomes darker overall. The crystal and statues also use packed
  `CBGG1` maps (EQG surface maps): correct channel decoding restores their outward normals and the crystal's authored
  glow; the beam and radial rays use the additive model pass. The indoor baked-light balance still needs visual tuning.
  An outdoor `qeynos2` control is unchanged; shade in the older EQG `draniksscar` becomes darker with its terrain
  sidecar restored. The loader correction needs that broader visual review before it is a finished lighting pass.
  A reference player's carried light and vision can brighten the floor as well;
  a comparison needs the same position, hour, lighting options and player-light state before changing those formulas.
- **Spell light** (`game::particle_lights`): the glowing spell effects light what is round them, up to 8 lights of
  the 32. An emitter glows when it is additive, unless its texture is smoke, dust, rock, cloud, water or darkness.
  An alpha-blended one glows when its texture is fire or energy (flame, fire, flare, explosion, electric, spark,
  glow and the like). Each glowing emitter is one light at the centre of its particles, weighted by their opacity
  and size, so sparks under 3 units count less than a sheet of flame. Its colour is their tint times its texture's
  average colour (`ParticleRenderer::texture_color`). Its strength follows how much of the effect is alive, and it
  reaches 40 to 100 units as the particles spread. Emitters within 12 units merge.
  Actor effects use the same modern light calculation, with up to four EDD lights and four WLD cloud lights before
  the shared scene budget is selected. These lights follow the posed model or item and obey `/dynamiclights`.

- **Shadows** (`render/shadows.h`, `render/vk_context.h`): four cascades out to 1200 units (mostly logarithmic
  splits), each an orthographic view of its slice's bounding sphere snapped to whole texels; a depth array image
  (D32), drawn by `ZoneRenderer::draw_shadow` (opaque and alpha-tested draws, no face culling, the chunks in the cascade's volume, slope-scaled bias) and
  `ActorRenderer::draw_shadow` (the spawns `prepare_spawns` posed before the cascades; `draw_prepared` draws them in
  the scene) and `ObjectRenderer::draw_shadow` (the doors and ground objects `prepare` placed, opaque and
  alpha-tested parts, no culling; `shaders/object_shadow.*`);
  sampled with a 1.5-texel normal offset and 3 x 3 hardware-filtered comparisons, fading out over the last 15 %. The
  sun term of the zone, character and object shaders is multiplied by it. The nine comparisons are taken as four
  bilinear lookups with the same texel weights (`shaders/shadow_filter.glsl`, after Castaño 2013). The light cubes use
  the same filter. `tests/lighting_test.cpp` checks it against the nine on a model of the hardware's filter.

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
  by it (strength 0.85) before the exposure. It darkens the rendered scene colour, including every lighting term,
  so it is kept to contact shadows.

  Hidden zero-height lawn blades stay collapsed at their root with finite current and previous motion positions
  (`shaders/lawn.vert`, `blade_at`); before, their rise divided 0 by 0 and sent NaN to the depth AO reads. Grass
  keeps writing depth; AO, distance blur and shafts use the final depth. With MSAA and AO active, a global memory
  barrier from colour-output writes to fragment sampled reads also follows the final depth image transition
  (`VkContext::begin_overlay`). It repeats that transition's own scopes, so by the Vulkan spec it does nothing; it
  is an unconfirmed workaround kept because the grass AO flicker on Radeon RX 9070 XT was reported gone with the
  change that brought both. See [World rendering](unknowns.md#world-rendering) for what would settle it.

- **View distance and haze** (`render::modern_view`, `render::zone_haze_length`, `set_haze_linear`, `apply_haze` in
  `shaders/lighting.glsl`): the far plane is the classic far plane x `display.view_distance` (not where the sky flag is
  off: a zone without a sky, under water, in lava or a thick-fog region, which keep the classic one), and the fog
  distances move with it, so characters are drawn out to it. Instead of the exp2 fog wall, a haze with optical depth
  3.5 (d / length)^2, thinner above the camera (scale 600 units), towards the zone's fog colour with a glow around
  the sun; the sky blends into it within about 7 degrees of the horizon. The post pass blurs up to 1.75 pixels from a
  quarter of the way out to the far plane. Mouse picking keeps the classic yon.
  - **As thick as the zone's fog**: the haze's length is set so that it is half clear where the original's fog,
    exp(-(10 density (d - start) / (end - start))^2), is: at start + sqrt(ln 2) (end - start) / (10 density), the
    length sqrt(3.5 / ln 2) times that. It is never longer than the far plane; with the fog off (no density), it is the
    far plane, or the fog's end where the view is not drawn further. So a zone keeps the murk its builders gave it, and
    the longer view only lets what lies past the original's far plane fade into the haze instead of being cut off.
    Density 0.1 (innothule, nektulos version 0, the commons) is a light fog, a third clear at its end; 0.33 (most
    zones, nektulos version 1) a thick one, nearly opaque by about 60 % of the way. Before, the haze ran over the
    whole extended distance (nektulos version 1, served from Rain of Fear on, showed no fog at all, half clear at
    1670 units against the original's 390), and where the view was not extended its length was the fog's end
    whatever the density (innothule's light fog about 3.5 times as thick, the dark swamp near the camera washed into
    the fog's bright colour).
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

## Horizon hills

In the modern lighting, outdoors, land stands round the zone past its edges, out to about the modern view's reach, so
the world does not end in a blank haze (not round a zone floating in the sky: Sky zones, below) (Hailstone's own; the classic client drew nothing past a zone's geometry). It is
real geometry: a mesh built while `ZoneRenderer::prepare` builds the zone (`render::horizon_mesh`, `render/horizon.h`)
and drawn with the zone's own materials through `zone.frag`, so it takes the zone's textures (a texture pack's photo
materials and normal maps too), its light, the sun and the haze exactly as the zone's ground does, and the depth test
puts it behind anything nearer. Scenery only: nothing collides with it. It is made from the current zone alone:
the zones past its zone lines are not drawn and do not shape it (up to 0.7 the neighbour zones were drawn past their
lines, then copies of the zones visited on the server; at the zone lines they showed walls, filler and seams that no
general rule fixed across the game's zones, and they were removed).

- **What the zone says** (`render::horizon_land`): the terrain's opaque triangles, and its water
  and lava surfaces. The level ones (the normal's z at least 0.8, either way up) give the level ground's extent (2 % of
  its area cut each side), its median height (the land's base) and its spread from the 10th to the 90th percentile,
  40 to 1200 units (the relief, which makes rugged zones' land taller). A zone with less than 250,000 square units of
  level ground gets none.
- **The grid**: the zone's terrain (geometry more than 4000 units past the level ground left out) is laid on squares
  64 units across. Terrain anywhere in a square covers it, with its lowest and highest point; a water surface marks it
  wet. Each empty square takes, from the nearest covered square, its distance and the zone's edge there: the highest
  terrain (a wall's top, the open ground at a zone line) or, where the water reaches the edge, the water's surface;
  smoothed over a few squares. Whether the edge is a shore there: water straight in from the edge within 192 units,
  behind a quay, averaged over 1024 units of edge, so a river's mouth is no shore.
- **The land**: a grid of vertices over the zone's squares and 2500 units past them (64 units apart, more for a zone
  wider than 256 squares with that), then wider by 12 % a step out to 10000 units past the zone's squares. Vertices in a covered square are tucked 40 units under its lowest terrain, and in
  the empty square beside one 5 units under the lowest terrain beside it (a wall's foot; 40 under the zone's water
  where only water is beside it), so the land meets the zone's edge
  without a gap and stays under its ground; the squares under the zone are drawn too (they show only through the back
  of its walls, which have only their inner faces). Past the edge the land starts at the edge's height and turns over
  2500 units into rolling hills (200 to 650 units high with the relief, growing from 300 to 3000 units out) and, from
  2500 to 8000 units out, ridged mountains (900 to 2250 units, up to half again where a broad mass lifts them), so it
  rises gently by the zone and the steep ground stands far off, behind the fog past the edge (up to 0.7 the hills were
  full height 800 units out and the mountains rose from there to 5000, steep right at the zone line), all seeded by the zone's short name
  (`render::horizon_seed`), so it is the same every visit. The hills are warped noise, their lines bending and
  branching (the coordinates pushed up to 900 units by a noise of their own before the hills and ridges are taken),
  not a value noise's grid of blobs, as up to 0.7. A pocket the zone's terrain closes round (a gap between its walls,
  a hole in it) is a flat lid just under its rim. Where the edge is a shore, the sea: its floor falls to 250 units
  under the zone's water level (the median of its water surfaces) within 800 units and stays there out to the land's
  reach, open sea with no coast (nothing says where the next coast is: docs/unknowns.md; up to 0.7 coasts and islands
  rose from it 2500 units out); where a shore ends the coast wanders in headlands, bays and the odd island (the shore's
  share pushed by a noise 700 units across) rather than following the edge's blur. Its surface is drawn at the water
  level with the zone's own water material, from the zone's own water's edge, over every square of the land the sea
  reaches at a corner, so the coast runs where the land rises through the water and not along the 64-unit squares (up
  to 0.7 a square was water only when all four corners were sea, which drew the coast as steps); not over the zone's
  own terrain or water, nor a square whose corners' seas stand at different levels.
- **Its wear** (`render::horizon_terrain`, Hailstone's own): the hills are worn by water and slumping, so they read as
  ranges with valleys from afar. A grid over the land (64-unit nodes, at most 512 a side) takes the land's heights;
  as many drops of water as it has nodes run downhill on it (Hans Theobald Beyer's particle erosion, "Implementation
  of a method for hydraulic erosion", 2015: inertia 0.08, at most 48 nodes each, taking ground up where they speed
  and dropping it where they slow), then six passes of slumping move ground off slopes steeper than it holds. Each
  node is worn only as far as it is free: not at all on the zone's squares or by its edge or under the sea, all of
  it 1500 units past the zone's edge; and at most 384 units down and 192
  up. What is worn is added to the land's heights (`HorizonTerrain::delta`), and where the most water ran (`wet`) the
  woods gather and the ground is darker. It is the same for the same ground: the drops come from a generator seeded by
  the zone, the rows of its first pass are shared out among up to eight threads, and its layers are kept to an eighth
  of a unit and a 255th. **Cached** per zone and server (`cache/terrain/<server>/<zone>.bin`,
  `render::write_horizon_terrain`, docs/layout.md), keyed by what it was worn from (`horizon_terrain_key`: the
  zone's grid and heights, the generator's version, 2 since the neighbours no longer shape it), so a change to any of
  them wears it again. Wearing takes 182 to 236 ms of the zone's build (Qeynos Hills, Butcherblock, North and
  South Karana, 400 to 513 nodes a side), read from the cache 1 to 2 ms; a file is 0.5 to 0.7 MB.
  Under the sea the land is darker with the depth, to 0.3 of its light 150 units down (the water map that darkens a
  zone's own sea floor leaves the land out).
- **The look**: the zone's plain material (no shader effect of its own, no scrolling) that covers the most level
  ground draws the land, the one on the most slopes its rock, and the most used water material the sea. The ground
  and the rock are counted by the edge of the zone's box (within 400 units, or 12% of the box across), where the land
  meets the zone, when anything plain lies there, else over the whole zone; level ground under water (a water surface
  over it, or the point just over it in a water region) is not counted, nor a slope standing more than 40 units over
  the lowest surface of its 16-unit square (a roof, an overhang), nor for the rock or the sand a texture named as
  building material (roof, brick, wall, tile, floor, plank, wood, shingle). Counted over the whole zone, Freeport
  East's harbour floor drew its land teal, North Ro's its sea bed, and North Qeynos's roofs its peaks pink. Each is each mapped
  with its own texture scale (the zone's UV area over area) and wound as the zone winds its own. The current zone's
  land is one draw (`zone.frag` `kLandSplat`): each vertex says how much rock (slopes under 0.8 up and the mountains'
  upper half) and sand (a few units over the sea) cover it
  (`HorizonMesh::splat`), the edges between broken up by the textures' own light and dark. The rock is mapped from all
  three axes and blended by how the surface faces each, at half the zone's own scale, twice (the second turned and at
  another scale, the two blended by a slow pattern over the world) and pulled half way to its own average colour: a
  rock texture with bands in it (Qeynos Hills' `HHDROCKGRASS.BMP`) drew the same bands down every slope, as stripes.
  The sand is the zone's level texture that says so by its name (`sand`, `beach`), else its ground made sandy (grey,
  then warmed). The ground's own repeat is broken up by itself at a fifteenth of its scale. The sea is drawn from both
  faces, since a classic water surface is two sheets, one
  facing each way, and says nothing of which is up (up to 0.7 the facing taken from it culled the whole sea from
  above, so the land's sea showed as bare floor: Butcherblock's and Lake Rathetear's). Its vertex colour is the level ground's median baked light, darker in woods-like
  patches on the level land, in the valleys and in broad patches that keep the texture's repeat from showing. The
  log says what it used: qeytoqrg 36892 triangles, ground `XGRASSDIRT2.BMP`, rock `HHDROCKGRASS.BMP`; butcher with
  10108 water triangles.
- **When**: outdoors (time types 1, 2, 5), under an open sky (1, 2, 4) or none (gfaydark, everfrost); not under the
  planes' and Luclin's painted skies. A thick fog, lava, a thick-fog region, a storm and the night hide it as they hide
  the zone, by the same haze.
- **Cost**: in Qeynos Hills about 99,000 triangles in 256-unit chunks (44,000 before its squares were kept fine out to
  2500 units past the zone), culled with the zone's; no shadows (neither cast nor
  drawn into the sun's cascades or the cover and water maps). Looking out over it, 9,000 more triangles and 70 more
  draw calls; the frame time did not move. The splatted look costs a little: at 1600 x 900 with the vsync off, looking
  west over Qeynos Hills' coast 1.00 ms a frame against 0.96 with the plain land, and north over Butcherblock's woods
  1.26 to 1.30 against 1.17 (the rock and the sand are sampled only where a vertex has some: sampled everywhere, 1.19
  and 1.43).
- **Its trees** (`render::horizon_trees`, `render/tree_impostor.h`): the zone's own trees stand on the land's woods.
  The zone's commonest placed plants at least 20 units tall (`render::kLandTreeMinHeight`) are its trees, at most
  three kinds, each placed at least a quarter as often as the commonest and at most twice its height (North Karana
  walls its edges with RPINE1, 304 units tall by its 91-unit TREE5, which across the land stood as a wall of giants).
  Each is drawn once at load, on the CPU, into a picture from two sides (its own textures, the far side of its crown
  a little darker; 2 x 2 samples a texel, the cover in alpha) carried by two crossed cards, cut out and two-sided,
  with their normals up so they take the light the land does; thousands of them are drawn instanced, 4 triangles
  each, tinted by the median of the zone's light on that tree's placements. They stand from 256 units past the
  zone's edge out to 7000, as far apart as the zone's own trees stand over its level ground (a third of that in the
  land's woods, the darker patches its shade paints, and one and a half times it elsewhere), fewer past 2500 (as
  2500 over the distance), on the land's surface as its mesh draws it, 2 units sunk; not on the sea, the mountains'
  rock, slopes steeper than 0.85 or in the zone's pockets; at most 40,000, the nearest kept. They cast no shadows and
  go with the land (`set_distant`). Seen in the zone viewer (up to 0.7, with the neighbours' ground): Qeynos Hills
  2,610 trees, Butcherblock 4,823, South Karana 13,949; placing
  them takes 16 to 58 ms of the zone's build; in South Karana at 2560 x 1440 the frame took 1.29 ms with them and
  1.22 without (178 more draw calls).

Out to 300 units past the zone's terrain it grows the zone's grass (Zone flora, "Past the zone's edge").

`display.horizon_hills` (Options, Horizon Hills) turns it off, its trees with it; the viewer takes `--no-horizon`, and
`--no-land-trees` for the land without its trees.

## Sky zones

A zone that floats in the sky, with open sky under its ground (`render::horizon_open_below`: Plane of Sky `airplane`,
Plane of Air `poair`), gets in the modern lighting:

- **No land round it and no fog past its edge**: they would be ground where there is none.
- **Its own painted sky dome**, not the atmosphere, which goes into the haze below the horizon; and the dome and its
  cloud layers drawn again mirrored under the horizon, behind themselves (`SkyFrame::mirror_below`), so the sky goes on
  to the nadir past the dome's foot, and past that the background is the open sky's colour straight up
  (`render::open_sky_below`), not the zone's fog.
- **The haze in the sky's own colour** at the horizon (`render::haze_target`, `in_the_sky`), whatever the zone's fog:
  PEQ's zone table gives Plane of Sky a black fog, which turned its far islands, and everything under them, black.

Its sky is the plain one (sky 1 in PEQ's zone table), so nothing the server sends says so, and the zone's shape does
not either: the share of open air between the zone's pieces of ground was 14 % in Plane of Sky but 3 % in Plane of
Air, against up to 6 % in the ordinary outdoor zones measured (and 27 to 35 % in the box round East Karana's, North
Ro's and Trakanon's level ground). So the zones are listed by name (docs/unknowns.md, World rendering).

## Zone edge fog

In the modern lighting, outdoors and out of the water, a fog bank lies on the ground past the zone's own terrain, so
at a zone line the world fades into mist instead of ending, and the land round the zone (Horizon hills, above) is not
seen up close; its taller hills and mountains rise out of the bank far off. Hailstone's own; the original drew nothing
past a zone's geometry and hid that in its fog wall. It replaces drawing the zones past the lines (up to 0.7), which
showed walls, filler and seams there.

- **Where** (`render::zone_fog_field`, `render/zone_fog.h`): the land's field (`render::HorizonField`, its 64-unit
  squares): each open square's distance to the zone's terrain, and as the bank's floor the zone's lowest ground by its
  edge (the nearest covered square's lowest terrain, or its water's surface over that, smoothed; never over the edge's
  height the land starts from). Not the edge's highest terrain: East Freeport's walls and cliffs lifted the bank
  hundreds of units over the open ground and into the sky; walls and cliff tops stand out of it instead. On the zone's
  own squares and in a pocket its terrain closes round there is none. Packed into an RGBA8 texture (distance in steps
  of 16 units, floor over the zone's range of floors); past the grid the distance goes on growing.
- **How thick**: density 0.0025 a unit (1000 units of it leave 8 % of the light; 0.006 until it was found a little
  strong) times a ramp from 0 at the zone's
  terrain to 1 at 1400 units past it, smooth, up to a top 250 units over its floor, thinning out over 100 either
  side of it. So across open ground at a zone line it thickens until nothing shows, looking up the sky is clear (30
  degrees up, under 10 % of it is hidden), and from a hill one looks down on the bank's top.
- **Never round the player** (the camera is always in the zone, whatever the field says): none within 300 units of the
  camera, full by 900. Where the field has the camera past the zone's terrain (ground the field does not count: it is
  made from the zone's terrain mesh alone, and East Freeport's ground by its North Ro gate is placed objects), the fog
  is pushed out by that much and its floor kept under the camera's feet; before, the camera stood in the bank there
  and the sky above it was fogged.
- **Broken up** (`shaders/zone_fog.frag` only; the CPU functions give the mean): the top rises and falls by up to
  100 units over swells about 900 units across, and the thickness varies in patches (0.45 to 1.55 times, about 360
  across), both drifting slowly with the time (6 and 2.5 units a second; the clock is wrapped every 4096 s, when the
  mist shifts once).
- **Drawn** (`VkContext::set_zone_fog_field`, `set_zone_fog`): a full-screen pass right after the scene ends, before
  the AO, the bloom and the upscalers, so they all see the fogged picture. Each pixel's line of sight is marched from
  the camera to the depth (the sky's to 16000 units, at most the far plane) in 40 steps, closer together near the
  camera (t = length s^2), with a 4 x 4 ordered dither, stopping where under 0.1 % shows through; the fog's colour is
  the haze's 1.35 times as bright (`kZoneFogLift`: the haze's own read a little dark as a mist close by) with its sun
  glow (as `apply_haze`), blended over the scene by what shows through. The field is uploaded
  when the zone's land is built (a new zone), the old one retired with the frames that use it. In North Karana at
  1280 x 720 on an RX 9060 XT the pass took 0.21 ms of the GPU (the AO 0.26).

`display.zone_fog` (Options, Zone Edge Fog) turns it off; the viewer takes `--no-zone-fog`, and logs the field and the
distance past the zone's terrain at the camera.

## Smoothed ground

A classic zone's ground is drawn with smooth normals and curved, subdivided triangles instead of the file's flat
facets (`assets/ground_refine.h`). A deliberate difference from the original, the look only, in both lightings:
`Zone::terrain` stays as loaded and keeps collision, the regions, the liquids' classification, the ambient probe and
everything else that must agree with the server's geometry. The refined copy goes into `Zone::draw_terrain` (with
`draw_terrain_source`, the terrain triangle each drawn one was cut from), which the zone renderer and the zone flora
use; the lawn and the plants stand on it, and the flora's cover test sees it.

- **Ground:** the solid, opaque triangles of the classic (WLD) terrain meshes. Not cut-outs, translucent or invisible
  surfaces, passable ones (water), heightmap tiles or EQG meshes.
- **Normals:** corners are welded by position (to 1/128 unit) across meshes and materials, and each corner of a
  triangle takes the angle-weighted average of the faces round it within 45 degrees of its own face. Hills turn
  smooth; a wall's foot, a cliff's edge or a building's corner stays sharp.
- **Shape:** each triangle becomes a cubic PN triangle (Vlachos, Peters, Boyd and Mitchell, "Curved PN Triangles",
  2001) over its corner normals. An edge is curved only where exactly two ground triangles meet on it and agree on
  both corners' normals; a crease, the ground's border and a seam with a wall or a cut-out stay the file's straight
  edge, uncut, so the refined ground meets the rest of the zone exactly. A curved edge is cut into 2, 4 or 8 segments,
  so a segment cuts through the curve by at most about 0.05 units. A triangle takes the most of its edges' (or what
  its own bulge needs), S: the points whose weights are all at least 1/S form a regular inner grid, and each edge's
  own samples are stitched to the inner grid's side along it. An edge's samples are counted and evaluated from its
  lower welded corner, so both triangles on it make the same points bit for bit, and no crack opens where two
  triangles cut it differently (Qeynos Hills: 9,655 ground triangles drawn as 127,293, the open edges the same 509
  as the file's).
- **Offset:** an edge's control points are held within 1 unit of the flat triangle, so the curve stays within 1
  unit of the ground that collision, feet and the server's spawn heights are measured on (Qeynos Hills: at most 0.88).
  Hills' outlines so change little; the shading changes most (flat light and dark planes on hills and rock become
  smooth gradients).
- **Attributes:** UVs, both vertex colours (the baked light) and the detail passes are interpolated over the original
  triangle.
- **Cost:** about 40 ms more on Qeynos Hills' load. In the viewer at 1280 x 720 on the RX 9060 XT, a view over its
  hills went from 1.52 to 1.60 ms a frame to 1.74 to 1.77 ms (65,000 zone triangles drawn instead of 18,000, most of
  the difference in the shadow cascades).

`display.smooth_ground` (on by default, read as each zone loads) turns it off; the viewer takes
`--no-smooth-ground`. The log says how many triangles were refined into how many.

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
   - The built-in upscaler (`texpack::upscale_texture`) refuses a texture whose output, or its colour model's padded
     output, would pass 8192 x 8192 texels (`kMaxUpscaleTexels`): its float planes take 16 bytes a texel. The job
     notes the error and goes on.
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

### Photo materials

A built pack can rebuild a classic zone's hand-painted ground from tiling photo textures (Poly Haven's, CC0) instead
of upscaling it (`texpack/materials.h`; Hailstone Texture Studio's "Photo ground materials"). The rules are data the
studio ships: `materials.txt` names each material (its Poly Haven asset, the metres one repeat shows, how much it is
magnified, its detail strength, how much of the original's painted shading it keeps, and its colour in the art), and
a file per zone (`qeytoqrg.txt`) gives each of the zone's textures one material or two
(`hhdrockgrass1 = grass greyrock`; `sharp` for small shapes such as a path's flagstones). A zone file may set a
material's colour in that zone's art (`colour grass = 0.30 0.27 0.12`), for its own tiles only: one material serves
zones whose art differs (Qeynos Hills' green grass, Everfrost's dark tundra), and its photos are loaded once.

- **Scale:** each texture's world units per repeat are measured on the zone's ground (the area-weighted median over
  its triangles). A material repeats a power of two times across a tile, so Qeynos Hills' 56-unit grass tile and its
  108-unit transitions show the grass at one scale. An EQ unit is taken as a foot.
- **Two materials:** the original is split by a 2-means on its blurred chromaticity, seeded with the two materials'
  colours (brightness barely counts, so the art's painted shadows are not taken for grass). The photos are blended
  along that split, the edge moved by their displacement maps so it follows stones and soil.
- **Detail transfer:** the result keeps the original's colour above about 6 of its texels (1.5 for `sharp`) and takes
  only the photos' luminance detail below that, with part of the original's own painted shading. A rebuilt tile so
  keeps the zone's palette and still meets the tiles round it, rebuilt or not; blurs and resizes wrap at the edges.
- **Files:** the colour, normal and roughness maps go through the pack's encoder (BC7, BC5, BC4 with mips), at 512 x
  the pack's scale on a side (1024 for a 2x pack). A texture with alpha or a variant, or not square, is upscaled.
- **Resuming:** `PACK/materials.txt` records the rules each tile was made by; a build makes a tile again when its
  rules change, and gives a texture back to the upscaler when the rules drop it.
- **Fetching:** the 1K JPEG diffuse, `nor_dx`, rough and disp maps of each asset from Poly Haven's download host, once,
  into the studio's cache.

The studio has rules for the original game's 34 outdoor zones (not the expansions', not the revamped ones; in cities
the ground only). A texture that is also drawn with a cut-out somewhere (three of North Ro's) stays with the upscaler.
Seen in the zone viewer, not in a live session: the layout and seams hold; the photos' repeat still shows on large
flat areas.

### Model overrides

A texture pack can also replace a zone's placed objects: `PACK/models/<zone>/<object>.glb`, a binary glTF 2.0 file
named after the object (`TREE1_ACTORDEF` or `TREE1`, any case), is drawn in place of that object
(`assets/model_overrides.h`, read with cgltf). A deliberate difference from the original, and the look only: the
collision, picking, ladders and the ambient probe keep the game's mesh, so where a player can walk does not change.
The pack folder is the one `display.texture_folder` (the viewer's `--textures`) chooses; the files are read as each
zone loads (`Zone::draw_objects`).

- **Frame:** glTF's own, Y up; its units are EQ units, and glTF (x, y, z) is the zone's (x, -z, y). Front faces are
  counter-clockwise; each node's transform is applied, made top-down as the node tree is walked, and primitives
  that share their accessors under one transform share their vertices (found in a hash map), so a file of many
  nodes or primitives loads in time linear in them.
- **Materials:** a material's base colour texture names its texture by its image's URI: the file name is looked up as
  a replacement texture is (the zone's subfolder first, so a model's textures can sit beside it), any extension, with
  its `.n` and `.r` maps. `MASK` is drawn as a cut-out, `BLEND` at 50 %, `doubleSided` without culling. Images inside
  the file and buffers outside it are refused.
- **Made for a mesh:** a file whose `asset.extras.hailstone_source` holds the hash of the game mesh it was made for
  (`object_mesh_hash`, 16 hex digits) is used only on that mesh, so a game update that changes the object turns it
  off; one without (made by hand) is used on any object of its name in that zone.
- **Instanced:** the zone renderer keeps each replacement mesh once, in its own space, and draws it at every placement
  with a per-instance transform (`GpuInstance`: the transform's rows, the normal matrix's, the sway's scale and a
  tint; the buffer's first entry is the identity, which the zone's own geometry uses, so one set of pipelines draws
  both). Each placement of each level is a chunk of its own, and neighbouring placements in view go in one call.
  Qeynos Hills with the generated trees: 52,000 vertices and 146,000 indices on the GPU, from 3.7 million and
  8.3 million (266 MB of vertices) when every placement was a copy.
- **Light:** a placement's baked light is one colour per vertex of the game's mesh; the replacement takes their
  average as its instance's tint, which multiplies its own vertex colours (a generated tree's shade inside its crown).
- **Plants:** a replacement sways in the wind as the game's object would (`is_plant`): its cut-out parts flutter. One
  with levels of detail (a generated tree) does not flutter, and its branches sway whole (`render::plant_sway`,
  `assets::Mesh::sway_heights`): the generator gives each vertex the height it sways as, in the second UV set
  (glTF TEXCOORD_1, asset extras `hailstone_sway`), a branch with its needle sprays the height of its
  root on the trunk, the trunk and the far cards their own. A sway by each vertex's own height, or a flutter, moved
  a branch's crossing cards apart, so up close its leaves seemed to flip back and forth (a third of the bend without
  the flutter still did). A generated tree from a pack built before stands still. Generated trees stood still for a
  while because a pine still flickered by its swaying branches; the cause was the upright card of the clump once at
  each branch's end (Styles, below), and with the clumps gone the branch sway is calm.
- **Levels of detail:** a node's extras `{"hailstone_lod": [near, far]}` draw its meshes only from `near` to `far` world
  units from the camera (`MeshPart::lod_near`, `lod_far`). The zone renderer keeps each band's draws apart, a chunk
  for each placement, and chooses by the camera's distance to where the placement stands (`render/lod_bands.h`), so
  all of a tree changes level together. A model's levels are its bands in order of their near ends.
  - The scene and the depth pre-pass fade one level into the next across 20 % of the distance where they meet,
    centred on it: both are drawn there, and each of a pixel's samples shows exactly one of them
    (`coverage.glsl lod_fade_samples`, a sample mask by the placement's distance from `zone.vert` and a per-pixel
    noise), more of the next level the further the tree, so it dissolves instead of jumping. With 8x MSAA that is
    nine steps a pixel; without MSAA, a dither.
  - A shadow needs less detail than the tree it falls from: the sun's two nearest cascades draw at least the second
    level (the camera's where coarser), the two further ones the last, and a light's shadow cube, drawn once and
    kept, always the second whatever the camera's distance. Shadows do not fade; they take the camera's position of
    the frame before. A banded draw is not merged into the untextured shadow casters.
  - `display.tree_detail` (Options, Tree Detail Distance, 50 to 200 %) scales every band's distances.

### Generated trees

Hailstone Texture Studio's "Generated trees" grows a classic zone's trees again (`texpack/trees.h`) and writes them as
model overrides with their textures (BC7 with mips; the bark's normal and roughness maps as `.n` and `.r`). The game's
trees are a trunk of a few solid prisms under crossed cut-out cards. Each new tree is fitted to the game's:

- **Trunk:** the prisms' rings, found by their hop count along the prism edges from each piece's bottom (a limb that is
  a prism of its own, as TREETRY's second one, hangs from the ring nearest its root), make the skeleton. The bark is a
  tube along it at 0.8 x the prism corners' distance: between the prism's faces and its corners, so a player stopped
  by the collision stands at the bark.
- **Leaves:** points spread over the cards and kept where their texture is opaque give the crown's shape; turned round
  the trunk, they fill it. The leaves are drawn in colours picked from the game texture's opaque texels, then scaled
  to its mean, so an autumn tree keeps its yellows among its greens.
  The gaps up to a few dozen texels across inside a spray or a cluster (3.5 % of the picture's side for needles, 3 %
  for leaves) are filled with a shadowed base (a closing of the cut-out), so a spray is cut out along its outer edge
  only: thin holes showing the dark or the sky behind it shimmered as the camera moved. A needle spray's two
  layers are close in shade and its twigs a darker shade of the needles, not brown: stark stripes and bright twig
  lines shimmered as the camera moved and read as streaks from afar. Each needle's shade goes only 0.4 of the way
  from the spray's mean towards the palette colour it picked (the game textures' palettes reach near white), and
  the gaps are filled at 0.8 of the mean: needles a few texels wide in stark light and dark changed every pixel
  they covered as a spray swayed or the camera moved, a flicker standing by a tree and a shimmer walking through a
  forest. In the zone viewer (frames 4 ms apart, pixels changing by more than 12) that took a flight over Greater
  Faydark from 11 % to 5 % (the game's own trees 3.5 %) and standing by Lake Rathe's pines in the wind from 0.67 %
  to 0.14 %; the shadows, MSAA, anisotropic filtering and the levels of detail made little difference.
- **Styles:** a pine (a single upright trunk up into leaves widest low down) grows whorls of drooping branches with
  two crossed needle sprays up a leader (no clump of crossed cards at a branch's end: its upright card showed as a
  flat slab from beside the tree), the whorls three units apart or more (9 to 18) with
  four to six branches each, so the crown shows gaps; a broadleaf tree grows branches into the crown by space
  colonisation (Runions, Lane and Prusinkiewicz 2007), leaf clusters at their tips; a willow hangs strands from its
  upper branches. A dead tree (`dead`: Everfrost's and Dagnor's Cauldron's bare pines) grows a few long limbs from its
  upper trunk, rising and forking once, with sprays of drawn bare twigs at their ends. A palm (`palm`) carries its
  trunk on up to near the top of the game's leaves (its solid prisms stop halfway; the cards paint the rest) and
  springs two dozen arching fronds from there, a drawn frond of narrow leaflets along a rib. The rules may name the
  style (`tree tree71 = willow bark`); `auto` takes pine or broadleaf by shape.
- **Bark:** a Poly Haven photo material scaled to the game bark's mean colour, or a drawn one.
- **Levels of detail:** the near tree to twice its height (at least 200 units); then a middle level with the same
  leaf cards, each one straight quad (a tree does not change its leaves where the levels meet: fewer cards, grown to
  keep the cover, showed as a flicker over a forest as the camera moved), about half the sides and no thin
  branches; and from eight times
  its height (at least 600 units) three crossed cards and a level one, with a picture of the near tree from the side
  and from above (rendered by the generator: a small rasteriser, twice the size and averaged down). The near and
  middle levels are built together, so they have one shape.
- **Cards cut to their leaves:** each leaf card is cut to its texture's outline (`card_outline`: the convex hull of
  the texels that are not transparent, cut down to six corners by the cheapest cuts, six texels out for the smaller
  mips), each quad of a strip clipped to it in texture space. A spray fills only part of its square: a pine's card
  keeps about 42 % of its area, a broadleaf cluster's 74 %. It costs triangles (a quad is a fan of up to four) and
  saves the pixels the alpha test would throw away.
- **Budget:** about 1,600 to 7,200 triangles a tree at the near level (the tenth to the ninetieth percentile of
  the 129 classic trees; 12,000 to 20,000 for the few biggest broadleaf trees), 1,000 to 3,000 at the middle, 8 at
  the far. In Greater Faydark at 3124 x 1366 with 8x MSAA and 4096 shadow maps a forest view took 8.5 ms (10.0 with
  the denser pines and fewer middle cards before; 7.5 with the game's trees; zone viewer, offscreen), 700,000
  triangles a frame from 1.1 million. The figures below are from before that change. At the
  Qeynos Hills test spot the zone drew 56,000 triangles a frame (3,400 with the game's trees; the trees in a fade
  draw two levels), 35,000 looking over the zone from above, and 9,300 into the shadow map (7,300 with the game's
  trees). Cutting the cards doubled the leaf triangles and took about 5 % off the frame at 2560 x 1440 with 8x MSAA
  in a view full of trees (3.29 ms from 3.47; zone viewer, offscreen). Each kind of tree is stored once (instanced),
  so the triangles cost no memory per tree.

The rules are lines of the studio's zone files (`tree <object> = <style> [<bark material>]`; the object a name of
letters, digits, `_` and `-`, since it names the tree's files); the 34 zones with
ground rules have them (129 trees in the 30 that have trees). A tree comes out the same on every build (its seed is its zone and name). Seen in the zone viewer, not
in a live session. Open: leaves thin out in the smaller mips of a cut-out.

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

NPC names are drawn in the spawn's consider colour (`game::con_rgb` of `game::con_level`, the colour Hailstone's target plate already uses) instead of this client's yellow (255, 220, 120). Mercenaries keep the original's amber (`#e1b511`, `game::mercenary_name_colour`) and corpses stay grey, while other players' names stay light blue. This is a deliberate difference (Hailstone option `con_name_colour`, Options > Display, `hailstone.ini` `display.con_name_colour`, default on); turning it off restores the yellow for NPC names. The target keeps its consider colour in both settings.

GM names are green (`#1da64b`) as the original draws them (`game::gm_name_colour`, after PlayerClient::SetNameSpriteTint @0x58bf00): a spawn with the gm flag that is not a live NPC (type 1) and is neither anonymous nor roleplaying (the spawn's Anon field 0; `/anon` and `/roleplay` both drop it). It covers the player's own GM character. A corpse is never green, a deliberate difference: the original keeps the green on a corpse that carries the flag, where Hailstone shows the ordinary corpse colour (stock EQEmu sends corpses without the flag, so only a server that sends it would show the difference). A targeted GM keeps the green instead of taking the consider colour: the original fades the target's tint toward white, and keeping the green is this client's choice. There is no option; it is the original's colour.

The other colours are not the original's: PlayerClient::SetNameSpriteTint gives every NPC but a mercenary one fixed tint, teal (`#17b8ad`), and only flashes the target's toward white; the con-coloured target plate is Hailstone's look too (`draw_nameplates`).

SetNameSpriteTint's colour is three bytes that it hands to the actor's `SetStringSpriteTint(const RGB*)` (CActorInterface vtable +0x190, CHierarchicalActor::vf100 @0x10040f00 in EQGraphicsDX9.dll). The bytes are blue, green, red in that order. eqlib declares the slot with its `RGB` type, whose bytes are Blue, Green, Red, Alpha (a little-endian 0xAARRGGBB, the D3D colour). MacroQuest2's caption colours call the same slot with con colours written as 0xAARRGGBB values (red 0xFFFF0000), and those names show in their con colours. The original's own tints agree: PvP names, bytes 14 14 a6, come out red; the PvP-team scheme makes the evil races' names orange-red (00 80 ff) and the elves' yellow (00 ff ff). So the bytes 4b a6 1d are the green `#1da64b`, a mercenary's 11 b5 e1 are amber `#e1b511` (this client used to read them red first and drew mercenaries light blue), and the NPC tint ad b8 17 is teal `#17b8ad`. The rof2decomp rewrite's `set(r, g, b)` names the bytes in memory order and does not mean they are red first. Which name sprites are drawn, and their parts: docs/mechanics.md "Name sprites".

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

## Detected settings

The first start fits the graphics to the computer (`render/graphics_tier.h`), so a weak machine does not start with
everything at its highest. Before the settings are first read, the GPU in use and the CPU are sorted into a tier:

| | Tier |
|---|---|
| Discrete GPU, 6 GiB of its own memory or more (its largest device-local heap) | High |
| Discrete GPU, 3 to 6 GiB | Medium |
| Discrete GPU under 3 GiB | Low |
| Integrated GPU: Apple's | Medium |
| Integrated GPU: any other (Intel, AMD); a software renderer or a virtual GPU | Low |
| Then the CPU: under 4 threads | Low at most |
| Under 6 threads | Medium at most |

Low and Medium are the Options window's quality presets of the same names (Graphics > Preset: shadows,
anti-aliasing, ambient occlusion, distance blur, view distance), with a few more of the costlier settings; High
leaves every one at its default, so it never picks Ultra. Low also turns upscaling and frame generation off, as they are by default, so Detect turns off an FSR 3.1 the player picked (each
costs a fixed share of the frame at the output's size, whatever the render scale; on an Intel Iris Xe tablet FSR 3.1
was far slower than FSR 1) and draws with the classic lighting and no radial flora; Medium and High keep what the player chose for
those four (kept), and Options > Graphics notes on a Low computer that FSR 3.1 and frame generation are slow there:

| Key | Low | Medium |
|---|---|---|
| `display.shadows` | 0 | 1024 |
| `display.msaa` | 1 | 2 |
| `display.ao` | 0 | 0 |
| `display.distance_blur` | 0 | (default) |
| `display.view_distance` | 1.5 | 2.0 |
| `display.light_shadows` | 0 | 4 |
| `display.render_scale` | (default) | (default) |
| `display.horizon_hills` | 0 | (default) |
| `display.volumetric_clouds` | 0 | (default) |
| `display.grass_density` | 50 | 75 |
| `display.tree_detail` | 50 | (default) |
| `display.upscaler` | off | (kept) |
| `display.frame_gen` | 0 | (kept) |
| `display.lighting` | classic | (kept) |
| `display.grass` | 0 (Radial Flora off) | (kept) |

The tier is written only to a new install's `hailstone.ini` (one with no `display.` key yet; the asset folder the
picker saves first does not count). An existing install keeps its settings. Either way the tier found is saved as
`display.gpu_tier`, and the detection does not run again while that key is there; the log names the GPU, its
memory, the CPU's threads and the tier. A character's settings file starts as a copy of `hailstone.ini`'s keys, so
characters made later start at the tier too.

Options > Graphics > Detect (gamepad: Display > Graphics for this computer) detects again and applies the tier over
the settings there, each key a tier leaves at its default erased; anti-aliasing and the upscaler change at the next
start, as always. The thresholds are a guess at what each class of hardware runs well, not a measurement: a GPU's memory
stands in for its speed, and an integrated GPU's heap (the system's memory) says nothing, so every integrated GPU
but Apple's is Low.

## Settings

In `hailstone.ini`, and in the Options window (`ui/modern/options.lua`, this client's own window: its Display and
Graphics tabs; the client's options by name in `app/client_app_options.cpp`, the graphics ones `display_option`); the
viewer (`--zone`, `--model`) takes `--msaa`, `--tonemap`, `--exposure`, `--render-scale`,
`--sharpness` and `--upscaler`, `--frame-gen` and `--screenshot-generated`. The client takes `--lighting` (`classic`,
`smooth`, `modern`; `none` is the viewer's alone), `--msaa`, `--tonemap`, `--exposure`, `--shadows`, `--light-shadows`,
`--no-ao`, `--depth-prepass`, `--bloom`, `--render-scale`, `--sharpness`, `--upscaler`, `--frame-gen` and
`--view-distance` as the `display.*` settings they stand for (`app/display_flags.h`), for that run only: they go over
hailstone.ini's and the character's values and are never saved (`Config::set_run_override`), and a change to one of
them in the Options window takes over and is saved as usual.

| Key | Values | Default | |
|---|---|---|---|
| `display.msaa` | 1, 2, 4 | 4 | samples per pixel, at most 4 (`render::kMaxMsaa`; 8 and up are drawn at 4), cut to what the GPU supports; off under `display.upscaler = taa`; read at start-up |
| `display.render_scale` | 33 .. 100 | 100 | percent of the 3D view's size the scene is drawn at; below 100 the upscaler (`display.upscaler`) brings it up (The frame); the UI stays sharp. Options > Upscale Quality's presets set it (`render::UpscaleQuality`): Native AA 100, Quality 67, Balanced 59, Performance 50, Ultra Performance 33 (FSR's ratios, 1.5x .. 3x); any other value shows as Custom |
| `display.upscaler` | off, fsr1, taa, fsr3 | off | off (the default on every computer): no upscaling and no frame generation, the view drawn at full size whatever `display.render_scale` and `display.frame_gen` say (both kept for later; applied at once, a temporal upscaler already running stays as anti-aliasing until the next start); fsr1: FSR 1 below a render scale of 100; taa: temporal anti-aliasing and upscaling at every render scale, MSAA off (Temporal upscaling); fsr3: the same with AMD FSR 3.1's upscaler (AMD FSR 3.1; taa where the GPU lacks what it needs); read at start-up |
| `display.frame_gen` | true, false | false | present a frame made between each two real ones (Frame generation); only with `display.upscaler` taa or fsr3; forces vertical sync and holds the game to half the refresh; applied at once |
| `display.sharpness` | 0 .. 1 | 0.5 | RCAS sharpening: FSR 1's below a render scale of 100, the temporal upscaler's or FSR 3.1's own at every scale; 0 none |
| `display.tonemap` | auto, none, agx, aces, soft | auto | auto: AgX (with a little extra saturation) for modern, soft for smooth classic (Smooth classic lighting), none for classic |
| `display.exposure` | EV | 0 | the scene is scaled by 2^EV before the tonemap |
| `display.shadows` | 0, 256 .. 8192 | 2048 | texels per side of each shadow cascade; 0 off; modern only |
| `display.light_shadows` | 0 .. 8 | 8 | how many lights cast shadows (Light shadows: the zone's, the carried light, spells); 0 off; modern only |
| `display.light_shadow_size` | 128 .. 1024 | 512 | texels per side of each face of a light's shadow cube |
| `display.view_distance` | multiplier | 2.5 | the modern mode draws this many times further than the classic client (at most 12000 units), into haze; picking keeps the classic distance |
| `display.ao` | 0, 1 | 1 | screen-space ambient occlusion; modern only; viewer `--no-ao` |
| `display.depth_prepass` | 0, 1 | 0 | draw the zone's opaque geometry depth-only first, then colour it with the depth test equal (Depth Pre-pass); switched from the next frame (`ZoneRenderer::set_depth_prepass`; a zone builds its two pipelines either way, a neighbour zone none); modern only; viewer `--depth-prepass` |
| `display.distance_blur` | 0, 1 | 1 | the far distance's slight blur; modern only |
| `display.horizon_hills` | 0, 1 | 1 | land round an outdoor zone past its edges (Horizon hills); modern only; viewer `--no-horizon` |
| `display.zone_fog` | 0, 1 | 1 | the fog bank on the ground past an outdoor zone's edge (Zone edge fog, Zone Edge Fog); modern only; viewer `--no-zone-fog` |
| `display.volumetric_clouds` | 0, 1 | 1 | clouds marched through the air over the plain skies, in place of the painted cloud layer and patches (Volumetric Clouds); modern only; viewer `--no-clouds` |
| `display.worker_threads` | -1, 0 .. 16 | -1 | the frame's worker threads (Worker threads); -1 one fewer than the hardware threads, at most 6; 0 none; read at start-up |
| `display.parallel_record` | 0, 1 | 1 | the scene's and the shadow layers' draws recorded on the worker threads into secondary command buffers (Worker threads); 0 records them on the main thread; read at start-up |
| `display.smooth_ground` | 0, 1 | 1 | a classic zone's ground drawn smooth and curved (Smoothed ground); read as each zone loads; viewer `--no-smooth-ground` |
| `display.tree_detail` | 50 to 200 | 100 | how far a texture pack's replacement models (generated trees) keep their nearer levels of detail, in percent of their own distances (Tree Detail Distance); viewer `--tree-detail` |
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
| `display.lighting` | classic, smooth, modern | modern | low to high (Smooth classic lighting); `render/scene_lighting.h`; `/lighting` switches; `enhanced` is read as modern |
| `display.window_mode` | windowed, borderless, fullscreen | windowed | borderless: a window over the whole display at the desktop's mode; fullscreen: exclusive at `display.resolution` |
| `display.resolution` | WxH | the desktop's | the fullscreen mode (the closest the display has); in a window, the Options window's Resolution sets the window's size |
| `display.vsync` | off, on, adaptive | on | the present mode: immediate (else mailbox), FIFO, FIFO relaxed; FIFO when the surface lacks one |
| `display.max_fps` | -1, 0, 10 .. 1000 | 0 | frame cap (0 none; on the login, server select, character select/creation and loading screens, including the last stretch of a zone-in, an unlimited cap, or Auto with no refresh rate, counts as 240; a set cap is followed exactly); `--max-fps` takes precedence (`--max-fps auto` for -1). -1 (Hailstone's own, the Options window's Auto) caps just under the refresh rate of the display the window is on now (`platform::Window::refresh_rate`, read each frame so it follows the window): refresh - refresh² / 3600, rounded down (`app::auto_frame_cap`: 59 at 60 Hz, 116 at 120, 138 at 144, 224 at 240), which keeps a variable-refresh display's frames inside its range so vsync's queue never fills; no cap when the system gives no refresh rate. Each frame starts one period after the last one's deadline, or at once when a long frame has passed it (`app::next_frame_deadline`; a late frame is not made up by running the next ones back to back, which made a later frame wait on the GPU), and the wait is `platform::precise_sleep_until`: the system's sleep (on Windows a high-resolution waitable timer) to 1 ms before the deadline (0.5 ms on POSIX; 16 ms on Windows without such timers, before 10 1803), then a spin that yields between looks at the clock |
| `display.background_fps` | -1, 0, 1 .. 1000 | 0 | frame cap while another window has the focus (0: as `display.max_fps`; -1: nothing drawn, the Options window's "0"). Below 10 the loop runs on at 10 a second (the game clock catches up at most 250 ms a frame) and draws only that many frames, or none (`app/frame_pace.h`); a frame not drawn is run as a minimised window's is |
| `display.flora_distance` | units, 400 .. 1200 | 400 | how far the flora is drawn (Radial flora) |
| `display.grass_density` | percent, 25 .. 300 | 100 | how thick Hailstone's lawn grows (`render::lawn_lod_blades`) |
| `display.zone_flora` | on, off | on | Hailstone's flora for zones with a rules file (Zone flora), read as each zone loads |
| `display.extra_shields` | on, off | on | a deliberate difference: shields RoF2 does not list (`game::is_shield_model`, docs/formats/characters.md "Held items") go on the shield point, not in the hand; read at start-up |
| `display.texture_folder` | pack name, absolute folder, off | `textures/default` | replacement textures (Texture overrides): the folder indexed at start, its files read as each zone loads; the Options window's Textures section and the login screen's More options (both packs; from the next start) and Hailstone Texture Studio set it |
| `display.fov` | fov units | none | the camera's field of view in 512ths of a circle (none: the classic 90, 63.3 degrees across below an aspect of 2); the Options window shows degrees, 45 .. 120 |
| `display.gpu_tier` | low, medium, high | (detected) | the tier the first start found for this computer (Detected settings); present, the detection does not run again; kept in `hailstone.ini` with a character's settings |
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
6. **Upscaling**: (done) a render scale with FSR 1 (EASU + RCAS); (done) the temporal groundwork: sub-pixel jitter
   on the projection, kept off picking and the nameplates; camera motion vectors from the depth; history reset on
   zone-in, camera cuts and resize; a TAA/TAAU of our own on it (`display.upscaler = taa`, Temporal upscaling);
   (done) motion written by the scene pipelines (last frame's bones, wind time and object matrices) and a reactive
   mask; (done) FSR 3.1 (MIT) as a second temporal upscaler (`display.upscaler = fsr3`, AMD FSR 3.1); next a negative
   texture LOD bias. MSAA is off while a temporal upscaler runs, so cut-outs lose alpha-to-coverage;
   particles, spells, water and lava need a reactive mask rather than motion vectors; (done) frame generation of our
   own on the engine's motion and depth (`display.frame_gen`, Frame generation); next maybe a choice of more than one
   in-between frame for very high refresh rates, or AMD FSR 3.1's frame generation should ours fall short. XeSS and
   FSR 4 are add-ons ("Upscaler providers"); DLSS is not planned.
