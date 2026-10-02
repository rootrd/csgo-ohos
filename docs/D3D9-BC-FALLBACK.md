# D3D9 BC textures and device creation

## Scope

CS:GO uses the D3D9 frontend. The existing D3D11 BC fallback was not on that
execution path. Merely removing `textureCompressionBC` from device creation
left DXT images unsupported; masking every requested core feature also removed
requirements that the generated shaders and meta pipelines still depend on.

D3D9 now retains its explicit hard feature requirements. Native BC is enabled
when the adapter reports it. Each unsupported BC format uses an upload-time
CPU decoder if the corresponding optimal-tiled, filterable Vulkan backing
formats are available. Unsupported backing formats remain unavailable rather
than being advertised as working.

The existing BC decoder and its license are shared between D3D9 and D3D11;
D3D11 retains its wrapper and existing behavior. D3D9 uses lossless decoded
RGBA8 for DXT1–5, R8 for ATI1 and RG8 for ATI2. It does not recompress to ETC2 or
pretend the application's BC data is ASTC. Native BC-capable adapters keep the
existing native path.

## Resource contract

- Logical formats, compressed lock pitches, mip/block arithmetic and ATI
  channel swizzles remain unchanged
- ATI fallback locks support whole subresources only. Partial ATI boxes are
  rejected because the legacy reported byte-layout pitch can address beyond
  the compressed allocation; the full-lock pitch workaround is preserved
- Physical Vulkan images and linear/sRGB views use the decoded formats
- UpdateSurface, UpdateTexture, managed uploads and lock/unlock updates share
  the same decode path, with block alignment and mip-edge clipping
- Fallback textures retain compressed CPU shadows. Uploads snapshot decoded
  bytes into separate staging allocations, and copy operations preserve the
  destination's compressed bytes for later locks/readback
- Initial physical contents are decoded zero blocks, including opaque-black
  DXT1, rather than incorrectly initializing every format to transparent RGBA
- Same-format default-pool BC copies preserve shadows. Stretching into BC,
  rendering/depth writes, multisampling, shared handles and front-buffer writes
  to BC are unsupported. Autogen queries return D3DOK_NOAUTOGEN and creation
  follows the existing single-mip fallback behavior
- A missing mandatory shader/device capability still fails device creation;
  it is never silently masked away

Decoded backing images use more memory than native BC (up to 8× the image
payload for DXT1, plus its retained compressed shadow) and cost CPU time at
upload. This correctness fallback is not a measured mobile performance claim.

## HUD layout

The branch is based on upstream `d22a6eefe8e98cffad4a97d46abe21a64f0d21d5`,
including the preceding `86e19b53` HUD layouts, CSNO touch injection and
packaging-time code.pbin removal, plus the latest map-load startup channel. The overlay and packaged hud.xml use the actual registered XML factory
names, including CSGOHudChat, rather than C++ class names. The upstream empty
base_hud.xml is preserved and its packaged copy is synchronized.

This repairs the parser contract. It does not invent or replace missing CSS,
images, child panel layouts or game assets. CSGOHudChat loads hudchat.xml from
game assets; the required ChatContainer and other child controls in that layout
remain unverified because hudchat.xml is not in this repository.

Packaging-time removal alone cannot prevent an already imported code.pbin from
shadowing the corrected layouts. OHOS builds with DEVELOPMENT_ONLY now choose
loose resources consistently for resource preload, named-path config and input
bindings, even when the imported pack contains code.pbin. The pack is never
deleted or rewritten. Other platforms and non-development build selection keep
their previous behavior.

Loose mode requires compatible loose assets, including panorama.cfg,
hudchat.xml and default_keybinds.cfg/window_keybinds.cfg/csgo_keybinds.cfg.
Missing named-path config fails setup; missing keybindings produce an explicit
OHOS development warning and are not silently replaced by incompatible packed
content. The game resource pack must supply assets not present in the seed.


## Repeatable host checks

Run from the repository root:

```sh
bash scripts/test-bc-decode.sh
python3 scripts/test-d3d9-bc-upload.py
python3 scripts/test-d3d9-feature-contract.py
python3 scripts/test-d3d9-bc-lock.py
python3 scripts/test-panorama-hud-layout.py
python3 scripts/test-panorama-resource-policy.py
python3 scripts/test-panorama-v8-guards.py
```

The decoder and uploader tests enable AddressSanitizer and UndefinedBehaviorSanitizer
by default. In environments that run commands under ptrace, set
`ASAN_OPTIONS=detect_leaks=0`; LeakSanitizer is incompatible with ptrace. This
still runs address and undefined-behavior checks.

The upload harness compiles the actual D3D9 fallback branch with a host CPU
context stub. It checks source/destination offsets, compressed shadow contents,
all cube faces/mip tails, NPOT edges, partial dirty regions, volume slices and
invalid origins. It is not a Vulkan driver or synchronization test.

The HUD harness compiles the actual Panorama StartElement, EndElement and
BAddPanel callbacks with engine-service stubs. It accepts the fixed XML and
rejects the old singular style tag, named root, duplicate root panel and C++
class-name tag. XML tokenization uses ElementTree; assets and real panel
constructors are outside this host test. The resource-policy harness compiles
the actual preload/config/keybinding selection blocks in OHOS/non-OHOS and
development/non-development modes, including packed-file presence/absence and
missing-loose-resource failures.

## Target compile and remaining runtime gates

Use the documented official native Linux SDK/toolchain setup, then:

```sh
BUILD_JOBS=3 bash scripts/build-dxvk-ohos-linux.sh
```

D3D9, DXGI and D3D11 must all compile/link after the shared-code move. The
aggregate engine/stage gates are documented in NATIVE-LINUX-BUILD.md.

A real-device run is still required to establish Vulkan validation cleanliness,
linear and sRGB sampling, HUD appearance, memory use, menu/map loading and
playability. No V8 executable-memory/security policy, broad V8 replacement,
shadow-mapping policy or game asset was changed by this patch.

## Build prerequisite provenance repair

The aggregate build exposed an existing gyp download check that pinned unstable
GoogleSource `+archive` gzip bytes, then skipped validation for a cached file.
The helper now preserves the same official revision
`e7079f0e0e14108ab0dba58728ff219637458563` and verifies its Git tree
`ea08eb644f21477d1f0dd77d3052e306d0a9da04` before producing a deterministic
uncompressed Git archive. Its SHA-256 is
`435def02979d91b9ec743785a5893e66248e4d9061f4349b9f9a723e8aba4b84` and is
checked on every cache hit. Existing extracted sources are preserved as build
cache backups and replaced by a fresh verified extraction. No dependency
revision, V8 implementation or runtime security setting is changed.

## Validation completed on 2026-10-02

For the fixed working tree on base `d22a6eef`:

- Official SDK bundle 26.0.0.851, native SDK 26.0.0.105/API26, clang 15.0.4
- Nine host suites passed: shared BC decoder, actual BC upload branch, ATI lock
  bounds, D3D9 feature selection, actual HUD parser callbacks, actual resource
  selection blocks, existing V8 guards, gyp cache integrity, and Vulkan feature
  negotiation/syntax checks
- AddressSanitizer/UndefinedBehaviorSanitizer were active for the BC tests;
  LeakSanitizer alone was disabled because the host executor uses ptrace
- Source dependencies, SDL, foundation, real V8, DXVK/D3D9/DXGI/D3D11, all 26
  engine modules and libmain compiled/linked successfully
- The three changed Panorama translation units used release `NDEBUG` with
  `DEVELOPMENT_ONLY=1`; the actual target compiler predefined `__OHOS__=1`
- The final incremental aggregate after integrating `d22a6eef` exited zero,
  including recompilation of its updated engine startup code
- Staged 91-file AArch64/ELF/API/dependency closure audit passed
- NAPI and ArkTS compiled into an unsigned HAP; all 34 Hvigor tasks ran,
  packaged 92-file ELF/API/dependency closure and ZIP CRC checks passed
- Unsigned HAP SHA-256:
  `3c2b3352e24a1ca1d57b02e36f7e3727a97b8cec670948a523d3183d4f7dc407`

The HAP was not signed or installed. On-device startup, JIT behavior, asset
completeness, rendering and playability remain unverified by these build gates.
