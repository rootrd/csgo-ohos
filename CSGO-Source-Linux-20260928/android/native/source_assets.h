#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Renderer upload boundary, independent of Source VTF and D3D9 format enums.
enum class TextureEncoding { BC1, BC2, BC3, ASTC };
struct SourceTexture {
    uint32_t width = 0, height = 0;
    TextureEncoding encoding = TextureEncoding::BC1;
    uint32_t blockWidth = 4, blockHeight = 4, blockBytes = 8;
    uint32_t rowBytes = 0, rows = 0;
    std::vector<uint8_t> pixels;
    std::string description;
};

SourceTexture readSourceAssets(const char* root);
