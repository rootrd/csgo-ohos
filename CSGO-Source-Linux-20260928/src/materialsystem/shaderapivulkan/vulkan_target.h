#pragma once
#include "vulkan_context.h"

namespace sourcevk {
// Synchronous initialization/readback-style operation, outside an active Frame.
// Tracks the resulting layout so later preserving render passes can load it.
void clearAttachmentImage(Context& context, const Image& image, const std::array<float,4>& color,
    VkImageAspectFlags aspects = VK_IMAGE_ASPECT_COLOR_BIT, float depth = 1, uint32_t stencil = 0);
// Before a frame, clear a viewport without discarding the rest of an attachment.
// Previously undefined pixels/aspects are initialized to transparent black / 1 / 0.
void clearAttachmentRegion(Context& context, const Image& image, const VkRect2D& rectangle,
    const std::array<float,4>& color, VkImageAspectFlags aspects = VK_IMAGE_ASPECT_COLOR_BIT,
    float depth = 1, uint32_t stencil = 0);
void blitAttachmentImage(Context& context, const Frame& frame, const Image& source, const Image& destination,
    const VkRect2D& sourceRect, const VkRect2D& destinationRect);

struct RenderTargetDescription {
    uint32_t width = 1, height = 1;
    VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkImageLayout colorFinalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkImageLayout depthFinalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
};

// Reusable offscreen color/depth target. The description constructor clears each
// pass; wrapping existing images preserves their color/depth/stencil contents.
class RenderTarget {
public:
    RenderTarget(Context& context, const RenderTargetDescription& description);
    // Preserve attachments across engine render-target switches.
    RenderTarget(Context& context, const Image& color, const Image& depth = {});
    ~RenderTarget();
    const Image& color() const;
    const Image& depth() const;
    VkRenderPass renderPass() const;
    VkExtent2D extent() const;
    void begin(const Frame& frame, const std::array<float, 4>& clear, float depth = 1);
    void end(const Frame& frame);
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
} // namespace sourcevk
