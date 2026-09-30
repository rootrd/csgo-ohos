# WineHua DXVK fork

> Provenance note for the GTA OHOS checkout (2026-09-04): the text below is
> retained from the WineHua reference, not this product's build instructions.
> This checkout uses DXVK 1.10.3 with the OHOS Native ABI and Direct WSI;
> no Wine, Broker, guest process or Venus transport is in its target path.
> Its verified incremental build is the parent repository's
> scripts/gtav-build.sh bash scripts/build-g5-hap.sh. See the parent
> docs/GTAV_OHOS_G7_HANDOFF_20260904.md for exact paths and acceptance scope.
> The Wine make command and x86 output paths below must not be used here.

This directory is the editable WineHua fork of DXVK 1.10.3.

- Product branch: `dxvk-legacy-1.10.3`
- Development branch: `feature/render-element-completeness`
- Base tag: `v1.10.3`
- Build outputs: `build/dxvk/legacy/x64/bin` and `build/dxvk/legacy/x86/bin`
- Incremental build: `make dxvk NATIVE_ARCH=arm64-v8a GUEST_ARCH=x86_64`

The WineHua compatibility path is opt-in through
`WINEHUA_DXVK_RELAXED_FEATURES=1`. It permits D3D11 device creation when the
current Venus driver lacks desktop-only capabilities. When native BC formats
are absent, the fork decodes BC1-BC7 uploads into uncompressed backing images;
this preserves the tested texture results but consumes more memory and upload
CPU time than native BC. Transform feedback/stream-output remains unsupported
when the Host capability is absent. The product selector must therefore keep
this Legacy profile separate from a fully qualified upstream DXVK profile.

Bool sampled-descriptor specialization is selected automatically from the
Vulkan adapter name for the current `Venus`/`Maleoon` path. The old
`DXVK_WINEHUA_FREEZE_BOOL_SPEC=1` setting remains a debug force-on override;
`0` disables it, and `WINEHUA_DXVK_QUIRKS=venus-bool-spec` can opt in a new
Venus adapter before its name is added to the built-in policy.

Maleoon's fragment compiler requires a padded vec4 coordinate for non-array
Cube `OpImageSampleDref*`; the otherwise equivalent minimal vec3 form produces
an all-one result. The fork enables this automatically when the Vulkan adapter
name contains `Maleoon`. `WINEHUA_DXVK_PAD_CUBE_DREF_COORD=0/1` is the explicit
debug override, and `WINEHUA_DXVK_QUIRKS=maleoon-cube-dref` can force it for a
new adapter identity. Other GPUs retain the standard minimal coordinate path.

Maleoon also hangs the Host Venus ring when a shader executes native
CubeArray shadow/Dref instructions. The fork analyzes DXBC before declaration,
marks only CubeArray resources used by comparison sampling, binds the existing
2D-array alternate view, and converts (direction, cubeIndex) to
(uv, cubeIndex * 6 + face) in the shader. The operation remains a native
2D-array OpImageSampleDref, preserving comparison-before-filtering behavior.
The policy is automatic for Maleoon;
WINEHUA_DXVK_EMULATE_CUBE_ARRAY_DREF=0/1 is the debug override, and
WINEHUA_DXVK_QUIRKS=maleoon-cube-array-dref can force it for another adapter.
Native CubeArray sampling remains the default for all other GPUs.

## Future Modern DXVK policy

DXVK 1.10.3 remains the product branch until a separate Modern profile passes
its capability and regression gates. Do not replace this fork in place with an
upstream 2.x DLL or rebase this branch to a new upstream major version.

The durable capability evidence, required compatibility forward-port inventory,
candidate version rationale, and VKD3D boundary are recorded in:

```text
../../docs/DXVK_MODERN_UPGRADE_READINESS.md
```

Any future Modern fork must preserve the current Legacy runtime as a separate
fallback and select it before Wine process startup.
