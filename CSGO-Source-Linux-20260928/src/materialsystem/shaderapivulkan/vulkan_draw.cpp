#include "vulkan_graphics_internal.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace sourcevk {
using detail::require;
struct DrawEncoder::Impl {
    struct SetBinding { std::shared_ptr<detail::DescriptorAllocation> set; std::vector<uint32_t> offsets; };
    Context& context;
    Frame frame;
    std::shared_ptr<detail::DeviceState> state;
    VkExtent2D extent;
    VkFormat color, depth;
    std::shared_ptr<detail::PipelineAllocation> pipeline;
    std::map<uint32_t, BufferSlice> vertices;
    std::map<uint32_t, SetBinding> descriptors;
    BufferSlice indices;
    VkIndexType indexType = VK_INDEX_TYPE_UINT16;
    VkRect2D scissor {};
    std::vector<bool> pushed;
    uint64_t draws = 0, token = 0;
    Impl(Context& owner, const Frame& target, VkExtent2D size, VkFormat colorFormat, VkFormat depthFormat)
        : context(owner), frame(target), state(detail::Access::state(context)), extent(size), color(colorFormat), depth(depthFormat) {
        token = detail::Access::claimDraw(context, frame);
        require(extent.width && extent.height && color != VK_FORMAT_UNDEFINED, "Invalid draw target extent/format");
        VkViewport viewport {0, 0, float(extent.width), float(extent.height), 0, 1};
        scissor.extent = extent;
        state->functions.vkCmdSetViewport(frame.commands, 0, 1, &viewport);
        state->functions.vkCmdSetScissor(frame.commands, 0, 1, &scissor);
    }
    void validate() const { detail::Access::validateDraw(context, frame, token); }
    void beforeDraw(uint32_t count, uint32_t instances, uint32_t firstVertex, uint32_t firstInstance, bool indexed) const {
        validate();
        require(pipeline && count && instances, "Drawing requires a pipeline and nonzero counts");
        for (const auto& binding : pipeline->vertexBindings) {
            const auto found = vertices.find(binding.binding);
            require(found != vertices.end(), "Pipeline vertex/instance stream is not bound");
            uint64_t elements = 1;
            if (binding.inputRate == VK_VERTEX_INPUT_RATE_INSTANCE) elements = uint64_t(firstInstance) + instances;
            else if (!indexed) elements = uint64_t(firstVertex) + count;
            require(!binding.stride || elements <= found->second.size / binding.stride, "Draw exceeds its vertex/instance buffer slice");
        }
        for (const auto& shader : {pipeline->vertex, pipeline->fragment}) {
            for (const auto& binding : shader->bindings) {
                const auto found = descriptors.find(binding.set);
                require(found != descriptors.end() && found->second.set->epoch == found->second.set->page->epoch,
                        "Required shader descriptor set is unbound or recycled");
            }
            require(!shader->pushConstants || std::all_of(pushed.begin(), pushed.end(), [](bool value) { return value; }),
                    "Push constants must be initialized before drawing");
        }
    }
};
DrawEncoder::DrawEncoder(Context& context, const Frame& frame)
    : DrawEncoder(context, frame, frame.extent, frame.colorFormat) {}
DrawEncoder::DrawEncoder(Context& context, const Frame& frame, VkExtent2D extent, VkFormat color, VkFormat depth)
    : impl_(std::make_unique<Impl>(context, frame, extent, color, depth)) {}
DrawEncoder::~DrawEncoder() = default;
uint64_t DrawEncoder::drawCalls() const { return impl_->draws; }
void DrawEncoder::pipeline(const GraphicsPipeline& pipeline) {
    auto& draw = *impl_;
    draw.validate();
    require(pipeline.allocation_ && pipeline.allocation_->state == draw.state &&
        pipeline.allocation_->colorFormat == draw.color && pipeline.allocation_->depthFormat == draw.depth,
        "Pipeline device/render-pass compatibility does not match the draw target");
    if (draw.pipeline == pipeline.allocation_) return;
    if (!draw.pipeline || draw.pipeline->layout != pipeline.allocation_->layout) {
        draw.descriptors.clear();
        draw.pushed.assign(pipeline.allocation_->layout->pushConstantBytes, false);
    }
    draw.pipeline = pipeline.allocation_;
    draw.state->functions.vkCmdBindPipeline(draw.frame.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
    detail::Access::retain(draw.context, draw.frame, pipeline.allocation_);
}
void DrawEncoder::descriptors(uint32_t set, const DescriptorSet& descriptors, const std::vector<uint32_t>& offsets) {
    auto& draw = *impl_;
    draw.validate();
    require(draw.pipeline && set < draw.pipeline->layout->sets.size(), "Bind a pipeline before its descriptor sets");
    const auto allocation = descriptors.allocation_;
    require(allocation && allocation->layout == draw.pipeline->layout->sets[set] &&
        allocation->frameSerial == detail::Access::serial(draw.context, draw.frame) && allocation->epoch == allocation->page->epoch,
        "Descriptor set belongs to another layout/frame or has been recycled");
    require(offsets.size() == allocation->dynamicBuffers.size(), "Dynamic descriptor offset count mismatch");
    for (size_t i = 0; i < offsets.size(); ++i) {
        const auto& buffer = allocation->dynamicBuffers[i];
        require(offsets[i] % buffer.alignment == 0 && buffer.offset <= buffer.capacity &&
            offsets[i] <= buffer.capacity - buffer.offset && buffer.range <= buffer.capacity - buffer.offset - offsets[i],
            "Dynamic descriptor offset is misaligned or out of bounds");
    }
    auto found = draw.descriptors.find(set);
    if (found != draw.descriptors.end() && found->second.set == allocation && found->second.offsets == offsets) return;
    const auto handle = descriptors.handle();
    draw.state->functions.vkCmdBindDescriptorSets(draw.frame.commands, VK_PIPELINE_BIND_POINT_GRAPHICS,
        draw.pipeline->layout->layout, set, 1, &handle, uint32_t(offsets.size()), offsets.data());
    draw.descriptors[set] = {allocation, offsets};
}
void DrawEncoder::vertexBuffer(uint32_t binding, const BufferSlice& buffer) {
    auto& draw = *impl_;
    draw.validate();
    require(binding < draw.state->caps.properties.limits.maxVertexInputBindings, "Vertex binding exceeds device limits");
    detail::validateSlice(buffer, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, draw.state);
    const auto found = draw.vertices.find(binding);
    const bool changed = found == draw.vertices.end() || found->second.buffer.handle() != buffer.buffer.handle() || found->second.offset != buffer.offset;
    draw.vertices[binding] = buffer;
    if (!changed) return;
    const auto handle = buffer.buffer.handle();
    draw.state->functions.vkCmdBindVertexBuffers(draw.frame.commands, binding, 1, &handle, &buffer.offset);
    draw.context.retain(draw.frame, buffer.buffer);
}
void DrawEncoder::indexBuffer(const BufferSlice& buffer, VkIndexType type) {
    auto& draw = *impl_;
    draw.validate();
    detail::validateSlice(buffer, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, draw.state);
    require(type == VK_INDEX_TYPE_UINT16 || type == VK_INDEX_TYPE_UINT32, "Only 16/32-bit index buffers are supported");
    const uint32_t bytes = type == VK_INDEX_TYPE_UINT16 ? 2 : 4;
    require(buffer.offset % bytes == 0 && buffer.size % bytes == 0, "Index buffer slice is not aligned");
    const bool changed = draw.indices.buffer.handle() != buffer.buffer.handle() || draw.indices.offset != buffer.offset || draw.indexType != type;
    draw.indices = buffer;
    draw.indexType = type;
    if (!changed) return;
    draw.state->functions.vkCmdBindIndexBuffer(draw.frame.commands, buffer.buffer.handle(), buffer.offset, type);
    draw.context.retain(draw.frame, buffer.buffer);
}
void DrawEncoder::scissor(VkRect2D rectangle) {
    auto& draw = *impl_;
    draw.validate();
    require(rectangle.offset.x >= 0 && rectangle.offset.y >= 0 &&
        uint64_t(rectangle.offset.x) + rectangle.extent.width <= draw.extent.width &&
        uint64_t(rectangle.offset.y) + rectangle.extent.height <= draw.extent.height, "Scissor exceeds the render target");
    if (draw.scissor.offset.x == rectangle.offset.x && draw.scissor.offset.y == rectangle.offset.y &&
        draw.scissor.extent.width == rectangle.extent.width && draw.scissor.extent.height == rectangle.extent.height) return;
    draw.state->functions.vkCmdSetScissor(draw.frame.commands, 0, 1, &rectangle);
    draw.scissor = rectangle;
}
void DrawEncoder::viewport(VkViewport viewport) {
    auto& draw = *impl_;
    draw.validate();
    require(std::isfinite(viewport.x) && std::isfinite(viewport.y) && std::isfinite(viewport.width) &&
        std::isfinite(viewport.height) && std::isfinite(viewport.minDepth) && std::isfinite(viewport.maxDepth) &&
        viewport.x >= 0 && viewport.y >= 0 && viewport.width > 0 && viewport.height > 0 &&
        viewport.x + viewport.width <= draw.extent.width && viewport.y + viewport.height <= draw.extent.height &&
        viewport.minDepth >= 0 && viewport.maxDepth <= 1 && viewport.minDepth <= viewport.maxDepth,
        "Viewport is invalid or outside its render target");
    draw.state->functions.vkCmdSetViewport(draw.frame.commands, 0, 1, &viewport);
}
void DrawEncoder::pushConstants(const void* data, uint32_t bytes, uint32_t offset) {
    auto& draw = *impl_;
    draw.validate();
    require(draw.pipeline && data && bytes && !((bytes | offset) & 3) && offset <= draw.pushed.size() && bytes <= draw.pushed.size() - offset,
        "Push constants are empty, unaligned or outside the pipeline range");
    draw.state->functions.vkCmdPushConstants(draw.frame.commands, draw.pipeline->layout->layout, detail::GraphicsStages, offset, bytes, data);
    std::fill(draw.pushed.begin() + offset, draw.pushed.begin() + offset + bytes, true);
}
void DrawEncoder::draw(uint32_t vertices, uint32_t instances, uint32_t firstVertex, uint32_t firstInstance) {
    auto& draw = *impl_;
    draw.beforeDraw(vertices, instances, firstVertex, firstInstance, false);
    draw.state->functions.vkCmdDraw(draw.frame.commands, vertices, instances, firstVertex, firstInstance);
    ++draw.draws;
}
void DrawEncoder::drawIndexed(uint32_t indices, uint32_t instances, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) {
    auto& draw = *impl_;
    draw.beforeDraw(indices, instances, 0, firstInstance, true);
    const uint32_t bytes = draw.indexType == VK_INDEX_TYPE_UINT16 ? 2 : 4;
    require(draw.indices.buffer && uint64_t(firstIndex) + indices <= draw.indices.size / bytes, "Indexed draw exceeds its index buffer slice");
    draw.state->functions.vkCmdDrawIndexed(draw.frame.commands, indices, instances, firstIndex, vertexOffset, firstInstance);
    ++draw.draws;
}
} // namespace sourcevk
