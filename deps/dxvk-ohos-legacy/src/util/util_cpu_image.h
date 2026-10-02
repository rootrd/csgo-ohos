#pragma once

#include <vector>
#include <cstdint>
#include <vulkan/vulkan_core.h>

namespace dxvk {
  struct CpuImage {
    std::vector<uint8_t> data;
    VkDeviceSize rowPitch = 0;
    VkDeviceSize slicePitch = 0;
  };
}
