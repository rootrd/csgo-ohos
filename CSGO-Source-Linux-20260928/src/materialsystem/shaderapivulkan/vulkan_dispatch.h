#pragma once

#include <vulkan/vulkan.h>
#include <stdexcept>

// All entry points come from SDL's loader, including VMA's entry points. No
// second loader or statically linked Vulkan entry points are used by this module.
#define SOURCE_VK_INSTANCE_FUNCTIONS(F) \
    F(vkDestroyInstance) \
    F(vkEnumeratePhysicalDevices) \
    F(vkGetPhysicalDeviceProperties) \
    F(vkGetPhysicalDeviceFeatures2) \
    F(vkGetPhysicalDeviceQueueFamilyProperties) \
    F(vkGetPhysicalDeviceFormatProperties) \
    F(vkGetPhysicalDeviceImageFormatProperties) \
    F(vkGetPhysicalDeviceSurfaceSupportKHR) \
    F(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    F(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    F(vkGetPhysicalDeviceSurfacePresentModesKHR) \
    F(vkEnumerateDeviceExtensionProperties) \
    F(vkCreateDevice) \
    F(vkGetDeviceProcAddr)

#define SOURCE_VK_DEVICE_FUNCTIONS(F) \
    F(vkDestroyDevice) \
    F(vkGetDeviceQueue) \
    F(vkDeviceWaitIdle) \
    F(vkQueueWaitIdle) \
    F(vkCreateSwapchainKHR) \
    F(vkDestroySwapchainKHR) \
    F(vkGetSwapchainImagesKHR) \
    F(vkAcquireNextImageKHR) \
    F(vkQueuePresentKHR) \
    F(vkCreateImageView) \
    F(vkDestroyImageView) \
    F(vkCreateRenderPass) \
    F(vkDestroyRenderPass) \
    F(vkCreateFramebuffer) \
    F(vkDestroyFramebuffer) \
    F(vkCreateCommandPool) \
    F(vkDestroyCommandPool) \
    F(vkResetCommandPool) \
    F(vkAllocateCommandBuffers) \
    F(vkBeginCommandBuffer) \
    F(vkEndCommandBuffer) \
    F(vkCreateSemaphore) \
    F(vkDestroySemaphore) \
    F(vkCreateFence) \
    F(vkDestroyFence) \
    F(vkWaitForFences) \
    F(vkGetFenceStatus) \
    F(vkCreateQueryPool) \
    F(vkDestroyQueryPool) \
    F(vkCmdResetQueryPool) \
    F(vkCmdBeginQuery) \
    F(vkCmdEndQuery) \
    F(vkGetQueryPoolResults) \
    F(vkResetFences) \
    F(vkQueueSubmit) \
    F(vkCmdPipelineBarrier) \
    F(vkCmdCopyBuffer) \
    F(vkCmdCopyBufferToImage) \
    F(vkCmdCopyImageToBuffer) \
    F(vkCmdClearColorImage) \
    F(vkCmdBlitImage) \
    F(vkCmdClearDepthStencilImage) \
    F(vkCmdBeginRenderPass) \
    F(vkCmdEndRenderPass) \
    F(vkCmdClearAttachments) \
    F(vkCreateSampler) \
    F(vkDestroySampler) \
    F(vkCreateShaderModule) \
    F(vkDestroyShaderModule) \
    F(vkCreateDescriptorSetLayout) \
    F(vkDestroyDescriptorSetLayout) \
    F(vkCreateDescriptorPool) \
    F(vkDestroyDescriptorPool) \
    F(vkResetDescriptorPool) \
    F(vkAllocateDescriptorSets) \
    F(vkUpdateDescriptorSets) \
    F(vkCreatePipelineLayout) \
    F(vkDestroyPipelineLayout) \
    F(vkCreatePipelineCache) \
    F(vkDestroyPipelineCache) \
    F(vkGetPipelineCacheData) \
    F(vkCreateGraphicsPipelines) \
    F(vkDestroyPipeline) \
    F(vkCmdBindPipeline) \
    F(vkCmdBindDescriptorSets) \
    F(vkCmdBindVertexBuffers) \
    F(vkCmdBindIndexBuffer) \
    F(vkCmdSetViewport) \
    F(vkCmdSetScissor) \
    F(vkCmdPushConstants) \
    F(vkCmdDraw) \
    F(vkCmdDrawIndexed)

namespace sourcevk {
class Error : public std::runtime_error {
public:
    Error(VkResult result, const char* operation);
    VkResult result() const { return result_; }
private:
    VkResult result_;
};

struct InstanceFunctions {
#define SOURCE_VK_FIELD(name) PFN_##name name = nullptr;
    SOURCE_VK_INSTANCE_FUNCTIONS(SOURCE_VK_FIELD)
#undef SOURCE_VK_FIELD
    PFN_vkCreateDebugUtilsMessengerEXT vkCreateDebugUtilsMessengerEXT = nullptr;
    PFN_vkDestroyDebugUtilsMessengerEXT vkDestroyDebugUtilsMessengerEXT = nullptr;
};
struct DeviceFunctions {
#define SOURCE_VK_FIELD(name) PFN_##name name = nullptr;
    SOURCE_VK_DEVICE_FUNCTIONS(SOURCE_VK_FIELD)
#undef SOURCE_VK_FIELD
};

void check(VkResult result, const char* operation);
const char* resultName(VkResult result);
} // namespace sourcevk
