#pragma once

#include "vulkan_dispatch.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

struct SDL_Window;

namespace sourcevk {
namespace detail {
struct BufferAllocation;
struct ImageAllocation;
struct DeviceState;
struct Access;
}

using LogSink = std::function<void(const std::string&)>;
inline constexpr uint32_t FrameSlotCount = 2;
struct ContextOptions {
    bool validation = false; // An explicit request fails if the layer is missing.
    LogSink log;
    bool blockCompression = true; // Use BC only when the device exposes it.
    bool anisotropicFiltering = true; // Optional core feature, queried separately.
};

// Available and enabled features are separate. The base path requires only
// VK_KHR_swapchain; fillModeNonSolid is enabled when available for wireframe.
struct Capabilities {
    VkPhysicalDeviceProperties properties {};
    VkPhysicalDeviceFeatures features {};
    VkPhysicalDeviceFeatures enabledFeatures {};
    bool shaderDrawParameters = false;
    bool descriptorIndexingQueried = false;
    bool runtimeDescriptorArray = false, descriptorBindingPartiallyBound = false;
    bool descriptorBindingVariableDescriptorCount = false, sampledImageNonUniformIndexing = false;
    uint32_t graphicsFamily = 0;
    uint32_t presentFamily = 0;
};

enum class MemoryAccess { Device, Upload, Readback };

// Small shared ownership handles. Resources can outlive Context, but all Vulkan
// objects must be released before SDL's video subsystem shuts down. A recorded
// GPU use must be covered by Context::retain or by another live owner until its
// completion fence. CPU mapping does not imply GPU/CPU synchronization.
class Buffer {
public:
    Buffer() = default;
    VkBuffer handle() const;
    VkDeviceSize size() const;
    VkBufferUsageFlags usage() const;
    explicit operator bool() const { return bool(allocation_); }
    void write(VkDeviceSize offset, const void* data, VkDeviceSize bytes) const;
    void read(VkDeviceSize offset, void* data, VkDeviceSize bytes) const;
private:
    friend class Context;
    friend struct detail::Access;
    std::shared_ptr<detail::BufferAllocation> allocation_;
};

struct BufferSlice {
    Buffer buffer;
    VkDeviceSize offset = 0, size = 0;
};

struct ImageDescription {
    uint32_t width = 1, height = 1, mipLevels = 1, layers = 1;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    // RGBA8/BGRA8/BC images may expose both linear and sRGB sampled views without
    // copying the texels. No optional Vulkan device feature is needed.
    bool srgbViews = false;
    bool cube = false;
};

class Image {
public:
    Image() = default;
    VkImage handle() const;
    VkImageView view() const; // Null for transfer-only images.
    const ImageDescription& description() const;
    Image samplingView(bool srgb) const;
    explicit operator bool() const { return bool(allocation_); }
private:
    friend class Context;
    friend struct detail::Access;
    std::shared_ptr<detail::ImageAllocation> allocation_;
    bool alternateView_ = false;
};

// Valid only between a successful beginFrame and endFrame, on the render thread.
// The pass uses UNDEFINED -> COLOR_ATTACHMENT -> PRESENT layouts and clears its
// attachment; sampled/offscreen images have explicit layouts owned by the caller.
struct Frame {
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkExtent2D extent {};
    VkFormat colorFormat = VK_FORMAT_UNDEFINED;
private:
    friend class Context;
    friend struct detail::Access;
    uint64_t serial = 0;
    uint32_t slot = 0, image = 0;
};

struct ContextStatistics {
    uint64_t submittedFrames = 0, presentedFrames = 0;
    uint64_t queueSubmissions = 0, idleWaits = 0;
    uint64_t swapchainGeneration = 0, surfaceGeneration = 0;
};
struct AllocationStatistics {
    uint32_t allocations = 0, blocks = 0;
    VkDeviceSize allocationBytes = 0, blockBytes = 0;
};

// Single render-thread owner; lifecycle callbacks only publish flags to that
// thread. The SDL window must outlive Context. Two frames may be in flight.
class Context {
public:
    explicit Context(SDL_Window* window, ContextOptions options = {});
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    const Capabilities& capabilities() const;
    const ContextStatistics& statistics() const;
    uint32_t validationErrors() const;
    AllocationStatistics allocationStatistics() const;
    const DeviceFunctions& vk() const;
    VkDevice device() const;
    bool supportsFormat(VkFormat format, VkFormatFeatureFlags features) const;

    Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, MemoryAccess access);
    Image createImage(const ImageDescription& description);

    // False means no image is available yet (paused, zero size, timeout or
    // swapchain invalidation). In that case no fence is reset and no frame began.
    bool beginFrame(Frame& frame);
    void beginPresentPass(const Frame& frame, const std::array<float, 4>& clear);
    void endPresentPass(const Frame& frame);
    bool endFrame(Frame& frame);
    void retain(const Frame& frame, const Buffer& buffer);
    void retain(const Frame& frame, const Image& image);
    // Poll a submitted frame once for all asynchronous consumers. Waiting is
    // reserved for the engine's explicit query flush, never ordinary polling.
    bool frameComplete(uint64_t serial, bool wait = false) const;

    void requestResize();
    void setSurfaceAvailable(bool available);
    void refreshSurface();
    void waitIdle(); // Resize/shutdown/diagnostics, never an ordinary frame.

    // Initialization and explicit readback only. Waits on its own fence, not the
    // device. Capture resource handles by value or keep them alive through return.
    void submitAndWait(const std::function<void(VkCommandBuffer)>& record);
    // Submit an offscreen prefix for explicit readback, preserving the Frame.
    // Acquire/present synchronization stays on its final submission.
    void submitFramePrefix(const Frame& frame);

private:
    friend struct detail::Access;
    std::shared_ptr<detail::DeviceState> deviceState() const;
    void checkFrame(const Frame& frame) const;
    uint64_t claimDrawFrame(const Frame& frame);
    void checkDrawFrame(const Frame& frame, uint64_t token) const;
    void beginTargetPass(const Frame& frame);
    void endTargetPass(const Frame& frame);
    void retainObject(const Frame& frame, std::shared_ptr<void> object);
    void submitUpload(VkCommandBuffer commands, VkFence complete);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace sourcevk
