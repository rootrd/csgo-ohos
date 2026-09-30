#include "vulkan_internal.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace sourcevk {
namespace detail {
BufferAllocation::~BufferAllocation() {
    if (buffer) vmaDestroyBuffer(state->allocator, buffer, allocation);
}
ImageAllocation::~ImageAllocation() {
    if (alternateView) state->functions.vkDestroyImageView(state->device, alternateView, nullptr);
    if (view) state->functions.vkDestroyImageView(state->device, view, nullptr);
    if (image) vmaDestroyImage(state->allocator, image, allocation);
}

std::shared_ptr<BufferAllocation> createBuffer(const std::shared_ptr<DeviceState>& state,
        VkDeviceSize size, VkBufferUsageFlags usage, MemoryAccess access) {
    if (!size || !usage) throw std::invalid_argument("Vulkan buffer size and usage must be nonzero");
    auto result = std::make_shared<BufferAllocation>();
    result->state = state;
    result->size = size;
    result->usage = usage;
    result->access = access;
    VkBufferCreateInfo create {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    create.size = size;
    create.usage = usage;
    create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo allocation {};
    allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (access != MemoryAccess::Device) {
        allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        allocation.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
            (access == MemoryAccess::Upload ? VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                                           : VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT);
        allocation.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    }
    VmaAllocationInfo info {};
    check(vmaCreateBuffer(state->allocator, &create, &allocation, &result->buffer,
                          &result->allocation, &info), "vmaCreateBuffer");
    result->mapped = info.pMappedData;
    if (access != MemoryAccess::Device && !result->mapped)
        throw std::runtime_error("VMA did not map a host-visible buffer");
    return result;
}

std::shared_ptr<ImageAllocation> createImage(const std::shared_ptr<DeviceState>& state,
        const ImageDescription& description) {
    const auto& d = description;
    uint32_t maximumMips = 0;
    for (uint32_t size = std::max(d.width, d.height); size; size >>= 1) ++maximumMips;
    if (!d.width || !d.height || !d.layers || !d.mipLevels || d.mipLevels > maximumMips ||
        !d.usage || !d.aspect || d.format == VK_FORMAT_UNDEFINED)
        throw std::invalid_argument("Invalid Vulkan 2D image description");
    VkFormat alternate = VK_FORMAT_UNDEFINED;
    if(compressedBlockBytes(d.format) && !state->caps.enabledFeatures.textureCompressionBC)
        throw std::invalid_argument("BC image storage requires the enabled textureCompressionBC feature");
    if (d.srgbViews) {
        alternate=alternateColorSpace(d.format);
        if(alternate==VK_FORMAT_UNDEFINED)throw std::invalid_argument("Image format has no compatible sRGB/linear pair");
        if (!(d.usage & VK_IMAGE_USAGE_SAMPLED_BIT)) throw std::invalid_argument("sRGB views require a sampled image");
        VkFormatProperties properties {};
        state->instanceFunctions.vkGetPhysicalDeviceFormatProperties(state->physical, alternate, &properties);
        if (!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
            throw std::invalid_argument("Alternate sRGB/linear view is not sampleable on this device");
    }
    if (d.cube && (d.layers != 6 || d.width != d.height)) throw std::invalid_argument("Invalid cubemap extent/layers");
    const VkImageCreateFlags flags = (d.srgbViews ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0) |
        (d.cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0);
    VkImageFormatProperties supported {};
    check(state->instanceFunctions.vkGetPhysicalDeviceImageFormatProperties(state->physical,
        d.format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, d.usage, flags, &supported),
        "vkGetPhysicalDeviceImageFormatProperties");
    if (d.width > supported.maxExtent.width || d.height > supported.maxExtent.height ||
        d.layers > supported.maxArrayLayers || d.mipLevels > supported.maxMipLevels)
        throw std::invalid_argument("Vulkan image exceeds this device's format limits");
    auto result = std::make_shared<ImageAllocation>();
    result->state = state;
    result->description = d;
    VkImageCreateInfo create {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    create.flags = flags;
    create.imageType = VK_IMAGE_TYPE_2D;
    create.format = d.format;
    create.extent = {d.width, d.height, 1};
    create.mipLevels = d.mipLevels;
    create.arrayLayers = d.layers;
    create.samples = VK_SAMPLE_COUNT_1_BIT;
    create.tiling = VK_IMAGE_TILING_OPTIMAL;
    create.usage = d.usage;
    create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo allocation {};
    allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    check(vmaCreateImage(state->allocator, &create, &allocation, &result->image,
                         &result->allocation, nullptr), "vmaCreateImage");
    const VkImageUsageFlags viewUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    if (d.usage & viewUsage) {
        VkImageViewCreateInfo view {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = result->image;
        view.viewType = d.cube ? VK_IMAGE_VIEW_TYPE_CUBE : d.layers == 1 ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        view.format = d.format;
        view.subresourceRange = {d.aspect, 0, d.mipLevels, 0, d.layers};
        check(state->functions.vkCreateImageView(state->device, &view, nullptr, &result->view),
              "vkCreateImageView(resource)");
        if (d.srgbViews) {
            // SRGB formats need not support every usage of the linear image
            // (notably storage writes). This alternate view is sampling-only.
            VkImageViewUsageCreateInfo sampledUsage {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
            sampledUsage.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
            if (d.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) {
                VkFormatProperties properties {};
                state->instanceFunctions.vkGetPhysicalDeviceFormatProperties(state->physical, alternate, &properties);
                if (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)
                    sampledUsage.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            }
            view.pNext = &sampledUsage;
            view.format = alternate;
            check(state->functions.vkCreateImageView(state->device, &view, nullptr, &result->alternateView),
                  "vkCreateImageView(sRGB/linear)");
            result->alternateDescription = d;
            result->alternateDescription.format = alternate;
            result->alternateDescription.usage = sampledUsage.usage;
        }
    }
    return result;
}
} // namespace detail

namespace {
void checkRange(const std::shared_ptr<detail::BufferAllocation>& allocation,
                VkDeviceSize offset, VkDeviceSize bytes, const void* data) {
    if (!allocation) throw std::logic_error("Access to an empty Vulkan buffer");
    if (offset > allocation->size || bytes > allocation->size - offset ||
        offset > std::numeric_limits<size_t>::max() ||
        bytes > std::numeric_limits<size_t>::max() - size_t(offset))
        throw std::out_of_range("Vulkan buffer access is out of bounds");
    if (bytes && !data) throw std::invalid_argument("Null Vulkan buffer transfer pointer");
    if (!allocation->mapped) throw std::logic_error("Vulkan buffer is not host mapped");
}
} // namespace

VkBuffer Buffer::handle() const { return allocation_ ? allocation_->buffer : VK_NULL_HANDLE; }
VkDeviceSize Buffer::size() const { return allocation_ ? allocation_->size : 0; }
VkBufferUsageFlags Buffer::usage() const { return allocation_ ? allocation_->usage : 0; }
void Buffer::write(VkDeviceSize offset, const void* data, VkDeviceSize bytes) const {
    checkRange(allocation_, offset, bytes, data);
    if (allocation_->access != MemoryAccess::Upload)
        throw std::logic_error("write requires an upload buffer");
    if (!bytes) return;
    std::memcpy(static_cast<char*>(allocation_->mapped) + size_t(offset), data, size_t(bytes));
    // VMA rounds to nonCoherentAtomSize and skips coherent allocations.
    check(vmaFlushAllocation(allocation_->state->allocator, allocation_->allocation, offset, bytes),
          "vmaFlushAllocation");
}
void Buffer::read(VkDeviceSize offset, void* data, VkDeviceSize bytes) const {
    checkRange(allocation_, offset, bytes, data);
    if (allocation_->access != MemoryAccess::Readback)
        throw std::logic_error("read requires a readback buffer");
    if (!bytes) return;
    check(vmaInvalidateAllocation(allocation_->state->allocator, allocation_->allocation, offset, bytes),
          "vmaInvalidateAllocation");
    std::memcpy(data, static_cast<const char*>(allocation_->mapped) + size_t(offset), size_t(bytes));
}
VkImage Image::handle() const { return allocation_ ? allocation_->image : VK_NULL_HANDLE; }
VkImageView Image::view() const { return allocation_ ? (alternateView_ ? allocation_->alternateView : allocation_->view) : VK_NULL_HANDLE; }
const ImageDescription& Image::description() const {
    if (!allocation_) throw std::logic_error("Access to an empty Vulkan image");
    return alternateView_ ? allocation_->alternateDescription : allocation_->description;
}
Image Image::samplingView(bool srgb) const {
    const auto format = description().format;
    const bool current = detail::isSRGB(format);
    Image result = *this;
    if (current != srgb) {
        if (!allocation_->alternateView) throw std::invalid_argument("Texture has no requested sRGB/linear view");
        result.alternateView_ = !alternateView_;
    }
    return result;
}
} // namespace sourcevk
