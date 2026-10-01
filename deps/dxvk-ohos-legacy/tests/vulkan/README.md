# Device feature negotiation regression tests

Run from the repository root:

```sh
bash deps/dxvk-ohos-legacy/tests/vulkan/run-host-tests.sh
```

Requires a host C++17 compiler with AddressSanitizer and UndefinedBehaviorSanitizer.
The runner uses the checkout's Vulkan/native headers and does not need an OHOS SDK,
Vulkan driver, display, or phone. LeakSanitizer is disabled because it cannot inspect
processes in ptrace-based sandbox runners. ASan stack-use-after-return checks and
UBSan remain enabled.

Coverage: full-width feature-mask parsing and selection, invalid/zero/old mask
rejection, named unsupported-feature errors, feature chain rebuilding, legacy and
Features2 request forms, an AMD wrapper around the chain, removal of chain heads,
truthful removed feature bits, cleared snapshot links, extension revision resets,
and repeated independent device states. The changed adapter and D3D9 translation
units also receive host syntax checks.

These checks do **not** validate an OHOS binary, shader execution, image quality,
D3D9 pixel readback, presentation, or full game startup.

## D3D9 rendering contract

Never create a zero-feature device as a renderer fallback. The existing D3D9/DXVK
code needs robustBufferAccess, fullDrawIndexUint32, imageCubeArray,
independentBlend, geometryShader, sampleRateShading, depthClamp, fillModeNonSolid,
textureCompressionBC, occlusionQueryPrecise, shaderStorageImageWriteWithoutFormat,
and shaderClipDistance. Supported depthBounds, pipelineStatisticsQuery, and
vertexPipelineStoresAndAtomics are also retained because existing advertised
capabilities or application paths use them. Anisotropy alone has an audited core
fallback in DxvkSampler.

D3D9 does not emit CullDistance, uses one viewport, and always sets depthBiasClamp
to zero. It therefore no longer requests shaderCullDistance, multiViewport, or
depthBiasClamp. ClipDistance remains required by both DXSO and fixed-function
vertex shaders. Image cube arrays are needed by dummy resources and cube views;
formatless storage-image writes and sample-rate shading are needed by meta paths.

Only audited optional extension groups are removed during retries. Host query
reset, 4444 formats, vertex attribute divisors, image format lists, mirror clamp,
and externally required extensions are preserved. Merely omitting a divisor
structure is not an emulation of divisors greater than one. The legacy command
query-reset path also is not sufficient justification to discard hostQueryReset.

The default packaged dxvk.conf has no csgoVkFeatureMask override. If used for
diagnostics, this option is a 64-bit allowlist of already requested core features,
applied before the first create call. It cannot select unrequested/unsupported
features or remove the required contract. Bits 32 and 37 respectively address
shaderStorageImageWriteWithoutFormat and shaderClipDistance. Unsupported masks
fail with feature names rather than reaching a misleading successful device.

Every retry rebuilds extension revisions, the feature chain, and the actual Vulkan
request together. Successful creation stops all retries. The same enabled feature
bits go into DxvkDevice, with stack-owned pNext links cleared. Driver rejections
retain the VkResult and requested features in the log; they are not proof that
feature queries are wrong or that a valid rendering device exists.
