#include "vulkan_upload.h"
#include "vulkan_internal.h"

#include <algorithm>
#include <limits>

namespace sourcevk {
namespace {
VkDeviceSize alignUp(VkDeviceSize size, VkDeviceSize alignment) {
    if (!alignment || (alignment & (alignment - 1)) || size > UINT64_MAX - (alignment - 1))
        throw std::invalid_argument("Invalid or overflowing buffer alignment");
    return (size + alignment - 1) & ~(alignment - 1);
}
void validSlice(const BufferSlice& slice) {
    if (!slice.buffer || !slice.size || slice.offset > slice.buffer.size() || slice.size > slice.buffer.size() - slice.offset)
        throw std::out_of_range("Buffer slice is empty or out of bounds");
}
uint32_t texelBytes(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8_UNORM: return 1;
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SNORM: case VK_FORMAT_R16_SFLOAT: return 2;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R8G8B8A8_SNORM: return 4;
    case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM: return 8;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
    default: throw std::invalid_argument("UploadBatch does not yet support this image format");
    }
}
} // namespace

namespace detail {
struct PendingUpload {
    std::shared_ptr<DeviceState> state;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkFence complete = VK_NULL_HANDLE;
    bool submitted = false;
    std::vector<Buffer> buffers, staging;
    std::vector<Image> images;
    ~PendingUpload() {
        if (!state) return;
        const auto& vk = state->functions;
        if (submitted) vk.vkWaitForFences(state->device, 1, &complete, VK_TRUE, UINT64_MAX);
        if (complete) vk.vkDestroyFence(state->device, complete, nullptr);
        if (pool) vk.vkDestroyCommandPool(state->device, pool, nullptr);
    }
};
} // namespace detail

bool UploadTicket::ready() const {
    if (!pending_) return true;
    const auto result = pending_->state->functions.vkGetFenceStatus(pending_->state->device, pending_->complete);
    if (result == VK_NOT_READY) return false;
    check(result, "batch upload status");
    return true;
}
void UploadTicket::wait(uint64_t timeout) const {
    if (pending_) check(pending_->state->functions.vkWaitForFences(pending_->state->device,
        1, &pending_->complete, VK_TRUE, timeout), "batch upload completion");
}

struct UploadBatch::Impl {
    Context& context;
    std::shared_ptr<detail::PendingUpload> pending = std::make_shared<detail::PendingUpload>();
    VkDeviceSize pageBytes, cursor = 0;
    uint32_t copies = 0;
    bool ended = false;
    explicit Impl(Context& owner, VkDeviceSize page) : context(owner), pageBytes(alignUp(page, 4)) {
        pending->state = detail::Access::state(context);
        if (!pageBytes || pageBytes > 64 * 1024 * 1024) throw std::invalid_argument("Invalid staging page budget");
        const auto& vk = context.vk();
        VkCommandPoolCreateInfo pool {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pool.queueFamilyIndex = context.capabilities().graphicsFamily;
        check(vk.vkCreateCommandPool(context.device(), &pool, nullptr, &pending->pool), "batch command pool");
        VkCommandBufferAllocateInfo allocate {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool = pending->pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        check(vk.vkAllocateCommandBuffers(context.device(), &allocate, &pending->commands), "batch command buffer");
        VkCommandBufferBeginInfo begin {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vk.vkBeginCommandBuffer(pending->commands, &begin), "begin upload batch");
    }
    void writable() const {
        if (ended) throw std::logic_error("Upload batch was already submitted or closed");
    }
    BufferSlice stage(const void* data, VkDeviceSize bytes, VkDeviceSize alignment) {
        writable();
        if (!data || !bytes || bytes > 64 * 1024 * 1024) throw std::invalid_argument("Invalid staging data or upload exceeds 64 MiB");
        auto offset = alignUp(cursor, alignment);
        if (pending->staging.empty() || offset > pending->staging.back().size() || bytes > pending->staging.back().size() - offset) {
            pending->staging.push_back(context.createBuffer(std::max(pageBytes, alignUp(bytes, alignment)),
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryAccess::Upload));
            offset = 0;
        }
        auto& buffer = pending->staging.back();
        buffer.write(offset, data, bytes);
        cursor = offset + bytes;
        return {buffer, offset, bytes};
    }
};
UploadBatch::UploadBatch(Context& context, VkDeviceSize page) : impl_(std::make_unique<Impl>(context, page)) {}
UploadBatch::~UploadBatch() = default;

void UploadBatch::buffer(const BufferSlice& destination, const void* data, BufferUse before, BufferUse after) {
    auto& batch = *impl_;
    batch.writable();
    validSlice(destination);
    if (detail::Access::state(destination.buffer) != batch.pending->state ||
        !(destination.buffer.usage() & VK_BUFFER_USAGE_TRANSFER_DST_BIT) ||
        ((destination.offset | destination.size) & 3) || !before.stages || !after.stages)
        throw std::invalid_argument("Invalid upload buffer, transfer alignment or consumer stages");
    const auto staged = batch.stage(data, destination.size, 4);
    const auto& vk = batch.context.vk();
    const auto commands = batch.pending->commands;
    VkBufferMemoryBarrier barrier {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask = before.access;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = destination.buffer.handle();
    barrier.offset = destination.offset;
    barrier.size = destination.size;
    vk.vkCmdPipelineBarrier(commands, before.stages, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &barrier, 0, nullptr);
    VkBufferCopy copy {staged.offset, destination.offset, destination.size};
    vk.vkCmdCopyBuffer(commands, staged.buffer.handle(), destination.buffer.handle(), 1, &copy);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = after.access;
    vk.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, after.stages, 0, 0, nullptr, 1, &barrier, 0, nullptr);
    batch.pending->buffers.push_back(destination.buffer);
    ++batch.copies;
}

void UploadBatch::image(const Image& destination, uint32_t mip, uint32_t layer, const void* data,
                       VkDeviceSize bytes, ImageUse before, ImageUse after) {
    auto& batch = *impl_;
    batch.writable();
    if (detail::Access::state(destination) != batch.pending->state) throw std::invalid_argument("Upload image belongs to another device");
    const auto& description = destination.description();
    if (!(description.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) || description.aspect != VK_IMAGE_ASPECT_COLOR_BIT ||
        mip >= description.mipLevels || layer >= description.layers || !before.stages || !after.stages ||
        after.layout == VK_IMAGE_LAYOUT_UNDEFINED || after.layout == VK_IMAGE_LAYOUT_PREINITIALIZED)
        throw std::invalid_argument("Invalid image upload subresource, usage or layout");
    const uint32_t width = std::max(1u, description.width >> mip), height = std::max(1u, description.height >> mip);
    const auto block=detail::compressedBlockBytes(description.format);
    const auto texel = block ? block : texelBytes(description.format);
    const auto columns=block?(width+3)/4:width,rows=block?(height+3)/4:height;
    if (VkDeviceSize(columns) * rows * texel != bytes) throw std::invalid_argument("Image upload byte count does not match its mip extent");
    auto staged = batch.stage(data, bytes, std::max(4u, texel));
    const auto& vk = batch.context.vk();
    const auto commands = batch.pending->commands;
    VkImageMemoryBarrier barrier {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = before.access;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = before.layout;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = destination.handle();
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, layer, 1};
    vk.vkCmdPipelineBarrier(commands, before.stages, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkBufferImageCopy copy {};
    copy.bufferOffset = staged.offset;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1};
    copy.imageExtent = {width, height, 1};
    vk.vkCmdCopyBufferToImage(commands, staged.buffer.handle(), destination.handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = after.access;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = after.layout;
    vk.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, after.stages, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    batch.pending->images.push_back(destination);
    ++batch.copies;
}

UploadTicket UploadBatch::submit() {
    auto& batch = *impl_;
    batch.writable();
    if (!batch.copies) throw std::logic_error("Cannot submit an empty upload batch");
    batch.ended = true;
    check(batch.context.vk().vkEndCommandBuffer(batch.pending->commands), "end upload batch");
    VkFenceCreateInfo fence {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    check(batch.context.vk().vkCreateFence(batch.context.device(), &fence, nullptr, &batch.pending->complete), "batch fence");
    detail::Access::submit(batch.context, batch.pending->commands, batch.pending->complete);
    batch.pending->submitted = true;
    UploadTicket ticket;
    ticket.pending_ = std::move(batch.pending);
    return ticket;
}

struct FrameArena::Impl {
    struct Page { Buffer buffer; VkDeviceSize used = 0; };
    struct Slot { uint64_t serial = 0; VkDeviceSize used = 0; std::vector<Page> pages; };
    Context& context;
    VkDeviceSize pageBytes, maximumBytes, alignment;
    std::array<Slot, FrameSlotCount> slots;
    Impl(Context& owner, VkDeviceSize page, VkDeviceSize maximum) : context(owner), pageBytes(page), maximumBytes(maximum) {
        const auto& limits = context.capabilities().properties.limits;
        alignment = std::max({VkDeviceSize(16), limits.minUniformBufferOffsetAlignment, limits.minStorageBufferOffsetAlignment});
        pageBytes = alignUp(pageBytes, alignment);
        if (!pageBytes || maximumBytes < pageBytes || maximumBytes > UINT32_MAX)
            throw std::invalid_argument("Invalid dynamic frame arena budget");
    }
};
FrameArena::FrameArena(Context& context, VkDeviceSize page, VkDeviceSize maximum)
    : impl_(std::make_unique<Impl>(context, page, maximum)) {}
FrameArena::~FrameArena() = default;
BufferSlice FrameArena::write(const Frame& frame, const void* data, VkDeviceSize bytes, VkDeviceSize requestedAlignment) {
    auto& arena = *impl_;
    const auto index = detail::Access::slot(arena.context, frame);
    const auto serial = detail::Access::serial(arena.context, frame);
    auto& slot = arena.slots.at(index);
    if (slot.serial != serial) {
        slot.serial = serial;
        slot.used = 0;
        for (auto& page : slot.pages) page.used = 0;
    }
    // Validate even smaller requested alignments; max() alone could hide a bad value.
    alignUp(0, requestedAlignment);
    const auto alignment = std::max(requestedAlignment, arena.alignment);
    const auto charged = alignUp(bytes, alignment);
    if (!bytes || !data || charged > arena.maximumBytes - slot.used)
        throw std::invalid_argument("Frame arena data is empty or exceeds its per-frame budget");
    Impl::Page* selected = nullptr;
    VkDeviceSize offset = 0, capacity = 0;
    for (auto& page : slot.pages) {
        capacity += page.buffer.size();
        const auto start = alignUp(page.used, alignment);
        if (!selected && start <= page.buffer.size() && bytes <= page.buffer.size() - start) { selected = &page; offset = start; }
    }
    if (!selected) {
        const auto size = std::max(arena.pageBytes, charged);
        if (size > arena.maximumBytes - capacity) throw std::runtime_error("Frame arena page budget exhausted");
        slot.pages.push_back({arena.context.createBuffer(size,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryAccess::Upload), 0});
        selected = &slot.pages.back();
    }
    selected->buffer.write(offset, data, bytes);
    selected->used = offset + bytes;
    slot.used += charged;
    arena.context.retain(frame, selected->buffer);
    return {selected->buffer, offset, bytes};
}
VkDeviceSize FrameArena::allocatedBytes() const {
    VkDeviceSize size = 0;
    for (const auto& slot : impl_->slots) for (const auto& page : slot.pages) size += page.buffer.size();
    return size;
}
} // namespace sourcevk
