#include "vulkan_graphics_internal.h"

#include <algorithm>
#include <map>

namespace sourcevk {
using detail::require;
struct DescriptorArena::Impl {
    struct Slot { uint64_t serial = 0; std::vector<std::shared_ptr<detail::DescriptorPoolPage>> pages; };
    Context& context;
    std::shared_ptr<detail::DeviceState> state;
    std::array<Slot, FrameSlotCount> slots;
    uint32_t setsPerPool, maximumPools;
    Impl(Context& owner, uint32_t sets, uint32_t maximum) : context(owner), state(detail::Access::state(context)), setsPerPool(sets), maximumPools(maximum) {
        require(sets && sets <= 4096 && maximum && maximum <= 64, "Invalid descriptor pool budget");
    }
    std::shared_ptr<detail::DescriptorPoolPage> page(const std::map<VkDescriptorType, uint32_t>& needed) {
        auto page = std::make_shared<detail::DescriptorPoolPage>();
        page->state = state;
        page->maximumSets = page->remainingSets = setsPerPool;
        std::vector<VkDescriptorPoolSize> sizes;
        for (const auto& [type, count] : needed) {
            require(count <= UINT32_MAX / setsPerPool, "Descriptor pool size overflow");
            page->capacity[type] = page->remaining[type] = count * setsPerPool;
            sizes.push_back({type, count * setsPerPool});
        }
        VkDescriptorPoolCreateInfo create {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        create.maxSets = setsPerPool;
        create.poolSizeCount = uint32_t(sizes.size());
        create.pPoolSizes = sizes.data();
        check(state->functions.vkCreateDescriptorPool(state->device, &create, nullptr, &page->pool), "vkCreateDescriptorPool(frame)");
        return page;
    }
};
DescriptorArena::DescriptorArena(Context& context, uint32_t sets, uint32_t maximum)
    : impl_(std::make_unique<Impl>(context, sets, maximum)) {}
DescriptorArena::~DescriptorArena() = default;
uint32_t DescriptorArena::poolCount() const {
    uint32_t count = 0;
    for (const auto& slot : impl_->slots) count += uint32_t(slot.pages.size());
    return count;
}
DescriptorSet DescriptorArena::allocate(const Frame& frame, const DescriptorLayout& layout, const std::vector<DescriptorWrite>& writes) {
    auto& arena = *impl_;
    const auto slotIndex = detail::Access::slot(arena.context, frame);
    const auto serial = detail::Access::serial(arena.context, frame);
    require(layout.allocation_ && layout.allocation_->state == arena.state, "Descriptor layout belongs to another device");
    const auto& bindings = layout.allocation_->bindings;
    std::map<VkDescriptorType, uint32_t> needed;
    uint64_t count = 0;
    for (const auto& binding : bindings) {
        count += binding.count;
        require(count <= 65536, "Descriptor set exceeds the base renderer's element budget");
        needed[binding.type] += binding.count;
    }
    require(writes.size() == count, "Every descriptor element must be initialized exactly once");
    std::vector<const DescriptorWrite*> ordered;
    for (const auto& write : writes) ordered.push_back(&write);
    std::sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) {
        return std::make_pair(a->binding, a->arrayElement) < std::make_pair(b->binding, b->arrayElement);
    });
    auto allocation = std::make_shared<detail::DescriptorAllocation>();
    allocation->layout = layout.allocation_;
    allocation->frameSerial = serial;
    std::vector<VkDescriptorBufferInfo> bufferInfos(writes.size());
    std::vector<VkDescriptorImageInfo> imageInfos(writes.size());
    std::vector<VkWriteDescriptorSet> updates(writes.size());
    const auto& limits = arena.state->caps.properties.limits;
    size_t i = 0;
    for (const auto& binding : bindings) for (uint32_t element = 0; element < binding.count; ++element, ++i) {
        const auto& write = *ordered[i];
        require(write.binding == binding.binding && write.arrayElement == element && write.type == binding.type,
                "Descriptor writes disagree with the layout or contain duplicate/missing elements");
        auto& update = updates[i];
        update.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        update.dstBinding = binding.binding;
        update.dstArrayElement = element;
        update.descriptorCount = 1;
        update.descriptorType = binding.type;
        if (detail::bufferDescriptor(binding.type)) {
            const bool uniform = detail::uniformDescriptor(binding.type);
            detail::validateSlice(write.buffer, uniform ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, arena.state);
            const auto alignment = uniform ? limits.minUniformBufferOffsetAlignment : limits.minStorageBufferOffsetAlignment;
            const auto maximum = uniform ? limits.maxUniformBufferRange : limits.maxStorageBufferRange;
            require(write.buffer.offset % alignment == 0 && write.buffer.size <= maximum, "Descriptor buffer range/alignment exceeds device limits");
            bufferInfos[i] = {write.buffer.buffer.handle(), write.buffer.offset, write.buffer.size};
            update.pBufferInfo = &bufferInfos[i];
            allocation->buffers.push_back(write.buffer.buffer);
            if (detail::dynamicDescriptor(binding.type)) allocation->dynamicBuffers.push_back({write.buffer.offset, write.buffer.size, write.buffer.buffer.size(), alignment});
        } else {
            const bool hasSampler = binding.type == VK_DESCRIPTOR_TYPE_SAMPLER || binding.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            const bool hasImage = binding.type != VK_DESCRIPTOR_TYPE_SAMPLER;
            if (hasSampler) {
                require(write.sampler.allocation_ && write.sampler.allocation_->state == arena.state, "Descriptor sampler belongs to another device");
                imageInfos[i].sampler = write.sampler.handle();
                allocation->samplers.push_back(write.sampler);
            }
            if (hasImage) {
                require(detail::Access::state(write.image) == arena.state && write.image.view(), "Descriptor image has no view or belongs to another device");
                const auto usage = binding.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? VK_IMAGE_USAGE_STORAGE_BIT :
                    (binding.type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT ? VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT : VK_IMAGE_USAGE_SAMPLED_BIT);
                require(write.image.description().usage & usage, "Descriptor image usage does not match its binding type");
                require(write.imageLayout == VK_IMAGE_LAYOUT_GENERAL ||
                    (binding.type != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE && (write.imageLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
                     write.imageLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL)), "Invalid descriptor image layout");
                imageInfos[i].imageView = write.image.view();
                imageInfos[i].imageLayout = write.imageLayout;
                allocation->images.push_back(write.image);
            }
            update.pImageInfo = &imageInfos[i];
        }
    }
    auto& slot = arena.slots.at(slotIndex);
    if (slot.serial != serial) {
        // Context's beginFrame already waited this slot's completion fence.
        for (const auto& page : slot.pages) {
            check(arena.state->functions.vkResetDescriptorPool(arena.state->device, page->pool, 0), "vkResetDescriptorPool(frame)");
            ++page->epoch;
            page->remaining = page->capacity;
            page->remainingSets = page->maximumSets;
        }
        slot.serial = serial;
    }
    for (;;) {
        std::shared_ptr<detail::DescriptorPoolPage> selected;
        for (const auto& page : slot.pages) {
            if (!page->remainingSets) continue;
            bool fits = true;
            for (const auto& [type, amount] : needed) {
                const auto found = page->remaining.find(type);
                if (found == page->remaining.end() || found->second < amount) { fits = false; break; }
            }
            if (fits) { selected = page; break; }
        }
        if (!selected) {
            require(slot.pages.size() < arena.maximumPools, "Per-frame descriptor pool budget exhausted");
            selected = arena.page(needed);
            slot.pages.push_back(selected);
        }
        const auto nativeLayout = layout.handle();
        VkDescriptorSetAllocateInfo allocate {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = selected->pool;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &nativeLayout;
        const auto result = arena.state->functions.vkAllocateDescriptorSets(arena.state->device, &allocate, &allocation->set);
        if (result == VK_ERROR_OUT_OF_POOL_MEMORY || result == VK_ERROR_FRAGMENTED_POOL) { selected->remainingSets = 0; continue; }
        check(result, "vkAllocateDescriptorSets(frame)");
        --selected->remainingSets;
        for (const auto& [type, amount] : needed) selected->remaining[type] -= amount;
        allocation->page = selected;
        allocation->epoch = selected->epoch;
        break;
    }
    for (auto& update : updates) update.dstSet = allocation->set;
    arena.state->functions.vkUpdateDescriptorSets(arena.state->device, uint32_t(updates.size()), updates.data(), 0, nullptr);
    detail::Access::retain(arena.context, frame, allocation);
    DescriptorSet result;
    result.allocation_ = std::move(allocation);
    return result;
}
} // namespace sourcevk
