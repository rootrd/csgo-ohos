#pragma once

#include "vulkan_graphics.h"
#include "vulkan_source.h"

class IShaderShadow;

namespace sourcevk {
struct SourceShaderSelection {
    std::string name;
    uint32_t staticIndex = 0;
};

// Shared with shaders/source_alpha.hlsl. Append to a material's push constants
// or uniform block, and call SourceAlphaTest after computing its final alpha.
struct alignas(16) SourceAlphaState {
    uint32_t enabled = 0, compare = VK_COMPARE_OP_GREATER_OR_EQUAL;
    float reference = 178.0f / 255.0f; // Source truncates 0.7 * 255 to 178.
    uint32_t reserved = 0;
};
static_assert(sizeof(SourceAlphaState) == 16);

struct SourceMaterialState {
    SourceShaderSelection vertex, pixel;
    uint64_t vertexUsage = 0;
    bool depthTest = true, depthWrite = true, cull = true;
    bool colorWrite = true, alphaWrite = false, srgbWrite = false;
    bool blend = false, forceOpaque = false, separateAlpha = false;
    bool depthBias = false, alphaToCoverage = false;
    VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;
    uint32_t fogMode = 0;
    VkCompareOp depthCompare = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkBlendFactor sourceColor = VK_BLEND_FACTOR_ZERO, destinationColor = VK_BLEND_FACTOR_ZERO;
    VkBlendFactor sourceAlpha = VK_BLEND_FACTOR_ZERO, destinationAlpha = VK_BLEND_FACTOR_ZERO;
    VkBlendOp colorBlend = VK_BLEND_OP_ADD, alphaBlend = VK_BLEND_OP_ADD;
    uint32_t textures = 0, vertexTextures = 0, srgbReads = 0;
    SourceAlphaState alpha;
};

class SourceSnapshot {
public:
    const SourceMaterialState& state() const;
    bool translucent() const;
    bool operator==(const SourceSnapshot& other) const { return state_ == other.state_; }
    explicit operator bool() const { return bool(state_); }
    // The actual mesh may include skinning data or padding beyond shader usage.
    void validateVertexFormat(uint64_t format) const;
    // Validate each enabled stage before writing its descriptor. Image views on
    // this path have fixed sRGB/linear interpretation; mismatches are errors.
    void validateTexture(SourceShaderStage stage, uint32_t sampler, const Image& image, bool dynamicColorSpace = false) const;
private:
    friend class SourceShadow;
    friend class SourceShaderLibrary;
    void applyPipelineState(GraphicsPipelineDescription& description) const;
    std::shared_ptr<const SourceMaterialState> state_;
};

// Implements the real ShaderShadow010 vtable behind a Source-header boundary.
// Capture is immutable and interned; the table is bounded for a map/material
// load. clearSnapshots() releases table ownership, not outstanding snapshots.
class SourceShadow {
public:
    explicit SourceShadow(float lightMapScale = 1, size_t maximumSnapshots = 65536);
    ~SourceShadow();
    SourceShadow(const SourceShadow&) = delete;
    SourceShadow& operator=(const SourceShadow&) = delete;
    IShaderShadow& interface();
    SourceSnapshot snapshot();
    size_t snapshotCount() const;
    void clearSnapshots();
    void setLightMapScale(float scale);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Loads the offline compiler's source-variants.tsv. Names and both combo indices
// are exact keys: a missing variant fails instead of silently selecting index 0.
// Shader modules are loaded once per variant, independent of snapshot identity.
class SourceShaderLibrary {
public:
    SourceShaderLibrary(GraphicsDevice& graphics, const std::string& directory);
    ~SourceShaderLibrary();
    SourceShaderLibrary(const SourceShaderLibrary&) = delete;
    SourceShaderLibrary& operator=(const SourceShaderLibrary&) = delete;
    Shader shader(SourceShaderStage stage, const SourceShaderSelection& selection, uint32_t dynamicIndex = 0);
    GraphicsPipeline pipeline(const SourceSnapshot& snapshot, GraphicsPipelineDescription description,
                              uint32_t vertexDynamicIndex = 0, uint32_t pixelDynamicIndex = 0);
    size_t variantCount() const;
    size_t loadedShaders() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace sourcevk
