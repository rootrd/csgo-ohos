#include "dxgi_format.h"

#include <array>
#include <fstream>

#include "../util/util_env.h"
#include "../dxvk/dxvk_winehua_trace.h"

namespace dxvk {

  static_assert(DXGI_FORMAT_BC1_UNORM == 0x47 && DXGI_FORMAT_BC7_UNORM_SRGB == 0x63,
    "the private AOT extension must not renumber standard BC formats");
  static_assert(DXGI_FORMAT_OHOS_AOT_ASTC_4X4_UNORM == DXGI_FORMAT_V408 + 1 &&
    DXGI_FORMAT_OHOS_AOT_EAC_RG11_SNORM == DXGI_FORMAT_V408 + 6,
    "OHOS AOT format table must be contiguous after standard DXGI formats");

  /* The DXVK log lives in a sandboxed directory the host cannot read back, so
   * the BC backing report is mirrored next to the shader dumps, which the
   * launcher points at a readable location. */
  static void winehuaBcFallbackLog(const std::string& text) {
    Logger::info(text);
    const char* dumpPath = std::getenv("DXVK_SHADER_DUMP_PATH");
    if (!dumpPath || !dumpPath[0])
      return;
    std::ofstream record(
      str::tows(str::format(dumpPath, "/g9-bc-fallback.log").c_str()).c_str(),
      std::ios_base::app);
    record << text << std::endl;
  }
  
  constexpr std::array<DXGI_VK_FORMAT_MAPPING, 139> g_dxgiFormats = {{
    // DXGI_FORMAT_UNKNOWN
    { },
    // DXGI_FORMAT_R32G32B32A32_TYPELESS
    { VK_FORMAT_R32G32B32A32_UINT,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R32G32B32A32_UINT },
    // DXGI_FORMAT_R32G32B32A32_FLOAT
    { VK_FORMAT_R32G32B32A32_SFLOAT,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R32G32B32A32_UINT,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_R32G32B32A32_UINT
    { VK_FORMAT_R32G32B32A32_UINT,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R32G32B32A32_UINT,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_R32G32B32A32_SINT
    { VK_FORMAT_R32G32B32A32_SINT,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R32G32B32A32_UINT,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_R32G32B32_TYPELESS
    { VK_FORMAT_R32G32B32_UINT,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R32G32B32_UINT },
    // DXGI_FORMAT_R32G32B32_FLOAT
    { VK_FORMAT_R32G32B32_SFLOAT,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R32G32B32_UINT,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_R32G32B32_UINT
    { VK_FORMAT_R32G32B32_UINT,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R32G32B32_UINT,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_R32G32B32_SINT
    { VK_FORMAT_R32G32B32_SINT,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R32G32B32_UINT,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_R16G16B16A16_TYPELESS
    { VK_FORMAT_R16G16B16A16_UNORM,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R16G16B16A16_UINT },
    // DXGI_FORMAT_R16G16B16A16_FLOAT
    { VK_FORMAT_R16G16B16A16_SFLOAT,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R16G16B16A16_UINT,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_R16G16B16A16_UNORM
    { VK_FORMAT_R16G16B16A16_UNORM,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R16G16B16A16_UINT,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_R16G16B16A16_UINT
    { VK_FORMAT_R16G16B16A16_UINT,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R16G16B16A16_UINT,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_R16G16B16A16_SNORM
    { VK_FORMAT_R16G16B16A16_SNORM,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R16G16B16A16_UINT,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_R16G16B16A16_SINT
    { VK_FORMAT_R16G16B16A16_SINT,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R16G16B16A16_UINT,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_R32G32_TYPELESS
    { VK_FORMAT_R32G32_UINT,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R32G32_UINT },
    // DXGI_FORMAT_R32G32_FLOAT
    { VK_FORMAT_R32G32_SFLOAT,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R32G32_UINT,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_R32G32_UINT
    { VK_FORMAT_R32G32_UINT,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R32G32_UINT,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_R32G32_SINT
    { VK_FORMAT_R32G32_SINT,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R32G32_UINT,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_R32G8X24_TYPELESS
    { VK_FORMAT_UNDEFINED,
      VK_FORMAT_D32_SFLOAT_S8_UINT,
      VK_FORMAT_UNDEFINED },
    // DXGI_FORMAT_D32_FLOAT_S8X24_UINT
    { VK_FORMAT_UNDEFINED,
      VK_FORMAT_D32_SFLOAT_S8_UINT,
      VK_FORMAT_UNDEFINED,
      0, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT },
    // DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS
    { VK_FORMAT_UNDEFINED,
      VK_FORMAT_D32_SFLOAT_S8_UINT,
      VK_FORMAT_UNDEFINED,
      0, VK_IMAGE_ASPECT_DEPTH_BIT },
    // DXGI_FORMAT_X32_TYPELESS_G8X24_UINT
    { VK_FORMAT_UNDEFINED,
      VK_FORMAT_D32_SFLOAT_S8_UINT,
      VK_FORMAT_UNDEFINED,
      0, VK_IMAGE_ASPECT_STENCIL_BIT },
    // DXGI_FORMAT_R10G10B10A2_TYPELESS
    { VK_FORMAT_A2B10G10R10_UNORM_PACK32,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_A2B10G10R10_UINT_PACK32 },
    // DXGI_FORMAT_R10G10B10A2_UNORM
    { VK_FORMAT_A2B10G10R10_UNORM_PACK32,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_A2B10G10R10_UINT_PACK32,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_R10G10B10A2_UINT
    { VK_FORMAT_A2B10G10R10_UINT_PACK32,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_A2B10G10R10_UINT_PACK32,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_R11G11B10_FLOAT
    { VK_FORMAT_B10G11R11_UFLOAT_PACK32,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R32_UINT,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_R8G8B8A8_TYPELESS
    { VK_FORMAT_R8G8B8A8_UNORM,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R8G8B8A8_UINT },
    // DXGI_FORMAT_R8G8B8A8_UNORM
    { VK_FORMAT_R8G8B8A8_UNORM,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R8G8B8A8_UINT,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
    { VK_FORMAT_R8G8B8A8_SRGB,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R8G8B8A8_UINT,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_R8G8B8A8_UINT
    { VK_FORMAT_R8G8B8A8_UINT,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R8G8B8A8_UINT,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_R8G8B8A8_SNORM
    { VK_FORMAT_R8G8B8A8_SNORM,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R8G8B8A8_UINT,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_R8G8B8A8_SINT
    { VK_FORMAT_R8G8B8A8_SINT,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R8G8B8A8_UINT,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_R16G16_TYPELESS
    { VK_FORMAT_R16G16_UNORM,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R16G16_UINT },
    // DXGI_FORMAT_R16G16_FLOAT
    { VK_FORMAT_R16G16_SFLOAT,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R16G16_UINT,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_R16G16_UNORM
    { VK_FORMAT_R16G16_UNORM,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R16G16_UINT,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_R16G16_UINT
    { VK_FORMAT_R16G16_UINT,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R16G16_UINT,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_R16G16_SNORM
    { VK_FORMAT_R16G16_SNORM,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R16G16_UINT,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_R16G16_SINT
    { VK_FORMAT_R16G16_SINT,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R16G16_UINT,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_R32_TYPELESS
    { VK_FORMAT_R32_UINT,
      VK_FORMAT_D32_SFLOAT,
      VK_FORMAT_R32_UINT },
    // DXGI_FORMAT_D32_FLOAT
    { VK_FORMAT_UNDEFINED,
      VK_FORMAT_D32_SFLOAT,
      VK_FORMAT_UNDEFINED,
      0, VK_IMAGE_ASPECT_DEPTH_BIT },
    // DXGI_FORMAT_R32_FLOAT
    { VK_FORMAT_R32_SFLOAT,
      VK_FORMAT_D32_SFLOAT,
      VK_FORMAT_R32_UINT,
      VK_IMAGE_ASPECT_COLOR_BIT,
      VK_IMAGE_ASPECT_DEPTH_BIT },
    // DXGI_FORMAT_R32_UINT
    { VK_FORMAT_R32_UINT,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R32_UINT,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_R32_SINT
    { VK_FORMAT_R32_SINT,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R32_UINT,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_R24G8_TYPELESS
    { VK_FORMAT_UNDEFINED,
      VK_FORMAT_D24_UNORM_S8_UINT,
      VK_FORMAT_UNDEFINED },
    // DXGI_FORMAT_D24_UNORM_S8_UINT
    { VK_FORMAT_UNDEFINED,
      VK_FORMAT_D24_UNORM_S8_UINT,
      VK_FORMAT_UNDEFINED,
      0, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT },
    // DXGI_FORMAT_R24_UNORM_X8_TYPELESS
    { VK_FORMAT_UNDEFINED,
      VK_FORMAT_D24_UNORM_S8_UINT,
      VK_FORMAT_UNDEFINED,
      0, VK_IMAGE_ASPECT_DEPTH_BIT },
    // DXGI_FORMAT_X24_TYPELESS_G8_UINT
    { VK_FORMAT_UNDEFINED,
      VK_FORMAT_D24_UNORM_S8_UINT,
      VK_FORMAT_UNDEFINED,
      0, VK_IMAGE_ASPECT_STENCIL_BIT },
    // DXGI_FORMAT_R8G8_TYPELESS
    { VK_FORMAT_R8G8_UNORM,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R8G8_UINT },
    // DXGI_FORMAT_R8G8_UNORM
    { VK_FORMAT_R8G8_UNORM,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R8G8_UINT,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_R8G8_UINT
    { VK_FORMAT_R8G8_UINT,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R8G8_UINT,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_R8G8_SNORM
    { VK_FORMAT_R8G8_SNORM,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R8G8_UINT,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_R8G8_SINT
    { VK_FORMAT_R8G8_SINT,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R8G8_UINT,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_R16_TYPELESS
    { VK_FORMAT_R16_UNORM,
      VK_FORMAT_D16_UNORM,
      VK_FORMAT_R16_UINT },
    // DXGI_FORMAT_R16_FLOAT
    { VK_FORMAT_R16_SFLOAT,
      VK_FORMAT_D16_UNORM,
      VK_FORMAT_R16_UINT,
      VK_IMAGE_ASPECT_COLOR_BIT,
      VK_IMAGE_ASPECT_DEPTH_BIT },
    // DXGI_FORMAT_D16_UNORM
    { VK_FORMAT_UNDEFINED,
      VK_FORMAT_D16_UNORM,
      VK_FORMAT_UNDEFINED,
      0, VK_IMAGE_ASPECT_DEPTH_BIT },
    // DXGI_FORMAT_R16_UNORM
    { VK_FORMAT_R16_UNORM,
      VK_FORMAT_D16_UNORM,
      VK_FORMAT_R16_UINT,
      VK_IMAGE_ASPECT_COLOR_BIT,
      VK_IMAGE_ASPECT_DEPTH_BIT },
    // DXGI_FORMAT_R16_UINT
    { VK_FORMAT_R16_UINT,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R16_UINT,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_R16_SNORM
    { VK_FORMAT_R16_SNORM,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R16_UINT,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_R16_SINT
    { VK_FORMAT_R16_SINT,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R16_UINT,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_R8_TYPELESS
    { VK_FORMAT_R8_UNORM,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R8_UINT },
    // DXGI_FORMAT_R8_UNORM
    { VK_FORMAT_R8_UNORM,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R8_UINT,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_R8_UINT
    { VK_FORMAT_R8_UINT,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R8_UINT,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_R8_SNORM
    { VK_FORMAT_R8_SNORM,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R8_UINT,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_R8_SINT
    { VK_FORMAT_R8_SINT,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R8_UINT,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_A8_UNORM
    { VK_FORMAT_R8_UNORM,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_UNDEFINED,
      VK_IMAGE_ASPECT_COLOR_BIT, 0,
      { VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO,
        VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R }},
    // DXGI_FORMAT_R1_UNORM
    { }, // Unsupported
    // DXGI_FORMAT_R9G9B9E5_SHAREDEXP
    { VK_FORMAT_E5B9G9R9_UFLOAT_PACK32,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_UNDEFINED,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_R8G8_B8G8_UNORM
    { VK_FORMAT_B8G8R8G8_422_UNORM,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_UNDEFINED,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_G8R8_G8B8_UNORM
    { VK_FORMAT_G8B8G8R8_422_UNORM,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_UNDEFINED,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_BC1_TYPELESS
    { VK_FORMAT_BC1_RGBA_UNORM_BLOCK,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_UNDEFINED },
    // DXGI_FORMAT_BC1_UNORM
    { VK_FORMAT_BC1_RGBA_UNORM_BLOCK,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_UNDEFINED,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_BC1_UNORM_SRGB
    { VK_FORMAT_BC1_RGBA_SRGB_BLOCK,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_UNDEFINED,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_BC2_TYPELESS
    { VK_FORMAT_BC2_UNORM_BLOCK,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_UNDEFINED },
    // DXGI_FORMAT_BC2_UNORM
    { VK_FORMAT_BC2_UNORM_BLOCK,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_UNDEFINED,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_BC2_UNORM_SRGB
    { VK_FORMAT_BC2_SRGB_BLOCK,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_UNDEFINED,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_BC3_TYPELESS
    { VK_FORMAT_BC3_UNORM_BLOCK,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_UNDEFINED },
    // DXGI_FORMAT_BC3_UNORM
    { VK_FORMAT_BC3_UNORM_BLOCK,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_UNDEFINED,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_BC3_UNORM_SRGB
    { VK_FORMAT_BC3_SRGB_BLOCK,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_UNDEFINED,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_BC4_TYPELESS
    { VK_FORMAT_BC4_UNORM_BLOCK,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_UNDEFINED },
    // DXGI_FORMAT_BC4_UNORM
    { VK_FORMAT_BC4_UNORM_BLOCK,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_UNDEFINED,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_BC4_SNORM
    { VK_FORMAT_BC4_SNORM_BLOCK,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_UNDEFINED,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_BC5_TYPELESS
    { VK_FORMAT_BC5_UNORM_BLOCK,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_UNDEFINED },
    // DXGI_FORMAT_BC5_UNORM
    { VK_FORMAT_BC5_UNORM_BLOCK,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_UNDEFINED,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_BC5_SNORM
    { VK_FORMAT_BC5_SNORM_BLOCK,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_UNDEFINED,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_B5G6R5_UNORM
    { VK_FORMAT_R5G6B5_UNORM_PACK16,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_UNDEFINED,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_B5G5R5A1_UNORM
    { VK_FORMAT_A1R5G5B5_UNORM_PACK16,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_UNDEFINED,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_B8G8R8A8_UNORM
    { VK_FORMAT_B8G8R8A8_UNORM,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_UNDEFINED,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_B8G8R8X8_UNORM
    { VK_FORMAT_B8G8R8A8_UNORM,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_UNDEFINED,
      VK_IMAGE_ASPECT_COLOR_BIT, 0,
      { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G,
        VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_ONE }},
    // DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM
    { }, // Unsupported
    // DXGI_FORMAT_B8G8R8A8_TYPELESS
    { VK_FORMAT_B8G8R8A8_UNORM,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_UNDEFINED },
    // DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
    { VK_FORMAT_B8G8R8A8_SRGB,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_UNDEFINED,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_B8G8R8X8_TYPELESS
    { VK_FORMAT_B8G8R8A8_UNORM,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_UNDEFINED,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_B8G8R8X8_UNORM_SRGB
    { VK_FORMAT_B8G8R8A8_SRGB,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_UNDEFINED,
      VK_IMAGE_ASPECT_COLOR_BIT, 0,
      { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G,
        VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_ONE }},
    // DXGI_FORMAT_BC6H_TYPELESS
    { VK_FORMAT_BC6H_UFLOAT_BLOCK,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_UNDEFINED },
    // DXGI_FORMAT_BC6H_UF16
    { VK_FORMAT_BC6H_UFLOAT_BLOCK,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_UNDEFINED,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_BC6H_SF16
    { VK_FORMAT_BC6H_SFLOAT_BLOCK,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_UNDEFINED,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_BC7_TYPELESS
    { VK_FORMAT_BC7_UNORM_BLOCK,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_UNDEFINED },
    // DXGI_FORMAT_BC7_UNORM
    { VK_FORMAT_BC7_UNORM_BLOCK,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_UNDEFINED,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_BC7_UNORM_SRGB
    { VK_FORMAT_BC7_SRGB_BLOCK,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_UNDEFINED,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_AYUV
    { VK_FORMAT_R8G8B8A8_UNORM,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_R8G8B8A8_UINT,
      VK_IMAGE_ASPECT_COLOR_BIT, 0,
      { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_B,
        VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_A } },
    // DXGI_FORMAT_Y410
    { }, // Unsupported
    // DXGI_FORMAT_Y416
    { }, // Unsupported
    // DXGI_FORMAT_NV12
    { VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_UNDEFINED,
      VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT },
    // DXGI_FORMAT_P010
    { }, // Unsupported
    // DXGI_FORMAT_P016
    { }, // Unsupported
    // DXGI_FORMAT_420_OPAQUE
    { VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_UNDEFINED,
      VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT },
    // DXGI_FORMAT_YUY2
    { VK_FORMAT_G8B8G8R8_422_UNORM,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_UNDEFINED,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_Y210
    { }, // Unsupported
    // DXGI_FORMAT_Y216
    { }, // Unsupported
    // DXGI_FORMAT_NV11
    { }, // Unsupported
    // DXGI_FORMAT_AI44
    { }, // Unsupported
    // DXGI_FORMAT_IA44
    { }, // Unsupported
    // DXGI_FORMAT_P8
    { }, // Unsupported
    // DXGI_FORMAT_A8P8
    { }, // Unsupported
    // DXGI_FORMAT_B4G4R4A4_UNORM
    { VK_FORMAT_A4R4G4B4_UNORM_PACK16_EXT,
      VK_FORMAT_UNDEFINED,
      VK_FORMAT_UNDEFINED,
      VK_IMAGE_ASPECT_COLOR_BIT },
    // Reserved DXGI_FORMAT values 0x74..0x81. Keep table indices aligned
    // with the sparse enum before P208 and the private OHOS AOT formats.
    { }, { }, { }, { }, { }, { }, { },
    { }, { }, { }, { }, { }, { }, { },
    // DXGI_FORMAT_P208
    { }, // Unsupported
    // DXGI_FORMAT_V208
    { }, // Unsupported
    // DXGI_FORMAT_V408
    { }, // Unsupported
    // Private OHOS AOT formats: the packed bytes already match Vulkan. These
    // entries do not remap or alter any standard BC format or fallback.
    // DXGI_FORMAT_OHOS_AOT_ASTC_4X4_UNORM
    { VK_FORMAT_ASTC_4x4_UNORM_BLOCK, VK_FORMAT_UNDEFINED,
      VK_FORMAT_UNDEFINED, VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_OHOS_AOT_ASTC_4X4_SRGB
    { VK_FORMAT_ASTC_4x4_SRGB_BLOCK, VK_FORMAT_UNDEFINED,
      VK_FORMAT_UNDEFINED, VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_OHOS_AOT_EAC_R11_UNORM
    { VK_FORMAT_EAC_R11_UNORM_BLOCK, VK_FORMAT_UNDEFINED,
      VK_FORMAT_UNDEFINED, VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_OHOS_AOT_EAC_R11_SNORM
    { VK_FORMAT_EAC_R11_SNORM_BLOCK, VK_FORMAT_UNDEFINED,
      VK_FORMAT_UNDEFINED, VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_OHOS_AOT_EAC_RG11_UNORM
    { VK_FORMAT_EAC_R11G11_UNORM_BLOCK, VK_FORMAT_UNDEFINED,
      VK_FORMAT_UNDEFINED, VK_IMAGE_ASPECT_COLOR_BIT },
    // DXGI_FORMAT_OHOS_AOT_EAC_RG11_SNORM
    { VK_FORMAT_EAC_R11G11_SNORM_BLOCK, VK_FORMAT_UNDEFINED,
      VK_FORMAT_UNDEFINED, VK_IMAGE_ASPECT_COLOR_BIT },
  }};

  static_assert(g_dxgiFormats[DXGI_FORMAT_BC1_UNORM].FormatColor
      == VK_FORMAT_BC1_RGBA_UNORM_BLOCK &&
    g_dxgiFormats[DXGI_FORMAT_B4G4R4A4_UNORM].FormatColor
      == VK_FORMAT_A4R4G4B4_UNORM_PACK16_EXT &&
    g_dxgiFormats[DXGI_FORMAT_P208].FormatColor == VK_FORMAT_UNDEFINED &&
    g_dxgiFormats[DXGI_FORMAT_OHOS_AOT_ASTC_4X4_UNORM].FormatColor
      == VK_FORMAT_ASTC_4x4_UNORM_BLOCK &&
    g_dxgiFormats[DXGI_FORMAT_OHOS_AOT_ASTC_4X4_SRGB].FormatColor
      == VK_FORMAT_ASTC_4x4_SRGB_BLOCK &&
    g_dxgiFormats[DXGI_FORMAT_OHOS_AOT_EAC_R11_UNORM].FormatColor
      == VK_FORMAT_EAC_R11_UNORM_BLOCK &&
    g_dxgiFormats[DXGI_FORMAT_OHOS_AOT_EAC_R11_SNORM].FormatColor
      == VK_FORMAT_EAC_R11_SNORM_BLOCK &&
    g_dxgiFormats[DXGI_FORMAT_OHOS_AOT_EAC_RG11_UNORM].FormatColor
      == VK_FORMAT_EAC_R11G11_UNORM_BLOCK &&
    g_dxgiFormats[DXGI_FORMAT_OHOS_AOT_EAC_RG11_SNORM].FormatColor
      == VK_FORMAT_EAC_R11G11_SNORM_BLOCK,
    "DXGI format table must be indexed by the sparse DXGI_FORMAT enum");


  const std::array<DXGI_VK_FORMAT_FAMILY, 139> g_dxgiFamilies = {{
    // DXGI_FORMAT_UNKNOWN
    { },
    // DXGI_FORMAT_R32G32B32A32_TYPELESS
    { VK_FORMAT_R32G32B32A32_UINT,
      VK_FORMAT_R32G32B32A32_SINT,
      VK_FORMAT_R32G32B32A32_SFLOAT },
    // DXGI_FORMAT_R32G32B32A32_FLOAT
    { },
    // DXGI_FORMAT_R32G32B32A32_UINT
    { },
    // DXGI_FORMAT_R32G32B32A32_SINT
    { },
    // DXGI_FORMAT_R32G32B32_TYPELESS
    { VK_FORMAT_R32G32B32_UINT,
      VK_FORMAT_R32G32B32_SINT,
      VK_FORMAT_R32G32B32_SFLOAT },
    // DXGI_FORMAT_R32G32B32_FLOAT
    { },
    // DXGI_FORMAT_R32G32B32_UINT
    { },
    // DXGI_FORMAT_R32G32B32_SINT
    { },
    // DXGI_FORMAT_R16G16B16A16_TYPELESS
    { VK_FORMAT_R16G16B16A16_UNORM,
      VK_FORMAT_R16G16B16A16_SNORM,
      VK_FORMAT_R16G16B16A16_UINT,
      VK_FORMAT_R16G16B16A16_SINT,
      VK_FORMAT_R16G16B16A16_SFLOAT },
    // DXGI_FORMAT_R16G16B16A16_FLOAT
    { },
    // DXGI_FORMAT_R16G16B16A16_UNORM
    { },
    // DXGI_FORMAT_R16G16B16A16_UINT
    { },
    // DXGI_FORMAT_R16G16B16A16_SNORM
    { },
    // DXGI_FORMAT_R16G16B16A16_SINT
    { },
    // DXGI_FORMAT_R32G32_TYPELESS
    { VK_FORMAT_R32G32_UINT,
      VK_FORMAT_R32G32_SINT,
      VK_FORMAT_R32G32_SFLOAT },
    // DXGI_FORMAT_R32G32_FLOAT
    { },
    // DXGI_FORMAT_R32G32_UINT
    { },
    // DXGI_FORMAT_R32G32_SINT
    { },
    // DXGI_FORMAT_R32G8X24_TYPELESS
    { },
    // DXGI_FORMAT_D32_FLOAT_S8X24_UINT
    { },
    // DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS
    { },
    // DXGI_FORMAT_X32_TYPELESS_G8X24_UINT
    { },
    // DXGI_FORMAT_R10G10B10A2_TYPELESS
    { VK_FORMAT_A2B10G10R10_UNORM_PACK32,
      VK_FORMAT_A2B10G10R10_UINT_PACK32 },
    // DXGI_FORMAT_R10G10B10A2_UNORM
    { },
    // DXGI_FORMAT_R10G10B10A2_UINT
    { },
    // DXGI_FORMAT_R11G11B10_FLOAT
    { },
    // DXGI_FORMAT_R8G8B8A8_TYPELESS
    { VK_FORMAT_R8G8B8A8_UNORM,
      VK_FORMAT_R8G8B8A8_SNORM,
      VK_FORMAT_R8G8B8A8_SRGB,
      VK_FORMAT_R8G8B8A8_UINT,
      VK_FORMAT_R8G8B8A8_SINT },
    // DXGI_FORMAT_R8G8B8A8_UNORM
    { VK_FORMAT_R8G8B8A8_UNORM,
      VK_FORMAT_R8G8B8A8_SRGB },
    // DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
    { VK_FORMAT_R8G8B8A8_UNORM,
      VK_FORMAT_R8G8B8A8_SRGB },
    // DXGI_FORMAT_R8G8B8A8_UINT
    { },
    // DXGI_FORMAT_R8G8B8A8_SNORM
    { },
    // DXGI_FORMAT_R8G8B8A8_SINT
    { },
    // DXGI_FORMAT_R16G16_TYPELESS
    { VK_FORMAT_R16G16_UNORM,
      VK_FORMAT_R16G16_SNORM,
      VK_FORMAT_R16G16_UINT,
      VK_FORMAT_R16G16_SINT,
      VK_FORMAT_R16G16_SFLOAT },
    // DXGI_FORMAT_R16G16_FLOAT
    { },
    // DXGI_FORMAT_R16G16_UNORM
    { },
    // DXGI_FORMAT_R16G16_UINT
    { },
    // DXGI_FORMAT_R16G16_SNORM
    { },
    // DXGI_FORMAT_R16G16_SINT
    { },
    // DXGI_FORMAT_R32_TYPELESS
    { VK_FORMAT_R32_UINT,
      VK_FORMAT_R32_SINT,
      VK_FORMAT_R32_SFLOAT },
    // DXGI_FORMAT_D32_FLOAT
    { },
    // DXGI_FORMAT_R32_FLOAT
    { },
    // DXGI_FORMAT_R32_UINT
    { },
    // DXGI_FORMAT_R32_SINT
    { },
    // DXGI_FORMAT_R24G8_TYPELESS
    { },
    // DXGI_FORMAT_D24_UNORM_S8_UINT
    { },
    // DXGI_FORMAT_R24_UNORM_X8_TYPELESS
    { },
    // DXGI_FORMAT_X24_TYPELESS_G8_UINT
    { },
    // DXGI_FORMAT_R8G8_TYPELESS
    { VK_FORMAT_R8G8_UNORM,
      VK_FORMAT_R8G8_SNORM,
      VK_FORMAT_R8G8_UINT,
      VK_FORMAT_R8G8_SINT },
    // DXGI_FORMAT_R8G8_UNORM
    { },
    // DXGI_FORMAT_R8G8_UINT
    { },
    // DXGI_FORMAT_R8G8_SNORM
    { },
    // DXGI_FORMAT_R8G8_SINT
    { },
    // DXGI_FORMAT_R16_TYPELESS
    { VK_FORMAT_R16_UNORM,
      VK_FORMAT_R16_SNORM,
      VK_FORMAT_R16_UINT,
      VK_FORMAT_R16_SINT,
      VK_FORMAT_R16_SFLOAT },
    // DXGI_FORMAT_R16_FLOAT
    { },
    // DXGI_FORMAT_D16_UNORM
    { },
    // DXGI_FORMAT_R16_UNORM
    { },
    // DXGI_FORMAT_R16_UINT
    { },
    // DXGI_FORMAT_R16_SNORM
    { },
    // DXGI_FORMAT_R16_SINT
    { },
    // DXGI_FORMAT_R8_TYPELESS
    { VK_FORMAT_R8_UNORM,
      VK_FORMAT_R8_SNORM,
      VK_FORMAT_R8_UINT,
      VK_FORMAT_R8_SINT },
    // DXGI_FORMAT_R8_UNORM
    { },
    // DXGI_FORMAT_R8_UINT
    { },
    // DXGI_FORMAT_R8_SNORM
    { },
    // DXGI_FORMAT_R8_SINT
    { },
    // DXGI_FORMAT_A8_UNORM
    { },
    // DXGI_FORMAT_R1_UNORM
    { }, // Unsupported
    // DXGI_FORMAT_R9G9B9E5_SHAREDEXP
    { },
    // DXGI_FORMAT_R8G8_B8G8_UNORM
    { },
    // DXGI_FORMAT_G8R8_G8B8_UNORM
    { },
    // DXGI_FORMAT_BC1_TYPELESS
    { VK_FORMAT_BC1_RGBA_UNORM_BLOCK,
      VK_FORMAT_BC1_RGBA_SRGB_BLOCK },
    // DXGI_FORMAT_BC1_UNORM
    { VK_FORMAT_BC1_RGBA_UNORM_BLOCK,
      VK_FORMAT_BC1_RGBA_SRGB_BLOCK },
    // DXGI_FORMAT_BC1_UNORM_SRGB
    { VK_FORMAT_BC1_RGBA_UNORM_BLOCK,
      VK_FORMAT_BC1_RGBA_SRGB_BLOCK },
    // DXGI_FORMAT_BC2_TYPELESS
    { VK_FORMAT_BC2_UNORM_BLOCK,
      VK_FORMAT_BC2_SRGB_BLOCK },
    // DXGI_FORMAT_BC2_UNORM
    { VK_FORMAT_BC2_UNORM_BLOCK,
      VK_FORMAT_BC2_SRGB_BLOCK },
    // DXGI_FORMAT_BC2_UNORM_SRGB
    { VK_FORMAT_BC2_UNORM_BLOCK,
      VK_FORMAT_BC2_SRGB_BLOCK },
    // DXGI_FORMAT_BC3_TYPELESS
    { VK_FORMAT_BC3_UNORM_BLOCK,
      VK_FORMAT_BC3_SRGB_BLOCK },
    // DXGI_FORMAT_BC3_UNORM
    { VK_FORMAT_BC3_UNORM_BLOCK,
      VK_FORMAT_BC3_SRGB_BLOCK },
    // DXGI_FORMAT_BC3_UNORM_SRGB
    { VK_FORMAT_BC3_UNORM_BLOCK,
      VK_FORMAT_BC3_SRGB_BLOCK },
    // DXGI_FORMAT_BC4_TYPELESS
    { VK_FORMAT_BC4_UNORM_BLOCK,
      VK_FORMAT_BC4_SNORM_BLOCK },
    // DXGI_FORMAT_BC4_UNORM
    { },
    // DXGI_FORMAT_BC4_SNORM
    { },
    // DXGI_FORMAT_BC5_TYPELESS
    { VK_FORMAT_BC5_UNORM_BLOCK,
      VK_FORMAT_BC5_SNORM_BLOCK },
    // DXGI_FORMAT_BC5_UNORM
    { },
    // DXGI_FORMAT_BC5_SNORM
    { },
    // DXGI_FORMAT_B5G6R5_UNORM
    { },
    // DXGI_FORMAT_B5G5R5A1_UNORM
    { },
    // DXGI_FORMAT_B8G8R8A8_UNORM
    { VK_FORMAT_B8G8R8A8_UNORM,
      VK_FORMAT_B8G8R8A8_SRGB },
    // DXGI_FORMAT_B8G8R8X8_UNORM
    { VK_FORMAT_B8G8R8A8_UNORM,
      VK_FORMAT_B8G8R8A8_SRGB },
    // DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM
    { }, // Unsupported
    // DXGI_FORMAT_B8G8R8A8_TYPELESS
    { VK_FORMAT_B8G8R8A8_UNORM,
      VK_FORMAT_B8G8R8A8_SRGB },
    // DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
    { VK_FORMAT_B8G8R8A8_UNORM,
      VK_FORMAT_B8G8R8A8_SRGB },
    // DXGI_FORMAT_B8G8R8X8_TYPELESS
    { VK_FORMAT_B8G8R8A8_UNORM,
      VK_FORMAT_B8G8R8A8_SRGB },
    // DXGI_FORMAT_B8G8R8X8_UNORM_SRGB
    { VK_FORMAT_B8G8R8A8_UNORM,
      VK_FORMAT_B8G8R8A8_SRGB },
    // DXGI_FORMAT_BC6H_TYPELESS
    { VK_FORMAT_BC6H_UFLOAT_BLOCK,
      VK_FORMAT_BC6H_SFLOAT_BLOCK },
    // DXGI_FORMAT_BC6H_UF16
    { },
    // DXGI_FORMAT_BC6H_SF16
    { },
    // DXGI_FORMAT_BC7_TYPELESS
    { VK_FORMAT_BC7_UNORM_BLOCK,
      VK_FORMAT_BC7_SRGB_BLOCK },
    // DXGI_FORMAT_BC7_UNORM
    { VK_FORMAT_BC7_UNORM_BLOCK,
      VK_FORMAT_BC7_SRGB_BLOCK },
    // DXGI_FORMAT_BC7_UNORM_SRGB
    { VK_FORMAT_BC7_UNORM_BLOCK,
      VK_FORMAT_BC7_SRGB_BLOCK },
    // DXGI_FORMAT_AYUV
    { VK_FORMAT_R8G8B8A8_UNORM,
      VK_FORMAT_R8G8B8A8_UINT },
    // DXGI_FORMAT_Y410
    { }, // Unsupported
    // DXGI_FORMAT_Y416
    { }, // Unsupported
    // DXGI_FORMAT_NV12
    { VK_FORMAT_R8_UNORM,
      VK_FORMAT_R8G8_UNORM,
      VK_FORMAT_R8_UINT,
      VK_FORMAT_R8G8_UINT },
    // DXGI_FORMAT_P010
    { }, // Unsupported
    // DXGI_FORMAT_P016
    { }, // Unsupported
    // DXGI_FORMAT_420_OPAQUE
    { VK_FORMAT_R8_UNORM,
      VK_FORMAT_R8G8_UNORM,
      VK_FORMAT_R8_UINT,
      VK_FORMAT_R8G8_UINT },
    // DXGI_FORMAT_YUY2
    { VK_FORMAT_G8B8G8R8_422_UNORM,
      VK_FORMAT_R8G8B8A8_UNORM,
      VK_FORMAT_R8G8B8A8_UINT },
    // DXGI_FORMAT_Y210
    { }, // Unsupported
    // DXGI_FORMAT_Y216
    { }, // Unsupported
    // DXGI_FORMAT_NV11
    { }, // Unsupported
    // DXGI_FORMAT_AI44
    { }, // Unsupported
    // DXGI_FORMAT_IA44
    { }, // Unsupported
    // DXGI_FORMAT_P8
    { }, // Unsupported
    // DXGI_FORMAT_A8P8
    { }, // Unsupported
    // DXGI_FORMAT_B4G4R4A4_UNORM
    { }, // Unsupported
    // Reserved DXGI_FORMAT values 0x74..0x81.
    { }, { }, { }, { }, { }, { }, { },
    { }, { }, { }, { }, { }, { }, { },
    // DXGI_FORMAT_P208
    { }, // Unsupported
    // DXGI_FORMAT_V208
    { }, // Unsupported
    // DXGI_FORMAT_V408
    { }, // Unsupported
    // Private OHOS AOT formats have no mutable reinterpretation family.
    { }, // ASTC 4x4 UNORM
    { }, // ASTC 4x4 SRGB
    { }, // EAC R11 UNORM
    { }, // EAC R11 SNORM
    { }, // EAC RG11 UNORM
    { }, // EAC RG11 SNORM
  }};
  
  
  DXGIVkFormatTable::DXGIVkFormatTable(const Rc<DxvkAdapter>& adapter)
  : m_dxgiFormats (g_dxgiFormats), m_dxgiFamilies(g_dxgiFamilies) {
    // AMD do not support 24-bit depth buffers on Vulkan,
    // so we have to fall back to a 32-bit depth format.
    if (!CheckImageFormatSupport(adapter, VK_FORMAT_D24_UNORM_S8_UINT,
          VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
          VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
      Logger::info("DXGI: VK_FORMAT_D24_UNORM_S8_UINT -> VK_FORMAT_D32_SFLOAT_S8_UINT");
      RemapDepthFormat(DXGI_FORMAT_R24G8_TYPELESS,        VK_FORMAT_D32_SFLOAT_S8_UINT);
      RemapDepthFormat(DXGI_FORMAT_R24_UNORM_X8_TYPELESS, VK_FORMAT_D32_SFLOAT_S8_UINT);
      RemapDepthFormat(DXGI_FORMAT_X24_TYPELESS_G8_UINT,  VK_FORMAT_D32_SFLOAT_S8_UINT);
      RemapDepthFormat(DXGI_FORMAT_D24_UNORM_S8_UINT,     VK_FORMAT_D32_SFLOAT_S8_UINT);
    }

    // Mobile Vulkan implementations generally expose ASTC/ETC2 rather than
    // desktop BC formats. D3D-visible BC payloads are decoded at upload time
    // and stored in format-equivalent uncompressed backing images.
    // On the native OHOS Vulkan stack, the feature bit alone is not a
    // sufficient indicator: some drivers advertise textureCompressionBC but
    // do not expose every D3D-visible BC format as a sampled image.  WineHua
    // relies on the feature bit in its controlled driver stack; direct native
    // DXVK must also inspect the concrete format properties before deciding
    // whether the upload-time decoder is required.
    bool lacksBcSampledImageSupport
      = !adapter->features().core.features.textureCompressionBC;

    if (!lacksBcSampledImageSupport) {
      const std::array<VkFormat, 14> bcSampledImageFormats = {{
        VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_SRGB_BLOCK,
        VK_FORMAT_BC2_UNORM_BLOCK,      VK_FORMAT_BC2_SRGB_BLOCK,
        VK_FORMAT_BC3_UNORM_BLOCK,      VK_FORMAT_BC3_SRGB_BLOCK,
        VK_FORMAT_BC4_UNORM_BLOCK,      VK_FORMAT_BC4_SNORM_BLOCK,
        VK_FORMAT_BC5_UNORM_BLOCK,      VK_FORMAT_BC5_SNORM_BLOCK,
        VK_FORMAT_BC6H_UFLOAT_BLOCK,    VK_FORMAT_BC6H_SFLOAT_BLOCK,
        VK_FORMAT_BC7_UNORM_BLOCK,      VK_FORMAT_BC7_SRGB_BLOCK,
      }};

      for (VkFormat format : bcSampledImageFormats) {
        // D3D11 default resources are optimal-tiled Vulkan images.  A
        // linear-only sampled capability cannot service this path.
        const VkFormatProperties formatProperties = adapter->formatProperties(format);
        if ((formatProperties.optimalTilingFeatures
              & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0) {
          lacksBcSampledImageSupport = true;
          break;
        }
      }
    }

    if (lacksBcSampledImageSupport
     && env::getEnvVar("WINEHUA_DXVK_BC_EMULATION") != "0") {
      const VkComponentMapping identity = {
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };

      auto remap = [this, identity](DXGI_FORMAT format, VkFormat target) {
        RemapColorFormat(format, target, identity);
      };

      /* Backing format for the unsupported BC colour textures.
       *
       * The device has no block-compression support, so the decoded pixels are
       * stored in a format it can sample.  Without an explicit request the port
       * picks the smallest backing the device can sample: ETC2 when supported,
       * else packed 16-bit colour, else RGBA8.
       *
       * The uncompressed decode is not the default because it is not affordable
       * here: measured on the reference device it grows the graphics footprint
       * from 1.9 GiB to 5.5-6.0 GiB, which is the range where the system starts
       * reclaiming the process, and it also costs frame time (P95 39 ms to
       * 44 ms).  "rgba8" selects it explicitly, which is what a device with
       * spare GPU memory would choose.
       *
       * The switch is process-local and only affects upload-time decoding. */
      const std::string backingMode = env::getEnvVar("WINEHUA_DXVK_BC_BACKING");
      const bool automaticBacking =
        backingMode.empty() || backingMode == "default";
      /* Bisection helpers for the ETC2 path: colour textures and the two
       * single/two-channel (normal, height, AO) textures can be enabled
       * separately to isolate visual regressions. */
      const bool wantEtc2Color = automaticBacking || backingMode == "etc2"
        || backingMode == "etc2color" || backingMode == "etc2eac"
        || backingMode == "etc2nosrgb";
      /* "etc2nosrgb" applies ETC2 only to linear colour textures; sRGB
       * textures keep their 8-bit layout, which isolates the sRGB block
       * formats from the rest of the transcode. */
      const bool allowEtc2Srgb = automaticBacking || backingMode != "etc2nosrgb";
      const bool wantEtc2Eac = automaticBacking
        || backingMode == "etc2" || backingMode == "etc2eac";
      const VkFormatProperties packedProperties =
        adapter->formatProperties(VK_FORMAT_B4G4R4A4_UNORM_PACK16);
      const bool packed16Allowed = (automaticBacking || backingMode == "packed16")
        && (packedProperties.optimalTilingFeatures
              & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;

      /* "etc2" re-encodes the decoded pixels into a block format the device
       * does support, keeping the footprint close to the original BC data and
       * preserving sRGB through the matching sRGB block formats. */
      const auto supportsSampled = [adapter](VkFormat format) {
        return (adapter->formatProperties(format).optimalTilingFeatures
          & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
      };
      const bool etc2Allowed = wantEtc2Color
        && supportsSampled(VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK)
        && supportsSampled(VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK);
      const bool etc2SrgbAllowed = etc2Allowed && allowEtc2Srgb;
      const bool eacAllowed = wantEtc2Eac
        && supportsSampled(VK_FORMAT_EAC_R11_UNORM_BLOCK)
        && supportsSampled(VK_FORMAT_EAC_R11G11_UNORM_BLOCK);

      const VkFormat colorBacking = etc2Allowed
        ? VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK
        : (packed16Allowed ? VK_FORMAT_B4G4R4A4_UNORM_PACK16
                           : VK_FORMAT_R8G8B8A8_UNORM);

      const VkFormat colorBackingSrgb = etc2Allowed
        ? (etc2SrgbAllowed ? VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK
                           : VK_FORMAT_R8G8B8A8_SRGB)
        : VK_FORMAT_R8G8B8A8_SRGB;

      remap(DXGI_FORMAT_BC1_TYPELESS,   colorBacking);
      remap(DXGI_FORMAT_BC1_UNORM,      colorBacking);
      remap(DXGI_FORMAT_BC1_UNORM_SRGB, colorBackingSrgb);
      remap(DXGI_FORMAT_BC2_TYPELESS,   colorBacking);
      remap(DXGI_FORMAT_BC2_UNORM,      colorBacking);
      remap(DXGI_FORMAT_BC2_UNORM_SRGB, colorBackingSrgb);
      remap(DXGI_FORMAT_BC3_TYPELESS,   colorBacking);
      remap(DXGI_FORMAT_BC3_UNORM,      colorBacking);
      remap(DXGI_FORMAT_BC3_UNORM_SRGB, colorBackingSrgb);
      remap(DXGI_FORMAT_BC4_TYPELESS,   eacAllowed
        ? VK_FORMAT_EAC_R11_UNORM_BLOCK : VK_FORMAT_R8_UNORM);
      remap(DXGI_FORMAT_BC4_UNORM,      eacAllowed
        ? VK_FORMAT_EAC_R11_UNORM_BLOCK : VK_FORMAT_R8_UNORM);
      remap(DXGI_FORMAT_BC4_SNORM,      VK_FORMAT_R8_SNORM);
      remap(DXGI_FORMAT_BC5_TYPELESS,   eacAllowed
        ? VK_FORMAT_EAC_R11G11_UNORM_BLOCK : VK_FORMAT_R8G8_UNORM);
      remap(DXGI_FORMAT_BC5_UNORM,      eacAllowed
        ? VK_FORMAT_EAC_R11G11_UNORM_BLOCK : VK_FORMAT_R8G8_UNORM);
      remap(DXGI_FORMAT_BC5_SNORM,      VK_FORMAT_R8G8_SNORM);
      remap(DXGI_FORMAT_BC6H_TYPELESS,  VK_FORMAT_R16G16B16A16_SFLOAT);
      remap(DXGI_FORMAT_BC6H_UF16,      VK_FORMAT_R16G16B16A16_SFLOAT);
      remap(DXGI_FORMAT_BC6H_SF16,      VK_FORMAT_R16G16B16A16_SFLOAT);
      remap(DXGI_FORMAT_BC7_TYPELESS,   colorBacking);
      remap(DXGI_FORMAT_BC7_UNORM,      colorBacking);
      remap(DXGI_FORMAT_BC7_UNORM_SRGB, colorBackingSrgb);

      /* Report the resolved BC backing mapping together with the device
       * capabilities it came from.  The port has no native BC support here, so
       * every BC format is served by a decoded native format; stating the
       * decision and the probed features from the device keeps later
       * investigations from having to infer what was chosen.  Nothing here
       * changes the mapping itself, and it follows the port's telemetry switch
       * because it runs on every adapter initialisation. */
      if (winehuaTelemetryEnabled()) {
        const auto featureString = [adapter](VkFormat format) {
          if (format == VK_FORMAT_UNDEFINED)
            return std::string("undefined");
          const VkFormatProperties properties = adapter->formatProperties(format);
          const VkFormatFeatureFlags flags = properties.optimalTilingFeatures;
          std::string result = (flags & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)
            ? "sampled" : "NOSAMPLE";
          if (flags & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
            result += "+linear";
          if (flags & VK_FORMAT_FEATURE_TRANSFER_DST_BIT)
            result += "+transferDst";
          return result;
        };
        const auto featureValue = [adapter](VkFormat format) {
          return format != VK_FORMAT_UNDEFINED
            && (adapter->formatProperties(format).optimalTilingFeatures
                & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
        };
        winehuaBcFallbackLog(str::format("[BC-Fallback] nativeBC=",
          lacksBcSampledImageSupport ? 0 : 1,
          " ETC2=", featureValue(VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK) ? 1 : 0,
          " EAC=", featureValue(VK_FORMAT_EAC_R11_UNORM_BLOCK) ? 1 : 0,
          " ASTC_LDR=", featureValue(VK_FORMAT_ASTC_4x4_UNORM_BLOCK) ? 1 : 0,
          " packed16=", packed16Allowed ? 1 : 0));
        winehuaBcFallbackLog(str::format("[BC-Fallback] probe ETC2_RGB=",
          featureString(VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK).c_str(),
          " ETC2_RGB8A1=", featureString(VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK).c_str(),
          " ETC2_RGBA=", featureString(VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK).c_str()));
        winehuaBcFallbackLog(str::format("[BC-Fallback] probe EAC_R11=",
          featureString(VK_FORMAT_EAC_R11_UNORM_BLOCK).c_str(),
          " EAC_R11_SNORM=", featureString(VK_FORMAT_EAC_R11_SNORM_BLOCK).c_str(),
          " EAC_R11G11=", featureString(VK_FORMAT_EAC_R11G11_UNORM_BLOCK).c_str(),
          " EAC_R11G11_SNORM=", featureString(VK_FORMAT_EAC_R11G11_SNORM_BLOCK).c_str()));
        winehuaBcFallbackLog(str::format("[BC-Fallback] probe ASTC_4x4=",
          featureString(VK_FORMAT_ASTC_4x4_UNORM_BLOCK).c_str(),
          " ASTC_4x4_SRGB=", featureString(VK_FORMAT_ASTC_4x4_SRGB_BLOCK).c_str(),
          " RGBA16F=", featureString(VK_FORMAT_R16G16B16A16_SFLOAT).c_str()));
        winehuaBcFallbackLog(str::format("[BC-Fallback] map BC1->",
          uint32_t(colorBacking),
          " BC2->", uint32_t(colorBacking),
          " BC3->", uint32_t(colorBacking),
          " BC4->", uint32_t(eacAllowed ? VK_FORMAT_EAC_R11_UNORM_BLOCK : VK_FORMAT_R8_UNORM),
          " BC5->", uint32_t(eacAllowed ? VK_FORMAT_EAC_R11G11_UNORM_BLOCK : VK_FORMAT_R8G8_UNORM),
          " BC6H->", uint32_t(VK_FORMAT_R16G16B16A16_SFLOAT),
          " BC7->", uint32_t(colorBacking),
          " srgbBC1/2/3/7->", uint32_t(colorBackingSrgb),
          " bc4Snorm->", uint32_t(VK_FORMAT_R8_SNORM),
          " bc5Snorm->", uint32_t(VK_FORMAT_R8G8_SNORM),
          " bc4SnormEac=", featureValue(VK_FORMAT_EAC_R11_SNORM_BLOCK) ? 1 : 0));
      }

      const DXGI_VK_FORMAT_FAMILY rgba8Family = {
        VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB };
      const DXGI_VK_FORMAT_FAMILY rgba8UnormFamily = {
        VK_FORMAT_R8G8B8A8_UNORM };
      const DXGI_VK_FORMAT_FAMILY rgba8SrgbFamily = {
        VK_FORMAT_R8G8B8A8_SRGB };
      const DXGI_VK_FORMAT_FAMILY r8Family = {
        VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SNORM };
      const DXGI_VK_FORMAT_FAMILY rg8Family = {
        VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SNORM };
      const DXGI_VK_FORMAT_FAMILY r8UnormFamily = {
        VK_FORMAT_R8_UNORM };
      const DXGI_VK_FORMAT_FAMILY r8SnormFamily = {
        VK_FORMAT_R8_SNORM };
      const DXGI_VK_FORMAT_FAMILY rg8UnormFamily = {
        VK_FORMAT_R8G8_UNORM };
      const DXGI_VK_FORMAT_FAMILY rg8SnormFamily = {
        VK_FORMAT_R8G8_SNORM };
      const DXGI_VK_FORMAT_FAMILY rgba16fFamily = {
        VK_FORMAT_R16G16B16A16_SFLOAT };
      const DXGI_VK_FORMAT_FAMILY packed16Family = {
        VK_FORMAT_B4G4R4A4_UNORM_PACK16 };
      const DXGI_VK_FORMAT_FAMILY etc2Family = {
        VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK };
      const DXGI_VK_FORMAT_FAMILY etc2SrgbFamily = {
        VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK };
      const DXGI_VK_FORMAT_FAMILY eacR11Family = {
        VK_FORMAT_EAC_R11_UNORM_BLOCK };
      const DXGI_VK_FORMAT_FAMILY eacR11G11Family = {
        VK_FORMAT_EAC_R11G11_UNORM_BLOCK };

      for (DXGI_FORMAT format : {
             DXGI_FORMAT_BC1_TYPELESS, DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM_SRGB,
             DXGI_FORMAT_BC2_TYPELESS, DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC2_UNORM_SRGB,
             DXGI_FORMAT_BC3_TYPELESS, DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC3_UNORM_SRGB,
             DXGI_FORMAT_BC7_TYPELESS, DXGI_FORMAT_BC7_UNORM, DXGI_FORMAT_BC7_UNORM_SRGB })
        m_dxgiFamilies[uint32_t(format)] = rgba8Family;

      m_dxgiFamilies[uint32_t(DXGI_FORMAT_BC4_TYPELESS)] = r8Family;
      m_dxgiFamilies[uint32_t(DXGI_FORMAT_BC4_UNORM)] = r8UnormFamily;
      m_dxgiFamilies[uint32_t(DXGI_FORMAT_BC4_SNORM)] = r8SnormFamily;
      m_dxgiFamilies[uint32_t(DXGI_FORMAT_BC5_TYPELESS)] = rg8Family;
      m_dxgiFamilies[uint32_t(DXGI_FORMAT_BC5_UNORM)] = rg8UnormFamily;
      m_dxgiFamilies[uint32_t(DXGI_FORMAT_BC5_SNORM)] = rg8SnormFamily;
      m_dxgiFamilies[uint32_t(DXGI_FORMAT_BC6H_TYPELESS)] = rgba16fFamily;
      m_dxgiFamilies[uint32_t(DXGI_FORMAT_BC6H_UF16)] = rgba16fFamily;
      m_dxgiFamilies[uint32_t(DXGI_FORMAT_BC6H_SF16)] = rgba16fFamily;

      for (DXGI_FORMAT format : {
             DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC2_UNORM,
             DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC7_UNORM })
        m_dxgiFamilies[uint32_t(format)] = etc2Allowed ? etc2Family
          : (packed16Allowed ? packed16Family : rgba8UnormFamily);

      for (DXGI_FORMAT format : {
             DXGI_FORMAT_BC1_UNORM_SRGB, DXGI_FORMAT_BC2_UNORM_SRGB,
             DXGI_FORMAT_BC3_UNORM_SRGB, DXGI_FORMAT_BC7_UNORM_SRGB })
        m_dxgiFamilies[uint32_t(format)] = etc2SrgbAllowed
          ? etc2SrgbFamily : rgba8SrgbFamily;

      for (DXGI_FORMAT format : {
             DXGI_FORMAT_BC4_TYPELESS, DXGI_FORMAT_BC4_UNORM })
        m_dxgiFamilies[uint32_t(format)] = eacAllowed
          ? eacR11Family : r8UnormFamily;

      for (DXGI_FORMAT format : {
             DXGI_FORMAT_BC5_TYPELESS, DXGI_FORMAT_BC5_UNORM })
        m_dxgiFamilies[uint32_t(format)] = eacAllowed
          ? eacR11G11Family : rg8UnormFamily;

      Logger::info(str::format("WineHua: BC1-BC7 upload-time decompression enabled",
        " backing=", etc2Allowed ? "etc2"
          : (packed16Allowed ? "packed16" : "rgba8"),
        " requested=", backingMode.empty() ? "default" : backingMode.c_str(),
        " etc2Sampled=", uint32_t(etc2Allowed),
        " etc2Srgb=", uint32_t(etc2SrgbAllowed),
        " eacSampled=", uint32_t(eacAllowed),
        " packed4444Sampled=", uint32_t(
          (packedProperties.optimalTilingFeatures
            & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0),
        " etc2=", uint32_t(adapter->features().core.features.textureCompressionETC2),
        " astcLdr=", uint32_t(adapter->features().core.features.textureCompressionASTC_LDR)));
    }

    if (!adapter->features().ext4444Formats.formatA4R4G4B4) {
      RemapColorFormat(DXGI_FORMAT_B4G4R4A4_UNORM, VK_FORMAT_B4G4R4A4_UNORM_PACK16,
        { VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R,
          VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_B });
    }
  }
  
  
  DXGIVkFormatTable::~DXGIVkFormatTable() {
    
  }
  
  
  DXGI_VK_FORMAT_INFO DXGIVkFormatTable::GetFormatInfo(
          DXGI_FORMAT         Format,
          DXGI_VK_FORMAT_MODE Mode) const {
    return GetFormatInfoFromMapping(
      GetFormatMapping(Format), Mode);
  }


  DXGI_VK_FORMAT_INFO DXGIVkFormatTable::GetPackedFormatInfo(
          DXGI_FORMAT         Format,
          DXGI_VK_FORMAT_MODE Mode) const {
    return GetFormatInfoFromMapping(
      GetPackedFormatMapping(Format), Mode);
  }
  
  
  DXGI_VK_FORMAT_FAMILY DXGIVkFormatTable::GetFormatFamily(
          DXGI_FORMAT         Format,
          DXGI_VK_FORMAT_MODE Mode) const {
    if (Mode == DXGI_VK_FORMAT_MODE_DEPTH)
      return DXGI_VK_FORMAT_FAMILY();
    
    const size_t formatId = size_t(Format);

    return formatId < m_dxgiFamilies.size()
      ? m_dxgiFamilies[formatId]
      : m_dxgiFamilies[0];
  }


  DXGI_VK_FORMAT_INFO DXGIVkFormatTable::GetFormatInfoFromMapping(
    const DXGI_VK_FORMAT_MAPPING* pMapping,
          DXGI_VK_FORMAT_MODE   Mode) const {
    switch (Mode) {
      case DXGI_VK_FORMAT_MODE_ANY:
        return pMapping->FormatColor != VK_FORMAT_UNDEFINED
          ? DXGI_VK_FORMAT_INFO { pMapping->FormatColor, pMapping->AspectColor, pMapping->Swizzle }
          : DXGI_VK_FORMAT_INFO { pMapping->FormatDepth, pMapping->AspectDepth };
      
      case DXGI_VK_FORMAT_MODE_COLOR:
        return { pMapping->FormatColor, pMapping->AspectColor, pMapping->Swizzle };
      
      case DXGI_VK_FORMAT_MODE_DEPTH:
        return { pMapping->FormatDepth, pMapping->AspectDepth };
      
      case DXGI_VK_FORMAT_MODE_RAW:
        return { pMapping->FormatRaw, pMapping->AspectColor };
    }
    
    Logger::err("DXGI: GetFormatInfoFromMapping: Internal error");
    return DXGI_VK_FORMAT_INFO();
  }


  const DXGI_VK_FORMAT_MAPPING* DXGIVkFormatTable::GetFormatMapping(
          DXGI_FORMAT         Format) const {
    const size_t formatId = size_t(Format);
    
    return formatId < m_dxgiFormats.size()
      ? &m_dxgiFormats[formatId]
      : &m_dxgiFormats[0];
  }
  

  const DXGI_VK_FORMAT_MAPPING* DXGIVkFormatTable::GetPackedFormatMapping(
          DXGI_FORMAT         Format) const {
    const size_t formatId = size_t(Format);
    
    return formatId < g_dxgiFormats.size()
      ? &g_dxgiFormats[formatId]
      : &g_dxgiFormats[0];
  }
  

  bool DXGIVkFormatTable::CheckImageFormatSupport(
    const Rc<DxvkAdapter>&      Adapter,
          VkFormat              Format,
          VkFormatFeatureFlags  Features) const {
    VkFormatProperties supported = Adapter->formatProperties(Format);
    
    return (supported.linearTilingFeatures  & Features) == Features
        || (supported.optimalTilingFeatures & Features) == Features;
  }
  
  
  void DXGIVkFormatTable::RemapDepthFormat(
          DXGI_FORMAT         Format,
          VkFormat            Target) {
    m_dxgiFormats[uint32_t(Format)].FormatDepth = Target;
  }
  

  void DXGIVkFormatTable::RemapColorFormat(
          DXGI_FORMAT         Format,
          VkFormat            Target,
          VkComponentMapping  Swizzle) {
    m_dxgiFormats[uint32_t(Format)].FormatColor = Target;
    m_dxgiFormats[uint32_t(Format)].AspectColor = VK_IMAGE_ASPECT_COLOR_BIT;
    m_dxgiFormats[uint32_t(Format)].Swizzle = Swizzle;
  }
  
}
