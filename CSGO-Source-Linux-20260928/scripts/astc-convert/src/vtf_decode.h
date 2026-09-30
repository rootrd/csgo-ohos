// VTF 7.0-7.5. Offsets refer to the original file; decode one subresource at a time.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

enum VtfImageFormat : int32_t {
    FMT_RGBA8888 = 0, FMT_ABGR8888, FMT_RGB888, FMT_BGR888, FMT_RGB565,
    FMT_I8, FMT_IA88, FMT_P8, FMT_A8,
    FMT_RGB888_BS, FMT_BGR888_BS, FMT_ARGB8888, FMT_BGRA8888,
    FMT_DXT1, FMT_DXT3, FMT_DXT5,
    FMT_BGRX8888, FMT_BGR565, FMT_BGRX5551, FMT_BGRA4444,
    FMT_DXT1_ONEBITALPHA, FMT_BGRA5551, FMT_UV88, FMT_UVWQ8888,
    FMT_RGBA16161616F, FMT_RGBA16161616, FMT_UVLX8888,
    FMT_R32F, FMT_RGB323232F, FMT_RGBA32323232F, FMT_RG1616F, FMT_RG3232F,
    FMT_RGBX8888, FMT_NULL, FMT_ATI2N, FMT_ATI1N,
};
constexpr uint32_t VTF_FLAG_CUBEMAP = 0x00004000;
constexpr uint32_t VTF_FLAG_NORMAL = 0x00000080;
constexpr uint32_t VTF_FLAG_SSBUMP = 0x08000000;

struct VtfTexture {
    uint32_t width = 0, height = 0, depth = 1, frames = 1, faces = 1, storedFaces = 1;
    uint32_t mipCount = 0, versionMinor = 0, startFrame = 0, flags = 0;
    int32_t format = -1;
    size_t imageOffset = 0, imageBytes = 0;
    std::vector<size_t> mipOffsets, sliceBytes;
    // Original header, thumbnail and auxiliary resource payloads for a future loader.
    std::map<std::string, std::vector<uint8_t>> metadata;
    size_t Offset(unsigned mip, unsigned frame, unsigned face, unsigned slice) const;
};
uint32_t MipDim(uint32_t base, unsigned level);
size_t VtfSliceBytes(int32_t format, uint32_t width, uint32_t height);
VtfTexture ParseVTF(const uint8_t* data, size_t size);
std::vector<uint8_t> DecodeVtfSlice(const uint8_t* data, size_t size, const VtfTexture& texture,
                                  unsigned mip, unsigned frame = 0, unsigned face = 0, unsigned slice = 0);
bool IsCompressedFormat(int32_t format);
int CompressedBlockBytes(int32_t format);
