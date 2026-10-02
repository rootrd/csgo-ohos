#include "d3d11_bc.h"
#include "../util/util_bc.h"

namespace dxvk {
  bool DecodeD3D11BcImage(VkFormat format, VkFormat targetFormat, VkExtent3D extent,
      const void* source, VkDeviceSize rowPitch, VkDeviceSize slicePitch, D3D11CpuImage& result) {
    return DecodeBcImage(format, targetFormat, extent, source, rowPitch, slicePitch, result);
  }
}
