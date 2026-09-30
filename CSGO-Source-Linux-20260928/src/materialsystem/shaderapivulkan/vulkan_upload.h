#pragma once

#include "vulkan_context.h"

namespace sourcevk {
namespace detail { struct PendingUpload; }

struct BufferUse {
    VkAccessFlags access = 0;
    VkPipelineStageFlags stages = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
};
struct ImageUse : BufferUse {
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

// Keep the ticket until ready(), then release it to reclaim staging pages.
// Dropping the last ticket early waits for its fence rather than freeing live
// resources. No device-wide idle or per-resource queue submission is used.
class UploadTicket {
public:
    bool ready() const;
    void wait(uint64_t timeoutNanoseconds = 5'000'000'000ull) const;
    explicit operator bool() const { return bool(pending_); }
private:
    friend class UploadBatch;
    std::shared_ptr<detail::PendingUpload> pending_;
};

// Single-use batch, submitted between frames on Context's graphics queue. A
// subsequent frame on that queue observes the declared consumer barriers.
// For updates to existing resources, describe their previous GPU use explicitly.
class UploadBatch {
public:
    explicit UploadBatch(Context& context, VkDeviceSize stagingPageBytes = 1024 * 1024);
    ~UploadBatch();
    UploadBatch(const UploadBatch&) = delete;
    UploadBatch& operator=(const UploadBatch&) = delete;
    void buffer(const BufferSlice& destination, const void* data,
                BufferUse before, BufferUse after);
    // Tightly packed byte R/RG/RGBA/BGRA, signed RG/RGBA, half/float R/RG/RGBA
    // or normalized RGBA16, one mip/layer.
    // Includes BC1/2/3 block rows and small terminal mips when BC is enabled.
    void image(const Image& destination, uint32_t mip, uint32_t layer,
               const void* data, VkDeviceSize bytes, ImageUse before, ImageUse after);
    UploadTicket submit();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Reuses host-visible pages per frame slot only after Context waited its fence.
// A slice can be used as a vertex/index/uniform/storage buffer. Uniform/storage
// alignment is applied automatically, with an explicit bounded per-frame budget.
class FrameArena {
public:
    explicit FrameArena(Context& context, VkDeviceSize pageBytes = 256 * 1024,
                        VkDeviceSize maximumBytesPerFrame = 16 * 1024 * 1024);
    ~FrameArena();
    FrameArena(const FrameArena&) = delete;
    FrameArena& operator=(const FrameArena&) = delete;
    BufferSlice write(const Frame& frame, const void* data, VkDeviceSize bytes,
                      VkDeviceSize alignment = 16);
    VkDeviceSize allocatedBytes() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace sourcevk
