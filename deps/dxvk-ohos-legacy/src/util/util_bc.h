#pragma once

#include "util_cpu_image.h"

namespace dxvk {
  // Shared upload-time BC decoder. Source pitches describe compressed blocks;
  // edge blocks are cropped to the actual mip extent.
  bool DecodeBcImage(VkFormat format, VkFormat targetFormat, VkExtent3D extent,
    const void* source, VkDeviceSize sourceRowPitch, VkDeviceSize sourceSlicePitch,
    CpuImage& result);
}
