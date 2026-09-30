#pragma once
#include "vulkan_graphics.h"
#include "vulkan_internal.h"
#include <map>

namespace sourcevk::detail {
constexpr VkShaderStageFlags GraphicsStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
struct ShaderAllocation {
    std::shared_ptr<DeviceState> state;
    VkShaderModule module = VK_NULL_HANDLE;
    VkShaderStageFlagBits stage;
    std::string entry;
    std::vector<ShaderBinding> bindings;
    bool pushConstants = false;
    ~ShaderAllocation() { if (module) state->functions.vkDestroyShaderModule(state->device, module, nullptr); }
};
struct SamplerAllocation {
    std::shared_ptr<DeviceState> state;
    VkSampler sampler = VK_NULL_HANDLE;
    SamplerDescription description;
    ~SamplerAllocation() { if (sampler) state->functions.vkDestroySampler(state->device, sampler, nullptr); }
};
struct SetLayoutAllocation {
    std::shared_ptr<DeviceState> state;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    std::vector<DescriptorBinding> bindings;
    ~SetLayoutAllocation() { if (layout) state->functions.vkDestroyDescriptorSetLayout(state->device, layout, nullptr); }
};
struct PipelineLayoutAllocation {
    std::shared_ptr<DeviceState> state;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    std::vector<std::shared_ptr<SetLayoutAllocation>> sets;
    uint32_t pushConstantBytes = 0;
    ~PipelineLayoutAllocation() { if (layout) state->functions.vkDestroyPipelineLayout(state->device, layout, nullptr); }
};
struct PipelineAllocation {
    std::shared_ptr<DeviceState> state;
    VkPipeline pipeline = VK_NULL_HANDLE;
    std::shared_ptr<PipelineLayoutAllocation> layout;
    std::shared_ptr<ShaderAllocation> vertex, fragment;
    std::vector<VkVertexInputBindingDescription> vertexBindings;
    VkFormat colorFormat = VK_FORMAT_UNDEFINED, depthFormat = VK_FORMAT_UNDEFINED;
    ~PipelineAllocation() { if (pipeline) state->functions.vkDestroyPipeline(state->device, pipeline, nullptr); }
};
struct DescriptorPoolPage {
    std::shared_ptr<DeviceState> state;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    uint64_t epoch = 0;
    uint32_t maximumSets = 0, remainingSets = 0;
    std::map<VkDescriptorType, uint32_t> capacity, remaining;
    ~DescriptorPoolPage() { if (pool) state->functions.vkDestroyDescriptorPool(state->device, pool, nullptr); }
};
struct DynamicBufferBinding {
    VkDeviceSize offset, range, capacity, alignment;
};
struct DescriptorAllocation {
    std::shared_ptr<DescriptorPoolPage> page;
    std::shared_ptr<SetLayoutAllocation> layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    uint64_t epoch = 0, frameSerial = 0;
    std::vector<Buffer> buffers;
    std::vector<Image> images;
    std::vector<Sampler> samplers;
    std::vector<DynamicBufferBinding> dynamicBuffers;
};

inline bool bufferDescriptor(VkDescriptorType type) {
    return type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
        type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
}
inline bool dynamicDescriptor(VkDescriptorType type) {
    return type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
}
inline bool uniformDescriptor(VkDescriptorType type) {
    return type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
}
inline bool supportedDescriptor(VkDescriptorType type) {
    return bufferDescriptor(type) || type == VK_DESCRIPTOR_TYPE_SAMPLER || type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
        type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
}
inline void require(bool value, const char* message) { if (!value) throw std::invalid_argument(message); }
inline void validateSlice(const BufferSlice& value, VkBufferUsageFlags usage, const std::shared_ptr<DeviceState>& state) {
    require(Access::state(value.buffer) == state && (value.buffer.usage() & usage) == usage,
            "Buffer device or usage does not match its GPU binding");
    require(value.size && value.offset <= value.buffer.size() && value.size <= value.buffer.size() - value.offset,
            "GPU buffer slice is empty or out of bounds");
}
} // namespace sourcevk::detail
