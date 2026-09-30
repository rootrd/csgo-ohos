#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>

#include "d3d11_format_convert.h"

namespace dxvk {
  namespace {

    uint16_t Float32ToFloat16(float value) {
      uint32_t bits = 0;
      std::memcpy(&bits, &value, sizeof(bits));

      const uint16_t sign = uint16_t((bits >> 16) & 0x8000u);
      uint32_t mantissa = bits & 0x007fffffu;
      int32_t exponent = int32_t((bits >> 23) & 0xffu) - 127 + 15;

      if (exponent <= 0) {
        if (exponent < -10)
          return sign;
        mantissa = (mantissa | 0x00800000u) >> uint32_t(1 - exponent);
        if (mantissa & 0x00001000u)
          mantissa += 0x00002000u;
        return uint16_t(sign | (mantissa >> 13));
      }

      if (exponent >= 31)
        return uint16_t(sign | 0x7c00u);

      if (mantissa & 0x00001000u) {
        mantissa += 0x00002000u;
        if (mantissa & 0x00800000u) {
          mantissa = 0;
          exponent += 1;
          if (exponent >= 31)
            return uint16_t(sign | 0x7c00u);
        }
      }

      return uint16_t(sign | (uint16_t(exponent) << 10) | uint16_t(mantissa >> 13));
    }

  }

  bool ConvertD3D11Rgba8SnormToRgba16Float(
          VkExtent3D               extent,
    const void*                    source,
          VkDeviceSize             sourceRowPitch,
          VkDeviceSize             sourceSlicePitch,
          D3D11CpuImage&           result) {
    if (!source || !extent.width || !extent.height || !extent.depth)
      return false;

    const uint64_t minimumSourceRow = uint64_t(extent.width) * 4u;
    if (!sourceRowPitch)
      sourceRowPitch = minimumSourceRow;
    if (sourceRowPitch < minimumSourceRow)
      return false;

    const uint64_t minimumSourceSlice = uint64_t(sourceRowPitch) * extent.height;
    if (!sourceSlicePitch)
      sourceSlicePitch = minimumSourceSlice;
    if (sourceSlicePitch < minimumSourceSlice)
      return false;

    const uint64_t outputRow = uint64_t(extent.width) * 8u;
    const uint64_t outputSlice = outputRow * extent.height;
    const uint64_t outputSize = outputSlice * extent.depth;
    if (outputRow > std::numeric_limits<VkDeviceSize>::max()
     || outputSlice > std::numeric_limits<VkDeviceSize>::max()
     || outputSize > std::numeric_limits<size_t>::max())
      return false;

    result.rowPitch = VkDeviceSize(outputRow);
    result.slicePitch = VkDeviceSize(outputSlice);
    result.data.resize(size_t(outputSize));

    for (uint32_t z = 0; z < extent.depth; z++) {
      const auto* sourceSlice = reinterpret_cast<const int8_t*>(
        reinterpret_cast<const uint8_t*>(source) + z * sourceSlicePitch);
      auto* targetSlice = reinterpret_cast<uint16_t*>(
        result.data.data() + z * result.slicePitch);

      for (uint32_t y = 0; y < extent.height; y++) {
        const int8_t* sourceRow = sourceSlice + y * sourceRowPitch;
        uint16_t* targetRow = reinterpret_cast<uint16_t*>(
          reinterpret_cast<uint8_t*>(targetSlice) + y * result.rowPitch);

        for (uint32_t x = 0; x < extent.width * 4u; x++) {
          const int32_t value = sourceRow[x];
          const float normalized = value <= -127
            ? -1.0f
            : float(value) * (1.0f / 127.0f);
          targetRow[x] = Float32ToFloat16(normalized);
        }
      }
    }

    return true;
  }

}
