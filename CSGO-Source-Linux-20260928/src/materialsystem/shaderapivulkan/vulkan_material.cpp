#include "vulkan_material.h"
#include "vulkan_internal.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <tuple>

#include "shaderapi/ishadershadow.h"
#include "materialsystem/imesh.h"

namespace sourcevk {
namespace {
// Legacy platform.h defines INT32_MAX as LONG_MAX on desktop POSIX/LP64.
constexpr uint32_t MaximumComboIndex = 0x7fffffffu;
void require(bool value, const char* message) { if (!value) throw std::invalid_argument(message); }
bool identifier(const std::string& value) {
    if (value.empty() || value.size() > 96) return false;
    const auto letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
    if (!letter(value.front())) return false;
    return std::all_of(value.begin(), value.end(), [&](char c) { return letter(c) || (c >= '0' && c <= '9'); });
}
std::string shaderName(std::string value) {
    require(identifier(value), "Source shader name must be a bare shader identifier");
    for (auto& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return value;
}
SourceShaderSelection selection(const char* name, int index) {
    require(index >= 0, "Negative Source static shader index");
    return {name ? shaderName(name) : std::string(), uint32_t(index)};
}
VkCompareOp depthFunction(ShaderDepthFunc_t value) {
    require(value >= SHADER_DEPTHFUNC_NEVER && value <= SHADER_DEPTHFUNC_ALWAYS, "Invalid Source depth function");
    const VkCompareOp functions[] = {VK_COMPARE_OP_NEVER, VK_COMPARE_OP_LESS, VK_COMPARE_OP_EQUAL, VK_COMPARE_OP_LESS_OR_EQUAL,
        VK_COMPARE_OP_GREATER, VK_COMPARE_OP_NOT_EQUAL, VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS};
    return functions[value];
}
VkBlendFactor blendFactor(ShaderBlendFactor_t value) {
    require(value >= SHADER_BLEND_ZERO && value <= SHADER_BLEND_ONE_MINUS_SRC_COLOR, "Invalid Source blend factor");
    const VkBlendFactor factors[] = {VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_DST_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
        VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR};
    return factors[value];
}
VkBlendOp blendOperation(ShaderBlendOp_t value) {
    require(value >= SHADER_BLEND_OP_ADD && value <= SHADER_BLEND_OP_MAX, "Invalid Source blend operation");
    const VkBlendOp operations[] = {VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT, VK_BLEND_OP_MIN, VK_BLEND_OP_MAX};
    return operations[value];
}
void setBit(uint32_t& mask, int index, int maximum, bool enabled) {
    require(index >= 0 && index < maximum, "Source texture sampler is out of range");
    const auto bit = uint32_t(1) << index;
    mask = enabled ? mask | bit : mask & ~bit;
}
bool srgb(VkFormat format) {
    return detail::isSRGB(format);
}
bool colorFormat(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R32G32B32A32_SFLOAT:
    case VK_FORMAT_R16G16B16A16_UNORM: return true;
    default: return false;
    }
}
uint32_t floatComponents(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R32_SFLOAT: return 1;
    case VK_FORMAT_R32G32_SFLOAT: return 2;
    case VK_FORMAT_R32G32B32_SFLOAT: return 3;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return 4;
    default: return 0;
    }
}
// Serialize fields, not struct padding. Inactive state is canonicalized before
// this key is made, so different setter histories share the same snapshot.
std::string snapshotKey(const SourceMaterialState& s) {
    std::string key;
    auto add = [&](uint64_t value) { for (unsigned i = 0; i < 8; ++i) key.push_back(char(value >> (i * 8))); };
    auto name = [&](const SourceShaderSelection& shader) { add(shader.name.size()); key += shader.name; add(shader.staticIndex); };
    name(s.vertex); name(s.pixel); add(s.vertexUsage);
    for (auto value : {s.depthTest, s.depthWrite, s.cull, s.colorWrite, s.alphaWrite, s.srgbWrite,
                      s.blend, s.forceOpaque, s.separateAlpha,s.depthBias,s.alphaToCoverage}) add(value);
    add(uint32_t(s.polygonMode));add(s.fogMode);
    for (auto value : {uint32_t(s.depthCompare), uint32_t(s.sourceColor), uint32_t(s.destinationColor), uint32_t(s.sourceAlpha),
            uint32_t(s.destinationAlpha), uint32_t(s.colorBlend), uint32_t(s.alphaBlend), s.textures, s.vertexTextures, s.srgbReads,
            s.alpha.enabled, s.alpha.compare}) add(value);
    uint32_t reference;
    std::memcpy(&reference, &s.alpha.reference, sizeof(reference));
    add(reference);
    return key;
}
SourceMaterialState canonical(SourceMaterialState s) {
    if (!s.depthTest) s.depthCompare = VK_COMPARE_OP_ALWAYS;
    if (!s.blend) {
        s.forceOpaque = s.separateAlpha = false;
        s.sourceColor = s.sourceAlpha = VK_BLEND_FACTOR_ONE;
        s.destinationColor = s.destinationAlpha = VK_BLEND_FACTOR_ZERO;
        s.colorBlend = s.alphaBlend = VK_BLEND_OP_ADD;
    } else if (!s.separateAlpha) {
        s.sourceAlpha = s.sourceColor; s.destinationAlpha = s.destinationColor; s.alphaBlend = s.colorBlend;
    }
    if (!s.alpha.enabled) s.alpha = {};
    s.srgbReads &= s.textures;
    return s;
}
} // namespace

struct SourceShadow::Impl final : IShaderShadow {
    SourceMaterialState current;
    float lightMapScale;
    size_t maximumSnapshots;
    std::map<std::string, std::shared_ptr<const SourceMaterialState>> snapshots;
    Impl(float scale, size_t maximum) : lightMapScale(scale), maximumSnapshots(maximum) {
        require(std::isfinite(scale) && scale > 0 && maximum > 0 && maximum <= 1024 * 1024, "Invalid Source shadow scale or snapshot budget");
    }
    void SetDefaultState() override { current = {}; }
    void DepthFunc(ShaderDepthFunc_t value) override { current.depthCompare = depthFunction(value); }
    void EnableDepthWrites(bool value) override { current.depthWrite = value; }
    void EnableDepthTest(bool value) override { current.depthTest = value; }
    void EnablePolyOffset(PolygonOffsetMode_t value) override {
        require(value>=SHADER_POLYOFFSET_DISABLE && value<=SHADER_POLYOFFSET_SHADOW_BIAS,"Invalid Source polygon offset mode");
        current.depthBias=value!=SHADER_POLYOFFSET_DISABLE;
    }
    void EnableColorWrites(bool value) override { current.colorWrite = value; }
    void EnableAlphaWrites(bool value) override { current.alphaWrite = value; }
    void EnableBlending(bool value) override { current.blend = value; current.forceOpaque = false; }
    void EnableBlendingForceOpaque(bool value) override { current.blend = value; current.forceOpaque = true; }
    void BlendFunc(ShaderBlendFactor_t source, ShaderBlendFactor_t destination) override {
        const auto src = blendFactor(source), dst = blendFactor(destination);
        current.sourceColor = src; current.destinationColor = dst;
    }
    void EnableBlendingSeparateAlpha(bool value) override { current.separateAlpha = value; }
    void BlendFuncSeparateAlpha(ShaderBlendFactor_t source, ShaderBlendFactor_t destination) override {
        const auto src = blendFactor(source), dst = blendFactor(destination);
        current.sourceAlpha = src; current.destinationAlpha = dst;
    }
    void BlendOp(ShaderBlendOp_t value) override { current.colorBlend = blendOperation(value); }
    void BlendOpSeparateAlpha(ShaderBlendOp_t value) override { current.alphaBlend = blendOperation(value); }
    void EnableAlphaTest(bool value) override { current.alpha.enabled = value; }
    void AlphaFunc(ShaderAlphaFunc_t value, float reference) override {
        require(value >= SHADER_ALPHAFUNC_NEVER && value <= SHADER_ALPHAFUNC_ALWAYS && std::isfinite(reference) && reference >= 0 && reference <= 1,
                "Invalid Source alpha-test function or reference");
        const VkCompareOp functions[] = {VK_COMPARE_OP_NEVER, VK_COMPARE_OP_LESS, VK_COMPARE_OP_EQUAL, VK_COMPARE_OP_LESS_OR_EQUAL,
            VK_COMPARE_OP_GREATER, VK_COMPARE_OP_NOT_EQUAL, VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS};
        current.alpha.compare = functions[value];
        current.alpha.reference = float(uint32_t(reference * 255)) / 255;
    }
    void PolyMode(ShaderPolyModeFace_t face, ShaderPolyMode_t mode) override {
        require(face >= SHADER_POLYMODEFACE_FRONT && face <= SHADER_POLYMODEFACE_FRONT_AND_BACK &&
            mode>=SHADER_POLYMODE_POINT && mode<=SHADER_POLYMODE_FILL,"Invalid Source polygon mode");
        current.polygonMode=mode==SHADER_POLYMODE_FILL?VK_POLYGON_MODE_FILL:mode==SHADER_POLYMODE_LINE?VK_POLYGON_MODE_LINE:VK_POLYGON_MODE_POINT;
    }
    void EnableCulling(bool value) override { current.cull = value; }
    void VertexShaderVertexFormat(unsigned flags, int count, int* dimensions, int userData) override {
        require(!(flags & ~((1u << (VERTEX_LAST_BIT + 1)) - 1)) && !(flags & VERTEX_BONE_INDEX),
            "Source shader usage cannot specify unknown flags or mesh bone indices");
        require(count >= 0 && count <= VERTEX_MAX_TEXTURE_COORDINATES && userData >= 0 && userData <= 4,
            "Invalid Source shader vertex usage");
        // Shader usage requests uncompressed attributes; the mesh independently
        // chooses padding/skinning. Do not add DX9's unrelated flex-stream padding.
        VertexFormat_t format = flags & ~(VERTEX_FORMAT_COMPRESSED | VERTEX_FORMAT_USE_EXACT_FORMAT);
        format |= VERTEX_USERDATA_SIZE(userData);
        for (int i = 0; i < count; ++i) {
            const int size = dimensions ? dimensions[i] : 2;
            require(size >= 0 && size <= 4, "Invalid Source shader texture-coordinate size");
            format |= VERTEX_TEXCOORD_SIZE(i, size);
        }
        sourceVertexLayout(format);
        current.vertexUsage = format;
    }
    void SetVertexShader(const char* name, int index) override { current.vertex = selection(name, index); }
    void SetPixelShader(const char* name, int index) override { current.pixel = selection(name, index); }
    void EnableSRGBWrite(bool value) override { current.srgbWrite = value; }
    void EnableSRGBRead(Sampler_t sampler, bool value) override { setBit(current.srgbReads, sampler, SHADER_SAMPLER_COUNT, value); }
    void EnableTexture(Sampler_t sampler, bool value) override { setBit(current.textures, sampler, SHADER_SAMPLER_COUNT, value); }
    void EnableVertexTexture(VertexTextureSampler_t sampler, bool value) override { setBit(current.vertexTextures, sampler, 4, value); }
    void FogMode(ShaderFogMode_t mode, bool vertexFog) override {
        require(mode>=SHADER_FOGMODE_DISABLED && mode<SHADER_FOGMODE_NUMFOGMODES,"Invalid Source fog mode");
        current.fogMode=uint32_t(mode);
    }
    void DisableFogGammaCorrection(bool disabled) override {
        require(!disabled, "Source fog gamma correction is not implemented");
    }
    void EnableAlphaToCoverage(bool value) override {
        current.alphaToCoverage=value;
    }
    float GetLightMapScaleFactor() const override { return lightMapScale; }
};

SourceShadow::SourceShadow(float scale, size_t maximum) : impl_(std::make_unique<Impl>(scale, maximum)) {}
SourceShadow::~SourceShadow() = default;
IShaderShadow& SourceShadow::interface() { return *impl_; }
SourceSnapshot SourceShadow::snapshot() {
    const auto& current = impl_->current;
    require(!current.vertex.name.empty() && !current.pixel.name.empty() && current.vertexUsage,
        "Source snapshot requires vertex/pixel shaders and vertex usage; fixed-function materials are unsupported");
    auto state = canonical(current);
    auto key = snapshotKey(state);
    auto found = impl_->snapshots.find(key);
    if (found == impl_->snapshots.end()) {
        if (impl_->snapshots.size() >= impl_->maximumSnapshots) throw std::length_error("Source snapshot budget exhausted");
        found = impl_->snapshots.emplace(std::move(key), std::make_shared<const SourceMaterialState>(std::move(state))).first;
    }
    SourceSnapshot result;
    result.state_ = found->second;
    return result;
}
size_t SourceShadow::snapshotCount() const { return impl_->snapshots.size(); }
void SourceShadow::clearSnapshots() { impl_->snapshots.clear(); }
void SourceShadow::setLightMapScale(float scale) {
    require(std::isfinite(scale) && scale>0,"Invalid Source lightmap scale");impl_->lightMapScale=scale;
}
const SourceMaterialState& SourceSnapshot::state() const {
    if (!state_) throw std::logic_error("Empty Source material snapshot");
    return *state_;
}
bool SourceSnapshot::translucent() const { return state().blend && !state().forceOpaque; }
void SourceSnapshot::validateVertexFormat(uint64_t value) const {
    const auto required = sourceVertexLayout(state().vertexUsage);
    const auto mesh = sourceVertexLayout(value);
    for (const auto& field : required.elements) {
        const auto found = std::find_if(mesh.elements.begin(), mesh.elements.end(), [&](const auto& actual) {
            return actual.semantic == field.semantic && actual.index == field.index;
        });
        require(found != mesh.elements.end(), "Source mesh lacks a material vertex semantic");
        require(found->format == field.format || (floatComponents(field.format) && floatComponents(found->format) >= floatComponents(field.format)),
                "Source mesh vertex field is narrower than material usage");
    }
}
void SourceSnapshot::validateTexture(SourceShaderStage stage, uint32_t sampler, const Image& image, bool dynamicColorSpace) const {
    require(stage == SourceShaderStage::Vertex || stage == SourceShaderStage::Pixel, "Invalid Source shader stage");
    const auto& s = state();
    const bool vertex = stage == SourceShaderStage::Vertex;
    require(sampler < (vertex ? 4u : 16u) && ((vertex ? s.vertexTextures : s.textures) & (1u << sampler)),
            "Source texture stage is not enabled by its material");
    require(image && image.view() && (image.description().usage & VK_IMAGE_USAGE_SAMPLED_BIT), "Source texture has no sampled image view");
    require(dynamicColorSpace || srgb(image.description().format) == (!vertex && bool(s.srgbReads & (1u << sampler))),
            "Source texture view does not match the material's sRGB-read state");
}
void SourceSnapshot::applyPipelineState(GraphicsPipelineDescription& d) const {
    const auto& s = state();
    require(colorFormat(d.colorFormat) && srgb(d.colorFormat) == s.srgbWrite, "Source sRGB-write state does not match its color attachment");
    if(!d.overrideDepth) {d.depthTest = s.depthTest; d.depthWrite = s.depthWrite;}
    if(!d.overrideDepthCompare)d.depthCompare = s.depthCompare;
    d.polygonMode=s.polygonMode;d.alphaToCoverage=s.alphaToCoverage;
    d.depthBias=s.depthBias;d.depthBiasConstant=s.depthBias?-1.f:0;d.depthBiasSlope=s.depthBias?-1.f:0;
    if(!d.overrideCull)d.cull = s.cull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    d.blend = s.blend;
    d.sourceColor = s.sourceColor; d.destinationColor = s.destinationColor; d.colorBlend = s.colorBlend;
    d.sourceAlpha = s.sourceAlpha; d.destinationAlpha = s.destinationAlpha; d.alphaBlend = s.alphaBlend;
    const auto rgb=VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    d.colorWriteMask = (d.overrideColor ? d.colorWriteMask & rgb : s.colorWrite ? rgb : 0) |
        (d.overrideAlpha ? d.colorWriteMask & VK_COLOR_COMPONENT_A_BIT : s.alphaWrite ? VK_COLOR_COMPONENT_A_BIT : 0);
}

struct SourceShaderLibrary::Impl {
    using Key = std::tuple<std::string, SourceShaderStage, uint32_t, uint32_t>;
    struct Variant { std::string file, entry; bool alphaTest = false; Shader shader; };
    GraphicsDevice& graphics;
    std::filesystem::path directory;
    std::map<Key, Variant> variants;
    size_t loaded = 0;
    Impl(GraphicsDevice& owner, const std::string& path) : graphics(owner), directory(path) {
        const auto index = directory / "source-variants.tsv";
        require(std::filesystem::file_size(index) <= 16 * 1024 * 1024, "Source shader index exceeds its size budget");
        std::ifstream input(index, std::ios::binary);
        std::string line;
        require(bool(std::getline(input, line)) && line == "SOURCEVK_VARIANTS\t1", "Unsupported Source shader index schema");
        auto number = [](const std::string& text) {
            uint32_t result = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
            require(!text.empty() && parsed.ec == std::errc() && parsed.ptr == text.data() + text.size() && result <= MaximumComboIndex,
                    "Invalid Source shader combo index");
            return result;
        };
        while (std::getline(input, line)) {
            std::vector<std::string> fields;
            size_t start = 0;
            for (;;) {
                const auto end = line.find('\t', start);
                fields.push_back(line.substr(start, end - start));
                if (end == std::string::npos) break;
                start = end + 1;
            }
            require(fields.size() == 7 && variants.size() < 65536, "Malformed or oversized Source shader index");
            require(fields[1] == "vertex" || fields[1] == "fragment", "Invalid Source shader stage in index");
            const auto stage = fields[1] == "vertex" ? SourceShaderStage::Vertex : SourceShaderStage::Pixel;
            const auto staticIndex = number(fields[2]), dynamicIndex = number(fields[3]);
            require(fields[4].size() > 4 && fields[4].substr(fields[4].size() - 4) == ".spv" &&
                identifier(fields[4].substr(0, fields[4].size() - 4)) && identifier(fields[5]), "Invalid Source shader file or entry point");
            require(fields[6] == "0" || (fields[6] == "1" && stage == SourceShaderStage::Pixel), "Invalid Source alpha-test shader contract");
            const auto inserted = variants.emplace(Key(shaderName(fields[0]), stage, staticIndex, dynamicIndex),
                Variant{fields[4], fields[5], fields[6] == "1", {}});
            require(inserted.second, "Duplicate Source shader combo in index");
        }
        require(input.eof() && !variants.empty(), "Cannot read Source shader index");
    }
    Variant& find(SourceShaderStage stage, const SourceShaderSelection& selection, uint32_t dynamicIndex) {
        require(stage == SourceShaderStage::Vertex || stage == SourceShaderStage::Pixel, "Invalid Source shader stage");
        require(selection.staticIndex <= MaximumComboIndex && dynamicIndex <= MaximumComboIndex, "Invalid Source shader index");
        const auto found = variants.find(Key(shaderName(selection.name), stage, selection.staticIndex, dynamicIndex));
        if (found == variants.end()) throw std::invalid_argument("Missing Vulkan Source shader variant: " + selection.name +
            " static=" + std::to_string(selection.staticIndex) + " dynamic=" + std::to_string(dynamicIndex));
        return found->second;
    }
    Shader load(Variant& variant, SourceShaderStage stage) {
        if (!variant.shader) {
            variant.shader = graphics.loadShader((directory / variant.file).string(),
                stage == SourceShaderStage::Vertex ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT, variant.entry);
            ++loaded;
        }
        return variant.shader;
    }
};
SourceShaderLibrary::SourceShaderLibrary(GraphicsDevice& graphics, const std::string& directory)
    : impl_(std::make_unique<Impl>(graphics, directory)) {}
SourceShaderLibrary::~SourceShaderLibrary() = default;
Shader SourceShaderLibrary::shader(SourceShaderStage stage, const SourceShaderSelection& selection, uint32_t dynamicIndex) {
    return impl_->load(impl_->find(stage, selection, dynamicIndex), stage);
}
GraphicsPipeline SourceShaderLibrary::pipeline(const SourceSnapshot& snapshot, GraphicsPipelineDescription description,
                                               uint32_t vertexDynamic, uint32_t pixelDynamic) {
    const auto& state = snapshot.state();
    snapshot.applyPipelineState(description);
    auto& vertex = impl_->find(SourceShaderStage::Vertex, state.vertex, vertexDynamic);
    auto& pixel = impl_->find(SourceShaderStage::Pixel, state.pixel, pixelDynamic);
    require(!state.alpha.enabled || pixel.alphaTest, "Source alpha test requires a shader variant implementing SourceAlphaTest");
    description.vertex = impl_->load(vertex, SourceShaderStage::Vertex);
    description.fragment = impl_->load(pixel, SourceShaderStage::Pixel);
    return impl_->graphics.pipeline(description);
}
size_t SourceShaderLibrary::variantCount() const { return impl_->variants.size(); }
size_t SourceShaderLibrary::loadedShaders() const { return impl_->loaded; }
} // namespace sourcevk
