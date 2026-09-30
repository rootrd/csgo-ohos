#include "d3d11_bc.h"

#include "../dxvk/dxvk_include.h"

namespace dxvk {

  bool IsD3D11Bc1Format(uint32_t format) {
    switch (VkFormat(format)) {
      case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
      case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
      case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
      case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
        return true;
      default:
        return false;
    }
  }

}
