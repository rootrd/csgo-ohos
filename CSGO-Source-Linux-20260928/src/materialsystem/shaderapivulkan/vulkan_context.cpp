#include "vulkan_internal.h"

#include "vulkan_platform.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace sourcevk {
const char* resultName(VkResult result) {
    switch (result) {
#define RESULT(value) case value: return #value;
        RESULT(VK_SUCCESS) RESULT(VK_NOT_READY) RESULT(VK_TIMEOUT) RESULT(VK_INCOMPLETE)
        RESULT(VK_ERROR_OUT_OF_HOST_MEMORY) RESULT(VK_ERROR_OUT_OF_DEVICE_MEMORY)
        RESULT(VK_ERROR_INITIALIZATION_FAILED) RESULT(VK_ERROR_DEVICE_LOST)
        RESULT(VK_ERROR_MEMORY_MAP_FAILED) RESULT(VK_ERROR_LAYER_NOT_PRESENT)
        RESULT(VK_ERROR_EXTENSION_NOT_PRESENT) RESULT(VK_ERROR_FEATURE_NOT_PRESENT)
        RESULT(VK_ERROR_INCOMPATIBLE_DRIVER) RESULT(VK_ERROR_FORMAT_NOT_SUPPORTED)
        RESULT(VK_ERROR_SURFACE_LOST_KHR) RESULT(VK_ERROR_OUT_OF_DATE_KHR)
        RESULT(VK_SUBOPTIMAL_KHR) RESULT(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR)
#undef RESULT
        default: return "unrecognized VkResult";
    }
}
Error::Error(VkResult result, const char* operation)
    : std::runtime_error(std::string(operation) + ": " + resultName(result) +
                         " (" + std::to_string(int(result)) + ")"), result_(result) {}
void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) throw Error(result, operation);
}

namespace detail {
void DeviceState::log(const std::string& message) const noexcept {
    try {
        if (logger) logger(message);
        else SDL_Log("%s", message.c_str());
    } catch (...) {
        // Never unwind across a Vulkan callback or a resource destructor.
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Vulkan log callback failed");
    }
}
DeviceState::~DeviceState() {
    if (device) {
        if (functions.vkDeviceWaitIdle) functions.vkDeviceWaitIdle(device);
        if (allocator) vmaDestroyAllocator(allocator);
        if (functions.vkDestroyDevice) functions.vkDestroyDevice(device, nullptr);
    }
    if (messenger && instanceFunctions.vkDestroyDebugUtilsMessengerEXT)
        instanceFunctions.vkDestroyDebugUtilsMessengerEXT(instance, messenger, nullptr);
    if (instance && instanceFunctions.vkDestroyInstance)
        instanceFunctions.vkDestroyInstance(instance, nullptr);
    if (loaderLoaded) SDL_Vulkan_UnloadLibrary();
}
} // namespace detail

namespace {
constexpr uint64_t GpuTimeout = 5'000'000'000ull;
constexpr uint64_t AcquireTimeout = 1'000'000ull;
constexpr uint32_t InvalidFamily = std::numeric_limits<uint32_t>::max();
constexpr uint32_t FramesInFlight = FrameSlotCount;

template<typename Function>
void load(Function& destination, PFN_vkVoidFunction source, const char* name) {
    destination = reinterpret_cast<Function>(source);
    if (!destination) throw std::runtime_error(std::string("Vulkan entry point missing: ") + name);
}
bool hasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name) {
    return std::any_of(extensions.begin(), extensions.end(), [name](const auto& item) {
        return std::strcmp(item.extensionName, name) == 0;
    });
}
std::string version(uint32_t value) {
    return std::to_string(VK_VERSION_MAJOR(value)) + "." + std::to_string(VK_VERSION_MINOR(value)) +
        "." + std::to_string(VK_VERSION_PATCH(value));
}
VKAPI_ATTR VkBool32 VKAPI_CALL debugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* message, void* userdata) {
    auto& state = *static_cast<detail::DeviceState*>(userdata);
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++state.validationErrors;
    try {
        state.log(std::string(severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ?
                  "VK_VALIDATION_ERROR: " : "VK_VALIDATION_WARNING: ") +
                  (message && message->pMessage ? message->pMessage : "missing message"));
    } catch (...) { /* callbacks cannot throw, including on allocation failure */ }
    return VK_FALSE;
}
} // namespace

struct Context::Impl {
    struct Slot {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer commands = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        VkFence complete = VK_NULL_HANDLE;
        uint64_t serial = 0;
        std::vector<std::shared_ptr<void>> retained;
    };
    struct Backbuffer {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        // Reacquiring THIS image establishes when its previous presentation
        // semaphore can be signalled again. A frame fence alone cannot do that.
        VkSemaphore rendered = VK_NULL_HANDLE;
    };
    std::shared_ptr<detail::DeviceState> state = std::make_shared<detail::DeviceState>();
    SDL_Window* window;
    bool validation, blockCompression, anisotropicFiltering;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkExtent2D extent {};
    VkFormat surfaceFormat = VK_FORMAT_UNDEFINED;
    std::vector<Backbuffer> images;
    std::array<Slot, FramesInFlight> slots;
    VkCommandPool immediatePool = VK_NULL_HANDLE;
    VkCommandBuffer immediateCommands = VK_NULL_HANDLE;
    VkFence immediateComplete = VK_NULL_HANDLE;
    ContextStatistics stats;
    bool available = true, dirty = true, surfaceRefresh = false, suboptimalLogged = false;
    bool active = false, passOpen = false, passCompleted = false, targetPass = false;
    uint64_t serial = 0, drawSerial = 0, submittedSerial = 0, completedSerial = 0;

    Impl(SDL_Window* target, ContextOptions options) : window(target), validation(options.validation),
        blockCompression(options.blockCompression), anisotropicFiltering(options.anisotropicFiltering) {
        if (!window) throw std::invalid_argument("Vulkan Context requires an SDL window");
        state->logger = std::move(options.log);
    }
    ~Impl() {
        if (state->device && state->functions.vkDeviceWaitIdle)
            state->functions.vkDeviceWaitIdle(state->device);
        destroySwapchain();
        if (surface) platform::destroySurface(state->getInstanceProcAddr, state->instance, surface);
        for (auto& slot : slots) {
            slot.retained.clear();
            destroySlot(slot);
        }
        const auto& vk = state->functions;
        if (immediateComplete) vk.vkDestroyFence(state->device, immediateComplete, nullptr);
        if (immediatePool) vk.vkDestroyCommandPool(state->device, immediatePool, nullptr);
    }
    void destroySlot(Slot& slot) {
        const auto& vk = state->functions;
        if (slot.complete) vk.vkDestroyFence(state->device, slot.complete, nullptr);
        if (slot.acquired) vk.vkDestroySemaphore(state->device, slot.acquired, nullptr);
        if (slot.pool) vk.vkDestroyCommandPool(state->device, slot.pool, nullptr);
    }
    void destroySwapchain() {
        const auto& vk = state->functions;
        for (const auto& image : images) {
            if (image.framebuffer) vk.vkDestroyFramebuffer(state->device, image.framebuffer, nullptr);
            if (image.view) vk.vkDestroyImageView(state->device, image.view, nullptr);
            if (image.rendered) vk.vkDestroySemaphore(state->device, image.rendered, nullptr);
        }
        images.clear();
        if (renderPass) vk.vkDestroyRenderPass(state->device, renderPass, nullptr);
        if (swapchain) vk.vkDestroySwapchainKHR(state->device, swapchain, nullptr);
        renderPass = VK_NULL_HANDLE;
        swapchain = VK_NULL_HANDLE;
        extent = {};
    }
    void requireInactive() const {
        if (active) throw std::logic_error("Vulkan operation requires a completed frame recording");
    }
    void validateFrame(const Frame& frame) const {
        if (!active || frame.serial != serial || !frame.commands || frame.slot >= slots.size() ||
            frame.commands != slots[frame.slot].commands || frame.image >= images.size())
            throw std::logic_error("Invalid or expired Vulkan frame");
    }
    void idle() {
        check(state->functions.vkDeviceWaitIdle(state->device), "vkDeviceWaitIdle");
        ++stats.idleWaits;
        for (auto& slot : slots) slot.retained.clear();
    }

    void createInstance() {
        if (!platform::loadVulkan()) throw std::runtime_error(SDL_GetError());
        state->loaderLoaded = true;
        state->getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
        if (!state->getInstanceProcAddr) throw std::runtime_error("SDL returned no Vulkan loader entry point");
        auto get = state->getInstanceProcAddr;
        PFN_vkEnumerateInstanceVersion enumerateVersion = nullptr;
        load(enumerateVersion, get(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"), "vkEnumerateInstanceVersion (Vulkan 1.1 required)");
        uint32_t loaderVersion = 0;
        check(enumerateVersion(&loaderVersion), "vkEnumerateInstanceVersion");
        if (loaderVersion < VK_API_VERSION_1_1) throw std::runtime_error("Vulkan 1.1 loader required");
        PFN_vkEnumerateInstanceExtensionProperties enumerateExtensions = nullptr;
        PFN_vkEnumerateInstanceLayerProperties enumerateLayers = nullptr;
        PFN_vkCreateInstance create = nullptr;
        load(enumerateExtensions, get(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties"), "vkEnumerateInstanceExtensionProperties");
        load(enumerateLayers, get(VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties"), "vkEnumerateInstanceLayerProperties");
        load(create, get(VK_NULL_HANDLE, "vkCreateInstance"), "vkCreateInstance");
        const auto supported = detail::enumerate<VkExtensionProperties>([&](auto* count, auto* data) {
            return enumerateExtensions(nullptr, count, data);
        }, "vkEnumerateInstanceExtensionProperties");
        const auto required = platform::instanceExtensions(window);
        std::vector<const char*> extensions(required.begin(), required.end());
        for (auto extension : extensions)
            if (!hasExtension(supported, extension))
                throw std::runtime_error(std::string("Required SDL Vulkan extension missing: ") + extension);
        const bool debugUtils = hasExtension(supported, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        if (debugUtils) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        const char* layer = "VK_LAYER_KHRONOS_validation";
        bool synchronizationValidation = false;
        if (validation) {
            const auto layers = detail::enumerate<VkLayerProperties>(enumerateLayers, "vkEnumerateInstanceLayerProperties");
            if (std::none_of(layers.begin(), layers.end(), [layer](const auto& item) {
                    return std::strcmp(item.layerName, layer) == 0;
                })) throw std::runtime_error("Requested VK_LAYER_KHRONOS_validation is unavailable");
            if (!debugUtils) throw std::runtime_error("Validation requires VK_EXT_debug_utils for error reporting");
            const auto layerExtensions = detail::enumerate<VkExtensionProperties>([&](auto* n, auto* data) {
                return enumerateExtensions(layer, n, data);
            }, "validation layer extensions");
            synchronizationValidation = hasExtension(layerExtensions, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
            if (synchronizationValidation) extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
        }
        VkDebugUtilsMessengerCreateInfoEXT debug {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debug.pfnUserCallback = debugMessage;
        debug.pUserData = state.get();
        VkValidationFeatureEnableEXT enable = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
        VkValidationFeaturesEXT validationFeatures {VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
        validationFeatures.enabledValidationFeatureCount = 1;
        validationFeatures.pEnabledValidationFeatures = &enable;
        validationFeatures.pNext = debugUtils ? &debug : nullptr;
        VkApplicationInfo application {VK_STRUCTURE_TYPE_APPLICATION_INFO};
        application.pApplicationName = "CSGO native Vulkan";
        application.pEngineName = "Source";
        application.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo info {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        info.pApplicationInfo = &application;
        info.enabledExtensionCount = uint32_t(extensions.size());
        info.ppEnabledExtensionNames = extensions.data();
        info.enabledLayerCount = validation ? 1 : 0;
        info.ppEnabledLayerNames = validation ? &layer : nullptr;
        info.pNext = synchronizationValidation ? static_cast<void*>(&validationFeatures)
                                               : (debugUtils ? static_cast<void*>(&debug) : nullptr);
        check(create(&info, nullptr, &state->instance), "vkCreateInstance");
#define LOAD_INSTANCE(name) load(state->instanceFunctions.name, get(state->instance, #name), #name);
        SOURCE_VK_INSTANCE_FUNCTIONS(LOAD_INSTANCE)
#undef LOAD_INSTANCE
        if (debugUtils) {
            auto& functions = state->instanceFunctions;
            load(functions.vkCreateDebugUtilsMessengerEXT, get(state->instance, "vkCreateDebugUtilsMessengerEXT"), "vkCreateDebugUtilsMessengerEXT");
            load(functions.vkDestroyDebugUtilsMessengerEXT, get(state->instance, "vkDestroyDebugUtilsMessengerEXT"), "vkDestroyDebugUtilsMessengerEXT");
            check(functions.vkCreateDebugUtilsMessengerEXT(state->instance, &debug, nullptr, &state->messenger),
                  "vkCreateDebugUtilsMessengerEXT");
        }
        state->log("VK_INSTANCE_READY: requested=1.1 loader=" + version(loaderVersion) +
            " validation=" + std::to_string(validation) + " synchronization_validation=" + std::to_string(synchronizationValidation));
    }

    void createSurface() {
        if (!platform::createSurface(window, state->instance, &surface))
            throw std::runtime_error(std::string("SDL_Vulkan_CreateSurface: ") + SDL_GetError());
        ++stats.surfaceGeneration;
        if (state->physical) {
            VkBool32 supported = VK_FALSE;
            check(state->instanceFunctions.vkGetPhysicalDeviceSurfaceSupportKHR(state->physical,
                state->caps.presentFamily, surface, &supported), "surface queue support after recreation");
            if (!supported) throw std::runtime_error("Existing Vulkan queue cannot present to the replacement Surface");
        }
        dirty = true;
    }

    void createDevice() {
        const auto& vk = state->instanceFunctions;
        const auto devices = detail::enumerate<VkPhysicalDevice>([&](auto* count, auto* data) {
            return vk.vkEnumeratePhysicalDevices(state->instance, count, data);
        }, "vkEnumeratePhysicalDevices");
        int bestScore = -1;
        bool portability = false;
        for (auto physical : devices) {
            Capabilities caps;
            vk.vkGetPhysicalDeviceProperties(physical, &caps.properties);
            if (caps.properties.apiVersion < VK_API_VERSION_1_1) continue;
            const auto extensions = detail::enumerate<VkExtensionProperties>([&](auto* count, auto* data) {
                return vk.vkEnumerateDeviceExtensionProperties(physical, nullptr, count, data);
            }, "vkEnumerateDeviceExtensionProperties");
            if (!hasExtension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;
            uint32_t count = 0;
            vk.vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
            std::vector<VkQueueFamilyProperties> queues(count);
            vk.vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, queues.data());
            uint32_t graphics = InvalidFamily, present = InvalidFamily;
            for (uint32_t i = 0; i < count; ++i) {
                if (!queues[i].queueCount) continue;
                VkBool32 canPresent = VK_FALSE;
                check(vk.vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, surface, &canPresent),
                      "vkGetPhysicalDeviceSurfaceSupportKHR");
                const bool canDraw = queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT;
                if (canDraw && canPresent) { graphics = present = i; break; }
                if (canDraw && graphics == InvalidFamily) graphics = i;
                if (canPresent && present == InvalidFamily) present = i;
            }
            if (graphics == InvalidFamily || present == InvalidFamily) continue;
            const auto formats = detail::enumerate<VkSurfaceFormatKHR>([&](auto* n, auto* data) {
                return vk.vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, n, data);
            }, "vkGetPhysicalDeviceSurfaceFormatsKHR");
            const auto modes = detail::enumerate<VkPresentModeKHR>([&](auto* n, auto* data) {
                return vk.vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, n, data);
            }, "vkGetPhysicalDeviceSurfacePresentModesKHR");
            if (formats.empty() || modes.empty()) continue;
            int score = caps.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 100 :
                (caps.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 80 : 10);
            if (graphics == present) ++score;
            if (score <= bestScore) continue;
            VkPhysicalDeviceShaderDrawParametersFeatures draw {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES};
            VkPhysicalDeviceDescriptorIndexingFeatures indexing {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
            caps.descriptorIndexingQueried = caps.properties.apiVersion >= VK_API_VERSION_1_2 ||
                hasExtension(extensions, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
            if (caps.descriptorIndexingQueried) draw.pNext = &indexing;
            VkPhysicalDeviceFeatures2 features {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            features.pNext = &draw;
            vk.vkGetPhysicalDeviceFeatures2(physical, &features);
            caps.features = features.features;
            caps.shaderDrawParameters = draw.shaderDrawParameters;
            caps.runtimeDescriptorArray = indexing.runtimeDescriptorArray;
            caps.descriptorBindingPartiallyBound = indexing.descriptorBindingPartiallyBound;
            caps.descriptorBindingVariableDescriptorCount = indexing.descriptorBindingVariableDescriptorCount;
            caps.sampledImageNonUniformIndexing = indexing.shaderSampledImageArrayNonUniformIndexing;
            caps.graphicsFamily = graphics;
            caps.presentFamily = present;
            state->caps = caps;
            state->physical = physical;
            portability = hasExtension(extensions, "VK_KHR_portability_subset");
            bestScore = score;
        }
        if (!state->physical) throw std::runtime_error("No Vulkan 1.1 device supports graphics and this SDL Surface");
        const float priority = 1;
        std::array<VkDeviceQueueCreateInfo, 2> queues {};
        queues[0].sType = queues[1].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queues[0].queueFamilyIndex = state->caps.graphicsFamily;
        queues[1].queueFamilyIndex = state->caps.presentFamily;
        queues[0].queueCount = queues[1].queueCount = 1;
        queues[0].pQueuePriorities = queues[1].pQueuePriorities = &priority;
        const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, "VK_KHR_portability_subset"};
        VkPhysicalDeviceFeatures enabled {};
        enabled.fillModeNonSolid = state->caps.features.fillModeNonSolid;
        enabled.textureCompressionBC = blockCompression && state->caps.features.textureCompressionBC;
        enabled.occlusionQueryPrecise = state->caps.features.occlusionQueryPrecise;
        enabled.samplerAnisotropy = anisotropicFiltering && state->caps.features.samplerAnisotropy;
        state->caps.enabledFeatures = enabled;
        VkDeviceCreateInfo info {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        info.queueCreateInfoCount = state->caps.graphicsFamily == state->caps.presentFamily ? 1 : 2;
        info.pQueueCreateInfos = queues.data();
        info.enabledExtensionCount = portability ? 2 : 1;
        info.ppEnabledExtensionNames = extensions;
        info.pEnabledFeatures = &enabled;
        check(vk.vkCreateDevice(state->physical, &info, nullptr, &state->device), "vkCreateDevice");
#define LOAD_DEVICE(name) load(state->functions.name, vk.vkGetDeviceProcAddr(state->device, #name), #name);
        SOURCE_VK_DEVICE_FUNCTIONS(LOAD_DEVICE)
#undef LOAD_DEVICE
        state->functions.vkGetDeviceQueue(state->device, state->caps.graphicsFamily, 0, &state->graphicsQueue);
        state->functions.vkGetDeviceQueue(state->device, state->caps.presentFamily, 0, &state->presentQueue);
        VmaVulkanFunctions functions {};
        functions.vkGetInstanceProcAddr = state->getInstanceProcAddr;
        functions.vkGetDeviceProcAddr = vk.vkGetDeviceProcAddr;
        VmaAllocatorCreateInfo allocator {};
        allocator.instance = state->instance;
        allocator.physicalDevice = state->physical;
        allocator.device = state->device;
        allocator.vulkanApiVersion = VK_API_VERSION_1_1;
        allocator.pVulkanFunctions = &functions;
        check(vmaCreateAllocator(&allocator, &state->allocator), "vmaCreateAllocator");
        const auto& caps = state->caps;
        state->log("VK_DEVICE_READY: " + std::string(caps.properties.deviceName) + " available_api=" +
            version(caps.properties.apiVersion) + " enabled_api=1.1 fillModeNonSolid=" +
            std::to_string(caps.enabledFeatures.fillModeNonSolid) + " textureCompressionBC=" +
            std::to_string(caps.enabledFeatures.textureCompressionBC) + " preciseOcclusionQueries=" +
            std::to_string(caps.enabledFeatures.occlusionQueryPrecise) + " samplerAnisotropy=" +
            std::to_string(caps.enabledFeatures.samplerAnisotropy) + " maxAnisotropy=" +
            std::to_string(caps.enabledFeatures.samplerAnisotropy ? caps.properties.limits.maxSamplerAnisotropy : 1.f) + " graphics_family=" +
            std::to_string(caps.graphicsFamily) + " present_family=" + std::to_string(caps.presentFamily));
        state->log("VK_AVAILABLE_FEATURES: multiDrawIndirect=" + std::to_string(caps.features.multiDrawIndirect) +
            " drawIndirectFirstInstance=" + std::to_string(caps.features.drawIndirectFirstInstance) +
            " shaderDrawParameters=" + std::to_string(caps.shaderDrawParameters) +
            " BC=" + std::to_string(caps.features.textureCompressionBC) +
            " ETC2=" + std::to_string(caps.features.textureCompressionETC2) +
            " ASTC=" + std::to_string(caps.features.textureCompressionASTC_LDR));
        state->log("VK_AVAILABLE_DESCRIPTOR_INDEXING: queried=" + std::to_string(caps.descriptorIndexingQueried) +
            " runtimeDescriptorArray=" + std::to_string(caps.runtimeDescriptorArray) +
            " partiallyBound=" + std::to_string(caps.descriptorBindingPartiallyBound) +
            " variableDescriptorCount=" + std::to_string(caps.descriptorBindingVariableDescriptorCount) +
            " sampledImageNonUniformIndexing=" + std::to_string(caps.sampledImageNonUniformIndexing) + " enabled=none");
    }

    void createCommands(VkCommandPool& pool, VkCommandBuffer& commands) {
        VkCommandPoolCreateInfo info {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        info.queueFamilyIndex = state->caps.graphicsFamily;
        check(state->functions.vkCreateCommandPool(state->device, &info, nullptr, &pool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo allocation {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool = pool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        check(state->functions.vkAllocateCommandBuffers(state->device, &allocation, &commands), "vkAllocateCommandBuffers");
    }
    void initialize() {
        createInstance();
        createSurface();
        createDevice();
        VkFenceCreateInfo fence {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VkSemaphoreCreateInfo semaphore {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (auto& slot : slots) {
            createCommands(slot.pool, slot.commands);
            check(state->functions.vkCreateFence(state->device, &fence, nullptr, &slot.complete), "vkCreateFence(frame)");
            check(state->functions.vkCreateSemaphore(state->device, &semaphore, nullptr, &slot.acquired), "vkCreateSemaphore(acquire)");
        }
        createCommands(immediatePool, immediateCommands);
        check(state->functions.vkCreateFence(state->device, &fence, nullptr, &immediateComplete), "vkCreateFence(upload)");
    }

    bool rebuildSwapchain() {
        int width = 0, height = 0;
        if (!platform::pixelSize(window, &width, &height)) throw std::runtime_error(SDL_GetError());
        if (width <= 0 || height <= 0 || (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)) return false;
        const auto& instance = state->instanceFunctions;
        VkSurfaceCapabilitiesKHR caps {};
        auto result = instance.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(state->physical, surface, &caps);
        if (result == VK_ERROR_SURFACE_LOST_KHR) { surfaceRefresh = true; return false; }
        check(result, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
        VkExtent2D target = caps.currentExtent;
        if (target.width == std::numeric_limits<uint32_t>::max()) {
            target.width = std::clamp(uint32_t(width), caps.minImageExtent.width, caps.maxImageExtent.width);
            target.height = std::clamp(uint32_t(height), caps.minImageExtent.height, caps.maxImageExtent.height);
        }
        if (!target.width || !target.height) return false;
        if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
            throw std::runtime_error("Vulkan Surface does not support color attachments");
        const auto formats = detail::enumerate<VkSurfaceFormatKHR>([&](auto* count, auto* data) {
            return instance.vkGetPhysicalDeviceSurfaceFormatsKHR(state->physical, surface, count, data);
        }, "vkGetPhysicalDeviceSurfaceFormatsKHR");
        if (formats.empty()) throw std::runtime_error("Vulkan Surface exposes no color formats");
        VkSurfaceFormatKHR selected = formats.front();
        if (selected.format == VK_FORMAT_UNDEFINED) selected.format = VK_FORMAT_B8G8R8A8_UNORM;
        for (auto preferred : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
                               VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB}) {
            auto found = std::find_if(formats.begin(), formats.end(), [preferred](const auto& format) {
                return format.format == preferred && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
            });
            if (found != formats.end()) { selected = *found; break; }
        }
        VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        for (auto candidate : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                               VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR}) {
            if (caps.supportedCompositeAlpha & candidate) { alpha = candidate; break; }
        }
        uint32_t count = caps.minImageCount + 1;
        if (caps.maxImageCount) count = std::min(count, caps.maxImageCount);
        if (swapchain) idle();
        destroySwapchain();
        const uint32_t families[] = {state->caps.graphicsFamily, state->caps.presentFamily};
        VkSwapchainCreateInfoKHR create {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        create.surface = surface;
        create.minImageCount = count;
        create.imageFormat = selected.format;
        create.imageColorSpace = selected.colorSpace;
        create.imageExtent = target;
        create.imageArrayLayers = 1;
        create.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        create.imageSharingMode = families[0] == families[1] ? VK_SHARING_MODE_EXCLUSIVE : VK_SHARING_MODE_CONCURRENT;
        create.queueFamilyIndexCount = families[0] == families[1] ? 0 : 2;
        create.pQueueFamilyIndices = families;
        create.preTransform = caps.currentTransform;
        create.compositeAlpha = alpha;
        create.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        create.clipped = VK_TRUE;
        const auto& vk = state->functions;
        result = vk.vkCreateSwapchainKHR(state->device, &create, nullptr, &swapchain);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_ERROR_SURFACE_LOST_KHR) {
            surfaceRefresh = result == VK_ERROR_SURFACE_LOST_KHR;
            return false;
        }
        check(result, "vkCreateSwapchainKHR");
        extent = target;
        surfaceFormat = selected.format;
        VkAttachmentDescription attachment {};
        attachment.format = surfaceFormat;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference reference {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass {};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &reference;
        VkSubpassDependency dependency {};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo pass {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        pass.attachmentCount = 1;
        pass.pAttachments = &attachment;
        pass.subpassCount = 1;
        pass.pSubpasses = &subpass;
        pass.dependencyCount = 1;
        pass.pDependencies = &dependency;
        check(vk.vkCreateRenderPass(state->device, &pass, nullptr, &renderPass), "vkCreateRenderPass(present)");
        const auto buffers = detail::enumerate<VkImage>([&](auto* n, auto* data) {
            return vk.vkGetSwapchainImagesKHR(state->device, swapchain, n, data);
        }, "vkGetSwapchainImagesKHR");
        images.resize(buffers.size());
        for (size_t i = 0; i < images.size(); ++i) {
            auto& image = images[i];
            image.image = buffers[i];
            VkImageViewCreateInfo view {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = image.image;
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = surfaceFormat;
            view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            check(vk.vkCreateImageView(state->device, &view, nullptr, &image.view), "vkCreateImageView(backbuffer)");
            VkFramebufferCreateInfo framebuffer {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            framebuffer.renderPass = renderPass;
            framebuffer.attachmentCount = 1;
            framebuffer.pAttachments = &image.view;
            framebuffer.width = extent.width;
            framebuffer.height = extent.height;
            framebuffer.layers = 1;
            check(vk.vkCreateFramebuffer(state->device, &framebuffer, nullptr, &image.framebuffer), "vkCreateFramebuffer(present)");
            VkSemaphoreCreateInfo semaphore {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            check(vk.vkCreateSemaphore(state->device, &semaphore, nullptr, &image.rendered), "vkCreateSemaphore(present image)");
        }
        dirty = false;
        suboptimalLogged = false;
        ++stats.swapchainGeneration;
        state->log("VK_SWAPCHAIN_READY: generation=" + std::to_string(stats.swapchainGeneration) + " extent=" +
            std::to_string(extent.width) + "x" + std::to_string(extent.height) + " images=" +
            std::to_string(images.size()) + " mode=FIFO format=" + std::to_string(surfaceFormat));
        return true;
    }

    void noteSuboptimal() {
        if (!suboptimalLogged) {
            state->log("VK_SUBOPTIMAL: preserving valid swapchain until resize or Surface invalidation");
            suboptimalLogged = true;
        }
    }
};

Context::Context(SDL_Window* window, ContextOptions options)
    : impl_(std::make_unique<Impl>(window, std::move(options))) {
    impl_->initialize();
}
Context::~Context() = default;
const Capabilities& Context::capabilities() const { return impl_->state->caps; }
const ContextStatistics& Context::statistics() const { return impl_->stats; }
uint32_t Context::validationErrors() const { return impl_->state->validationErrors; }
const DeviceFunctions& Context::vk() const { return impl_->state->functions; }
VkDevice Context::device() const { return impl_->state->device; }
std::shared_ptr<detail::DeviceState> Context::deviceState() const { return impl_->state; }
void Context::checkFrame(const Frame& frame) const { impl_->validateFrame(frame); }
uint64_t Context::claimDrawFrame(const Frame& frame) {
    impl_->validateFrame(frame);
    if (!impl_->passOpen) throw std::logic_error("Draw commands require an open render pass");
    return ++impl_->drawSerial;
}
void Context::checkDrawFrame(const Frame& frame, uint64_t token) const {
    impl_->validateFrame(frame);
    if (!impl_->passOpen || impl_->drawSerial != token)
        throw std::logic_error("Draw encoder is outside its pass or was replaced by another encoder");
}
void Context::beginTargetPass(const Frame& frame) {
    impl_->validateFrame(frame);
    if (impl_->passOpen) throw std::logic_error("Render passes cannot be nested");
    impl_->passOpen = impl_->targetPass = true;
    ++impl_->drawSerial;
}
void Context::endTargetPass(const Frame& frame) {
    impl_->validateFrame(frame);
    if (!impl_->passOpen || !impl_->targetPass) throw std::logic_error("No open offscreen render pass");
    impl_->passOpen = impl_->targetPass = false;
}
void Context::retainObject(const Frame& frame, std::shared_ptr<void> object) {
    impl_->validateFrame(frame);
    if (!object) throw std::invalid_argument("Cannot retain an empty GPU object");
    impl_->slots[frame.slot].retained.push_back(std::move(object));
}
void Context::submitUpload(VkCommandBuffer commands, VkFence complete) {
    // COW uploads can precede the submission of an in-progress frame recording.
    VkSubmitInfo submit {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;
    check(vk().vkQueueSubmit(impl_->state->graphicsQueue, 1, &submit, complete), "vkQueueSubmit(batch upload)");
    ++impl_->stats.queueSubmissions;
}
bool Context::supportsFormat(VkFormat format, VkFormatFeatureFlags features) const {
    VkFormatProperties properties {};
    impl_->state->instanceFunctions.vkGetPhysicalDeviceFormatProperties(impl_->state->physical, format, &properties);
    return (properties.optimalTilingFeatures & features) == features;
}
AllocationStatistics Context::allocationStatistics() const {
    VmaTotalStatistics statistics {};
    vmaCalculateStatistics(impl_->state->allocator, &statistics);
    const auto& total = statistics.total.statistics;
    return {total.allocationCount, total.blockCount, total.allocationBytes, total.blockBytes};
}
Buffer Context::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, MemoryAccess access) {
    Buffer result;
    result.allocation_ = detail::createBuffer(impl_->state, size, usage, access);
    return result;
}
Image Context::createImage(const ImageDescription& description) {
    Image result;
    result.allocation_ = detail::createImage(impl_->state, description);
    return result;
}
void Context::retain(const Frame& frame, const Buffer& buffer) {
    impl_->validateFrame(frame);
    if (!buffer || buffer.allocation_->state != impl_->state)
        throw std::invalid_argument("Cannot retain an empty buffer or a different device's buffer");
    impl_->slots[frame.slot].retained.push_back(buffer.allocation_);
}
void Context::retain(const Frame& frame, const Image& image) {
    impl_->validateFrame(frame);
    if (!image || image.allocation_->state != impl_->state)
        throw std::invalid_argument("Cannot retain an empty image or a different device's image");
    impl_->slots[frame.slot].retained.push_back(image.allocation_);
}
bool Context::frameComplete(uint64_t serial, bool wait) const {
    auto& c=*impl_;
    if(!serial || serial>c.submittedSerial)return false;
    if(serial<=c.completedSerial)return true;
    for(const auto& slot:c.slots)if(slot.serial==serial) {
        const auto result=wait?vk().vkWaitForFences(device(),1,&slot.complete,VK_TRUE,GpuTimeout):vk().vkGetFenceStatus(device(),slot.complete);
        if(result==VK_NOT_READY)return false;
        check(result,"occlusion query frame completion");
        c.completedSerial=std::max(c.completedSerial,serial);return true;
    }
    // Reusing a frame slot already waited for its previous submission.
    c.completedSerial=std::max(c.completedSerial,serial);return true;
}
void Context::requestResize() {
    int width = 0, height = 0;
    // SDL can deliver the same physical size through multiple window events.
    // Preserve a pending OUT_OF_DATE request, but do not create a new one when
    // the existing swapchain already has the requested pixel dimensions.
    if (impl_->swapchain && platform::pixelSize(impl_->window, &width, &height) &&
        width > 0 && height > 0 && uint32_t(width) == impl_->extent.width && uint32_t(height) == impl_->extent.height &&
        !(SDL_GetWindowFlags(impl_->window) & SDL_WINDOW_MINIMIZED)) return;
    impl_->dirty = true;
}
void Context::waitIdle() { impl_->requireInactive(); impl_->idle(); }
void Context::setSurfaceAvailable(bool available) {
    auto& c = *impl_;
    c.requireInactive();
    if (c.available == available) return;
    c.available = available;
    if (!available) {
        c.idle();
        c.destroySwapchain();
        if (c.surface) platform::destroySurface(c.state->getInstanceProcAddr, c.state->instance, c.surface);
        c.surface = VK_NULL_HANDLE;
        c.state->log("VK_SURFACE_SUSPENDED: device and regular resources retained");
    }
    c.dirty = true;
}
void Context::refreshSurface() { impl_->requireInactive(); impl_->surfaceRefresh = true; }

bool Context::beginFrame(Frame& frame) {
    auto& c = *impl_;
    c.requireInactive();
    frame = {};
    if (!c.available) return false;
    if (SDL_GetWindowFlags(c.window) & SDL_WINDOW_MINIMIZED) return false;
    int pixelWidth=0,pixelHeight=0;
    if (!platform::pixelSize(c.window,&pixelWidth,&pixelHeight)) throw std::runtime_error(SDL_GetError());
    if (pixelWidth<=0 || pixelHeight<=0) return false;
    // SDL3 completes window changes asynchronously, after the mode setter may
    // have requested a resize. This cached SDL query also catches user drags.
    if (uint32_t(pixelWidth)!=c.extent.width || uint32_t(pixelHeight)!=c.extent.height) c.dirty=true;
    if (c.surfaceRefresh) {
        if (c.surface || c.swapchain) c.idle();
        c.destroySwapchain();
        if (c.surface) platform::destroySurface(c.state->getInstanceProcAddr, c.state->instance, c.surface);
        c.surface = VK_NULL_HANDLE;
        c.surfaceRefresh = false;
    }
    try {
        if (!c.surface) c.createSurface();
        if ((c.dirty || !c.swapchain) && !c.rebuildSwapchain()) return false;
    } catch (const Error& error) {
        // A Surface can disappear between any two WSI queries, including the
        // format enumeration performed during reconstruction.
        if (error.result() == VK_ERROR_SURFACE_LOST_KHR) { c.surfaceRefresh = true; return false; }
        if (error.result() == VK_ERROR_OUT_OF_DATE_KHR) { c.dirty = true; return false; }
        throw;
    }
    auto& slot = c.slots[c.stats.submittedFrames % FramesInFlight];
    check(vk().vkWaitForFences(device(), 1, &slot.complete, VK_TRUE, GpuTimeout), "frame completion fence");
    c.completedSerial=std::max(c.completedSerial,slot.serial);
    slot.retained.clear();
    uint32_t index = 0;
    const auto result = vk().vkAcquireNextImageKHR(device(), c.swapchain, AcquireTimeout, slot.acquired, VK_NULL_HANDLE, &index);
    if (result == VK_TIMEOUT || result == VK_NOT_READY) return false;
    if (result == VK_ERROR_OUT_OF_DATE_KHR) { c.dirty = true; return false; }
    if (result == VK_ERROR_SURFACE_LOST_KHR) { c.surfaceRefresh = true; return false; }
    if (result == VK_SUBOPTIMAL_KHR) c.noteSuboptimal();
    else check(result, "vkAcquireNextImageKHR");
    if (index >= c.images.size()) throw std::runtime_error("Invalid acquired swapchain image index");
    check(vk().vkResetCommandPool(device(), slot.pool, 0), "vkResetCommandPool(frame)");
    VkCommandBufferBeginInfo begin {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vk().vkBeginCommandBuffer(slot.commands, &begin), "vkBeginCommandBuffer(frame)");
    c.active = true;
    c.passOpen = c.passCompleted = c.targetPass = false;
    frame.commands = slot.commands;
    frame.renderPass = c.renderPass;
    frame.framebuffer = c.images[index].framebuffer;
    frame.extent = c.extent;
    frame.colorFormat = c.surfaceFormat;
    frame.serial = ++c.serial;
    slot.serial=frame.serial;
    frame.slot = uint32_t(c.stats.submittedFrames % FramesInFlight);
    frame.image = index;
    return true;
}
void Context::beginPresentPass(const Frame& frame, const std::array<float, 4>& clear) {
    auto& c = *impl_;
    c.validateFrame(frame);
    if (c.passOpen || c.passCompleted) throw std::logic_error("Present pass already recorded");
    VkClearValue value {};
    std::copy(clear.begin(), clear.end(), value.color.float32);
    VkRenderPassBeginInfo pass {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = frame.renderPass;
    pass.framebuffer = frame.framebuffer;
    pass.renderArea.extent = frame.extent;
    pass.clearValueCount = 1;
    pass.pClearValues = &value;
    vk().vkCmdBeginRenderPass(frame.commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
    c.passOpen = true;
    ++c.drawSerial;
}
void Context::endPresentPass(const Frame& frame) {
    auto& c = *impl_;
    c.validateFrame(frame);
    if (!c.passOpen || c.targetPass) throw std::logic_error("No open present pass");
    vk().vkCmdEndRenderPass(frame.commands);
    c.passOpen = false;
    c.passCompleted = true;
}
bool Context::endFrame(Frame& frame) {
    auto& c = *impl_;
    c.validateFrame(frame);
    if (c.passOpen || !c.passCompleted) throw std::logic_error("Frame must complete its present pass before submission");
    auto& slot = c.slots[frame.slot];
    auto& image = c.images[frame.image];
    check(vk().vkEndCommandBuffer(frame.commands), "vkEndCommandBuffer(frame)");
    // Only reset when a real submission will signal this fence. Acquire timeout,
    // minimization and out-of-date paths leave it signalled.
    check(vk().vkResetFences(device(), 1, &slot.complete), "vkResetFences(frame)");
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &slot.acquired;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &frame.commands;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &image.rendered;
    check(vk().vkQueueSubmit(c.state->graphicsQueue, 1, &submit, slot.complete), "vkQueueSubmit(frame)");
    c.submittedSerial=frame.serial;
    ++c.stats.queueSubmissions;
    ++c.stats.submittedFrames;
    VkPresentInfoKHR present {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &image.rendered;
    present.swapchainCount = 1;
    present.pSwapchains = &c.swapchain;
    present.pImageIndices = &frame.image;
    const auto result = vk().vkQueuePresentKHR(c.state->presentQueue, &present);
    c.active = false;
    frame = {};
    if (result == VK_ERROR_OUT_OF_DATE_KHR) { c.dirty = true; return false; }
    if (result == VK_ERROR_SURFACE_LOST_KHR) { c.surfaceRefresh = true; return false; }
    if (result == VK_SUBOPTIMAL_KHR) c.noteSuboptimal();
    else check(result, "vkQueuePresentKHR");
    ++c.stats.presentedFrames;
    return true;
}
void Context::submitFramePrefix(const Frame& frame) {
    auto& c = *impl_;
    c.validateFrame(frame);
    if (c.passOpen || c.passCompleted) throw std::logic_error("Readback must close offscreen passes before presentation");
    auto& slot = c.slots[frame.slot];
    check(vk().vkEndCommandBuffer(frame.commands), "vkEndCommandBuffer(readback prefix)");
    check(vk().vkResetFences(device(), 1, &slot.complete), "vkResetFences(readback prefix)");
    VkSubmitInfo submit {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1; submit.pCommandBuffers = &frame.commands;
    check(vk().vkQueueSubmit(c.state->graphicsQueue, 1, &submit, slot.complete), "vkQueueSubmit(readback prefix)");
    ++c.stats.queueSubmissions;
    check(vk().vkWaitForFences(device(), 1, &slot.complete, VK_TRUE, GpuTimeout), "readback prefix fence");
    check(vk().vkResetCommandPool(device(), slot.pool, 0), "vkResetCommandPool(readback prefix)");
    VkCommandBufferBeginInfo begin {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vk().vkBeginCommandBuffer(frame.commands, &begin), "vkBeginCommandBuffer(after readback)");
    ++c.drawSerial;
}
void Context::submitAndWait(const std::function<void(VkCommandBuffer)>& record) {
    auto& c = *impl_;
    c.requireInactive();
    check(vk().vkWaitForFences(device(), 1, &c.immediateComplete, VK_TRUE, GpuTimeout), "previous upload fence");
    check(vk().vkResetCommandPool(device(), c.immediatePool, 0), "vkResetCommandPool(upload)");
    VkCommandBufferBeginInfo begin {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vk().vkBeginCommandBuffer(c.immediateCommands, &begin), "vkBeginCommandBuffer(upload)");
    record(c.immediateCommands);
    check(vk().vkEndCommandBuffer(c.immediateCommands), "vkEndCommandBuffer(upload)");
    check(vk().vkResetFences(device(), 1, &c.immediateComplete), "vkResetFences(upload)");
    VkSubmitInfo submit {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &c.immediateCommands;
    check(vk().vkQueueSubmit(c.state->graphicsQueue, 1, &submit, c.immediateComplete), "vkQueueSubmit(upload)");
    ++c.stats.queueSubmissions;
    const auto completed = vk().vkWaitForFences(device(), 1, &c.immediateComplete, VK_TRUE, GpuTimeout);
    // The caller may release staging resources as soon as this function exits,
    // including during exception unwinding. A timeout must not free live work.
    if (completed != VK_SUCCESS) vk().vkDeviceWaitIdle(device());
    check(completed, "upload/readback completion fence");
}
} // namespace sourcevk
