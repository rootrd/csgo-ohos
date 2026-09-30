#pragma once

#include "vulkan_context.h"

#include <vector>

namespace sourcevk {
namespace detail {
struct ShaderAllocation;
struct SamplerAllocation;
struct SetLayoutAllocation;
struct PipelineLayoutAllocation;
struct PipelineAllocation;
struct DescriptorAllocation;
}

struct ShaderBinding {
    uint32_t set = 0, binding = 0, count = 1;
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
};
class Shader {
public:
    VkShaderModule handle() const;
    VkShaderStageFlagBits stage() const;
    const std::vector<ShaderBinding>& bindings() const;
    explicit operator bool() const { return bool(allocation_); }
private:
    friend class GraphicsDevice;
    std::shared_ptr<detail::ShaderAllocation> allocation_;
};

struct SamplerDescription {
    VkFilter minFilter = VK_FILTER_LINEAR, magFilter = VK_FILTER_LINEAR;
    VkSamplerMipmapMode mipFilter = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    VkSamplerAddressMode addressU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkSamplerAddressMode addressV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkSamplerAddressMode addressW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    float minLod = 0, maxLod = VK_LOD_CLAMP_NONE;
    float maxAnisotropy = 1; // Values above one require samplerAnisotropy.
};
class Sampler {
public:
    VkSampler handle() const;
    const SamplerDescription& description() const;
    explicit operator bool() const { return bool(allocation_); }
private:
    friend class GraphicsDevice;
    friend class DescriptorArena;
    std::shared_ptr<detail::SamplerAllocation> allocation_;
};

struct DescriptorBinding {
    uint32_t binding = 0;
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uint32_t count = 1;
    VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
};
class DescriptorLayout {
public:
    VkDescriptorSetLayout handle() const;
    explicit operator bool() const { return bool(allocation_); }
private:
    friend class GraphicsDevice;
    friend class DescriptorArena;
    std::shared_ptr<detail::SetLayoutAllocation> allocation_;
};
class PipelineLayout {
public:
    VkPipelineLayout handle() const;
    explicit operator bool() const { return bool(allocation_); }
private:
    friend class GraphicsDevice;
    friend class DrawEncoder;
    std::shared_ptr<detail::PipelineLayoutAllocation> allocation_;
};

// One color attachment, optional depth, one sample. Topology, vertex/instance
// streams, blend/depth/cull state form the immutable pipeline key. Viewport and
// scissor are dynamic. Shader variants are separate Shader objects.
struct GraphicsPipelineDescription {
    Shader vertex, fragment;
    PipelineLayout layout;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkFormat colorFormat = VK_FORMAT_UNDEFINED, depthFormat = VK_FORMAT_UNDEFINED;
    std::vector<VkVertexInputBindingDescription> vertexBindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkCullModeFlags cull = VK_CULL_MODE_NONE;
    VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;
    bool depthBias = false, alphaToCoverage = false, stencilTest = false;
    float depthBiasConstant = 0, depthBiasSlope = 0;
    VkStencilOpState stencil {};
    bool overrideDepth=false, overrideDepthCompare=false, overrideCull=false, overrideAlpha=false, overrideColor=false;
    bool depthTest = false, depthWrite = false, blend = false;
    VkCompareOp depthCompare = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkBlendFactor sourceColor = VK_BLEND_FACTOR_SRC_ALPHA, destinationColor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    VkBlendFactor sourceAlpha = VK_BLEND_FACTOR_ONE, destinationAlpha = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    VkBlendOp colorBlend = VK_BLEND_OP_ADD, alphaBlend = VK_BLEND_OP_ADD;
    VkColorComponentFlags colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
};
class GraphicsPipeline {
public:
    VkPipeline handle() const;
    explicit operator bool() const { return bool(allocation_); }
private:
    friend class GraphicsDevice;
    friend class DrawEncoder;
    std::shared_ptr<detail::PipelineAllocation> allocation_;
};
struct GraphicsStatistics {
    uint64_t pipelineCreations = 0, pipelineHits = 0, loadedCacheBytes = 0;
    uint64_t samplerCreations = 0, samplerHits = 0;
};

class GraphicsDevice {
public:
    explicit GraphicsDevice(Context& context, const std::string& pipelineCachePath = {});
    ~GraphicsDevice();
    GraphicsDevice(const GraphicsDevice&) = delete;
    GraphicsDevice& operator=(const GraphicsDevice&) = delete;
    // Loads SPIR-V <= 1.3, checks its entry point/stage, reflects ordinary
    // descriptor bindings and rejects runtime descriptor arrays on the base path.
    Shader loadShader(const std::string& path, VkShaderStageFlagBits stage, const std::string& entry = "main");
    // IShaderBuffer bridge. Empty entry selects the sole entry for this stage;
    // ambiguous/missing entries and legacy D3D bytecode are rejected.
    Shader createShader(const void* spirv, size_t bytes, VkShaderStageFlagBits stage, const std::string& entry = {});
    Sampler sampler(const SamplerDescription& description = {});
    DescriptorLayout descriptorLayout(std::vector<DescriptorBinding> bindings);
    PipelineLayout pipelineLayout(const std::vector<DescriptorLayout>& sets, uint32_t pushConstantBytes = 0);
    GraphicsPipeline pipeline(const GraphicsPipelineDescription& description);
    const GraphicsStatistics& statistics() const;
    // Driver/cache UUID, driver version and a corruption checksum gate loading.
    // Save atomically after a loading/warmup batch, never once per draw.
    void savePipelineCache(const std::string& path) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Exactly one descriptor element per write; repeat binding with arrayElement
// for fixed-size arrays. Every element must be initialized. Dynamic buffers use
// the same type as in their DescriptorBinding and receive offsets when bound.
struct DescriptorWrite {
    uint32_t binding = 0, arrayElement = 0;
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    BufferSlice buffer;
    Image image;
    Sampler sampler;
    VkImageLayout imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
};
class DescriptorSet {
public:
    VkDescriptorSet handle() const;
    explicit operator bool() const { return bool(allocation_); }
private:
    friend class DescriptorArena;
    friend class DrawEncoder;
    std::shared_ptr<detail::DescriptorAllocation> allocation_;
};
class DescriptorArena {
public:
    explicit DescriptorArena(Context& context, uint32_t setsPerPool = 128, uint32_t maximumPoolsPerFrame = 16);
    ~DescriptorArena();
    DescriptorArena(const DescriptorArena&) = delete;
    DescriptorArena& operator=(const DescriptorArena&) = delete;
    DescriptorSet allocate(const Frame& frame, const DescriptorLayout& layout, const std::vector<DescriptorWrite>& writes);
    uint32_t poolCount() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Records into an already open compatible render pass. The first constructor
// targets Frame's present pass; the second also supports an offscreen pass.
// No draw reordering: transparent/UI order stays with the caller.
class DrawEncoder {
public:
    DrawEncoder(Context& context, const Frame& frame);
    DrawEncoder(Context& context, const Frame& frame, VkExtent2D extent,
                VkFormat colorFormat, VkFormat depthFormat = VK_FORMAT_UNDEFINED);
    ~DrawEncoder();
    DrawEncoder(const DrawEncoder&) = delete;
    DrawEncoder& operator=(const DrawEncoder&) = delete;
    void pipeline(const GraphicsPipeline& pipeline);
    void descriptors(uint32_t set, const DescriptorSet& descriptors, const std::vector<uint32_t>& dynamicOffsets = {});
    void vertexBuffer(uint32_t binding, const BufferSlice& buffer);
    void indexBuffer(const BufferSlice& buffer, VkIndexType type);
    void viewport(VkViewport viewport);
    void scissor(VkRect2D rectangle);
    void pushConstants(const void* data, uint32_t bytes, uint32_t offset = 0);
    void draw(uint32_t vertices, uint32_t instances = 1, uint32_t firstVertex = 0, uint32_t firstInstance = 0);
    void drawIndexed(uint32_t indices, uint32_t instances = 1, uint32_t firstIndex = 0,
                     int32_t vertexOffset = 0, uint32_t firstInstance = 0);
    uint64_t drawCalls() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace sourcevk
