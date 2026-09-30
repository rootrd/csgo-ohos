#include "vtf_decode.h"
#include "binary.h"
#include <cstring>
#include <algorithm>
#include <cstdio>

uint32_t MipDim(uint32_t base, unsigned level) {
    Require(level < 32, "invalid mip level");
    uint32_t v = base >> level;
    return v ? v : 1;
}

bool IsCompressedFormat(int32_t f) {
    switch (f) {
        case FMT_DXT1: case FMT_DXT3: case FMT_DXT5:
        case FMT_DXT1_ONEBITALPHA: case FMT_ATI1N: case FMT_ATI2N:
            return true;
        default: return false;
    }
}

int CompressedBlockBytes(int32_t f) {
    switch (f) {
        case FMT_DXT1: case FMT_DXT1_ONEBITALPHA: case FMT_ATI1N: return 8;
        case FMT_DXT3: case FMT_DXT5: case FMT_ATI2N: return 16;
        default: return 0;
    }
}

static uint8_t Expand5(uint32_t v) { return (uint8_t)((v << 3) | (v >> 2)); }
static uint8_t Expand6(uint32_t v) { return (uint8_t)((v << 2) | (v >> 4)); }

// Decode one 4x4 DXT1 block into rgba (16*4 bytes, row-major).
static void DecodeDXT1Block(const uint8_t* src, uint8_t* rgba, int stride, bool forceFourColors) {
    uint16_t c0, c1;
    c0 = LE16(src); c1 = LE16(src + 2);
    uint8_t col[4][4];
    col[0][0] = Expand5((c0 >> 11) & 31); col[0][1] = Expand6((c0 >> 5) & 63); col[0][2] = Expand5(c0 & 31); col[0][3] = 255;
    col[1][0] = Expand5((c1 >> 11) & 31); col[1][1] = Expand6((c1 >> 5) & 63); col[1][2] = Expand5(c1 & 31); col[1][3] = 255;
    if (c0 > c1 || forceFourColors) {
        for (int i = 0; i < 3; ++i) { col[2][i] = (2 * col[0][i] + col[1][i]) / 3; col[3][i] = (col[0][i] + 2 * col[1][i]) / 3; }
        col[2][3] = col[3][3] = 255;
    } else {
        for (int i = 0; i < 3; ++i) { col[2][i] = (col[0][i] + col[1][i]) / 2; col[3][i] = 0; }
        col[2][3] = 255; col[3][3] = 0;
    }
    uint32_t idx;
    memcpy(&idx, src + 4, 4);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            int sel = (idx >> (2 * (4 * y + x))) & 3;
            memcpy(rgba + y * stride + x * 4, col[sel], 4);
        }
}

static void DecodeDXT3Alpha(const uint8_t* src, uint8_t* rgba, int stride) {
    uint64_t a;
    memcpy(&a, src, 8);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            int bits = (a >> (4 * (4 * y + x))) & 0xF;
            rgba[y * stride + x * 4 + 3] = (uint8_t)((bits << 4) | bits);
        }
}

static void DecodeDXT5Alpha(const uint8_t* src, uint8_t* rgba, int stride) {
    uint8_t a0 = src[0], a1 = src[1];
    uint8_t pal[8];
    pal[0] = a0; pal[1] = a1;
    if (a0 > a1) {
        for (int i = 2; i < 8; ++i) pal[i] = (uint8_t)(((8 - i) * a0 + (i - 1) * a1) / 7);
    } else {
        for (int i = 2; i < 6; ++i) pal[i] = (uint8_t)(((6 - i) * a0 + (i - 1) * a1) / 5);
        pal[6] = 0; pal[7] = 255;
    }
    uint64_t bits = 0;
    memcpy(&bits, src + 2, 6);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            int sel = (bits >> (3 * (4 * y + x))) & 7;
            rgba[y * stride + x * 4 + 3] = pal[sel];
        }
}

static void DecodeCompressedMip(const uint8_t* src, int w, int h, int32_t fmt, uint8_t* out /* w*h*4 */) {
    int bw = (w + 3) / 4, bh = (h + 3) / 4;
    int blockBytes = CompressedBlockBytes(fmt);
    std::vector<uint8_t> blockRGBA(16 * 4);
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx) {
            const uint8_t* blk = src + (by * bw + bx) * blockBytes;
            // fresh block buffer, alpha default 255
            memset(blockRGBA.data(), 0, 64);
            for (int i = 3; i < 64; i += 4) blockRGBA[i] = 255;
            switch (fmt) {
                case FMT_DXT1:
                case FMT_DXT1_ONEBITALPHA:
                    DecodeDXT1Block(blk, blockRGBA.data(), 16, false);
                    break;
                case FMT_DXT3:
                    DecodeDXT1Block(blk + 8, blockRGBA.data(), 16, true);
                    DecodeDXT3Alpha(blk, blockRGBA.data(), 16);
                    break;
                case FMT_DXT5:
                    DecodeDXT1Block(blk + 8, blockRGBA.data(), 16, true);
                    DecodeDXT5Alpha(blk, blockRGBA.data(), 16);
                    break;
                case FMT_ATI1N: {
                    // single-channel; replicate to RGB
                    DecodeDXT5Alpha(blk, blockRGBA.data(), 16);
                    for (int i = 0; i < 16; ++i) {
                        uint8_t v = blockRGBA[i * 4 + 3];
                        blockRGBA[i * 4 + 0] = v; blockRGBA[i * 4 + 1] = 0; blockRGBA[i * 4 + 2] = 0;
                        blockRGBA[i * 4 + 3] = 255;
                    }
                    break;
                }
                case FMT_ATI2N: {
                    // two channels: X in first block, Y in second; reconstruct in RG
                    std::vector<uint8_t> tmp(64);
                    for (int i = 3; i < 64; i += 4) tmp[i] = 255;
                    DecodeDXT5Alpha(blk, tmp.data(), 16);
                    DecodeDXT5Alpha(blk + 8, blockRGBA.data(), 16); // Y -> alpha slot
                    for (int i = 0; i < 16; ++i) {
                        uint8_t x = tmp[i * 4 + 3], y = blockRGBA[i * 4 + 3];
                        blockRGBA[i * 4 + 0] = x; blockRGBA[i * 4 + 1] = y;
                        blockRGBA[i * 4 + 2] = 0; blockRGBA[i * 4 + 3] = 255;
                    }
                    break;
                }
                default: break;
            }
            // blit 4x4 into out (clipped at edges)
            for (int y = 0; y < 4; ++y) {
                int py = by * 4 + y; if (py >= h) break;
                for (int x = 0; x < 4; ++x) {
                    int px = bx * 4 + x; if (px >= w) break;
                    memcpy(out + (py * w + px) * 4, blockRGBA.data() + y * 16 + x * 4, 4);
                }
            }
        }
}

static int UncompressedBpp(int32_t fmt) {
    switch (fmt) {
        case FMT_RGBA8888: case FMT_ABGR8888: case FMT_ARGB8888: case FMT_BGRA8888:
        case FMT_BGRX8888: case FMT_RGBX8888: case FMT_UVWQ8888: case FMT_UVLX8888:
            return 4;
        case FMT_RGB888: case FMT_BGR888: case FMT_RGB888_BS: case FMT_BGR888_BS: return 3;
        case FMT_RGB565: case FMT_BGR565: case FMT_UV88: case FMT_IA88:
        case FMT_BGRA4444: case FMT_BGRA5551: case FMT_BGRX5551: return 2;
        case FMT_I8: case FMT_A8: return 1;
        case FMT_RGBA16161616F: case FMT_RGBA16161616: return 8;
        case FMT_R32F: case FMT_RG1616F: return 4;
        case FMT_RG3232F: return 8;
        case FMT_RGB323232F: return 12;
        case FMT_RGBA32323232F: return 16;
        default: return 0;
    }
}

static bool DecodeUncompressedMip(const uint8_t* src, int w, int h, int32_t fmt, uint8_t* out) {
    int bpp = UncompressedBpp(fmt);
    if (!bpp) return false;
    int n = w * h;
    for (int i = 0; i < n; ++i) {
        const uint8_t* s = src + i * bpp;
        uint8_t* d = out + i * 4;
        switch (fmt) {
            case FMT_RGBA8888: d[0]=s[0]; d[1]=s[1]; d[2]=s[2]; d[3]=s[3]; break;
            case FMT_ABGR8888: d[0]=s[3]; d[1]=s[2]; d[2]=s[1]; d[3]=s[0]; break;
            case FMT_ARGB8888: d[0]=s[1]; d[1]=s[2]; d[2]=s[3]; d[3]=s[0]; break;
            case FMT_BGRA8888: d[0]=s[2]; d[1]=s[1]; d[2]=s[0]; d[3]=s[3]; break;
            case FMT_BGRX8888: d[0]=s[2]; d[1]=s[1]; d[2]=s[0]; d[3]=255; break;
            case FMT_RGBX8888: d[0]=s[0]; d[1]=s[1]; d[2]=s[2]; d[3]=255; break;
            case FMT_RGB888: d[0]=s[0]; d[1]=s[1]; d[2]=s[2]; d[3]=255; break;
            case FMT_BGR888: d[0]=s[2]; d[1]=s[1]; d[2]=s[0]; d[3]=255; break;
            case FMT_RGB565: { uint16_t v; memcpy(&v,s,2); d[0]=Expand5((v>>11)&31); d[1]=Expand6((v>>5)&63); d[2]=Expand5(v&31); d[3]=255; break; }
            case FMT_BGR565: { uint16_t v = LE16(s); d[0]=Expand5((v>>11)&31); d[1]=Expand6((v>>5)&63); d[2]=Expand5(v&31); d[3]=255; break; }
            case FMT_I8: d[0]=s[0]; d[1]=s[0]; d[2]=s[0]; d[3]=255; break;
            case FMT_A8: d[0]=255; d[1]=255; d[2]=255; d[3]=s[0]; break;
            case FMT_IA88: d[0]=s[0]; d[1]=s[0]; d[2]=s[0]; d[3]=s[1]; break;
            case FMT_BGRA4444: { uint16_t v = LE16(s); d[0]=((v>>8)&15)*17; d[1]=((v>>4)&15)*17; d[2]=(v&15)*17; d[3]=((v>>12)&15)*17; break; }
            case FMT_BGRA5551: case FMT_BGRX5551: { uint16_t v = LE16(s); d[0]=Expand5((v>>10)&31); d[1]=Expand5((v>>5)&31); d[2]=Expand5(v&31); d[3]=(fmt == FMT_BGRX5551 || (v&0x8000))?255:0; break; }
            case FMT_RGB888_BS: case FMT_BGR888_BS: {
                d[0]=s[fmt==FMT_RGB888_BS?0:2]; d[1]=s[1]; d[2]=s[fmt==FMT_RGB888_BS?2:0];
                d[3]=(d[0]==0 && d[1]==0 && d[2]==255)?0:255; break;
            }
            default: return false;
        }
    }
    return true;
}

size_t VtfSliceBytes(int32_t format, uint32_t width, uint32_t height) {
    if (IsCompressedFormat(format))
        return size_t((width + 3) / 4) * ((height + 3) / 4) * CompressedBlockBytes(format);
    const int bytes = UncompressedBpp(format);
    Require(bytes != 0, "unsupported VTF format " + std::to_string(format));
    return size_t(width) * height * bytes;
}

// Source pads compressed volume mips with a depth of 2 or 3 to four slices.
// See ImageLoader::GetMemRequired in src/bitmap/imageformat.cpp.
static unsigned StoredDepth(const VtfTexture& texture, unsigned mip) {
    unsigned depth = MipDim(texture.depth, mip);
    return IsCompressedFormat(texture.format) && depth > 1 && depth < 4 ? 4 : depth;
}

size_t VtfTexture::Offset(unsigned mip, unsigned frame, unsigned face, unsigned slice) const {
    Require(mip < mipCount && frame < frames && face < faces && slice < MipDim(depth, mip), "VTF subresource out of range");
    return mipOffsets[mip] + ((size_t(frame) * storedFaces + face) * StoredDepth(*this, mip) + slice) * sliceBytes[mip];
}

VtfTexture ParseVTF(const uint8_t* data, size_t size) {
    Require(size >= 63 && memcmp(data, "VTF\0", 4) == 0, "not a VTF");
    VtfTexture out;
    out.versionMinor = LE32(data + 8);
    Require(LE32(data + 4) == 7 && out.versionMinor <= 5, "unsupported VTF version");
    const uint32_t headerSize = LE32(data + 12);
    const uint32_t minimumHeader = out.versionMinor >= 3 ? 80 : out.versionMinor >= 2 ? 65 : 63;
    Require(headerSize >= minimumHeader && headerSize <= size, "invalid VTF header size");
    out.width = LE16(data + 16); out.height = LE16(data + 18);
    out.flags = LE32(data + 20); out.frames = LE16(data + 24); out.startFrame = LE16(data + 26);
    out.format = int32_t(LE32(data + 52)); out.mipCount = data[56];
    out.depth = out.versionMinor >= 2 ? LE16(data + 63) : 1;
    out.faces = (out.flags & VTF_FLAG_CUBEMAP) ? 6 : 1;
    out.storedFaces = out.faces;
    Require(out.width && out.height && out.depth && out.frames, "zero VTF dimension/frame count");
    Require(out.faces == 1 || (out.width == out.height && out.depth == 1), "invalid VTF cubemap dimensions");
    unsigned maxMips = 1;
    for (auto dim = std::max({out.width, out.height, out.depth}); dim > 1; dim >>= 1) ++maxMips;
    Require(out.mipCount && out.mipCount <= maxMips, "invalid VTF mip count");
    const int32_t lowFormat = int32_t(LE32(data + 57));
    const unsigned lowWidth = data[61], lowHeight = data[62];
    const size_t lowBytes = lowWidth && lowHeight && lowFormat != -1 ? VtfSliceBytes(lowFormat, lowWidth, lowHeight) : 0;
    out.metadata["source.vtf.header"] = {data, data + headerSize};
    out.imageOffset = headerSize + lowBytes;
    size_t lowOffset = headerSize;
    size_t imageEnd = size;
    const uint32_t resources = out.versionMinor >= 3 ? LE32(data + 68) : 0;
    Require(resources <= 32 && (resources == 0 || RangeFits(80, size_t(resources) * 8, headerSize)), "invalid VTF resource table");
    bool foundImage = false;
    for (unsigned i = 0; i < resources; ++i) {
        const uint32_t type = LE32(data + 80 + i * 8), value = LE32(data + 84 + i * 8);
        const uint32_t tag = type & 0xffffff;
        const bool inlineData = (type & 0x02000000) != 0;
        Require((type & 0xfd000000) == 0, "unsupported VTF resource flags");
        if (tag == 0x30) {
            Require(!inlineData && !foundImage, "invalid/duplicate VTF image resource");
            foundImage = true; out.imageOffset = value;
        } else {
            char key[48]; snprintf(key, sizeof(key), "source.vtf.resource.%08x", type);
            Require(!out.metadata.count(key), "duplicate VTF resource");
            if (inlineData) out.metadata[key] = {data + 84 + i * 8, data + 88 + i * 8};
            else {
                Require(value >= headerSize, "VTF resource overlaps header");
                size_t bytes = lowBytes;
                if (tag == 1) lowOffset = value;
                else {
                    Require(RangeFits(value, 4, size), "truncated VTF resource length");
                    bytes = size_t(LE32(data + value)) + 4;
                }
                Require(RangeFits(value, bytes, size), "truncated VTF resource payload");
                out.metadata[key] = {data + value, data + value + bytes};
            }
        }
    }
    Require(resources == 0 || foundImage, "no VTF image resource");
    Require(out.imageOffset >= headerSize && out.imageOffset < size, "VTF image offset out of range");
    if (resources == 0 && lowBytes) {
        Require(RangeFits(lowOffset, lowBytes, size), "truncated VTF thumbnail");
        out.metadata["source.vtf.resource.00000001"] = {data + lowOffset, data + lowOffset + lowBytes};
    }
    for (unsigned i = 0; i < resources; ++i) {
        const uint32_t type = LE32(data + 80 + i * 8), value = LE32(data + 84 + i * 8);
        if (!(type & 0x02000000) && value > out.imageOffset) imageEnd = std::min(imageEnd, size_t(value));
    }
    // VTF <= 7.4 can carry a seventh, legacy sphere face. CS:GO also writes six-face
    // files with these versions. Resolve only exact payload sizes, never shift by guesswork.
    size_t oneFaceBytes = 0;
    for (unsigned m = 0; m < out.mipCount; ++m)
        oneFaceBytes += VtfSliceBytes(out.format, MipDim(out.width, m), MipDim(out.height, m)) * StoredDepth(out, m);
    if (out.faces == 6 && out.versionMinor >= 1 && out.versionMinor < 5 && out.startFrame != 0xffff &&
        imageEnd - out.imageOffset == oneFaceBytes * out.frames * 7)
        out.storedFaces = 7;
    size_t cur = out.imageOffset;
    out.mipOffsets.resize(out.mipCount); out.sliceBytes.resize(out.mipCount);
    for (int m = int(out.mipCount) - 1; m >= 0; --m) {
        out.mipOffsets[m] = cur;
        out.sliceBytes[m] = VtfSliceBytes(out.format, MipDim(out.width, m), MipDim(out.height, m));
        const uint64_t bytes = uint64_t(out.sliceBytes[m]) * StoredDepth(out, m) * out.frames * out.storedFaces;
        Require(RangeFits(cur, bytes, imageEnd), "truncated VTF mip/frame/face/slice data");
        if (out.storedFaces == 7) {
            auto& sphere = out.metadata["source.vtf.legacy_spheremap"];
            for (unsigned f = 0; f < out.frames; ++f) {
                const auto offset = cur + (size_t(f) * 7 + 6) * out.sliceBytes[m];
                sphere.insert(sphere.end(), data + offset, data + offset + out.sliceBytes[m]);
            }
        }
        cur += bytes;
    }
    Require(cur == imageEnd, "unexpected VTF image payload size");
    out.imageBytes = cur - out.imageOffset;
    return out;
}

std::vector<uint8_t> DecodeVtfSlice(const uint8_t* data, size_t size, const VtfTexture& texture,
                                  unsigned mip, unsigned frame, unsigned face, unsigned slice) {
    const size_t offset = texture.Offset(mip, frame, face, slice);
    Require(RangeFits(offset, texture.sliceBytes[mip], size), "truncated VTF slice");
    const auto width = MipDim(texture.width, mip), height = MipDim(texture.height, mip);
    Require(size_t(width) * height <= (1ULL << 28), "VTF decoded slice exceeds 1 GiB");
    std::vector<uint8_t> rgba(size_t(width) * height * 4);
    if (IsCompressedFormat(texture.format)) DecodeCompressedMip(data + offset, width, height, texture.format, rgba.data());
    else Require(DecodeUncompressedMip(data + offset, width, height, texture.format, rgba.data()), "format requires a lossless KTX2 representation");
    return rgba;
}
