#pragma once

#include "../dxvk/dxvk_device.h"

namespace dxvk {

  struct D3D11Options;

  enum class DxbcFloatControlFlag : uint32_t {
    DenormFlushToZero32,
    DenormPreserve64,
    PreserveNan32,
    PreserveNan64,
  };

  using DxbcFloatControlFlags = Flags<DxbcFloatControlFlag>;

  struct DxbcOptions {
    DxbcOptions();
    DxbcOptions(const Rc<DxvkDevice>& device, const D3D11Options& options);

    // Clamp oDepth in fragment shaders if the depth
    // clip device feature is not supported
    bool useDepthClipWorkaround = false;

    /// Use the ShaderImageReadWithoutFormat capability.
    bool useStorageImageReadWithoutFormat = false;

    /// Use subgroup operations to reduce the number of
    /// atomic operations for append/consume buffers.
    bool useSubgroupOpsForAtomicCounters = false;

    /// Use a SPIR-V extension to implement D3D-style discards
    bool useDemoteToHelperInvocation = false;

    /// The consumer may declare fewer stage-interface components
    /// than its producer.
    bool supportsMaintenance4 = false;

    /// Use subgroup operations to discard fragment
    /// shader invocations if derivatives remain valid.
    bool useSubgroupOpsForEarlyDiscard = false;

    /// Use SDiv instead of SHR to converte byte offsets to
    /// dword offsets. Fixes RE2 and DMC5 on Nvidia drivers.
    bool useSdivForBufferIndex = false;

    /// Enables NaN fixup for render target outputs
    bool enableRtOutputNanFixup = false;

    /// Implement dynamically indexed uniform buffers
    /// with storage buffers for tight bounds checking
    bool dynamicIndexedConstantBufferAsSsbo = false;

    /// Clear thread-group shared memory to zero
    bool zeroInitWorkgroupMemory = false;

    /// Declare vertex positions as invariant
    bool invariantPosition = false;

    /// Insert memory barriers after TGSM stoes
    bool forceTgsmBarriers = false;

    /// Replace ld_ms with ld
    bool disableMsaa = false;

    /// Use combined image samplers for the WineHua sampled-image probe.
    /// This is intentionally opt-in until separated sampled-image reads are
    /// verified on the target Venus/Host driver.
    bool useCombinedImageSampler = false;

    /// Correct arbitrary D3D11 border colors in generated shaders when the
    /// active Vulkan device cannot provide format-independent custom colors.
    bool emulateCustomBorderColor = false;

    /// Pad non-array Cube comparison coordinates with the Dref value.  Both
    /// vec3 and vec4 forms are valid SPIR-V, but the Maleoon fragment compiler
    /// miscompiles OpImageSampleDref* with the minimal vec3 form.
    bool padCubeDrefCoordinates = false;

    /// Replace CubeArray sampling with an equivalent 2D-array view and
    /// shader-side face selection for resources used by comparison sampling.
    /// Maleoon otherwise hangs the Host Venus ring on CubeArray Dref.
    bool emulateCubeArrayDref = false;

    /// Diagnostic only. Pixel-shader non-array Cube SampleC/SampleClz
    /// returns this constant and omits OpImageSampleDref. -1 keeps native
    /// comparison. 0/1 are fully shadowed / unshadowed.
    int replaceCubeDref = -1;

    /// Float control flags
    DxbcFloatControlFlags floatControl;

    /// Minimum storage buffer alignment
    VkDeviceSize minSsboAlignment = 0;
  };
  
}
