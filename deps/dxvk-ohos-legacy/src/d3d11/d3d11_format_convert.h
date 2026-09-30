#pragma once

#include <vector>

#include "../dxvk/dxvk_include.h"

namespace dxvk {

  struct D3D11CpuImage {
    std::vector<uint8_t> data;
    VkDeviceSize rowPitch = 0;
    VkDeviceSize slicePitch = 0;
  };

  /**
   * Converts the D3D-visible R8G8B8A8_SNORM byte layout to the
   * R16G16B16A16_SFLOAT image layout used by the WineHua render-target
   * fallback. Values keep the Vulkan/D3D SNORM domain: -128 maps to -1,
   * 127 maps to 1, and all other values map to n / 127.
   */
  bool ConvertD3D11Rgba8SnormToRgba16Float(
          VkExtent3D               extent,
    const void*                    source,
          VkDeviceSize             sourceRowPitch,
          VkDeviceSize             sourceSlicePitch,
          D3D11CpuImage&           result);

}
