#include "vulkan_target.h"
#include "vulkan_graphics_internal.h"

#include <algorithm>
#include <cmath>

namespace sourcevk {
void clearAttachmentImage(Context& context, const Image& image, const std::array<float,4>& color,
        VkImageAspectFlags aspects, float depth, uint32_t stencil) {
    detail::require(image && (image.description().usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) && aspects &&
        !(aspects & ~image.description().aspect), "Invalid image/aspect for attachment initialization");
    auto allocation = detail::Access::allocation(image);
    const bool isColor = image.description().aspect == VK_IMAGE_ASPECT_COLOR_BIT;
    const auto finalLayout = isColor ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    context.submitAndWait([&](VkCommandBuffer commands) {
        VkImageMemoryBarrier barrier {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout=allocation->attachmentLayout; barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcAccessMask=barrier.oldLayout==VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        barrier.image=image.handle(); barrier.subresourceRange={image.description().aspect,0,1,0,1};
        context.vk().vkCmdPipelineBarrier(commands,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,0,nullptr,0,nullptr,1,&barrier);
        VkImageSubresourceRange range=barrier.subresourceRange;
        if (allocation->attachmentDefined) range.aspectMask=aspects;
        if (isColor) {
            VkClearColorValue value {}; std::copy(color.begin(),color.end(),value.float32);
            context.vk().vkCmdClearColorImage(commands,image.handle(),barrier.newLayout,&value,1,&range);
        } else {
            VkClearDepthStencilValue value {depth,stencil};
            context.vk().vkCmdClearDepthStencilImage(commands,image.handle(),barrier.newLayout,&value,1,&range);
        }
        barrier.oldLayout=barrier.newLayout; barrier.newLayout=finalLayout;
        barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        context.vk().vkCmdPipelineBarrier(commands,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0,0,nullptr,0,nullptr,1,&barrier);
    });
    allocation->attachmentLayout=finalLayout; allocation->attachmentDefined=true;
}

void clearAttachmentRegion(Context& context, const Image& image, const VkRect2D& rectangle,
        const std::array<float,4>& color, VkImageAspectFlags aspects, float depth, uint32_t stencil) {
    detail::require(image && aspects && !(aspects & ~image.description().aspect) &&
        image.description().mipLevels==1 && image.description().layers==1 &&
        detail::Access::state(image)==detail::Access::state(context),"Invalid attachment region clear");
    const auto& description=image.description();
    detail::require(rectangle.offset.x>=0 && rectangle.offset.y>=0 &&
        uint64_t(rectangle.offset.x)+rectangle.extent.width<=description.width &&
        uint64_t(rectangle.offset.y)+rectangle.extent.height<=description.height,
        "Attachment clear region is out of bounds");
    detail::require(std::isfinite(depth) && depth>=0 && depth<=1,"Invalid attachment clear depth");
    if(!rectangle.extent.width || !rectangle.extent.height)return;
    if(rectangle.offset.x==0 && rectangle.offset.y==0 && rectangle.extent.width==description.width &&
            rectangle.extent.height==description.height && (description.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        clearAttachmentImage(context,image,color,aspects,depth,stencil);return;
    }
    const bool isColor=description.aspect==VK_IMAGE_ASPECT_COLOR_BIT;
    detail::require(description.usage & (isColor?VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT:VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT),
        "Region clear requires an attachment image");
    // A temporary pass is needed only for pre-frame partial clears. Ordinary
    // frame clears keep using the current encoder/pass with no submission wait.
    struct ClearPass {
        std::shared_ptr<detail::DeviceState> state;
        VkRenderPass pass=VK_NULL_HANDLE;
        VkFramebuffer framebuffer=VK_NULL_HANDLE;
        ~ClearPass() {
            if(framebuffer)state->functions.vkDestroyFramebuffer(state->device,framebuffer,nullptr);
            if(pass)state->functions.vkDestroyRenderPass(state->device,pass,nullptr);
        }
    } temporary {detail::Access::state(context)};
    const auto allocation=detail::Access::allocation(image);
    VkAttachmentDescription attachment {};
    attachment.format=description.format;attachment.samples=VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp=allocation->attachmentDefined?VK_ATTACHMENT_LOAD_OP_LOAD:VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp=description.aspect & VK_IMAGE_ASPECT_STENCIL_BIT?attachment.loadOp:VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp=description.aspect & VK_IMAGE_ASPECT_STENCIL_BIT?VK_ATTACHMENT_STORE_OP_STORE:VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout=allocation->attachmentLayout;
    attachment.finalLayout=isColor?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference reference {0,isColor?VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass {};subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
    if(isColor) {subpass.colorAttachmentCount=1;subpass.pColorAttachments=&reference;}
    else subpass.pDepthStencilAttachment=&reference;
    const auto stages=isColor?VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT:
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    const auto access=isColor?VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT:
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkSubpassDependency dependencies[2] {};
    dependencies[0].srcSubpass=dependencies[1].dstSubpass=VK_SUBPASS_EXTERNAL;
    dependencies[0].srcStageMask=dependencies[1].dstStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dependencies[0].srcAccessMask=dependencies[1].dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    dependencies[0].dstStageMask=dependencies[1].srcStageMask=stages;
    dependencies[0].dstAccessMask=dependencies[1].srcAccessMask=access;
    VkRenderPassCreateInfo pass {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    pass.attachmentCount=1;pass.pAttachments=&attachment;pass.subpassCount=1;pass.pSubpasses=&subpass;
    pass.dependencyCount=2;pass.pDependencies=dependencies;
    check(context.vk().vkCreateRenderPass(context.device(),&pass,nullptr,&temporary.pass),"vkCreateRenderPass(region clear)");
    const auto view=image.view();
    VkFramebufferCreateInfo framebuffer {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebuffer.renderPass=temporary.pass;framebuffer.attachmentCount=1;framebuffer.pAttachments=&view;
    framebuffer.width=description.width;framebuffer.height=description.height;framebuffer.layers=1;
    check(context.vk().vkCreateFramebuffer(context.device(),&framebuffer,nullptr,&temporary.framebuffer),"vkCreateFramebuffer(region clear)");
    context.submitAndWait([&](VkCommandBuffer commands) {
        VkClearValue initial {};if(!isColor)initial.depthStencil={1,0};
        VkRenderPassBeginInfo begin {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.renderPass=temporary.pass;begin.framebuffer=temporary.framebuffer;
        begin.renderArea.extent={description.width,description.height};begin.clearValueCount=1;begin.pClearValues=&initial;
        context.vk().vkCmdBeginRenderPass(commands,&begin,VK_SUBPASS_CONTENTS_INLINE);
        VkClearAttachment clear {};clear.aspectMask=aspects;
        if(isColor)std::copy(color.begin(),color.end(),clear.clearValue.color.float32);
        else clear.clearValue.depthStencil={depth,stencil};
        const VkClearRect region {rectangle,0,1};
        context.vk().vkCmdClearAttachments(commands,1,&clear,1,&region);
        context.vk().vkCmdEndRenderPass(commands);
    });
    allocation->attachmentLayout=attachment.finalLayout;allocation->attachmentDefined=true;
}

void blitAttachmentImage(Context& context, const Frame& frame, const Image& source, const Image& destination,
        const VkRect2D& sourceRect, const VkRect2D& destinationRect) {
    detail::require(source && destination && source.handle()!=destination.handle() &&
        source.description().aspect==VK_IMAGE_ASPECT_COLOR_BIT && destination.description().aspect==VK_IMAGE_ASPECT_COLOR_BIT &&
        (source.description().usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) &&
        (destination.description().usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT), "Invalid Source color-target copy");
    auto validRect=[](const VkRect2D& rect,const Image& image) {
        return rect.offset.x>=0 && rect.offset.y>=0 && rect.extent.width && rect.extent.height &&
            uint64_t(rect.offset.x)+rect.extent.width<=image.description().width &&
            uint64_t(rect.offset.y)+rect.extent.height<=image.description().height;
    };
    detail::require(validRect(sourceRect,source) && validRect(destinationRect,destination),"Source target copy is out of bounds");
    detail::require(context.supportsFormat(source.description().format,VK_FORMAT_FEATURE_BLIT_SRC_BIT) &&
        context.supportsFormat(destination.description().format,VK_FORMAT_FEATURE_BLIT_DST_BIT),"Source target format cannot be blitted");
    auto src=detail::Access::allocation(source), dst=detail::Access::allocation(destination);
    detail::require(src->attachmentDefined,"Source target copy reads an uninitialized image");
    context.retain(frame,source); context.retain(frame,destination);
    auto transition=[&](const Image& image,VkImageLayout from,VkImageLayout to,VkAccessFlags before,VkAccessFlags after) {
        VkImageMemoryBarrier barrier {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout=from; barrier.newLayout=to; barrier.srcAccessMask=before; barrier.dstAccessMask=after;
        barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        barrier.image=image.handle(); barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        context.vk().vkCmdPipelineBarrier(frame.commands,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0,0,nullptr,0,nullptr,1,&barrier);
    };
    transition(source,src->attachmentLayout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,VK_ACCESS_TRANSFER_READ_BIT);
    transition(destination,dst->attachmentLayout,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        dst->attachmentLayout==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
    if (!dst->attachmentDefined) {
        VkClearColorValue black {};
        VkImageSubresourceRange range {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        context.vk().vkCmdClearColorImage(frame.commands,destination.handle(),VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&black,1,&range);
        transition(destination,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
    }
    VkImageBlit region {};
    region.srcSubresource=region.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    auto offsets=[](VkOffset3D (&out)[2],const VkRect2D& rect) {
        out[0]={rect.offset.x,rect.offset.y,0};
        out[1]={int32_t(rect.offset.x+rect.extent.width),int32_t(rect.offset.y+rect.extent.height),1};
    };
    offsets(region.srcOffsets,sourceRect); offsets(region.dstOffsets,destinationRect);
    const auto filter=context.supportsFormat(source.description().format,VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)?VK_FILTER_LINEAR:VK_FILTER_NEAREST;
    context.vk().vkCmdBlitImage(frame.commands,source.handle(),VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        destination.handle(),VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region,filter);
    transition(source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,src->attachmentLayout,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_MEMORY_READ_BIT);
    transition(destination,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_MEMORY_READ_BIT);
    dst->attachmentLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; dst->attachmentDefined=true;
}

struct RenderTarget::Impl {
    Context& context;
    std::shared_ptr<detail::DeviceState> state;
    Image color, depth;
    VkRenderPass pass = VK_NULL_HANDLE;
    std::array<VkRenderPass,3> loadPasses {};
    bool preserve = false;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkExtent2D extent {};
    uint64_t openSerial = 0;
    explicit Impl(Context& owner) : context(owner), state(detail::Access::state(context)) {}
    ~Impl() {
        if (framebuffer) state->functions.vkDestroyFramebuffer(state->device, framebuffer, nullptr);
        if (pass) state->functions.vkDestroyRenderPass(state->device, pass, nullptr);
        for (auto loaded : loadPasses) if (loaded) state->functions.vkDestroyRenderPass(state->device, loaded, nullptr);
    }
};
RenderTarget::RenderTarget(Context& context, const RenderTargetDescription& d) : impl_(std::make_shared<Impl>(context)) {
    detail::require(d.colorFinalLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL || d.colorFinalLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        "Offscreen color must finish in sampled or transfer-source layout");
    detail::require(d.depthFinalLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL || d.depthFinalLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        "Unsupported offscreen depth final layout");
    const auto& limits = context.capabilities().properties.limits;
    detail::require(d.width && d.height && d.width <= limits.maxFramebufferWidth && d.height <= limits.maxFramebufferHeight,
        "Offscreen extent exceeds framebuffer limits");
    auto& target = *impl_;
    target.extent = {d.width, d.height};
    ImageDescription color;
    color.width = d.width; color.height = d.height; color.format = d.colorFormat;
    color.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    target.color = context.createImage(color);
    if (d.depthFormat != VK_FORMAT_UNDEFINED) {
        ImageDescription depth = color;
        depth.format = d.depthFormat; depth.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
        depth.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        if (d.depthFinalLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) depth.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        target.depth = context.createImage(depth);
    }
    VkAttachmentDescription attachments[2] {};
    for (auto& attachment : attachments) {
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    attachments[0].format = d.colorFormat; attachments[0].finalLayout = d.colorFinalLayout;
    attachments[1].format = d.depthFormat; attachments[1].finalLayout = d.depthFinalLayout;
    VkAttachmentReference colorRef {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = target.depth ? &depthRef : nullptr;
    const auto stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    const auto writes = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkSubpassDependency dependencies[2] {};
    dependencies[0].srcSubpass = dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].srcStageMask = dependencies[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    dependencies[0].dstStageMask = dependencies[1].srcStageMask = stages;
    dependencies[0].dstAccessMask = writes | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    dependencies[1].srcAccessMask = writes;
    dependencies[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    VkRenderPassCreateInfo pass {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    pass.attachmentCount = target.depth ? 2 : 1; pass.pAttachments = attachments;
    pass.subpassCount = 1; pass.pSubpasses = &subpass;
    pass.dependencyCount = 2; pass.pDependencies = dependencies;
    check(context.vk().vkCreateRenderPass(context.device(), &pass, nullptr, &target.pass), "vkCreateRenderPass(offscreen)");
    const VkImageView views[] = {target.color.view(), target.depth.view()};
    VkFramebufferCreateInfo framebuffer {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebuffer.renderPass = target.pass; framebuffer.attachmentCount = pass.attachmentCount; framebuffer.pAttachments = views;
    framebuffer.width = d.width; framebuffer.height = d.height; framebuffer.layers = 1;
    check(context.vk().vkCreateFramebuffer(context.device(), &framebuffer, nullptr, &target.framebuffer), "vkCreateFramebuffer(offscreen)");
}
RenderTarget::RenderTarget(Context& context, const Image& color, const Image& depth)
    : impl_(std::make_shared<Impl>(context)) {
    detail::require(color && color.view() && (color.description().usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) &&
        color.description().mipLevels == 1 && color.description().layers == 1 &&
        detail::Access::state(color) == detail::Access::state(context), "Invalid engine color attachment");
    auto& target = *impl_;
    target.color = color; target.depth = depth; target.preserve = true;
    target.extent = {color.description().width, color.description().height};
    if (depth) detail::require((depth.description().usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) &&
        depth.description().width >= target.extent.width && depth.description().height >= target.extent.height &&
        detail::Access::state(depth) == target.state, "Invalid engine depth attachment");
    const bool stencil = depth && (depth.description().aspect & VK_IMAGE_ASPECT_STENCIL_BIT);
    for (uint32_t mask = 0; mask < 4; ++mask) {
        VkAttachmentDescription attachments[2] {};
        for (uint32_t i = 0; i < 2; ++i) {
            auto& attachment = attachments[i];
            attachment.samples = VK_SAMPLE_COUNT_1_BIT;
            attachment.loadOp = mask & (1u << i) ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachment.stencilLoadOp = stencil ? attachment.loadOp : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachment.stencilStoreOp = stencil ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachment.initialLayout = i ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        attachments[0].format = color.description().format;
        attachments[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        attachments[1].format = depth ? depth.description().format : VK_FORMAT_UNDEFINED;
        attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference colorRef {0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depthRef {1,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass {};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &colorRef;
        subpass.pDepthStencilAttachment = depth ? &depthRef : nullptr;
        VkSubpassDependency dependencies[2] {};
        dependencies[0].srcSubpass = dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[0].srcStageMask = dependencies[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        dependencies[0].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        dependencies[0].dstStageMask = dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        VkRenderPassCreateInfo pass {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        pass.attachmentCount = depth ? 2 : 1; pass.pAttachments = attachments;
        pass.subpassCount = 1; pass.pSubpasses = &subpass;
        pass.dependencyCount = 2; pass.pDependencies = dependencies;
        auto* destination = mask ? &target.loadPasses[mask-1] : &target.pass;
        check(context.vk().vkCreateRenderPass(context.device(), &pass, nullptr, destination), "vkCreateRenderPass(engine target)");
    }
    const VkImageView views[] = {color.view(),depth.view()};
    VkFramebufferCreateInfo framebuffer {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebuffer.renderPass = target.pass; framebuffer.attachmentCount = depth ? 2 : 1; framebuffer.pAttachments = views;
    framebuffer.width = target.extent.width; framebuffer.height = target.extent.height; framebuffer.layers = 1;
    check(context.vk().vkCreateFramebuffer(context.device(), &framebuffer, nullptr, &target.framebuffer), "vkCreateFramebuffer(engine target)");
}
RenderTarget::~RenderTarget() = default;
const Image& RenderTarget::color() const { return impl_->color; }
const Image& RenderTarget::depth() const { return impl_->depth; }
VkRenderPass RenderTarget::renderPass() const { return impl_->pass; }
VkExtent2D RenderTarget::extent() const { return impl_->extent; }
void RenderTarget::begin(const Frame& frame, const std::array<float, 4>& clear, float depth) {
    auto& target = *impl_;
    detail::require(std::isfinite(depth) && depth >= 0 && depth <= 1, "Depth clear is outside 0..1");
    uint32_t loadMask = 0;
    if (target.preserve) {
        detail::Access::validate(target.context, frame);
        auto transition = [&](const Image& image, VkImageLayout layout, uint32_t bit) {
            auto allocation = detail::Access::allocation(image);
            if (allocation->attachmentDefined) loadMask |= bit;
            VkImageMemoryBarrier barrier {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.oldLayout = allocation->attachmentLayout; barrier.newLayout = layout;
            barrier.srcAccessMask = barrier.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image.handle(); barrier.subresourceRange = {image.description().aspect,0,1,0,1};
            target.context.vk().vkCmdPipelineBarrier(frame.commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,nullptr, 0,nullptr, 1,&barrier);
        };
        transition(target.color,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,1);
        if (target.depth) transition(target.depth,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,2);
    }
    detail::Access::beginTarget(target.context, frame);
    target.openSerial = detail::Access::serial(target.context, frame);
    detail::Access::retain(target.context, frame, impl_);
    VkClearValue values[2] {};
    std::copy(clear.begin(), clear.end(), values[0].color.float32);
    values[1].depthStencil = {depth, 0};
    VkRenderPassBeginInfo pass {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = loadMask ? target.loadPasses[loadMask-1] : target.pass;
    pass.framebuffer = target.framebuffer; pass.renderArea.extent = target.extent;
    pass.clearValueCount = target.depth ? 2 : 1; pass.pClearValues = values;
    target.state->functions.vkCmdBeginRenderPass(frame.commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
}
void RenderTarget::end(const Frame& frame) {
    auto& target = *impl_;
    detail::require(target.openSerial && target.openSerial == detail::Access::serial(target.context, frame), "Offscreen pass belongs to another frame");
    detail::Access::endTarget(target.context, frame);
    target.state->functions.vkCmdEndRenderPass(frame.commands);
    target.openSerial = 0;
    if (target.preserve) {
        auto color = detail::Access::allocation(target.color);
        color->attachmentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; color->attachmentDefined = true;
        if (target.depth) {
            auto depth = detail::Access::allocation(target.depth);
            depth->attachmentLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL; depth->attachmentDefined = true;
        }
    }
}
} // namespace sourcevk
