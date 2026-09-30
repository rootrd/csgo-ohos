#pragma once

#include "../dxvk/dxvk_include.h"
#include "d3d11_format_convert.h"

namespace dxvk {

  /**
   * Decompresses one BC image subresource into the uncompressed Vulkan
   * format selected by WineHua's DXGI format fallback.  This is deliberately
   * an upload-time operation: no frame readback and no per-sample shader
   * replacement is involved.
   */
  bool DecodeD3D11BcImage(
          VkFormat                 format,
          VkFormat                 targetFormat,
          VkExtent3D               extent,
    const void*                    source,
          VkDeviceSize             sourceRowPitch,
          VkDeviceSize             sourceSlicePitch,
          D3D11CpuImage&           result);

}
