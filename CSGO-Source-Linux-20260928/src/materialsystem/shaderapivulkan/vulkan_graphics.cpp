#include "vulkan_graphics_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <tuple>
#include <unordered_map>

namespace sourcevk {
namespace {
using detail::require;
constexpr size_t MaximumShaderBytes = 16 * 1024 * 1024, MaximumCacheBytes = 64 * 1024 * 1024;
constexpr size_t CacheHeaderBytes = 56;
std::vector<uint8_t> readFile(const std::string& path, size_t maximum) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Cannot open " + path);
    const auto size = file.tellg();
    require(size >= 0 && uint64_t(size) <= maximum, "GPU file exceeds its size limit");
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    file.seekg(0);
    if (!bytes.empty()) file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
    if (!file) throw std::runtime_error("Cannot read " + path);
    return bytes;
}
uint32_t read32(const uint8_t* value) {
    return uint32_t(value[0]) | uint32_t(value[1]) << 8 | uint32_t(value[2]) << 16 | uint32_t(value[3]) << 24;
}
uint64_t read64(const uint8_t* value) { return read32(value) | uint64_t(read32(value + 4)) << 32; }
void append32(std::vector<uint8_t>& bytes, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes.push_back(uint8_t(value >> (i * 8)));
}
void append64(std::vector<uint8_t>& bytes, uint64_t value) { append32(bytes, uint32_t(value)); append32(bytes, uint32_t(value >> 32)); }
uint64_t checksum(const uint8_t* bytes, size_t size) {
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
    return hash;
}

// Reflect the ordinary descriptor types required by the base renderer. DXC's
// explicit bindings remain authoritative; no implicit register-number remapping.
void reflect(const std::vector<uint32_t>& words, detail::ShaderAllocation& shader) {
    require(words.size() >= 5 && words[0] == 0x07230203 && words[1] <= 0x00010300 &&
        words[1] >= 0x00010000 && words[4] == 0, "Expected Vulkan 1.1-compatible SPIR-V (1.0..1.3)");
    struct Type { uint32_t opcode; std::vector<uint32_t> operands; };
    struct Decorations { uint32_t set = UINT32_MAX, binding = UINT32_MAX; bool bufferBlock = false; };
    struct Variable { uint32_t type, storage; };
    std::unordered_map<uint32_t, Type> types;
    std::unordered_map<uint32_t, Decorations> decorations;
    std::unordered_map<uint32_t, Variable> variables;
    std::unordered_map<uint32_t, uint32_t> constants;
    bool entryFound = false;
    const bool inferEntry = shader.entry.empty();
    for (size_t i = 5; i < words.size();) {
        const uint32_t count = words[i] >> 16, opcode = words[i] & 0xffff;
        require(count && count <= words.size() - i, "Malformed SPIR-V instruction length");
        const auto* op = words.data() + i;
        if (opcode == 15) { // OpEntryPoint
            require(count >= 4, "Malformed SPIR-V entry point");
            const auto* name = reinterpret_cast<const char*>(op + 3);
            const auto* end = static_cast<const char*>(std::memchr(name, 0, (count - 3) * 4));
            require(end, "Unterminated SPIR-V entry point name");
            if (inferEntry && op[1] == (shader.stage == VK_SHADER_STAGE_VERTEX_BIT ? 0u : 4u)) {
                require(!entryFound, "SPIR-V has multiple entries for this stage; select one explicitly");
                shader.entry.assign(name, end);
                entryFound = true;
            } else if (!inferEntry && std::string(name, end) == shader.entry) {
                require(op[1] == (shader.stage == VK_SHADER_STAGE_VERTEX_BIT ? 0u : 4u), "SPIR-V entry point stage mismatch");
                entryFound = true;
            }
        } else if (opcode >= 19 && opcode <= 33) {
            require(count >= 2, "Malformed SPIR-V type");
            types.emplace(op[1], Type {opcode, {op + 2, op + count}});
        } else if (opcode == 43 && count == 4) { // Scalar OpConstant, used for fixed array lengths.
            constants[op[2]] = op[3];
        } else if (opcode == 59) {
            require(count >= 4, "Malformed SPIR-V variable");
            variables[op[2]] = {op[1], op[3]};
            shader.pushConstants |= op[3] == 9;
        } else if (opcode == 71) {
            require(count >= 3, "Malformed SPIR-V decoration");
            if (op[2] == 33 || op[2] == 34) {
                require(count == 4, "Malformed descriptor binding decoration");
                if (op[2] == 33) decorations[op[1]].binding = op[3];
                else decorations[op[1]].set = op[3];
            } else if (op[2] == 3) decorations[op[1]].bufferBlock = true;
        }
        i += count;
    }
    require(entryFound, "SPIR-V does not contain the requested entry point");
    for (const auto& [id, variable] : variables) {
        if (variable.storage != 0 && variable.storage != 2 && variable.storage != 12) continue;
        const auto decoration = decorations.find(id);
        require(decoration != decorations.end() && decoration->second.set != UINT32_MAX &&
            decoration->second.binding != UINT32_MAX, "Shader descriptor has no explicit set/binding");
        auto type = types.find(variable.type);
        require(type != types.end() && type->second.opcode == 32 && type->second.operands.size() == 2,
                "Shader descriptor does not have a pointer type");
        uint32_t idType = type->second.operands[1], count = 1;
        for (unsigned nesting = 0;; ++nesting) {
            require(nesting < 16, "Excessive SPIR-V descriptor array nesting");
            type = types.find(idType);
            require(type != types.end(), "Missing descriptor element type");
            require(type->second.opcode != 29, "Runtime descriptor arrays are not enabled on the base path");
            if (type->second.opcode != 28) break;
            require(type->second.operands.size() == 2, "Invalid descriptor array type");
            const auto length = constants.find(type->second.operands[1]);
            require(length != constants.end() && length->second && count <= UINT32_MAX / length->second,
                    "Descriptor arrays require a fixed, nonzero 32-bit length");
            count *= length->second;
            idType = type->second.operands[0];
        }
        VkDescriptorType descriptor;
        if (variable.storage == 12 || (variable.storage == 2 && decorations[idType].bufferBlock)) descriptor = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        else if (variable.storage == 2) descriptor = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        else if (type->second.opcode == 26) descriptor = VK_DESCRIPTOR_TYPE_SAMPLER;
        else if (type->second.opcode == 27) descriptor = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        else {
            require(type->second.opcode == 25 && type->second.operands.size() >= 7, "Unsupported shader descriptor type");
            const auto& image = type->second.operands;
            require(image[1] != 5, "Texel-buffer descriptors are not implemented");
            descriptor = image[1] == 6 ? VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT :
                (image[5] == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
        }
        shader.bindings.push_back({decoration->second.set, decoration->second.binding, count, descriptor});
    }
    std::sort(shader.bindings.begin(), shader.bindings.end(), [](const auto& a, const auto& b) {
        return std::make_pair(a.set, a.binding) < std::make_pair(b.set, b.binding);
    });
    for (size_t i = 1; i < shader.bindings.size(); ++i)
        require(shader.bindings[i - 1].set != shader.bindings[i].set || shader.bindings[i - 1].binding != shader.bindings[i].binding,
                "Shader has duplicate descriptor bindings");
}

bool compatibleDescriptor(VkDescriptorType reflected, VkDescriptorType layout) {
    return reflected == layout || (reflected == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && layout == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) ||
        (reflected == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && layout == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC);
}
void validateShaderLayout(const detail::ShaderAllocation& shader, const detail::PipelineLayoutAllocation& layout) {
    require(!shader.pushConstants || layout.pushConstantBytes, "Shader requires a push-constant range");
    for (const auto& required : shader.bindings) {
        require(required.set < layout.sets.size(), "Shader descriptor set is absent from the pipeline layout");
        const auto& bindings = layout.sets[required.set]->bindings;
        const auto found = std::find_if(bindings.begin(), bindings.end(), [&](const auto& binding) { return binding.binding == required.binding; });
        require(found != bindings.end() && compatibleDescriptor(required.type, found->type) &&
                found->count >= required.count && (found->stages & shader.stage), "Shader descriptor type/count/stage disagrees with the pipeline layout");
    }
}
} // namespace

struct GraphicsDevice::Impl {
    std::shared_ptr<detail::DeviceState> state;
    VkPipelineCache cache = VK_NULL_HANDLE;
    GraphicsStatistics statistics;
    std::unordered_map<std::string, std::shared_ptr<detail::PipelineAllocation>> pipelines;
    using SamplerKey = std::tuple<VkFilter,VkFilter,VkSamplerMipmapMode,VkSamplerAddressMode,
        VkSamplerAddressMode,VkSamplerAddressMode,float,float,float>;
    std::map<SamplerKey,std::shared_ptr<detail::SamplerAllocation>> samplers;
    explicit Impl(Context& context) : state(detail::Access::state(context)) {}
    ~Impl() { if (cache) state->functions.vkDestroyPipelineCache(state->device, cache, nullptr); }
    std::vector<uint8_t> loadCache(const std::string& path) {
        if (path.empty()) return {};
        try {
            if (!std::filesystem::exists(path)) return {};
            const auto bytes = readFile(path, MaximumCacheBytes + CacheHeaderBytes);
            const auto& p = state->caps.properties;
            require(bytes.size() >= CacheHeaderBytes + 32 && !std::memcmp(bytes.data(), "SVKCACH1", 8), "Pipeline cache header mismatch");
            require(read32(bytes.data() + 8) == p.driverVersion && read64(bytes.data() + 12) == bytes.size() - CacheHeaderBytes &&
                !std::memcmp(bytes.data() + 28, p.pipelineCacheUUID, VK_UUID_SIZE) && read32(bytes.data() + 44) == p.vendorID &&
                read32(bytes.data() + 48) == p.deviceID && read32(bytes.data() + 52) == VK_API_VERSION_1_1,
                "Pipeline cache driver/device/version changed");
            const auto* data = bytes.data() + CacheHeaderBytes;
            const auto size = bytes.size() - CacheHeaderBytes;
            require(read64(bytes.data() + 20) == checksum(data, size) && read32(data) >= 32 && read32(data) <= size &&
                read32(data + 4) == VK_PIPELINE_CACHE_HEADER_VERSION_ONE && read32(data + 8) == p.vendorID &&
                read32(data + 12) == p.deviceID && !std::memcmp(data + 16, p.pipelineCacheUUID, VK_UUID_SIZE), "Pipeline cache is corrupt");
            return {data, data + size};
        } catch (const std::exception& error) {
            state->log("VK_PIPELINE_CACHE_IGNORED: " + std::string(error.what()));
            return {};
        }
    }
};

GraphicsDevice::GraphicsDevice(Context& context, const std::string& path) : impl_(std::make_unique<Impl>(context)) {
    const auto data = impl_->loadCache(path);
    VkPipelineCacheCreateInfo create {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    create.initialDataSize = data.size();
    create.pInitialData = data.empty() ? nullptr : data.data();
    check(context.vk().vkCreatePipelineCache(context.device(), &create, nullptr, &impl_->cache), "vkCreatePipelineCache");
    impl_->statistics.loadedCacheBytes = data.size();
}
GraphicsDevice::~GraphicsDevice() = default;
const GraphicsStatistics& GraphicsDevice::statistics() const { return impl_->statistics; }
VkShaderModule Shader::handle() const { return allocation_ ? allocation_->module : VK_NULL_HANDLE; }
VkShaderStageFlagBits Shader::stage() const { require(bool(allocation_), "Empty shader"); return allocation_->stage; }
const std::vector<ShaderBinding>& Shader::bindings() const { require(bool(allocation_), "Empty shader"); return allocation_->bindings; }
VkSampler Sampler::handle() const { return allocation_ ? allocation_->sampler : VK_NULL_HANDLE; }
const SamplerDescription& Sampler::description() const {
    require(bool(allocation_),"Empty sampler has no description");return allocation_->description;
}
VkDescriptorSetLayout DescriptorLayout::handle() const { return allocation_ ? allocation_->layout : VK_NULL_HANDLE; }
VkPipelineLayout PipelineLayout::handle() const { return allocation_ ? allocation_->layout : VK_NULL_HANDLE; }
VkPipeline GraphicsPipeline::handle() const { return allocation_ ? allocation_->pipeline : VK_NULL_HANDLE; }
VkDescriptorSet DescriptorSet::handle() const {
    require(allocation_ && allocation_->epoch == allocation_->page->epoch, "Empty or recycled descriptor set");
    return allocation_->set;
}

Shader GraphicsDevice::loadShader(const std::string& path, VkShaderStageFlagBits stage, const std::string& entry) {
    const auto data = readFile(path, MaximumShaderBytes);
    return createShader(data.data(), data.size(), stage, entry);
}
Shader GraphicsDevice::createShader(const void* data, size_t bytes, VkShaderStageFlagBits stage, const std::string& entry) {
    require(stage == VK_SHADER_STAGE_VERTEX_BIT || stage == VK_SHADER_STAGE_FRAGMENT_BIT, "Only vertex/fragment shaders are implemented");
    require(entry.find('\0') == std::string::npos, "Invalid shader entry point");
    require(data && bytes >= 20 && bytes <= MaximumShaderBytes && bytes % 4 == 0, "Invalid SPIR-V size or data");
    std::vector<uint32_t> words(bytes / 4);
    std::memcpy(words.data(), data, bytes);
    auto allocation = std::make_shared<detail::ShaderAllocation>();
    allocation->state = impl_->state;
    allocation->entry = entry;
    allocation->stage = stage;
    reflect(words, *allocation);
    VkShaderModuleCreateInfo create {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    create.codeSize = bytes;
    create.pCode = words.data();
    check(impl_->state->functions.vkCreateShaderModule(impl_->state->device, &create, nullptr, &allocation->module), "vkCreateShaderModule");
    Shader shader;
    shader.allocation_ = std::move(allocation);
    return shader;
}
Sampler GraphicsDevice::sampler(const SamplerDescription& description) {
    require((description.minFilter == VK_FILTER_LINEAR || description.minFilter == VK_FILTER_NEAREST) &&
        (description.magFilter == VK_FILTER_LINEAR || description.magFilter == VK_FILTER_NEAREST) &&
        (description.mipFilter == VK_SAMPLER_MIPMAP_MODE_LINEAR || description.mipFilter == VK_SAMPLER_MIPMAP_MODE_NEAREST),
        "Only core nearest/linear sampler filtering is enabled");
    for (auto address : {description.addressU, description.addressV, description.addressW})
        require(address >= VK_SAMPLER_ADDRESS_MODE_REPEAT && address <= VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
                "Sampler addressing requires an optional feature that is not enabled");
    require(std::isfinite(description.minLod) && std::isfinite(description.maxLod) && description.minLod >= 0 &&
            description.maxLod >= description.minLod, "Invalid sampler LOD range");
    const auto& caps=impl_->state->caps;
    require(std::isfinite(description.maxAnisotropy) && description.maxAnisotropy>=1 &&
        (description.maxAnisotropy==1 || (caps.enabledFeatures.samplerAnisotropy &&
            description.maxAnisotropy<=caps.properties.limits.maxSamplerAnisotropy)),
        "Sampler anisotropy exceeds the enabled device capability");
    const Impl::SamplerKey key {description.minFilter,description.magFilter,description.mipFilter,
        description.addressU,description.addressV,description.addressW,description.minLod,description.maxLod,description.maxAnisotropy};
    Sampler sampler;
    const auto cached=impl_->samplers.find(key);
    if(cached!=impl_->samplers.end()) {
        sampler.allocation_=cached->second;++impl_->statistics.samplerHits;return sampler;
    }
    // Retain a bounded set across texture deletion/setting changes. Descriptors
    // own independent references, so eviction never invalidates recorded draws.
    const size_t maximumCached=std::min(512u,caps.properties.limits.maxSamplerAllocationCount/4);
    if(impl_->samplers.size()>=maximumCached) {
        for(auto it=impl_->samplers.begin();it!=impl_->samplers.end();++it)if(it->second.use_count()==1) {
            impl_->samplers.erase(it);break;
        }
    }
    auto allocation = std::make_shared<detail::SamplerAllocation>();
    allocation->state = impl_->state;
    allocation->description = description;
    VkSamplerCreateInfo create {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    create.minFilter = description.minFilter;
    create.magFilter = description.magFilter;
    create.mipmapMode = description.mipFilter;
    create.addressModeU = description.addressU;
    create.addressModeV = description.addressV;
    create.addressModeW = description.addressW;
    create.minLod = description.minLod;
    create.maxLod = description.maxLod;
    create.anisotropyEnable = description.maxAnisotropy>1;
    create.maxAnisotropy = description.maxAnisotropy;
    check(impl_->state->functions.vkCreateSampler(impl_->state->device, &create, nullptr, &allocation->sampler), "vkCreateSampler");
    ++impl_->statistics.samplerCreations;
    if(impl_->samplers.size()<maximumCached)impl_->samplers.emplace(key,allocation);
    sampler.allocation_ = std::move(allocation);
    return sampler;
}
DescriptorLayout GraphicsDevice::descriptorLayout(std::vector<DescriptorBinding> bindings) {
    std::sort(bindings.begin(), bindings.end(), [](const auto& a, const auto& b) { return a.binding < b.binding; });
    std::vector<VkDescriptorSetLayoutBinding> native;
    uint32_t previous = UINT32_MAX;
    for (const auto& binding : bindings) {
        require(binding.binding != previous && binding.count && binding.stages && !(binding.stages & ~detail::GraphicsStages) &&
            detail::supportedDescriptor(binding.type), "Invalid or duplicate descriptor layout binding");
        native.push_back({binding.binding, binding.type, binding.count, binding.stages, nullptr});
        previous = binding.binding;
    }
    auto allocation = std::make_shared<detail::SetLayoutAllocation>();
    allocation->state = impl_->state;
    allocation->bindings = std::move(bindings);
    VkDescriptorSetLayoutCreateInfo create {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    create.bindingCount = uint32_t(native.size());
    create.pBindings = native.data();
    check(impl_->state->functions.vkCreateDescriptorSetLayout(impl_->state->device, &create, nullptr, &allocation->layout), "vkCreateDescriptorSetLayout");
    DescriptorLayout layout;
    layout.allocation_ = std::move(allocation);
    return layout;
}
PipelineLayout GraphicsDevice::pipelineLayout(const std::vector<DescriptorLayout>& sets, uint32_t pushBytes) {
    const auto& limits = impl_->state->caps.properties.limits;
    require(sets.size() <= limits.maxBoundDescriptorSets && pushBytes <= limits.maxPushConstantsSize && !(pushBytes & 3), "Pipeline layout exceeds device limits");
    auto allocation = std::make_shared<detail::PipelineLayoutAllocation>();
    allocation->state = impl_->state;
    allocation->pushConstantBytes = pushBytes;
    std::vector<VkDescriptorSetLayout> layouts;
    for (const auto& set : sets) {
        require(set.allocation_ && set.allocation_->state == impl_->state, "Descriptor layout belongs to another device");
        layouts.push_back(set.handle());
        allocation->sets.push_back(set.allocation_);
    }
    VkPushConstantRange push {detail::GraphicsStages, 0, pushBytes};
    VkPipelineLayoutCreateInfo create {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    create.setLayoutCount = uint32_t(layouts.size());
    create.pSetLayouts = layouts.data();
    create.pushConstantRangeCount = pushBytes ? 1 : 0;
    create.pPushConstantRanges = pushBytes ? &push : nullptr;
    check(impl_->state->functions.vkCreatePipelineLayout(impl_->state->device, &create, nullptr, &allocation->layout), "vkCreatePipelineLayout");
    PipelineLayout layout;
    layout.allocation_ = std::move(allocation);
    return layout;
}

GraphicsPipeline GraphicsDevice::pipeline(const GraphicsPipelineDescription& description) {
    const auto& d = description;
    const auto state = impl_->state;
    require(d.vertex.allocation_ && d.fragment.allocation_ && d.layout.allocation_ && d.renderPass && d.colorFormat != VK_FORMAT_UNDEFINED,
            "Graphics pipeline is missing shaders, layout or render-pass compatibility");
    require(d.vertex.allocation_->state == state && d.fragment.allocation_->state == state && d.layout.allocation_->state == state &&
            d.vertex.stage() == VK_SHADER_STAGE_VERTEX_BIT && d.fragment.stage() == VK_SHADER_STAGE_FRAGMENT_BIT,
            "Pipeline shaders/layout belong to another device or stage");
    require(d.depthFormat != VK_FORMAT_UNDEFINED || (!d.depthTest && !d.depthWrite), "Depth state requires a depth attachment");
    require(!(d.cull & ~VK_CULL_MODE_FRONT_AND_BACK) && (d.frontFace == VK_FRONT_FACE_CLOCKWISE || d.frontFace == VK_FRONT_FACE_COUNTER_CLOCKWISE) &&
        d.depthCompare >= VK_COMPARE_OP_NEVER && d.depthCompare <= VK_COMPARE_OP_ALWAYS && !(d.colorWriteMask & ~15u), "Invalid raster/depth/color-write state");
    for (auto factor : {d.sourceColor, d.destinationColor, d.sourceAlpha, d.destinationAlpha})
        require(factor >= VK_BLEND_FACTOR_ZERO && factor <= VK_BLEND_FACTOR_SRC_ALPHA_SATURATE, "Dual-source blending is not enabled");
    require(d.colorBlend >= VK_BLEND_OP_ADD && d.colorBlend <= VK_BLEND_OP_MAX &&
        d.alphaBlend >= VK_BLEND_OP_ADD && d.alphaBlend <= VK_BLEND_OP_MAX, "Advanced blending is not enabled");
    require(d.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST || d.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ||
            d.topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST, "This pipeline topology is not implemented");
    validateShaderLayout(*d.vertex.allocation_, *d.layout.allocation_);
    validateShaderLayout(*d.fragment.allocation_, *d.layout.allocation_);
    auto bindings = d.vertexBindings;
    auto attributes = d.attributes;
    std::sort(bindings.begin(), bindings.end(), [](const auto& a, const auto& b) { return a.binding < b.binding; });
    std::sort(attributes.begin(), attributes.end(), [](const auto& a, const auto& b) { return a.location < b.location; });
    const auto& limits = state->caps.properties.limits;
    require(bindings.size() <= limits.maxVertexInputBindings && attributes.size() <= limits.maxVertexInputAttributes, "Too many vertex bindings/attributes");
    uint32_t previous = UINT32_MAX;
    for (const auto& binding : bindings) {
        require(binding.binding != previous && binding.binding < limits.maxVertexInputBindings && binding.stride <= limits.maxVertexInputBindingStride &&
                (binding.inputRate == VK_VERTEX_INPUT_RATE_VERTEX || binding.inputRate == VK_VERTEX_INPUT_RATE_INSTANCE), "Invalid vertex/instance binding");
        previous = binding.binding;
    }
    previous = UINT32_MAX;
    for (const auto& attribute : attributes) {
        require(attribute.location != previous && attribute.location < limits.maxVertexInputAttributes && attribute.offset <= limits.maxVertexInputAttributeOffset &&
            std::any_of(bindings.begin(), bindings.end(), [&](const auto& b) { return b.binding == attribute.binding; }), "Invalid vertex attribute binding/location");
        VkFormatProperties format {};
        state->instanceFunctions.vkGetPhysicalDeviceFormatProperties(state->physical, attribute.format, &format);
        require(format.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT, "Unsupported vertex attribute format");
        previous = attribute.location;
    }
    // Pointer identities stay live through cached PipelineAllocation ownership.
    // RenderPass handles are intentionally absent: compatible replacement passes
    // share pipelines even after resize. This API supports exactly one subpass.
    std::string key;
    auto add = [&](uint64_t value) { for (unsigned i = 0; i < 8; ++i) key.push_back(char(value >> (i * 8))); };
    add(reinterpret_cast<uintptr_t>(d.vertex.allocation_.get()));
    add(reinterpret_cast<uintptr_t>(d.fragment.allocation_.get()));
    add(reinterpret_cast<uintptr_t>(d.layout.allocation_.get()));
    for (auto value : {uint32_t(d.colorFormat), uint32_t(d.depthFormat), uint32_t(d.topology), uint32_t(d.cull), uint32_t(d.frontFace),
            uint32_t(d.depthTest), uint32_t(d.depthWrite), uint32_t(d.depthCompare), uint32_t(d.blend), uint32_t(d.sourceColor),
            uint32_t(d.destinationColor), uint32_t(d.sourceAlpha), uint32_t(d.destinationAlpha), uint32_t(d.colorBlend), uint32_t(d.alphaBlend), uint32_t(d.colorWriteMask)}) add(value);
    add(bindings.size());
    add(uint32_t(d.polygonMode));add(d.depthBias);add(d.alphaToCoverage);add(d.stencilTest);
    uint32_t biasConstant,biasSlope;
    std::memcpy(&biasConstant,&d.depthBiasConstant,4);std::memcpy(&biasSlope,&d.depthBiasSlope,4);
    add(biasConstant);add(biasSlope);
    for(auto value:{uint32_t(d.stencil.failOp),uint32_t(d.stencil.passOp),uint32_t(d.stencil.depthFailOp),uint32_t(d.stencil.compareOp),
        d.stencil.compareMask,d.stencil.writeMask,d.stencil.reference})add(value);
    require(d.polygonMode==VK_POLYGON_MODE_FILL || state->caps.enabledFeatures.fillModeNonSolid,"Wireframe requires the queried fillModeNonSolid feature");
    for (const auto& binding : bindings) { add(binding.binding); add(binding.stride); add(binding.inputRate); }
    add(attributes.size());
    for (const auto& attribute : attributes) { add(attribute.location); add(attribute.binding); add(attribute.format); add(attribute.offset); }
    GraphicsPipeline pipeline;
    const auto cached = impl_->pipelines.find(key);
    if (cached != impl_->pipelines.end()) { ++impl_->statistics.pipelineHits; pipeline.allocation_ = cached->second; return pipeline; }
    auto allocation = std::make_shared<detail::PipelineAllocation>();
    allocation->state = state;
    allocation->layout = d.layout.allocation_;
    allocation->vertex = d.vertex.allocation_;
    allocation->fragment = d.fragment.allocation_;
    allocation->vertexBindings = bindings;
    allocation->colorFormat = d.colorFormat;
    allocation->depthFormat = d.depthFormat;
    VkPipelineShaderStageCreateInfo stages[2] {};
    for (unsigned i = 0; i < 2; ++i) {
        const auto& shader = i ? allocation->fragment : allocation->vertex;
        stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[i].stage = shader->stage;
        stages[i].module = shader->module;
        stages[i].pName = shader->entry.c_str();
    }
    VkPipelineVertexInputStateCreateInfo vertex {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex.vertexBindingDescriptionCount = uint32_t(bindings.size()); vertex.pVertexBindingDescriptions = bindings.data();
    vertex.vertexAttributeDescriptionCount = uint32_t(attributes.size()); vertex.pVertexAttributeDescriptions = attributes.data();
    VkPipelineInputAssemblyStateCreateInfo assembly {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = d.topology;
    VkPipelineViewportStateCreateInfo viewport {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = d.polygonMode; raster.cullMode = d.cull; raster.frontFace = d.frontFace; raster.lineWidth = 1;
    raster.depthBiasEnable=d.depthBias;raster.depthBiasConstantFactor=d.depthBiasConstant;raster.depthBiasSlopeFactor=d.depthBiasSlope;
    VkPipelineMultisampleStateCreateInfo samples {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    samples.alphaToCoverageEnable=d.alphaToCoverage;
    VkPipelineDepthStencilStateCreateInfo depth {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = d.depthTest; depth.depthWriteEnable = d.depthWrite; depth.depthCompareOp = d.depthCompare;
    depth.stencilTestEnable=d.stencilTest;depth.front=depth.back=d.stencil;
    VkPipelineColorBlendAttachmentState blend {};
    blend.blendEnable = d.blend; blend.colorWriteMask = d.colorWriteMask;
    blend.srcColorBlendFactor = d.sourceColor; blend.dstColorBlendFactor = d.destinationColor; blend.colorBlendOp = d.colorBlend;
    blend.srcAlphaBlendFactor = d.sourceAlpha; blend.dstAlphaBlendFactor = d.destinationAlpha; blend.alphaBlendOp = d.alphaBlend;
    VkPipelineColorBlendStateCreateInfo blending {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blending.attachmentCount = 1; blending.pAttachments = &blend;
    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2; dynamic.pDynamicStates = dynamicStates;
    VkGraphicsPipelineCreateInfo create {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    create.stageCount = 2; create.pStages = stages;
    create.pVertexInputState = &vertex; create.pInputAssemblyState = &assembly; create.pViewportState = &viewport;
    create.pRasterizationState = &raster; create.pMultisampleState = &samples; create.pDepthStencilState = &depth;
    create.pColorBlendState = &blending; create.pDynamicState = &dynamic;
    create.layout = d.layout.handle(); create.renderPass = d.renderPass;
    check(state->functions.vkCreateGraphicsPipelines(state->device, impl_->cache, 1, &create, nullptr, &allocation->pipeline), "vkCreateGraphicsPipelines");
    impl_->pipelines.emplace(std::move(key), allocation);
    ++impl_->statistics.pipelineCreations;
    pipeline.allocation_ = std::move(allocation);
    return pipeline;
}

void GraphicsDevice::savePipelineCache(const std::string& path) const {
    require(!path.empty(), "Pipeline cache path is empty");
    const auto state = impl_->state;
    std::vector<uint8_t> data;
    for (;;) {
        size_t bytes = 0;
        check(state->functions.vkGetPipelineCacheData(state->device, impl_->cache, &bytes, nullptr), "pipeline cache size");
        require(bytes <= MaximumCacheBytes, "Pipeline cache exceeds 64 MiB");
        data.resize(bytes);
        const auto result = state->functions.vkGetPipelineCacheData(state->device, impl_->cache, &bytes, data.data());
        if (result == VK_INCOMPLETE) continue;
        check(result, "pipeline cache data");
        data.resize(bytes);
        break;
    }
    const auto& p = state->caps.properties;
    std::vector<uint8_t> file {'S','V','K','C','A','C','H','1'};
    append32(file, p.driverVersion); append64(file, data.size()); append64(file, checksum(data.data(), data.size()));
    file.insert(file.end(), p.pipelineCacheUUID, p.pipelineCacheUUID + VK_UUID_SIZE);
    append32(file, p.vendorID); append32(file, p.deviceID); append32(file, VK_API_VERSION_1_1);
    file.insert(file.end(), data.begin(), data.end());
    const auto destination = std::filesystem::path(path);
    if (!destination.parent_path().empty()) std::filesystem::create_directories(destination.parent_path());
    const auto temporary = destination.string() + ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(file.data()), std::streamsize(file.size()));
    output.close();
    if (!output) throw std::runtime_error("Cannot write pipeline cache " + temporary);
    std::filesystem::rename(temporary, destination);
}
} // namespace sourcevk
