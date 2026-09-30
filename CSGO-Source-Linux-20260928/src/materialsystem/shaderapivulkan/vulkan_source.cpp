#include "vulkan_source.h"
#include "vulkan_internal.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <set>

// Read the live Source ABI rather than maintaining a second table of bit values,
// vertex offsets, cache-line padding or VertexElement_t sizes.
#include "../shaderapidx9/meshbase.h"

namespace sourcevk {
namespace {
void require(bool condition, const char* message) { if (!condition) throw std::invalid_argument(message); }
VkFormat floatFormat(uint32_t count) {
    const VkFormat formats[] = {VK_FORMAT_UNDEFINED, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT,
        VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT};
    require(count >= 1 && count <= 4, "Source vertex field must have one to four float components");
    return formats[count];
}
} // namespace

SourceVertexLayout sourceVertexLayout(uint64_t value, bool bgra) {
    static_assert(sizeof(VertexFormat_t) == sizeof(value));
    const VertexFormat_t format = value;
    if (!value || (value >> (TEX_COORD_SIZE_BIT + 3 * VERTEX_MAX_TEXTURE_COORDINATES)))
        throw std::invalid_argument("Unknown Source vertex format bits: "+std::to_string(value));
    require(!(format & VERTEX_FORMAT_COMPRESSED), "Source packed normal/tangent decoding is not implemented");
    // VERTEX_COLOR_STREAM_1 describes a separate mesh supplied at draw time;
    // it adds no bytes to the primary interleaved stream. SourceAPI binds it
    // explicitly when the selected shader requests baked-lighting inputs.
    require(!(format & VERTEX_WRINKLE) || ((format & VERTEX_POSITION) && !(format & VERTEX_FORMAT_PAD_POS_NORM)),
        "Wrinkle requires an unpadded Source position");
    const int weights = NumBoneWeights(format), user = UserDataSize(format);
    require((weights == 0 && !(format & VERTEX_BONE_INDEX)) || (weights == 2 && (format & VERTEX_BONE_INDEX)),
        "Source mesh ABI requires exactly two weights together with four bone indices");
    require(user <= 4, "Invalid Source vertex user-data size");
    for (int i = 0; i < VERTEX_MAX_TEXTURE_COORDINATES; ++i)
        require(TexCoordSize(i, format) <= 4, "Invalid Source texture-coordinate dimension");
    alignas(32) std::array<unsigned char, 1024> storage {};
    VertexDesc_t description {};
    const int stride = ComputeVertexDesc<false>(storage.data(), format, description);
    require(stride > 0 && size_t(stride) <= storage.size(), "Invalid Source vertex stride");
    SourceVertexLayout result;
    result.stride = uint32_t(stride);
    auto add = [&](SourceSemantic semantic, uint32_t index, const void* data, VkFormat format) {
        const auto offset = static_cast<const unsigned char*>(data) - storage.data();
        require(offset >= 0 && uint64_t(offset) < result.stride, "Source vertex offset is outside its record");
        result.elements.push_back({semantic, index, uint32_t(offset), format});
    };
    const uint32_t padded = format & VERTEX_FORMAT_PAD_POS_NORM ? 4 : 3;
    if (format & VERTEX_POSITION) add(SourceSemantic::Position, 0, description.m_pPosition,
        floatFormat(format & VERTEX_WRINKLE ? 4 : padded));
    if (format & VERTEX_WRINKLE) add(SourceSemantic::Wrinkle, 0, description.m_pWrinkle, VK_FORMAT_R32_SFLOAT);
    if (weights) {
        add(SourceSemantic::BoneWeights, 0, description.m_pBoneWeight, VK_FORMAT_R32G32_SFLOAT);
        add(SourceSemantic::BoneIndices, 0, description.m_pBoneMatrixIndex, VK_FORMAT_R8G8B8A8_UINT);
    }
    if (format & VERTEX_NORMAL) add(SourceSemantic::Normal, 0, description.m_pNormal, floatFormat(padded));
    const auto color = bgra ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
    if (format & VERTEX_COLOR) add(SourceSemantic::Color, 0, description.m_pColor, color);
    if (format & VERTEX_SPECULAR) add(SourceSemantic::Specular, 0, description.m_pSpecular, color);
    for (int i = 0; i < VERTEX_MAX_TEXTURE_COORDINATES; ++i)
        if (const auto count = TexCoordSize(i, format)) add(SourceSemantic::TexCoord, uint32_t(i), description.m_pTexCoord[i], floatFormat(count));
    if (format & VERTEX_TANGENT_S) add(SourceSemantic::TangentS, 0, description.m_pTangentS, VK_FORMAT_R32G32B32_SFLOAT);
    if (format & VERTEX_TANGENT_T) add(SourceSemantic::TangentT, 0, description.m_pTangentT, VK_FORMAT_R32G32B32_SFLOAT);
    if (user) add(SourceSemantic::UserData, 0, description.m_pUserData, floatFormat(user));
    return result;
}
uint64_t sourceVertexFormat(const SourceVertexFormatDescription& d) {
    require(d.boneWeights == 0 || d.boneWeights == 2, "Source bone-weight count must be zero or two");
    require(d.userData <= 4, "Source vertex user data exceeds four components");
    VertexFormat_t value = VERTEX_BONEWEIGHT(d.boneWeights) | VERTEX_USERDATA_SIZE(d.userData);
    if (d.position) value |= VERTEX_POSITION;
    if (d.normal) value |= VERTEX_NORMAL;
    if (d.color) value |= VERTEX_COLOR;
    if (d.specular) value |= VERTEX_SPECULAR;
    if (d.tangentS) value |= VERTEX_TANGENT_S;
    if (d.tangentT) value |= VERTEX_TANGENT_T;
    if (d.wrinkle) value |= VERTEX_WRINKLE;
    if (d.paddedPositionNormal) value |= VERTEX_FORMAT_PAD_POS_NORM;
    if (d.exact) value |= VERTEX_FORMAT_USE_EXACT_FORMAT;
    if (d.boneWeights) value |= VERTEX_BONE_INDEX;
    for (size_t i = 0; i < d.texCoords.size(); ++i) {
        require(d.texCoords[i] <= 4, "Invalid Source texture-coordinate dimension");
        value |= VERTEX_TEXCOORD_SIZE(int(i), int(d.texCoords[i]));
    }
    sourceVertexLayout(value); // Apply the same structural validation as existing formats.
    return value;
}
std::vector<VkVertexInputAttributeDescription> SourceVertexLayout::attributes(const std::vector<SourceShaderInput>& inputs, uint32_t binding) const {
    std::vector<VkVertexInputAttributeDescription> result;
    std::set<uint32_t> locations;
    for (const auto& input : inputs) {
        require(locations.insert(input.location).second, "Duplicate Vulkan vertex attribute location");
        const auto found = std::find_if(elements.begin(), elements.end(), [&](const auto& element) {
            return element.semantic == input.semantic && element.index == input.index;
        });
        require(found != elements.end(), "Source mesh does not contain a required shader semantic");
        result.push_back({input.location, binding, found->format, found->offset});
    }
    return result;
}

namespace {
constexpr size_t MaximumMipBytes = 64 * 1024 * 1024;
uint16_t little16(const uint8_t* p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
uint32_t little32(const uint8_t* p) { return uint32_t(little16(p)) | uint32_t(little16(p + 2)) << 16; }
std::array<uint8_t, 4> rgb565(uint16_t v) {
    const unsigned r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    return {uint8_t((r << 3) | (r >> 2)), uint8_t((g << 2) | (g >> 4)), uint8_t((b << 3) | (b >> 2)), 255};
}
void decompressBlock(const uint8_t* source, int bc, std::array<std::array<uint8_t, 4>, 16>& output) {
    const auto* color = source + (bc == 1 ? 0 : 8);
    const auto c0 = little16(color), c1 = little16(color + 2);
    std::array<std::array<uint8_t, 4>, 4> palette {rgb565(c0), rgb565(c1), {}, {}};
    for (unsigned c = 0; c < 3; ++c) {
        if (bc == 1 && c0 <= c1) {
            palette[2][c] = uint8_t((unsigned(palette[0][c]) + palette[1][c]) / 2);
        } else {
            palette[2][c] = uint8_t((2 * unsigned(palette[0][c]) + palette[1][c]) / 3);
            palette[3][c] = uint8_t((unsigned(palette[0][c]) + 2 * palette[1][c]) / 3);
        }
    }
    palette[2][3] = 255; palette[3][3] = bc == 1 && c0 <= c1 ? 0 : 255;
    const auto selectors = little32(color + 4);
    for (unsigned p = 0; p < 16; ++p) output[p] = palette[(selectors >> (p * 2)) & 3];
    if (bc == 2) {
        for (unsigned p = 0; p < 16; ++p) output[p][3] = uint8_t(((source[p / 2] >> ((p & 1) * 4)) & 15) * 17);
    } else if (bc == 3) {
        uint8_t alpha[8] {source[0], source[1], 0, 0, 0, 0, 0, 0};
        const unsigned interpolated = alpha[0] > alpha[1] ? 6 : 4;
        for (unsigned i = 1; i <= interpolated; ++i)
            alpha[i + 1] = uint8_t(((interpolated + 1 - i) * alpha[0] + i * alpha[1]) / (interpolated + 1));
        if (interpolated == 4) { alpha[6] = 0; alpha[7] = 255; }
        uint64_t selectors = 0;
        for (unsigned i = 0; i < 6; ++i) selectors |= uint64_t(source[2 + i]) << (i * 8);
        for (unsigned p = 0; p < 16; ++p) output[p][3] = alpha[(selectors >> (p * 3)) & 7];
    }
}
ImageFormat linearAlias(ImageFormat format) {
    switch (format) {
    case IMAGE_FORMAT_LINEAR_RGBA8888: return IMAGE_FORMAT_RGBA8888;
    case IMAGE_FORMAT_LINEAR_BGRA8888: case IMAGE_FORMAT_LE_BGRA8888: return IMAGE_FORMAT_BGRA8888;
    case IMAGE_FORMAT_LINEAR_ABGR8888: return IMAGE_FORMAT_ABGR8888;
    case IMAGE_FORMAT_LINEAR_ARGB8888: return IMAGE_FORMAT_ARGB8888;
    case IMAGE_FORMAT_LINEAR_BGRX8888: case IMAGE_FORMAT_LE_BGRX8888: return IMAGE_FORMAT_BGRX8888;
    case IMAGE_FORMAT_LINEAR_RGB888: return IMAGE_FORMAT_RGB888;
    case IMAGE_FORMAT_LINEAR_BGR888: return IMAGE_FORMAT_BGR888;
    case IMAGE_FORMAT_LINEAR_I8: return IMAGE_FORMAT_I8;
    case IMAGE_FORMAT_LINEAR_A8: return IMAGE_FORMAT_A8;
    case IMAGE_FORMAT_LINEAR_RGBA16161616: return IMAGE_FORMAT_RGBA16161616;
    case IMAGE_FORMAT_LINEAR_DXT1: case IMAGE_FORMAT_DXT1_RUNTIME: return IMAGE_FORMAT_DXT1;
    case IMAGE_FORMAT_LINEAR_DXT3: case IMAGE_FORMAT_DXT3_RUNTIME: return IMAGE_FORMAT_DXT3;
    case IMAGE_FORMAT_LINEAR_DXT5: case IMAGE_FORMAT_DXT5_RUNTIME: return IMAGE_FORMAT_DXT5;
    default: return format;
    }
}
} // namespace

namespace {
struct TextureLayout : SourceTextureDataLayout {
    ImageFormat linearFormat = IMAGE_FORMAT_UNKNOWN;
    unsigned sourceTexel = 0;
    bool direct = false;
    int bc = 0;
};
TextureLayout textureLayout(ImageFormat input, uint32_t width, uint32_t height, size_t rowPitch, bool srgb) {
    require(width && height, "Source texture mip has no dimensions");
    const auto format = linearAlias(input);
    TextureLayout result;
    result.linearFormat = format;
    result.format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    unsigned sourceTexel = 0, outputTexel = 4;
    bool direct = false;
    int bc = 0;
    switch (format) {
    case IMAGE_FORMAT_DXT1: case IMAGE_FORMAT_DXT1_ONEBITALPHA: bc = 1; break;
    case IMAGE_FORMAT_DXT3: bc = 2; break;
    case IMAGE_FORMAT_DXT5: bc = 3; break;
    case IMAGE_FORMAT_RGBA8888: sourceTexel = 4; direct = true; break;
    case IMAGE_FORMAT_BGRA8888: case IMAGE_FORMAT_ABGR8888: case IMAGE_FORMAT_ARGB8888:
    case IMAGE_FORMAT_BGRX8888: case IMAGE_FORMAT_RGBX8888: sourceTexel = 4; break;
    case IMAGE_FORMAT_RGB888: case IMAGE_FORMAT_BGR888:
    case IMAGE_FORMAT_RGB888_BLUESCREEN: case IMAGE_FORMAT_BGR888_BLUESCREEN: sourceTexel = 3; break;
    case IMAGE_FORMAT_I8: case IMAGE_FORMAT_A8: sourceTexel = 1; break;
    case IMAGE_FORMAT_IA88: sourceTexel = 2; break;
    case IMAGE_FORMAT_R16F: sourceTexel = 2; result.format = VK_FORMAT_R16_SFLOAT; direct = true; break;
    case IMAGE_FORMAT_R32F: sourceTexel = 4; result.format = VK_FORMAT_R32_SFLOAT; direct = true; break;
    case IMAGE_FORMAT_RG1616F: sourceTexel = 4; result.format = VK_FORMAT_R16G16_SFLOAT; direct = true; break;
    case IMAGE_FORMAT_RG3232F: sourceTexel = 8; result.format = VK_FORMAT_R32G32_SFLOAT; direct = true; break;
    case IMAGE_FORMAT_RGBA16161616F: sourceTexel = 8; result.format = VK_FORMAT_R16G16B16A16_SFLOAT; direct = true; break;
    case IMAGE_FORMAT_RGBA16161616: sourceTexel = 8; result.format = VK_FORMAT_R16G16B16A16_UNORM; direct = true; break;
    case IMAGE_FORMAT_RGBA32323232F: sourceTexel = 16; result.format = VK_FORMAT_R32G32B32A32_SFLOAT; direct = true; break;
    case IMAGE_FORMAT_UV88: sourceTexel = 2; result.format = VK_FORMAT_R8G8_SNORM; direct = true; break;
    case IMAGE_FORMAT_UVWQ8888: sourceTexel = 4; result.format = VK_FORMAT_R8G8B8A8_SNORM; direct = true; break;
    default: throw std::invalid_argument("Source texture format requires a palette, packed-format converter, depth API or unsupported codec: " + std::to_string(int(input)));
    }
    if (direct && format != IMAGE_FORMAT_RGBA8888) {
        require(!srgb, "Source float/signed/16-bit textures cannot use an sRGB image view");
        outputTexel = sourceTexel;
    }
    const uint64_t texels = uint64_t(width) * height;
    require(texels <= MaximumMipBytes / outputTexel, "Converted Source mip exceeds the 64 MiB upload budget");
    const size_t outputBytes = size_t(texels) * outputTexel;
    const size_t rows = bc ? (uint64_t(height) + 3) / 4 : height;
    const uint64_t rowBytes64 = bc ? ((uint64_t(width) + 3) / 4) * (bc == 1 ? 8 : 16) : uint64_t(width) * sourceTexel;
    require(rowBytes64 <= SIZE_MAX, "Source texture row size overflow");
    const auto rowBytes = size_t(rowBytes64);
    if (!rowPitch) rowPitch = rowBytes;
    require(rowPitch >= rowBytes && rows, "Source texture pitch is shorter than a row");
    require(rows == 1 || rowPitch <= (SIZE_MAX - rowBytes) / (rows - 1), "Source texture range overflow");
    result.rowBytes = rowBytes; result.rowPitch = rowPitch; result.rows = rows;
    result.sourceBytes = (rows - 1) * rowPitch + rowBytes;
    result.outputBytes = outputBytes; result.outputTexelBytes = outputTexel;
    result.sourceTexel = sourceTexel; result.direct = direct; result.bc = bc;
    return result;
}
} // namespace

SourceTextureDataLayout sourceTextureDataLayout(ImageFormat input, uint32_t width, uint32_t height, size_t rowPitch, bool srgb) {
    return textureLayout(input, width, height, rowPitch, srgb);
}
PreparedSourceTexture prepareSourceTexture(ImageFormat input, uint32_t width, uint32_t height,
        const void* source, size_t bytes, size_t rowPitch, bool srgb) {
    require(source, "Source texture mip has no data");
    const auto layout = textureLayout(input, width, height, rowPitch, srgb);
    require(bytes >= layout.sourceBytes, "Source texture mip data is truncated");
    PreparedSourceTexture result;
    result.description.width = width; result.description.height = height;
    result.description.format = layout.format;
    const auto format = layout.linearFormat;
    const auto sourceTexel = layout.sourceTexel, outputTexel = layout.outputTexelBytes;
    const auto rows = layout.rows, outputBytes = layout.outputBytes;
    const auto direct = layout.direct;
    const auto bc = layout.bc;
    rowPitch = layout.rowPitch;
    result.pixels.resize(size_t(outputBytes));
    const auto* data = static_cast<const uint8_t*>(source);
    if (bc) {
        const size_t columns = (uint64_t(width) + 3) / 4;
        for (size_t by = 0; by < rows; ++by) for (size_t bx = 0; bx < columns; ++bx) {
            std::array<std::array<uint8_t, 4>, 16> block;
            decompressBlock(data + by * rowPitch + bx * (bc == 1 ? 8 : 16), bc, block);
            for (uint32_t y = 0; y < 4 && by * 4 + y < height; ++y)
                for (uint32_t x = 0; x < 4 && bx * 4 + x < width; ++x)
                    std::copy(block[y * 4 + x].begin(), block[y * 4 + x].end(), result.pixels.begin() + ((by * 4 + y) * width + bx * 4 + x) * 4);
        }
    } else for (uint32_t y = 0; y < height; ++y) {
        const auto* row = data + size_t(y) * rowPitch;
        auto* destination = result.pixels.data() + size_t(y) * width * outputTexel;
        if (direct) { std::memcpy(destination, row, size_t(width) * sourceTexel); continue; }
        for (uint32_t x = 0; x < width; ++x) {
            const auto* pixel = row + size_t(x) * sourceTexel;
            auto* out = destination + size_t(x) * 4;
            out[3] = 255;
            switch (format) {
            case IMAGE_FORMAT_BGRA8888: out[3] = pixel[3]; [[fallthrough]];
            case IMAGE_FORMAT_BGRX8888: case IMAGE_FORMAT_BGR888: case IMAGE_FORMAT_BGR888_BLUESCREEN:
                out[0] = pixel[2]; out[1] = pixel[1]; out[2] = pixel[0]; break;
            case IMAGE_FORMAT_RGBX8888: case IMAGE_FORMAT_RGB888: case IMAGE_FORMAT_RGB888_BLUESCREEN:
                std::copy_n(pixel,3,out); break;
            case IMAGE_FORMAT_ABGR8888: out[0] = pixel[3]; out[1] = pixel[2]; out[2] = pixel[1]; out[3] = pixel[0]; break;
            case IMAGE_FORMAT_ARGB8888: out[0] = pixel[1]; out[1] = pixel[2]; out[2] = pixel[3]; out[3] = pixel[0]; break;
            case IMAGE_FORMAT_IA88: out[3] = pixel[1]; [[fallthrough]];
            case IMAGE_FORMAT_I8: out[0] = out[1] = out[2] = pixel[0]; break;
            case IMAGE_FORMAT_A8: out[0] = out[1] = out[2] = 255; out[3] = pixel[0]; break;
            default: throw std::logic_error("Missing Source texture conversion");
            }
            if ((format == IMAGE_FORMAT_RGB888_BLUESCREEN || format == IMAGE_FORMAT_BGR888_BLUESCREEN) &&
                out[0] == 0 && out[1] == 0 && out[2] == 255) out[3] = 0;
        }
    }
    return result;
}

namespace {
template<size_t Floats> struct alignas(16) Registers {
    std::array<std::array<float, 4>, Floats> floating {};
    std::array<std::array<int32_t, 4>, SourceIntegerRegisters> integer {};
    std::array<uint32_t, SourceBooleanRegisters> boolean {};
};
static_assert(sizeof(Registers<256>) == SourceVertexRegisterBytes && sizeof(Registers<224>) == SourcePixelRegisterBytes);
template<class T> void writeRegisters(T* destination, size_t capacity, int first, const T* data, int count, uint64_t& revision) {
    require(first >= 0 && count > 0 && size_t(first) <= capacity && size_t(count) <= capacity - size_t(first) && data, "Source constant register range is invalid");
    if (std::memcmp(destination + first, data, size_t(count) * sizeof(T))) {
        std::memcpy(destination + first, data, size_t(count) * sizeof(T));
        ++revision;
    }
}
size_t stageIndex(SourceShaderStage stage) {
    require(stage == SourceShaderStage::Vertex || stage == SourceShaderStage::Pixel, "Invalid Source shader stage");
    return stage == SourceShaderStage::Vertex ? 0 : 1;
}
} // namespace

struct SourceConstants::Impl {
    Context& context;
    FrameArena& arena;
    Registers<SourceVertexFloatRegisters> vertex;
    Registers<SourcePixelFloatRegisters> pixel;
    struct Cached { uint64_t revision = 1, uploadedRevision = 0, serial = 0; BufferSlice slice; };
    std::array<Cached, 2> cached;
    uint64_t uploads = 0;
    Impl(Context& owner, FrameArena& dynamic) : context(owner), arena(dynamic) {}
};
SourceConstants::SourceConstants(Context& context, FrameArena& arena) : impl_(std::make_unique<Impl>(context, arena)) {}
SourceConstants::~SourceConstants() = default;
void SourceConstants::floats(SourceShaderStage stage, int first, const float* values, int vectors) {
    auto& constants = *impl_;
    const auto index = stageIndex(stage);
    auto* target = index == 0 ? constants.vertex.floating.data() : constants.pixel.floating.data();
    const auto capacity = index == 0 ? SourceVertexFloatRegisters : SourcePixelFloatRegisters;
    static_assert(sizeof(*target) == 4 * sizeof(float));
    require(first >= 0 && vectors > 0 && uint32_t(first) <= capacity && uint32_t(vectors) <= capacity - uint32_t(first) && values,
            "Source float constant register range is invalid");
    const auto bytes = size_t(vectors) * sizeof(*target);
    if (std::memcmp(target + first, values, bytes)) { std::memcpy(target + first, values, bytes); ++constants.cached[index].revision; }
}
void SourceConstants::integers(SourceShaderStage stage, int first, const int32_t* values, int vectors) {
    auto& constants = *impl_;
    const auto index = stageIndex(stage);
    auto* target = index == 0 ? constants.vertex.integer.data() : constants.pixel.integer.data();
    require(first >= 0 && vectors > 0 && first <= int(SourceIntegerRegisters) && vectors <= int(SourceIntegerRegisters) - first && values,
            "Source integer constant register range is invalid");
    const auto bytes = size_t(vectors) * sizeof(*target);
    if (std::memcmp(target + first, values, bytes)) { std::memcpy(target + first, values, bytes); ++constants.cached[index].revision; }
}
void SourceConstants::booleans(SourceShaderStage stage, int first, const int32_t* values, int count) {
    auto& constants = *impl_;
    const auto index = stageIndex(stage);
    require(first >= 0 && count > 0 && first <= int(SourceBooleanRegisters) && count <= int(SourceBooleanRegisters) - first && values,
        "Source boolean constant register range is invalid");
    std::array<uint32_t, SourceBooleanRegisters> normalized {};
    for (int i = 0; i < count; ++i) normalized[size_t(i)] = values[i] != 0;
    auto& target = index == 0 ? constants.vertex.boolean : constants.pixel.boolean;
    writeRegisters(target.data(), target.size(), first, normalized.data(), count, constants.cached[index].revision);
}
uint64_t SourceConstants::revision(SourceShaderStage stage) const { return impl_->cached[stageIndex(stage)].revision; }
uint64_t SourceConstants::uploads() const { return impl_->uploads; }
BufferSlice SourceConstants::snapshot(SourceShaderStage stage, const Frame& frame) {
    auto& constants = *impl_;
    const auto index = stageIndex(stage);
    const auto serial = detail::Access::serial(constants.context, frame);
    auto& cached = constants.cached[index];
    if (cached.serial != serial || cached.uploadedRevision != cached.revision) {
        const void* data = index == 0 ? static_cast<void*>(&constants.vertex) : static_cast<void*>(&constants.pixel);
        const auto bytes = index == 0 ? SourceVertexRegisterBytes : SourcePixelRegisterBytes;
        cached.slice = constants.arena.write(frame, data, bytes);
        cached.serial = serial; cached.uploadedRevision = cached.revision;
        ++constants.uploads;
    }
    return cached.slice;
}
} // namespace sourcevk
