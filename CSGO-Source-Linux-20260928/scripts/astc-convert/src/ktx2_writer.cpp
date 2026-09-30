#include "ktx2_writer.h"
#include "binary.h"
#include <cstring>
#include <filesystem>
#include <memory>

void CheckKtx(KTX_error_code result, const char* operation) {
    Require(result == KTX_SUCCESS, std::string(operation) + ": " + ktxErrorString(result));
}

VkFormat AstcFormat(unsigned block, bool srgb) {
    switch (block) {
        case 4: return srgb ? VK_FORMAT_ASTC_4x4_SRGB_BLOCK : VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
        case 6: return srgb ? VK_FORMAT_ASTC_6x6_SRGB_BLOCK : VK_FORMAT_ASTC_6x6_UNORM_BLOCK;
        case 8: return srgb ? VK_FORMAT_ASTC_8x8_SRGB_BLOCK : VK_FORMAT_ASTC_8x8_UNORM_BLOCK;
        default: throw std::runtime_error("unsupported ASTC block size");
    }
}

VkFormat LosslessFormat(const VtfTexture& texture) {
    switch (texture.format) {
        case FMT_RGBA16161616F: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case FMT_RGBA16161616: return VK_FORMAT_R16G16B16A16_UNORM;
        case FMT_UV88: return VK_FORMAT_R8G8_SNORM;
        case FMT_UVWQ8888: return VK_FORMAT_R8G8B8A8_SNORM;
        case FMT_R32F: return VK_FORMAT_R32_SFLOAT;
        case FMT_RGB323232F: return VK_FORMAT_R32G32B32_SFLOAT;
        case FMT_RGBA32323232F: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case FMT_RG1616F: return VK_FORMAT_R16G16_SFLOAT;
        case FMT_RG3232F: return VK_FORMAT_R32G32_SFLOAT;
        case FMT_UVLX8888: throw std::runtime_error("UVLX8888 requires mixed signed/unsigned channel handling");
        default:
            // Portable volume representation; ASTC 2D support does not promise 3D support.
            return texture.depth > 1 ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_UNDEFINED;
    }
}

Ktx2Writer::Ktx2Writer(const VtfTexture& source, VkFormat format) {
    Require(source.depth == 1 || source.frames == 1, "3D texture arrays are not a Vulkan texture type");
    ktxTextureCreateInfo info{};
    info.vkFormat = format;
    info.baseWidth = source.width; info.baseHeight = source.height; info.baseDepth = source.depth;
    info.numDimensions = source.depth > 1 ? 3 : 2;
    info.numLevels = source.mipCount; info.numLayers = source.frames; info.numFaces = source.faces;
    info.isArray = source.frames > 1;
    CheckKtx(ktxTexture2_Create(&info, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &texture_), "create KTX2");
    // libktx may have alignment padding between subresources. Initialize it for reproducibility.
    memset(texture_->pData, 0, texture_->dataSize);
}
Ktx2Writer::~Ktx2Writer() {
    if (texture_) ktxTexture_Destroy(ktxTexture(texture_));
}
void Ktx2Writer::SetImage(unsigned mip, unsigned frame, unsigned faceOrSlice, const uint8_t* data, size_t size) {
    CheckKtx(ktxTexture_SetImageFromMemory(ktxTexture(texture_), mip, frame, faceOrSlice, data, size), "set KTX2 image");
}
void Ktx2Writer::Metadata(const std::string& key, const std::vector<uint8_t>& value) {
    CheckKtx(ktxHashList_AddKVPair(&texture_->kvDataHead, key.c_str(), value.size(), value.data()), "set KTX2 metadata");
}
void Ktx2Writer::Metadata(const std::string& key, const std::string& value) {
    CheckKtx(ktxHashList_AddKVPair(&texture_->kvDataHead, key.c_str(), value.size() + 1, value.c_str()), "set KTX2 metadata");
}
void Ktx2Writer::Save(const std::string& path) {
    namespace fs = std::filesystem;
    fs::create_directories(fs::path(path).parent_path());
    const std::string temporary = path + ".partial";
    try {
        CheckKtx(ktxTexture2_WriteToNamedFile(texture_, temporary.c_str()), "write KTX2");
        ktxTexture2* loaded = nullptr;
        CheckKtx(ktxTexture2_CreateFromNamedFile(temporary.c_str(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &loaded), "reload KTX2");
        std::unique_ptr<ktxTexture2, void(*)(ktxTexture2*)> checked(loaded, [](ktxTexture2* p) { ktxTexture_Destroy(ktxTexture(p)); });
        Require(loaded->vkFormat == texture_->vkFormat && loaded->dataSize == texture_->dataSize &&
                loaded->baseWidth == texture_->baseWidth && loaded->baseHeight == texture_->baseHeight &&
                loaded->baseDepth == texture_->baseDepth && loaded->numLevels == texture_->numLevels &&
                loaded->numLayers == texture_->numLayers && loaded->numFaces == texture_->numFaces &&
                loaded->isArray == texture_->isArray && memcmp(loaded->pData, texture_->pData, texture_->dataSize) == 0,
                "KTX2 readback differs from encoded subresources");
        fs::rename(temporary, path);
    } catch (...) {
        std::error_code ignored; fs::remove(temporary, ignored); throw;
    }
}
