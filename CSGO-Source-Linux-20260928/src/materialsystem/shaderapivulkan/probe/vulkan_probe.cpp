#include "vulkan_probe.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace sourcevk {
namespace {
constexpr uint32_t TargetSize = 64, TextureSize = 16, BufferWords = 1024;
constexpr VkImageSubresourceRange ColorRange {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void imageBarrier(const DeviceFunctions& vk, VkCommandBuffer commands, VkImage image,
        VkImageLayout from, VkImageLayout to, VkAccessFlags source, VkAccessFlags destination,
        VkPipelineStageFlags sourceStage, VkPipelineStageFlags destinationStage) {
    VkImageMemoryBarrier barrier {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = source;
    barrier.dstAccessMask = destination;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = ColorRange;
    vk.vkCmdPipelineBarrier(commands, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void memoryBarrier(const DeviceFunctions& vk, VkCommandBuffer commands,
        VkAccessFlags source, VkAccessFlags destination,
        VkPipelineStageFlags sourceStage, VkPipelineStageFlags destinationStage) {
    VkMemoryBarrier barrier {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = source;
    barrier.dstAccessMask = destination;
    vk.vkCmdPipelineBarrier(commands, sourceStage, destinationStage, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}

// Four primary-color panels and a white center, using only Vulkan 1.0 commands.
// This intentionally tests attachments, not shaders or Source materials.
void pattern(const DeviceFunctions& vk, VkCommandBuffer commands, VkExtent2D extent) {
    const float colors[5][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 0, 1}, {1, 1, 1, 1}};
    const uint32_t halfW = extent.width / 2, halfH = extent.height / 2;
    const VkRect2D rectangles[5] = {
        {{0, 0}, {halfW, halfH}},
        {{int32_t(halfW), 0}, {extent.width - halfW, halfH}},
        {{0, int32_t(halfH)}, {halfW, extent.height - halfH}},
        {{int32_t(halfW), int32_t(halfH)}, {extent.width - halfW, extent.height - halfH}},
        {{int32_t(extent.width * 7 / 16), int32_t(extent.height * 7 / 16)},
         {extent.width / 8, extent.height / 8}}
    };
    for (unsigned i = 0; i < 5; ++i) {
        if (!rectangles[i].extent.width || !rectangles[i].extent.height) continue;
        VkClearAttachment attachment {};
        attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        std::copy_n(colors[i], 4, attachment.clearValue.color.float32);
        VkClearRect rectangle {rectangles[i], 0, 1};
        vk.vkCmdClearAttachments(commands, 1, &attachment, 1, &rectangle);
    }
}

struct RenderTarget {
    Context& context;
    Image color, depth;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    ~RenderTarget() {
        // Also covers exceptions during command recording or a timed-out readback.
        context.vk().vkDeviceWaitIdle(context.device());
        if (framebuffer) context.vk().vkDestroyFramebuffer(context.device(), framebuffer, nullptr);
        if (pass) context.vk().vkDestroyRenderPass(context.device(), pass, nullptr);
    }
};

class Diagnostic {
    Context& context;
    const ProbeOptions& options;
    Buffer gpu, uploadedReadback, textureReadback, colorReadback, depthReadback;
    Image texture;
    RenderTarget target;
    std::vector<uint32_t> words;
    std::vector<uint8_t> texels;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    uint32_t baselineAllocations = 0;
    bool retentionRecorded = false;

    void createTarget() {
        const auto& vk = context.vk();
        ImageDescription color;
        color.width = color.height = TargetSize;
        color.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        target.color = context.createImage(color);
        for (auto candidate : {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM}) {
            if (context.supportsFormat(candidate,
                    VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT)) {
                depthFormat = candidate;
                break;
            }
        }
        require(depthFormat != VK_FORMAT_UNDEFINED, "No depth format supports attachment and diagnostic readback");
        ImageDescription depth = color;
        depth.format = depthFormat;
        depth.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
        depth.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        target.depth = context.createImage(depth);
        VkAttachmentDescription attachments[2] {};
        for (auto& attachment : attachments) {
            attachment.samples = VK_SAMPLE_COUNT_1_BIT;
            attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        }
        attachments[0].format = color.format;
        attachments[1].format = depthFormat;
        VkAttachmentReference colorRef {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depthRef {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass {};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;
        subpass.pDepthStencilAttachment = &depthRef;
        VkSubpassDependency dependencies[2] {};
        dependencies[0].srcSubpass = dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass = dependencies[1].srcSubpass = 0;
        const VkPipelineStageFlags attachmentStages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        const VkAccessFlags attachmentWrites = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dependencies[0].dstStageMask = attachmentStages;
        dependencies[0].dstAccessMask = attachmentWrites;
        dependencies[1].srcStageMask = attachmentStages;
        dependencies[1].srcAccessMask = attachmentWrites;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        VkRenderPassCreateInfo pass {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        pass.attachmentCount = 2;
        pass.pAttachments = attachments;
        pass.subpassCount = 1;
        pass.pSubpasses = &subpass;
        pass.dependencyCount = 2;
        pass.pDependencies = dependencies;
        check(vk.vkCreateRenderPass(context.device(), &pass, nullptr, &target.pass), "diagnostic render pass");
        const VkImageView views[] = {target.color.view(), target.depth.view()};
        VkFramebufferCreateInfo framebuffer {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebuffer.renderPass = target.pass;
        framebuffer.attachmentCount = 2;
        framebuffer.pAttachments = views;
        framebuffer.width = framebuffer.height = TargetSize;
        framebuffer.layers = 1;
        check(vk.vkCreateFramebuffer(context.device(), &framebuffer, nullptr, &target.framebuffer), "diagnostic framebuffer");
    }

    void saveImage(const std::vector<uint8_t>& pixels) const {
        if (options.outputDirectory.empty()) return;
        const auto path = std::filesystem::path(options.outputDirectory) / "vulkan-readback.ppm";
        std::ofstream file(path, std::ios::binary);
        file << "P6\n" << TargetSize << " " << TargetSize << "\n255\n";
        for (size_t i = 0; i < pixels.size(); i += 4)
            file.write(reinterpret_cast<const char*>(pixels.data() + i), 3);
        file.close();
        require(bool(file), "Cannot save Vulkan readback: " + path.string());
    }

public:
    Diagnostic(Context& owner, const ProbeOptions& settings)
        : context(owner), options(settings), target {context}, words(BufferWords),
          texels(TextureSize * TextureSize * 4) {
        require(context.supportsFormat(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
            VK_FORMAT_FEATURE_TRANSFER_DST_BIT), "RGBA8 diagnostic format is unsupported");
        for (uint32_t i = 0; i < BufferWords; ++i) words[i] = 0xa17c390du ^ (i * 2654435761u);
        for (size_t i = 0; i < texels.size(); ++i) texels[i] = uint8_t((i * 73u + i / 7u) & 255u);
        gpu = context.createBuffer(words.size() * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT |
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, MemoryAccess::Device);
        auto upload = context.createBuffer(words.size() * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryAccess::Upload);
        auto textureUpload = context.createBuffer(texels.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryAccess::Upload);
        upload.write(0, words.data(), words.size() * 4);
        textureUpload.write(0, texels.data(), texels.size());
        ImageDescription description;
        description.width = description.height = TextureSize;
        description.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        texture = context.createImage(description);
        const auto& vk = context.vk();
        context.submitAndWait([&](VkCommandBuffer commands) {
            VkBufferCopy copy {0, 0, gpu.size()};
            vk.vkCmdCopyBuffer(commands, upload.handle(), gpu.handle(), 1, &copy);
            imageBarrier(vk, commands, texture.handle(), VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy region {};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {TextureSize, TextureSize, 1};
            vk.vkCmdCopyBufferToImage(commands, textureUpload.handle(), texture.handle(),
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            imageBarrier(vk, commands, texture.handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            memoryBarrier(vk, commands, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        });
        upload = {};
        textureUpload = {};
        createTarget();
        uploadedReadback = context.createBuffer(gpu.size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryAccess::Readback);
        textureReadback = context.createBuffer(texels.size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryAccess::Readback);
        colorReadback = context.createBuffer(TargetSize * TargetSize * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryAccess::Readback);
        depthReadback = context.createBuffer(TargetSize * TargetSize * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryAccess::Readback);
        baselineAllocations = context.allocationStatistics().allocations;
    }

    void verify(const std::string& reason) {
        const auto& vk = context.vk();
        context.submitAndWait([&](VkCommandBuffer commands) {
            VkClearValue values[2] {};
            values[0].color.float32[3] = 1;
            values[1].depthStencil = {0.25f, 0};
            VkRenderPassBeginInfo pass {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            pass.renderPass = target.pass;
            pass.framebuffer = target.framebuffer;
            pass.renderArea.extent = {TargetSize, TargetSize};
            pass.clearValueCount = 2;
            pass.pClearValues = values;
            vk.vkCmdBeginRenderPass(commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
            pattern(vk, commands, {TargetSize, TargetSize});
            vk.vkCmdEndRenderPass(commands);
            VkBufferCopy copy {0, 0, gpu.size()};
            vk.vkCmdCopyBuffer(commands, gpu.handle(), uploadedReadback.handle(), 1, &copy);
            auto readImage = [&](const Image& source, const Buffer& destination, uint32_t size, VkImageAspectFlags aspect) {
                VkBufferImageCopy region {};
                region.imageSubresource = {aspect, 0, 0, 1};
                region.imageExtent = {size, size, 1};
                vk.vkCmdCopyImageToBuffer(commands, source.handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    destination.handle(), 1, &region);
            };
            readImage(texture, textureReadback, TextureSize, VK_IMAGE_ASPECT_COLOR_BIT);
            readImage(target.color, colorReadback, TargetSize, VK_IMAGE_ASPECT_COLOR_BIT);
            readImage(target.depth, depthReadback, TargetSize, VK_IMAGE_ASPECT_DEPTH_BIT);
            memoryBarrier(vk, commands, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT);
        });
        std::vector<uint32_t> result(words.size());
        uploadedReadback.read(0, result.data(), result.size() * 4);
        require(result == words, "GPU buffer upload/readback mismatch: " + reason);
        std::vector<uint8_t> image(texels.size());
        textureReadback.read(0, image.data(), image.size());
        require(image == texels, "GPU texture upload/readback mismatch: " + reason);
        std::vector<uint8_t> pixels(TargetSize * TargetSize * 4);
        colorReadback.read(0, pixels.data(), pixels.size());
        saveImage(pixels); // Preserve the actual GPU image even if comparison fails.
        const std::array<uint8_t, 4> colors[] = {{255, 0, 0, 255}, {0, 255, 0, 255},
            {0, 0, 255, 255}, {255, 255, 0, 255}, {255, 255, 255, 255}};
        for (uint32_t y = 0; y < TargetSize; ++y) {
            for (uint32_t x = 0; x < TargetSize; ++x) {
                unsigned panel = (y >= 32 ? 2 : 0) + (x >= 32 ? 1 : 0);
                if (x >= 28 && x < 36 && y >= 28 && y < 36) panel = 4;
                require(std::equal(colors[panel].begin(), colors[panel].end(), pixels.begin() + (y * TargetSize + x) * 4),
                    "GPU color mismatch at " + std::to_string(x) + "," + std::to_string(y) + ": " + reason);
            }
        }
        if (depthFormat == VK_FORMAT_D32_SFLOAT) {
            std::vector<float> depths(TargetSize * TargetSize);
            depthReadback.read(0, depths.data(), depths.size() * sizeof(float));
            for (float depth : depths) require(std::isfinite(depth) && std::abs(depth - 0.25f) < 0.0001f, "GPU D32 clear/readback mismatch");
        } else {
            std::vector<uint16_t> depths(TargetSize * TargetSize);
            depthReadback.read(0, depths.data(), depths.size() * sizeof(uint16_t));
            for (uint16_t depth : depths) require(std::abs(int(depth) - 16384) <= 1, "GPU D16 clear/readback mismatch");
        }
        options.log("VK_RESOURCE_TEST_PASS: " + reason +
            " buffer_bytes=4096 texture_bytes=1024 color_pixels=4096 depth_pixels=4096 depth_format=" + std::to_string(depthFormat));
    }

    bool render() {
        Frame frame;
        if (!context.beginFrame(frame)) return false;
        if (!retentionRecorded) {
            auto temporary = context.createBuffer(256, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryAccess::Device);
            ImageDescription description;
            description.width = description.height = 8;
            description.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            auto temporaryImage = context.createImage(description);
            context.retain(frame, temporary);
            context.retain(frame, temporaryImage);
            context.retain(frame, gpu);
            VkBufferCopy copy {0, 0, 256};
            context.vk().vkCmdCopyBuffer(frame.commands, gpu.handle(), temporary.handle(), 1, &copy);
            imageBarrier(context.vk(), frame.commands, temporaryImage.handle(), VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            const VkClearColorValue white {{1, 1, 1, 1}};
            context.vk().vkCmdClearColorImage(frame.commands, temporaryImage.handle(),
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &ColorRange);
            temporary = {};
            temporaryImage = {};
            require(context.allocationStatistics().allocations == baselineAllocations + 2,
                "Frame retention released recorded GPU resources prematurely");
            retentionRecorded = true;
        }
        context.beginPresentPass(frame, {0, 0, 0, 1});
        pattern(context.vk(), frame.commands, frame.extent);
        context.endPresentPass(frame);
        return context.endFrame(frame);
    }

    void finish() {
        context.waitIdle();
        require(retentionRecorded, "Diagnostic never submitted a frame");
        require(context.allocationStatistics().allocations == baselineAllocations, "Completed frame retained temporary allocations");
        verify("final; original uploads retained");
        options.log("VK_LIFETIME_TEST_PASS: temporary buffer/image survived submission and were released after completion");
    }
};

struct Lifecycle {
    std::atomic<bool> paused {false}, quit {false};
    std::atomic<uint32_t> generation {0};
};
bool SDLCALL lifecycleEvent(void* userdata, SDL_Event* event) {
    auto& state = *static_cast<Lifecycle*>(userdata);
    if (event->type == SDL_EVENT_WILL_ENTER_BACKGROUND) state.paused = true;
    if (event->type == SDL_EVENT_DID_ENTER_FOREGROUND) { ++state.generation; state.paused = false; }
    if (event->type == SDL_EVENT_TERMINATING) state.quit = true;
    return true;
}
struct Watch {
    Lifecycle state;
    Watch() { require(SDL_AddEventWatch(lifecycleEvent, &state), SDL_GetError()); }
    ~Watch() { SDL_RemoveEventWatch(lifecycleEvent, &state); }
};
} // namespace

void runProbe(SDL_Window* window, const ProbeOptions& settings) {
    ProbeOptions options = settings;
    if (!options.log) options.log = [](const std::string& message) { SDL_Log("%s", message.c_str()); };
    try {
        require(options.seconds >= 1 && options.seconds <= 600, "Probe duration must be 1..600 seconds");
        require(!options.selfTest || options.seconds >= 5, "Desktop lifecycle test requires at least 5 seconds");
        if (!options.outputDirectory.empty()) std::filesystem::create_directories(options.outputDirectory);
        Watch lifecycle;
        std::atomic<uint32_t> validationMessages {0};
        auto owner = std::make_unique<Context>(window, ContextOptions {options.validation, [&](const std::string& message) {
            if (message.rfind("VK_VALIDATION_ERROR:", 0) == 0) ++validationMessages;
            options.log(message);
        }});
        Context& context = *owner;
        const VkDevice originalDevice = context.device();
        uint32_t resumed = lifecycle.state.generation;
        uint64_t lastSwapchain = 0, steadyFrames = 0, resourceChecks = 0;
        unsigned testStep = 0;
        bool quit = false, available = true, syntheticPause = false;
#ifdef __ANDROID__
        void* previousSurface = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr);
#endif
        const uint64_t started = SDL_GetTicks();
        uint64_t nextLog = started + 5000;
        {
            Diagnostic diagnostic(context, options);
            diagnostic.verify("initial upload");
            while (!quit && !lifecycle.state.quit && SDL_GetTicks() - started < uint64_t(options.seconds) * 1000) {
                SDL_Event event;
                while (SDL_PollEvent(&event)) {
                    switch (event.type) {
                    case SDL_EVENT_QUIT: case SDL_EVENT_WINDOW_CLOSE_REQUESTED: quit = true; break;
                    case SDL_EVENT_KEY_DOWN:
                        if (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_AC_BACK) quit = true;
                        break;
                    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: case SDL_EVENT_WINDOW_MINIMIZED:
                    case SDL_EVENT_WINDOW_RESTORED: context.requestResize(); break;
                    default: break;
                    }
                }
                if (quit || lifecycle.state.quit) break;
                const uint64_t elapsed = SDL_GetTicks() - started;
                if (options.selfTest) {
                    if (testStep == 0 && elapsed >= 1000) {
                        options.log("VK_DESKTOP_TEST: resize");
                        require(SDL_SetWindowSize(window, 800, 450), SDL_GetError());
                        context.requestResize();
                        ++testStep;
                    } else if (testStep == 1 && elapsed >= 2000) {
                        options.log("VK_DESKTOP_TEST: minimize");
                        require(SDL_MinimizeWindow(window), SDL_GetError());
                        syntheticPause = true;
                        ++testStep;
                    } else if (testStep == 2 && elapsed >= 2250) {
                        options.log("VK_DESKTOP_TEST: restore");
                        require(SDL_RestoreWindow(window), SDL_GetError());
                        // Wayland's restore request only removes maximization;
                        // activation is required to leave the minimized state.
                        require(SDL_RaiseWindow(window), SDL_GetError());
                        require(SDL_SetWindowSize(window, 960, 540), SDL_GetError());
                        syntheticPause = false;
                        ++testStep;
                    } else if (testStep == 3 && elapsed >= 3250) {
                        options.log("VK_DESKTOP_TEST: replace Surface");
                        context.refreshSurface();
                        ++testStep;
                    }
                }
                bool ready = !lifecycle.state.paused && !syntheticPause && !(SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED);
#ifdef __ANDROID__
                void* nativeSurface = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr);
                ready = ready && nativeSurface;
                if (ready && nativeSurface != previousSurface) context.refreshSurface();
                previousSurface = nativeSurface;
#endif
                if (ready && lifecycle.state.generation != resumed) {
                    resumed = lifecycle.state.generation;
                    context.refreshSurface();
                    options.log("VK_LIFECYCLE_RESUME: generation=" + std::to_string(resumed));
                }
                if (ready != available) {
                    context.setSurfaceAvailable(ready);
                    available = ready;
                    options.log(ready ? "VK_LIFECYCLE: foreground" : "VK_LIFECYCLE: background/minimized");
                }
                if (!ready) {
                    Frame frame;
                    const auto before = context.statistics();
                    require(!context.beginFrame(frame), "Suspended Surface unexpectedly acquired an image");
                    require(context.statistics().queueSubmissions == before.queueSubmissions, "Suspended frame submitted GPU work");
                    SDL_Delay(10);
                    continue;
                }
                const auto before = context.statistics();
                const bool presented = diagnostic.render();
                const auto after = context.statistics();
                if (presented && before.swapchainGeneration == after.swapchainGeneration) {
                    require(before.idleWaits == after.idleWaits, "Ordinary frame called vkDeviceWaitIdle");
                    ++steadyFrames;
                }
                if (presented && lastSwapchain != after.swapchainGeneration) {
                    require(context.device() == originalDevice, "Surface recreation replaced the Vulkan device");
                    diagnostic.verify("swapchain=" + std::to_string(after.swapchainGeneration) + "; same device and uploads");
                    lastSwapchain = after.swapchainGeneration;
                    ++resourceChecks;
                }
                if (!presented) SDL_Delay(2);
                if (SDL_GetTicks() >= nextLog) {
                    options.log("VK_RENDER_PROGRESS: frames=" + std::to_string(after.presentedFrames) +
                        " swapchains=" + std::to_string(after.swapchainGeneration) + " surfaces=" +
                        std::to_string(after.surfaceGeneration) + " idle_waits=" + std::to_string(after.idleWaits));
                    nextLog = SDL_GetTicks() + 5000;
                }
            }
            require(context.statistics().presentedFrames > 0, "No Vulkan frames were presented");
            if (options.selfTest) require(testStep == 4 && resourceChecks >= 4 && steadyFrames >= 30,
                "Desktop resize/minimize/Surface test did not complete");
            diagnostic.finish();
        }
        require(context.allocationStatistics().allocations == 0, "VMA allocations leaked after diagnostic cleanup");
        require(context.validationErrors() == 0, "Vulkan validation reported " + std::to_string(context.validationErrors()) + " error(s)");
        const std::string summary = "VK_PROBE_PASS: frames=" + std::to_string(context.statistics().presentedFrames) +
            " steady_frames=" + std::to_string(steadyFrames) + " swapchains=" + std::to_string(lastSwapchain) +
            " surfaces=" + std::to_string(context.statistics().surfaceGeneration) +
            " validation_errors=0 live_allocations=0";
        owner.reset();
        require(validationMessages == 0, "Vulkan validation reported errors during device cleanup");
        options.log(summary);
    } catch (const std::exception& error) {
        options.log("VK_PROBE_FAIL: " + std::string(error.what()));
        throw;
    }
}
} // namespace sourcevk
