// Use Khronos libktx for the DFD, Vulkan format mapping, mip ordering and alignment.
#pragma once
#include <ktx.h>
#include <vulkan/vulkan_core.h>
#include <cstdint>
#include <string>
#include <vector>
#include "vtf_decode.h"

void CheckKtx(KTX_error_code result, const char* operation);
VkFormat AstcFormat(unsigned block, bool srgb);
VkFormat LosslessFormat(const VtfTexture& texture);

class Ktx2Writer {
public:
    Ktx2Writer(const VtfTexture& source, VkFormat format);
    ~Ktx2Writer();
    Ktx2Writer(const Ktx2Writer&) = delete;
    Ktx2Writer& operator=(const Ktx2Writer&) = delete;
    void SetImage(unsigned mip, unsigned frame, unsigned faceOrSlice, const uint8_t* data, size_t size);
    void Metadata(const std::string& key, const std::vector<uint8_t>& value);
    void Metadata(const std::string& key, const std::string& value);
    void Save(const std::string& path);
    size_t DataSize() const { return texture_->dataSize; }
private:
    ktxTexture2* texture_ = nullptr;
};
