# Source Vulkan backend

This module contains the Vulkan device/drawing framework and Source device,
mesh, texture, snapshot and ShaderAPI adapters from
[the backend plan](../../../docs/vulkan-backend-plan.md). The game loads
`shaderapivulkan_client.so` and `stdshader_vulkan_client.so` through the real Source
interfaces. Linux Dust II now renders its world, static props, weapons, arms and
Panorama HUD. Stage 2 remains open for basic shadows and material comparison;
Android builds and device acceptance are deferred.

The current HDR path is Source integer HDR: linear RGBA16 lightmaps, scale 16,
HDR baked prop lighting, exposure and sRGB writes. Presentation remains SDR;
HDR10 display output and floating-point Source HDR are not implemented.

- `vulkan_context.*`: SDL loader, Vulkan 1.1 instance/device, individually queried
  capabilities, FIFO swapchain, two reusable frame slots, Surface recovery.
- `vulkan_platform.h`: SDL3 loader, surface, display and window operations.
  Linux and Android share the SDL3 window ABI; the game's window owner is
  `src/appframework/sdlmgr_android.cpp` on both platforms.
- `vulkan_resources.cpp`: shared buffer/image ownership, VMA allocation, checked
  mapped transfers, noncoherent flush/invalidate and optional linear/sRGB views.
- `vulkan_upload.*`: one-submit upload batches with fence-owned staging pages;
  bounded dynamic data pages recycled per completed frame slot.
- `vulkan_graphics.*`: vertex/pixel SPIR-V, ordinary binding reflection, sampler
  and pipeline layouts, cached graphics pipelines and persistent driver cache.
- `vulkan_descriptors.cpp`: frame descriptor pools, complete descriptor writes,
  dynamic offsets and epoch checks to reject recycled sets.
- `vulkan_target.*`, `vulkan_draw.cpp`: offscreen color/depth passes, vertex and
  instance streams, indices, push constants, depth/blend/scissor and actual
  indexed instancing. State is cached within one draw encoder per pass.
- `vulkan_source.*`: live Source vertex ABI/semantic mapping, checked texture
  conversion and CPU BC1/2/3 fallback, immutable VS/PS register-bank uploads.
- `vulkan_material.*`: the real `ShaderShadow010` vtable, interned immutable
  material states, vertex/sRGB checks, alpha-test state and exact static/dynamic
  shader-combo lookup.
- `vulkan_device.*`: attached `ShaderDeviceMgr001` / `ShaderDevice001`, offline
  shader buffers, Source vertex/index buffers, static/dynamic `IMesh`, immutable
  GPU updates, pending upload queues, mesh alias lookup, resource suspension and resize callbacks.
- `vulkan_texture.*`: bounded 2D/cube mip chains, native BC1/2/3 or CPU fallback,
  Source handles, partial updates, color/depth targets, sampler state, shared
  linear/sRGB views and GPU image replacement on updates.
- `vulkan_api.*`: `ShaderApi029` / `ShaderDynamic001` core, short snapshot handles,
  draw dispatch, descriptor reuse, render targets, clear/copy/readback and viewport.
  `vulkan_api_unsupported.h` provides named errors for the unported ABI slots.
- `vulkan_state.*`: matrices, skinning, ambient/local lights, material command
  buffers, tone mapping and cached fast-clip projection with transform override.
- `vulkan_queries.*`: precise asynchronous occlusion/luminance queries, split at
  render-pass boundaries; physical slots retire with their frame fences.
- `vulkan_module.cpp`: game factory, hardware configuration, `IShaderUtil`,
  material callbacks, SDL3 mode changes and shutdown diagnostics.
- `native_*.cpp`, `shaders/`: the initial material library, rope, two-texture sky
  clouds and screen effects; existing Panorama shader sources are also compiled.
- `vulkan_dispatch.h`: pointers obtained through the SDL loader; no Vulkan loader
  library is linked. Non-solid fill, BC compression and precise queries are
  individually enabled when supported; descriptor indexing remains disabled.
- `probe/`: a common Linux/Android attachment/lifecycle diagnostic and Linux
  graphics/API integration tests. `shaders/framework.hlsl` and `api_fixture.hlsl`
  are acceptance fixtures, not migrated Source materials.

All Context methods and Vulkan submissions run on one rendering thread. SDL
lifecycle watchers only publish atomic state. The SDL window and video subsystem
must outlive the Context and every remaining resource handle.

Use `beginFrame`, `beginPresentPass`, record commands, `endPresentPass`, `endFrame`.
A false `beginFrame` means no frame started; do not submit or reset a fence.
Exceptions from device operations are fatal for that Context. A returned Frame
belongs only to its current recording and cannot be reused.

Call `retain(frame, resource)` for recorded GPU use unless another owner keeps the
resource alive until its completion fence. Retention does not synchronize CPU
access: wait for the corresponding GPU work before reading or overwriting memory.
`submitAndWait` is for initial uploads and explicit diagnostics, not ordinary
frame rendering. Use `UploadBatch` to combine asynchronous initialization/update
work; retain its ticket until `ready()` to reclaim staging without blocking.
Declare previous and next GPU uses when updating an existing resource. Supported
tightly packed upload formats are documented in `vulkan_upload.h`.

Write per-frame constants/instance arrays with `FrameArena`, initialize a complete
set through `DescriptorArena`, open a target/present pass and use `DrawEncoder`.
Creating a new encoder invalidates the previous encoder's cached state; do not
mix raw state-changing Vulkan commands with an active encoder. Slices and sets
belong to their frame, while immutable layouts, samplers and pipelines can be
reused. Image layouts outside the provided passes belong to the caller.

The graphics API currently supports one color attachment, optional depth, one
sample and vertex/fragment shaders. MRT, tessellation, geometry, input attachments,
bindless and indirect draws are not implemented. SPIR-V must be offline validated
and use only enabled device features; binding reflection is not a general SPIR-V
validator. Pipeline layout checks cover ordinary resource bindings; complete
material shaders must also honor the Source register and alpha-state contracts.

`sourceVertexLayout` calls the existing `ComputeVertexDesc` rather than duplicating
Source offsets. Vertex usage is checked independently of mesh skinning/padding;
three baked color streams are explicitly bound. Packed normals/tangents remain unsupported.
`prepareSourceTexture` handles one mip/layer at a time and bounds converted output
to 64 MiB. `sourceTextureDataLayout` supplies the same pitch/block-row/size checks
to the raw Source texture API, which does not carry the caller's buffer length.
Length-aware direct clients should keep using `prepareSourceTexture`.
BC1/2/3 uploads preserve compressed mip blocks when available. CPU fallback is
also GPU-tested with BC deliberately disabled. Automatic mip generation remains unsupported.

`SourceConstants` mirrors the SM3 c/i/b register numbering: VS has 256 float4,
PS 224 float4, and each has 16 int4 and 16 packed 32-bit booleans. The matching
`source_registers.hlsl` uses set 0 bindings 0 and 1, with 4416/3904-byte banks.
An unchanged bank reuses its slice within a frame; mutations allocate a new slice.
The owning Context and FrameArena must outlive the constants adapter.

`SourceDevice` attaches to an existing Context, GraphicsDevice, FrameArena,
SourceShadow and SDL window; those objects must outlive it. It exposes the
selected physical device as logical adapter 0, not pre-window adapter selection.
Its manager's `Connect`/`Init`/`SetMode` produces a factory for the attached
interfaces. `vulkan_module.cpp` owns engine bootstrap, `IShaderUtil` and hardware
configuration. Modes use one SDL3 window, FIFO/vsync and no MSAA; backbuffer
depth/stencil is available. Mode changes notify engine shader objects and follow
the actual SDL pixel size, retaining the GPU device and managed resources.
`MATERIAL_RESTORE_RESIZE_ONLY` tells the engine and model cache to retain world
meshes, model hardware data and HDR baked lighting during this change. Other
release/restore callbacks still rebuild targets and UI caches. Device-loss or
vertex-format changes must use the full restore path instead.
Actual GPU device loss and D3D-specific reset events are not emulated.

Static Source buffer updates are combined by `flushUploads` before a frame or
before a later draw and replace GPU allocations. Dynamic buffer/mesh revisions use independent FrameArena
slices, so modifying or destroying CPU objects after recording cannot overwrite
earlier draws. CPU budgets include retained lock scratch capacity. Static meshes
and dynamic meshes use 16-bit indices; separate buffers support 16/32-bit indices.
The dynamic mesh is device-owned and valid until the next dynamic mesh request.
Use typed casts when accessing typeless dynamic buffers; buffer data must be
unlocked before frame boundaries, destruction or another format cast. Upload
checks visit only pending static resources, retaining failed batches for retry.
Unchanged draws do not scan all meshes/textures or poll all upload fences.

`SourceAPI` installs `ShaderApi029` and `ShaderDynamic001` on that device and owns
its draw sink. It must be destroyed before SourceDevice; only one API may attach.
Register each vertex shader's semantic inputs with `registerShaderInputs`.
ShaderApi029 shaders use `source_api.hlsl`: dynamic register banks at bindings
0/1, pixel image/sampler pairs at 2..33, vertex image/sampler pairs at 34..41,
and a 32-byte push block containing alpha-test state and mesh modulation. Only
enabled sampler slots enter the descriptor layout. Names/combos are exact offline
keys; missing contracts, shader variants or bindings fail explicitly. This is a
defined shader interface, not an automatic adapter for existing `.vcs`.

`TakeSnapshot` returns an interned, immutable Source `short` handle. Cleared
handles are never recycled during this API object's lifetime; exceeding the
32768-handle space fails. `BeginPass` selects one. An unbound diagnostic mesh may
draw directly with that selection. For actual materials, the game module installs
`setMaterialPassCallback` before `Bind(IMaterial*)`; the callback bridges to
`IMaterialInternal::DrawMesh`, select snapshots/constants and invoke `RenderPass`.
The API probe uses an opaque material identity to check that protocol, without
loading or calling an actual Source material. Real material coverage is tested
separately by `scripts/test-linux-renderer.py`.

Call `SourceAPI::beginFrame()` to handle temporary surface unavailability, then
record via its `IShaderAPI`. `EndFrame` closes passes and guarantees a present
pass; `IShaderDevice::Present` submits it. Minimized or zero-size surfaces skip
frame recording until they are available again. Offscreen callers
can use `beginTarget`/`endTarget` before the present pass; the target must outlive
its open pass. Texture target handles and backbuffer color/depth/stencil are
integrated, including shared depth, preserving passes and scaled copies.

`SourceTextures` accepts 2D/cube textures with explicit mip data and single-level
color/depth targets. Creation
preflights the whole mip-chain/handle/CPU-shadow budget. Incomplete chains cannot
be sampled. `TexImage2D(nullptr)` initializes an allocated mip to zero;
`TexSubImage2D` checks rectangles and pitch; a first partial update leaves the
rest of the newly allocated mip zeroed. Data uploads merge with static mesh
uploads at frame start or before a draw. Updates replace the image; recorded descriptors keep its previous
version alive. Managed resource suspension retains CPU mips for re-upload.
Deletion invalidates the handle and future bindings. Multi-frame creation rolls
back all newly created handles on failure.

RGBA8 texture storage provides linear and sRGB sampled views of the same image.
`TEXTURE_BINDFLAGS_SRGBREAD` selects the view and must agree with the snapshot.
Alternate views have sampling-only usage, including when the original linear
image allows storage writes. `NOMIP`, point/linear mip filtering and clamp/repeat/
border address modes are supported, as are texture locks for supported uncompressed
formats. Format capabilities are checked. Anisotropy, shadow comparison, volume,
native ASTC and automatic mip generation are not implemented. Cube data upload
does not yet imply complete environment-reflection material support.

The `IShaderAPI` implementation is partial. Unported methods retain named errors;
CSM, flashlight shadows, general fog shading, MRT and MSAA are still missing.
The Source material instance path currently submits objects separately;
hardware instancing is available through DrawEncoder. Rendering/submissions run
on one thread, with Source ownership calls sharing the module's recursive mutex.
The game acceptance uses `mat_queue_mode 0`.

`SetDefaultState` preserves context cull/stencil/scissor and write overrides set
by Panorama before `CMaterialSystem::DrawElements`; `ResetRenderState` performs
the full reset. Precise occlusion queries return pending without waiting. Only
an explicit flush can wait for an older frame or submit the current frame prefix.
Queries interrupted by an unavailable surface report an error so the engine can
retain its conservative result. `VK_QUERY_STATS` records waits and pool usage.

Use `SourceShadow::interface()` for existing `IShaderShadow` calls and `snapshot()`
to capture a material. Its `SetDefaultState` retains Source shadow defaults;
depth bias and supported polygon modes are captured with the material. Fog state
is retained, but general material fog shading and multisample coverage are pending.
The module's hardware configuration updates the lightmap scale when HDR changes.
Snapshot tables have a budget and can be cleared after
a map unload; outstanding snapshots retain their state. `translucent()` preserves
Source's force-opaque blend classification and does not reorder draws.

Before drawing, validate the actual mesh with `validateVertexFormat` and each
enabled texture with `validateTexture`. `SourceShaderLibrary::pipeline` resolves
both shader combos, checks alpha-test support, maps fixed pipeline states and
checks the attachment's sRGB interpretation. Supply the appropriate `frontFace`
for the engine's cull direction/mirrored views. Copy `snapshot.state().alpha` into
the shader's push/uniform data for every draw and call `SourceAlphaTest` in the
fragment shader after computing final alpha. SourceAPI fills its own 32-byte
alpha/modulation push block automatically. The offline `alpha_test` flag is an
explicit contract that the variant implements this helper; reflection does not
prove semantic behavior. `framework.hlsl` demonstrates the 16-byte state appended
to its 32-byte draw block and verifies both discard/color and discard/depth behavior.

Presentation semaphores belong to swapchain images, so reacquisition establishes
when they can be signalled again. Acquire semaphores, fences, command pools and
command buffers belong to frame slots. Resize/Surface recovery waits and replaces
presentation objects while retaining the device and ordinary VMA allocations.

From the repository root (scripts enter Distrobox `dev`):

```sh
bash scripts/build-vulkan-module.sh
bash scripts/run-linux.sh -shaderapi shaderapivulkan_client.so \
  -nomobileui -windowed -w 1280 -h 720 +mat_queue_mode 0 +map de_dust2
python3 scripts/test-linux-renderer.py --shaderapi shaderapivulkan_client.so \
  --validation --output runtime/vulkan/validation/game-native

bash scripts/build-vulkan-probe.sh test
bash scripts/build-vulkan-probe.sh framework-test
bash scripts/build-vulkan-probe.sh api-test
bash scripts/build-vulkan-probe.sh run --validation --seconds 15
BUILD_CONFIG=debug bash scripts/build-android.sh engine
BUILD_CONFIG=debug bash scripts/build-android.sh native
BUILD_CONFIG=debug bash scripts/build-android.sh install
BUILD_CONFIG=debug bash scripts/build-android.sh vulkan-probe 15
python3 scripts/test-vulkan-android.py
```

The Android commands are retained for later acceptance, not executed in the
current Linux batch. Do not regenerate `runtime/vulkan/shaders/` while the game
is running: shader modules are loaded lazily. Native game validation uses a
separate output directory to preserve `runtime/linux-sdl3/validation/game/` for DXVK.
Use `--skip-keyboard` if a desktop IME intercepts injected keys. The result records
that check as skipped while still exercising rendering, resizing, recovery and shutdown.

`test` builds once, then runs lifecycle, graphics and Source API checks.
`framework-test` runs the graphics and API portions together.
`api-test` runs only the API portion after adapter changes.
The automated desktop lifecycle test defaults to X11/XWayland. Wayland can deny
programmatic restoration of a minimized window without a user activation token;
normal Wayland rendering and user-driven restoration use SDL's regular events.
The Android test takes about 35 seconds and includes two background/resume cycles.
It checks the installed build ID, unchanged PID/device, original upload contents,
three swapchain generations and the final success marker. Vulkan diagnostics use
Debug private storage and need no game assets or storage permission.

Desktop logs and GPU PPM readback are in `runtime/vulkan/<configuration>/`.
`framework-test/graphics-probe.log` records actual draws, pipeline reuse, descriptor
pool count, dynamic memory and validation results; the same directory holds the
driver cache. Invalid cache identity/checksum is ignored and rebuilt.
`api-test/api-probe.log` records the 60-frame/780-draw interface test, descriptor
reuse and texture lifecycle. GPU PPMs show the image before/after a pitched update;
the scene checks sRGB/mip sampling, same-frame constants and dynamic mesh changes,
32-bit indices, clear, viewport, depth, alpha test and blending. Additional GPU
readbacks cover integer HDR, independent dual-texture transforms, precise query
counts, fast clipping, baked color streams and Panorama context overrides.
The 2026-09-26 graphics/API results are in `runtime/vulkan/validation/framework-hdr-perf/`,
`api-hdr-perf/` and `api-hdr-perf-no-bc/`; the last explicitly disables native BC.
`VK_SOURCE_UPLOAD_PASS` records 5 texture and 6 buffer inspections over 780 draws,
with no unchanged-resource scans. Graphics acceptance also injects a failed upload
recording and verifies cleanup and retry. These counts do not measure an FPS gain.
Game tests save module hashes, RSS, timings, upload/query statistics and screenshots;
the automatic image check is not a substitute for visual material/HUD inspection.
Android test artifacts are in `runtime/vulkan/android/self-test/`; the native log
can also be read with `adb shell run-as com.csgosource.android.debug cat files/launcher.log`.
Failures throw, the native entry returns nonzero, and the automated test fails.
Optional Android `vulkan-probe 15 --validation` requires a Debug APK containing
`libVkLayer_khronos_validation.so`; it fails explicitly if the layer is absent.

The full APK still includes the DXVK game path and its Vulkan 1.3 manifest
requirement. A Vulkan 1.1 diagnostic does not make the game or whole APK compatible
with Vulkan 1.1-only devices; lowering that packaging requirement awaits Android
game integration and device acceptance.

DXC is pinned to the official `v1.8.2505.1` Linux release, archive SHA-256
`f2213da1fc99dc8778c8823078e16ba97c7f80f86a1d4520ab1adf4b462bc48c`.
`scripts/build-vulkan-shaders.sh` prepares it only in the ignored runtime tree,
then compiles named variants from `shaders/manifest.json` with explicit bindings,
row-major constants and vertex Y inversion for a positive Vulkan viewport.
Outputs target Vulkan 1.1 and pass `spirv-val`; `runtime/vulkan/shaders/manifest.json`
records compiler, profile, defines, source/include hashes and output hashes.
Each input variant can specify `source_name`, `static_index`, `dynamic_index` and
`alpha_test`; defaults are its artifact name, 0, 0 and false. Shader identifiers
are bare ASCII identifiers (maximum 96 characters), and combo indices range from
0 to 2147483647. `source-variants.tsv` is a versioned runtime index with these
fields: name, stage, static index, dynamic index, SPIR-V filename, entry, alpha-test
flag. Source names are case-insensitive; missing combinations fail explicitly.
The library caches shader modules and passes the selected shaders to the existing
pipeline cache. It does not load or translate legacy `.vcs` files.
Descriptor types/counts/stages are
checked against runtime layouts before graphics pipeline creation. The current
Android acceptance remains pending; the completed batch was Linux-only.
