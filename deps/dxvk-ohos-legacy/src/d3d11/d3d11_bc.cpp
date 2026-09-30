#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>

#include "d3d11_bc.h"

#include "../util/etcpak/ProcessRGB.hpp"

/* Standalone MIT-licensed BC decoder copied into the native DXVK source.
 * This is decoder code only: it has no Wine runtime, ABI, loader, process,
 * Broker, or IPC dependency. WINE_UNUSED enables the BC6H/BC7 routines. */
#define BCDEC_STATIC
#define WINE_UNUSED 1
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"
#undef BCDEC_IMPLEMENTATION
#undef WINE_UNUSED
#undef BCDEC_STATIC

namespace dxvk {
  namespace {

    struct BcFormatInfo {
      uint32_t blockBytes = 0;
      uint32_t pixelBytes = 0;
      bool bc6h = false;
      bool bc6hSigned = false;
      bool signedRgtc = false;
      void (*decode)(const void*, void*, int) = nullptr;
    };

    BcFormatInfo GetBcFormatInfo(VkFormat format) {
      switch (format) {
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
          return { BCDEC_BC1_BLOCK_SIZE, 4, false, false, false, bcdec_bc1 };
        case VK_FORMAT_BC2_UNORM_BLOCK:
        case VK_FORMAT_BC2_SRGB_BLOCK:
          return { BCDEC_BC2_BLOCK_SIZE, 4, false, false, false, bcdec_bc2 };
        case VK_FORMAT_BC3_UNORM_BLOCK:
        case VK_FORMAT_BC3_SRGB_BLOCK:
          return { BCDEC_BC3_BLOCK_SIZE, 4, false, false, false, bcdec_bc3 };
        case VK_FORMAT_BC4_UNORM_BLOCK:
          return { BCDEC_BC4_BLOCK_SIZE, 1, false, false, false, bcdec_bc4 };
        case VK_FORMAT_BC4_SNORM_BLOCK:
          return { BCDEC_BC4_BLOCK_SIZE, 1, false, false, true, nullptr };
        case VK_FORMAT_BC5_UNORM_BLOCK:
          return { BCDEC_BC5_BLOCK_SIZE, 2, false, false, false, bcdec_bc5 };
        case VK_FORMAT_BC5_SNORM_BLOCK:
          return { BCDEC_BC5_BLOCK_SIZE, 2, false, false, true, nullptr };
        case VK_FORMAT_BC6H_UFLOAT_BLOCK:
          return { BCDEC_BC6H_BLOCK_SIZE, 8, true, false, false, nullptr };
        case VK_FORMAT_BC6H_SFLOAT_BLOCK:
          return { BCDEC_BC6H_BLOCK_SIZE, 8, true, true, false, nullptr };
        case VK_FORMAT_BC7_UNORM_BLOCK:
        case VK_FORMAT_BC7_SRGB_BLOCK:
          return { BCDEC_BC7_BLOCK_SIZE, 4, false, false, false, bcdec_bc7 };
        default:
          return { };
      }
    }

    int8_t ClampSnormEndpoint(int8_t value) {
      return value == INT8_MIN ? int8_t(-127) : value;
    }

    void DecodeSnormChannel(const uint8_t* source, int8_t* target,
                            uint32_t targetPitch, uint32_t pixelStride) {
      const int endpoint0 = ClampSnormEndpoint(int8_t(source[0]));
      const int endpoint1 = ClampSnormEndpoint(int8_t(source[1]));
      int values[8] = { endpoint0, endpoint1 };

      if (endpoint0 > endpoint1) {
        for (int i = 2; i < 8; i++)
          values[i] = (endpoint0 * (8 - i) + endpoint1 * (i - 1)) / 7;
      } else {
        for (int i = 2; i < 6; i++)
          values[i] = (endpoint0 * (6 - i) + endpoint1 * (i - 1)) / 5;
        values[6] = -127;
        values[7] = 127;
      }

      uint64_t indices = 0;
      for (uint32_t i = 0; i < 6; i++)
        indices |= uint64_t(source[2 + i]) << (8 * i);

      for (uint32_t y = 0; y < 4; y++) {
        for (uint32_t x = 0; x < 4; x++) {
          target[y * targetPitch + x * pixelStride]
            = int8_t(values[indices & 7]);
          indices >>= 3;
        }
      }
    }

    void DecodeSnormBlock(const uint8_t* source, uint8_t* target,
                          uint32_t pixelBytes) {
      DecodeSnormChannel(source, reinterpret_cast<int8_t*>(target),
                         4 * pixelBytes, pixelBytes);
      if (pixelBytes == 2)
        DecodeSnormChannel(source + 8, reinterpret_cast<int8_t*>(target + 1),
                           4 * pixelBytes, pixelBytes);
    }

    bool ComputeOutputSize(VkExtent3D extent, uint32_t pixelBytes,
                           VkDeviceSize& rowPitch, VkDeviceSize& slicePitch,
                           size_t& totalSize) {
      const uint64_t row = uint64_t(extent.width) * pixelBytes;
      const uint64_t slice = row * extent.height;
      const uint64_t total = slice * extent.depth;
      if (row > std::numeric_limits<VkDeviceSize>::max()
       || slice > std::numeric_limits<VkDeviceSize>::max()
       || total > std::numeric_limits<size_t>::max())
        return false;
      rowPitch = VkDeviceSize(row);
      slicePitch = VkDeviceSize(slice);
      totalSize = size_t(total);
      return true;
    }

    /* Packed 16-bit targets halve the decoded footprint of colour textures on
     * drivers without BC support.  Alpha survives in the 4444 layout, which is
     * what the alpha-tested foliage and UI textures need. */
    uint32_t PackedPixelBytes(VkFormat targetFormat) {
      switch (targetFormat) {
        case VK_FORMAT_B5G6R5_UNORM_PACK16:
        case VK_FORMAT_B4G4R4A4_UNORM_PACK16:
        case VK_FORMAT_A4R4G4B4_UNORM_PACK16:
          return 2;
        default:
          return 0;
      }
    }

    uint16_t PackRgb565(const uint8_t* rgba) {
      const uint16_t r = uint16_t(rgba[0] >> 3);
      const uint16_t g = uint16_t(rgba[1] >> 2);
      const uint16_t b = uint16_t(rgba[2] >> 3);
      return uint16_t((r << 11) | (g << 5) | b);
    }

    uint16_t PackBgra4444(const uint8_t* rgba) {
      const uint16_t r = uint16_t(rgba[0] >> 4);
      const uint16_t g = uint16_t(rgba[1] >> 4);
      const uint16_t b = uint16_t(rgba[2] >> 4);
      const uint16_t a = uint16_t(rgba[3] >> 4);
      return uint16_t((b << 12) | (g << 8) | (r << 4) | a);
    }

    uint16_t PackRgba4444(const uint8_t* rgba) {
      const uint16_t r = uint16_t(rgba[0] >> 4);
      const uint16_t g = uint16_t(rgba[1] >> 4);
      const uint16_t b = uint16_t(rgba[2] >> 4);
      const uint16_t a = uint16_t(rgba[3] >> 4);
      return uint16_t((a << 12) | (b << 8) | (g << 4) | r);
    }

    /* Block-compressed targets the device does support, used when the driver
     * has no BC at all: the decoded RGBA block is re-encoded with etcpak. */
    enum class Etc2Target {
      None,
      Rgb,        // ETC2 R8G8B8        - 8 bytes per block
      Rgba,       // ETC2 R8G8B8A8      - 16 bytes per block
      EacR,       // EAC  R11           - 8 bytes per block
      EacRg,      // EAC  R11G11        - 16 bytes per block
    };

    Etc2Target GetEtc2Target(VkFormat targetFormat) {
      switch (targetFormat) {
        case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
          return Etc2Target::Rgb;
        case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
          return Etc2Target::Rgba;
        case VK_FORMAT_EAC_R11_UNORM_BLOCK:
          return Etc2Target::EacR;
        case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:
          return Etc2Target::EacRg;
        default:
          return Etc2Target::None;
      }
    }

    uint32_t Etc2BlockBytes(Etc2Target target) {
      switch (target) {
        case Etc2Target::Rgb:   return 8;
        case Etc2Target::Rgba:  return 16;
        case Etc2Target::EacR:  return 8;
        case Etc2Target::EacRg: return 16;
        default:                return 0;
      }
    }

  }

  bool DecodeD3D11BcImage(
          VkFormat                 format,
          VkFormat                 targetFormat,
          VkExtent3D               extent,
    const void*                    source,
          VkDeviceSize             sourceRowPitch,
          VkDeviceSize             sourceSlicePitch,
          D3D11CpuImage&           result) {
    const BcFormatInfo info = GetBcFormatInfo(format);
    if (!info.blockBytes || !info.pixelBytes || !source
     || !extent.width || !extent.height || !extent.depth)
      return false;

    /* Only the four-byte colour formats carry RGBA data that can be packed
     * into a 16-bit target; RGTC and BC6H keep their own layouts. */
    const bool packable = info.pixelBytes == 4;
    const uint32_t packedBytes = packable ? PackedPixelBytes(targetFormat) : 0;
    const Etc2Target etc2Target = packable
      ? GetEtc2Target(targetFormat) : Etc2Target::None;
    const uint32_t etc2Bytes = Etc2BlockBytes(etc2Target);
    const uint32_t outputPixelBytes = etc2Bytes ? 4
      : (packedBytes ? packedBytes : info.pixelBytes);

    const uint32_t blocksX = (extent.width + 3) / 4;
    const uint32_t blocksY = (extent.height + 3) / 4;
    const VkDeviceSize minimumRowPitch = VkDeviceSize(blocksX) * info.blockBytes;
    if (!sourceRowPitch)
      sourceRowPitch = minimumRowPitch;
    if (sourceRowPitch < minimumRowPitch)
      return false;

    const VkDeviceSize minimumSlicePitch = sourceRowPitch * blocksY;
    if (!sourceSlicePitch)
      sourceSlicePitch = minimumSlicePitch;
    if (sourceSlicePitch < minimumSlicePitch)
      return false;

    /* etcpak walks whole rows, so the intermediate RGBA image is padded to a
     * multiple of the block size. */
    const uint32_t paddedWidth = etc2Bytes ? blocksX * 4u : extent.width;
    const VkExtent3D outputExtent = { paddedWidth, extent.height, extent.depth };

    size_t totalSize = 0;
    if (!ComputeOutputSize(outputExtent, outputPixelBytes,
                           result.rowPitch, result.slicePitch, totalSize))
      return false;
    result.data.assign(totalSize, 0);

    const auto* sourceBytes = reinterpret_cast<const uint8_t*>(source);
    for (uint32_t z = 0; z < extent.depth; z++) {
      const uint8_t* sourceSlice = sourceBytes + z * sourceSlicePitch;
      uint8_t* targetSlice = result.data.data() + z * result.slicePitch;

      for (uint32_t blockY = 0; blockY < blocksY; blockY++) {
        for (uint32_t blockX = 0; blockX < blocksX; blockX++) {
          const uint8_t* compressed = sourceSlice
            + blockY * sourceRowPitch + blockX * info.blockBytes;
          alignas(4) uint8_t block[4 * 4 * 8] = { };

          if (info.bc6h) {
            alignas(4) uint16_t rgb[4 * 4 * 3] = { };
            /* BCDEC's BC6H routine uses a destination pitch measured in
             * uint16_t elements (the other decoders use bytes). */
            bcdec_bc6h_half(compressed, rgb, 4 * 3,
                            info.bc6hSigned ? 1 : 0);
            auto* rgba = reinterpret_cast<uint16_t*>(block);
            for (uint32_t i = 0; i < 16; i++) {
              rgba[i * 4 + 0] = rgb[i * 3 + 0];
              rgba[i * 4 + 1] = rgb[i * 3 + 1];
              rgba[i * 4 + 2] = rgb[i * 3 + 2];
              rgba[i * 4 + 3] = 0x3c00; // half-float 1.0
            }
          } else if (info.signedRgtc) {
            DecodeSnormBlock(compressed, block, info.pixelBytes);
          } else {
            info.decode(compressed, block, 4 * info.pixelBytes);
          }

          const uint32_t copyWidth = std::min(4u, extent.width - blockX * 4);
          const uint32_t copyHeight = std::min(4u, extent.height - blockY * 4);
          for (uint32_t y = 0; y < copyHeight; y++) {
            uint8_t* targetRow = targetSlice
              + (blockY * 4 + y) * result.rowPitch
              + blockX * 4 * outputPixelBytes;
            if (packedBytes) {
              auto* packed = reinterpret_cast<uint16_t*>(targetRow);
              for (uint32_t x = 0; x < copyWidth; x++) {
                const uint8_t* rgba = block + (y * 4 + x) * 4;
                switch (targetFormat) {
                  case VK_FORMAT_B5G6R5_UNORM_PACK16:
                    packed[x] = PackRgb565(rgba);
                    break;
                  case VK_FORMAT_A4R4G4B4_UNORM_PACK16:
                    packed[x] = PackRgba4444(rgba);
                    break;
                  default:
                    packed[x] = PackBgra4444(rgba);
                    break;
                }
              }
            } else if (etc2Bytes) {
              /* etcpak's ETC/EAC encoders take BGRA input (its CLI passes
               * bgr=true for those codecs), while the BC decoders we use emit
               * RGBA, so the red and blue channels are swapped here. */
              for (uint32_t x = 0; x < copyWidth; x++) {
                const uint8_t* rgba = block + (y * 4 + x) * 4;
                uint8_t* dst = targetRow + x * 4;
                dst[0] = rgba[2];
                dst[1] = rgba[1];
                dst[2] = rgba[0];
                dst[3] = rgba[3];
              }
            } else {
              std::memcpy(targetRow,
                          block + y * 4 * info.pixelBytes,
                          copyWidth * info.pixelBytes);
            }
          }
        }
      }
    }

    if (etc2Bytes) {
      /* Re-encode the decoded pixels with the device-supported block format. */
      std::vector<uint8_t> decoded = std::move(result.data);
      const VkDeviceSize decodedSlicePitch = result.slicePitch;
      result.rowPitch = VkDeviceSize(blocksX) * etc2Bytes;
      result.slicePitch = result.rowPitch * blocksY;
      result.data.assign(size_t(result.slicePitch) * extent.depth, 0);

      /* ETC2 mode selection: the heuristic path is much faster but picks modes
       * greedily, which is visible as banding on smooth skin gradients.  The
       * full search costs more CPU on the first encode of a subresource and the
       * result is cached from then on, so quality is preferred by default;
       * GTAV_OHOS_ETC2_FAST=1 restores the fast path. */
      const bool useFastEtc2 = [] {
        const char* value = std::getenv("GTAV_OHOS_ETC2_FAST");
        return value && value[0] == '1' && value[1] == '\0';
      }();
      for (uint32_t z = 0; z < extent.depth; z++) {
        const auto* src = reinterpret_cast<const uint32_t*>(
          decoded.data() + z * decodedSlicePitch);
        auto* dst = reinterpret_cast<uint64_t*>(
          result.data.data() + z * result.slicePitch);
        const uint32_t blocks = blocksX * blocksY;

        switch (etc2Target) {
          case Etc2Target::Rgb:
            CompressEtc2Rgb(src, dst, blocks, paddedWidth, useFastEtc2);
            break;
          case Etc2Target::Rgba:
            CompressEtc2Rgba(src, dst, blocks, paddedWidth, useFastEtc2);
            break;
          case Etc2Target::EacR:
            CompressEacR(src, dst, blocks, paddedWidth);
            break;
          default:
            CompressEacRg(src, dst, blocks, paddedWidth);
            break;
        }
      }
    }

    return true;
  }

}
