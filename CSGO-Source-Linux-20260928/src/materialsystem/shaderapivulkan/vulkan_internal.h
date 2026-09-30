#pragma once

#include "vulkan_context.h"
#include <vk_mem_alloc.h>

#include <atomic>
#include <utility>
#include <vector>

namespace sourcevk::detail {
inline VkFormat alternateColorSpace(VkFormat format) {
    switch(format) {
#define COLOR_SPACE_PAIR(linear,srgb) case linear:return srgb;case srgb:return linear;
    COLOR_SPACE_PAIR(VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_R8G8B8A8_SRGB)
    COLOR_SPACE_PAIR(VK_FORMAT_B8G8R8A8_UNORM,VK_FORMAT_B8G8R8A8_SRGB)
    COLOR_SPACE_PAIR(VK_FORMAT_BC1_RGB_UNORM_BLOCK,VK_FORMAT_BC1_RGB_SRGB_BLOCK)
    COLOR_SPACE_PAIR(VK_FORMAT_BC1_RGBA_UNORM_BLOCK,VK_FORMAT_BC1_RGBA_SRGB_BLOCK)
    COLOR_SPACE_PAIR(VK_FORMAT_BC2_UNORM_BLOCK,VK_FORMAT_BC2_SRGB_BLOCK)
    COLOR_SPACE_PAIR(VK_FORMAT_BC3_UNORM_BLOCK,VK_FORMAT_BC3_SRGB_BLOCK)
#undef COLOR_SPACE_PAIR
    default:return VK_FORMAT_UNDEFINED;
    }
}
inline bool isSRGB(VkFormat format) {
    return format==VK_FORMAT_R8G8B8A8_SRGB || format==VK_FORMAT_B8G8R8A8_SRGB ||
        format==VK_FORMAT_BC1_RGB_SRGB_BLOCK || format==VK_FORMAT_BC1_RGBA_SRGB_BLOCK ||
        format==VK_FORMAT_BC2_SRGB_BLOCK || format==VK_FORMAT_BC3_SRGB_BLOCK;
}
inline uint32_t compressedBlockBytes(VkFormat format) {
    switch(format) {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK:case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:return 8;
    case VK_FORMAT_BC2_UNORM_BLOCK:case VK_FORMAT_BC2_SRGB_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK:case VK_FORMAT_BC3_SRGB_BLOCK:return 16;
    default:return 0;
    }
}
struct DeviceState {
    LogSink logger;
    std::atomic<uint32_t> validationErrors {0};
    bool loaderLoaded = false;
    PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE, presentQueue = VK_NULL_HANDLE;
    InstanceFunctions instanceFunctions;
    DeviceFunctions functions;
    VmaAllocator allocator = VK_NULL_HANDLE;
    Capabilities caps;

    ~DeviceState();
    void log(const std::string& message) const noexcept;
};

struct BufferAllocation {
    std::shared_ptr<DeviceState> state;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    VkBufferUsageFlags usage = 0;
    MemoryAccess access = MemoryAccess::Device;
    void* mapped = nullptr;
    ~BufferAllocation();
};
struct ImageAllocation {
    std::shared_ptr<DeviceState> state;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageView alternateView = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    ImageDescription description;
    ImageDescription alternateDescription;
    VkImageLayout attachmentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool attachmentDefined = false;
    ~ImageAllocation();
};

// Private bridge for reusable subsystems. It preserves Context's frame checks
// without publishing queues, frame-slot indices or allocation internals.
struct Access {
    static auto state(const Context& context) { return context.deviceState(); }
    static auto state(const Buffer& buffer) { return buffer.allocation_ ? buffer.allocation_->state : nullptr; }
    static auto state(const Image& image) { return image.allocation_ ? image.allocation_->state : nullptr; }
    static auto allocation(const Image& image) { return image.allocation_; }
    static uint32_t slot(const Context& context, const Frame& frame) { context.checkFrame(frame); return frame.slot; }
    static uint64_t serial(const Context& context, const Frame& frame) { context.checkFrame(frame); return frame.serial; }
    static void validate(const Context& context, const Frame& frame) { context.checkFrame(frame); }
    static uint64_t claimDraw(Context& context, const Frame& frame) { return context.claimDrawFrame(frame); }
    static void validateDraw(const Context& context, const Frame& frame, uint64_t token) { context.checkDrawFrame(frame, token); }
    static void beginTarget(Context& context, const Frame& frame) { context.beginTargetPass(frame); }
    static void endTarget(Context& context, const Frame& frame) { context.endTargetPass(frame); }
    static void retain(Context& context, const Frame& frame, std::shared_ptr<void> object) {
        context.retainObject(frame, std::move(object));
    }
    static void submit(Context& context, VkCommandBuffer commands, VkFence complete) {
        context.submitUpload(commands, complete);
    }
};

std::shared_ptr<BufferAllocation> createBuffer(const std::shared_ptr<DeviceState>& state,
    VkDeviceSize size, VkBufferUsageFlags usage, MemoryAccess access);
std::shared_ptr<ImageAllocation> createImage(const std::shared_ptr<DeviceState>& state,
    const ImageDescription& description);

template<typename T, typename Function>
std::vector<T> enumerate(Function function, const char* operation) {
    for (;;) {
        uint32_t count = 0;
        check(function(&count, static_cast<T*>(nullptr)), operation);
        std::vector<T> values(count);
        if (!count) return values;
        const auto result = function(&count, values.data());
        if (result == VK_INCOMPLETE) continue;
        check(result, operation);
        values.resize(count);
        return values;
    }
}
} // namespace sourcevk::detail
