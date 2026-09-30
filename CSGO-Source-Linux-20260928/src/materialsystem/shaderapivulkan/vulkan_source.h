#pragma once

#include "vulkan_upload.h"
#include "bitmap/imageformat_declarations.h"

#include <vector>

namespace sourcevk {
enum class SourceSemantic { Position, Normal, Color, Specular, BoneWeights, BoneIndices, TexCoord, TangentS, TangentT, UserData, Wrinkle };
struct SourceVertexElement {
    SourceSemantic semantic;
    uint32_t index, offset;
    VkFormat format;
};
struct SourceShaderInput {
    SourceSemantic semantic;
    uint32_t index, location;
};
struct SourceVertexLayout {
    uint32_t stride = 0;
    std::vector<SourceVertexElement> elements;
    std::vector<VkVertexInputAttributeDescription> attributes(const std::vector<SourceShaderInput>& inputs, uint32_t binding = 0) const;
};
struct SourceVertexFormatDescription {
    bool position = true, normal = false, color = false, specular = false;
    bool tangentS = false, tangentT = false, wrinkle = false, paddedPositionNormal = false;
    bool exact = false;
    uint32_t boneWeights = 0, userData = 0;
    std::array<uint32_t, 8> texCoords {};
};
// Takes the real Source VertexFormat_t value. Offsets/stride are obtained from
// the existing ComputeVertexDesc implementation, including its PC padding.
// Packed normals/tangents fail explicitly. External baked-lighting colors use
// SourceAPI's separate stream binding and add no bytes to the primary record.
SourceVertexLayout sourceVertexLayout(uint64_t format, bool bgraVertexColors = true);
uint64_t sourceVertexFormat(const SourceVertexFormatDescription& description);

struct PreparedSourceTexture {
    ImageDescription description;
    std::vector<uint8_t> pixels;
};
// One tightly packed output mip, with checked input row pitch and byte ranges.
// Common byte/float formats retain their sampling meaning. BC1/2/3 expands to
// RGBA8 on this no-optional-feature path; native BC/ASTC policy comes later.
struct SourceTextureDataLayout {
    VkFormat format = VK_FORMAT_UNDEFINED;
    size_t rowBytes = 0, rowPitch = 0, rows = 0, sourceBytes = 0, outputBytes = 0;
    uint32_t outputTexelBytes = 0;
};
// The raw Source API carries no data length. Its adapter uses this same checked
// layout calculation as the length-aware converter, including BC block rows.
SourceTextureDataLayout sourceTextureDataLayout(ImageFormat sourceFormat, uint32_t width, uint32_t height,
    size_t rowPitch = 0, bool srgb = false);
PreparedSourceTexture prepareSourceTexture(ImageFormat sourceFormat, uint32_t width, uint32_t height,
    const void* source, size_t bytes, size_t rowPitch = 0, bool srgb = false);

enum class SourceShaderStage { Vertex, Pixel };
inline constexpr uint32_t SourceVertexFloatRegisters = 256;
inline constexpr uint32_t SourcePixelFloatRegisters = 224;
inline constexpr uint32_t SourceIntegerRegisters = 16;
inline constexpr uint32_t SourceBooleanRegisters = 16;
inline constexpr VkDeviceSize SourceVertexRegisterBytes = 256 * 16 + 16 * 16 + 16 * 4;
inline constexpr VkDeviceSize SourcePixelRegisterBytes = 224 * 16 + 16 * 16 + 16 * 4;

// Set calls preserve register numbering and compare bit patterns, including NaN
// and signed zero. An unchanged bank uploads once per frame; a changed bank gets
// a new immutable slice so earlier recorded draws retain their own constants.
class SourceConstants {
public:
    SourceConstants(Context& context, FrameArena& arena);
    ~SourceConstants();
    SourceConstants(const SourceConstants&) = delete;
    SourceConstants& operator=(const SourceConstants&) = delete;
    void floats(SourceShaderStage stage, int first, const float* values, int vectors = 1);
    void integers(SourceShaderStage stage, int first, const int32_t* values, int vectors = 1);
    void booleans(SourceShaderStage stage, int first, const int32_t* values, int count = 1);
    uint64_t revision(SourceShaderStage stage) const;
    BufferSlice snapshot(SourceShaderStage stage, const Frame& frame);
    uint64_t uploads() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace sourcevk
