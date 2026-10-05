#include "vulkan_presenter.h"

#if defined(DXVK_NATIVE_OHOS)
#include <native_buffer/buffer_common.h>
#include <native_window/external_window.h>
#include <ohos/dxvk_native_ohos.h>
#include <dlfcn.h>
#include <optional>
#endif

#include "../dxvk/dxvk_format.h"
#include "../dxvk/dxvk_winehua_trace.h"

namespace dxvk::vk {

#if defined(DXVK_NATIVE_OHOS)
  static std::shared_ptr<ohos::WindowState> autoRegisterFromSdl(HWND window, const PresenterDesc& desc) {
    using GetWindowPropertiesFn = uint32_t (*)(void*);
    using GetPointerPropertyFn = void* (*)(uint32_t, const char*, void*);
    using GetWindowSizeInPixelsFn = int (*)(void*, int*, int*);

    static void* sdlLib = []{
      void* h = dlopen("libSDL3.so", RTLD_NOW | RTLD_NOLOAD);
      if (!h) h = dlopen("libSDL3.so", RTLD_NOW);
      return h;
    }();

    if (!sdlLib) {
      Logger::warn("Presenter: autoRegisterFromSdl: libSDL3.so not found");
      return nullptr;
    }

    static auto getProps = (GetWindowPropertiesFn)dlsym(sdlLib, "SDL_GetWindowProperties");
    static auto getPtrProp = (GetPointerPropertyFn)dlsym(sdlLib, "SDL_GetPointerProperty");
    static auto getSize = (GetWindowSizeInPixelsFn)dlsym(sdlLib, "SDL_GetWindowSizeInPixels");

    if (!getProps || !getPtrProp || !getSize) {
      Logger::warn(str::format("Presenter: autoRegisterFromSdl: SDL symbol resolution failed",
        " getProps=", !!getProps, " getPtrProp=", !!getPtrProp, " getSize=", !!getSize));
      return nullptr;
    }

    void* sdlWindow = reinterpret_cast<void*>(window);
    uint32_t props = getProps(sdlWindow);
    if (!props) {
      Logger::warn("Presenter: autoRegisterFromSdl: SDL_GetWindowProperties returned 0");
      return nullptr;
    }

    // SDL 3.0.5 属性名是 SDL.window.openharmony.window（旧名 SDL.prop.* 不存在）
    void* nativeWindow = getPtrProp(props, "SDL.window.openharmony.window", nullptr);
    if (!nativeWindow)
      nativeWindow = getPtrProp(props, "SDL.prop.window.openharmony.window.pointer", nullptr);
    if (!nativeWindow) {
      Logger::warn("Presenter: autoRegisterFromSdl: OHNativeWindow not found in SDL properties");
      return nullptr;
    }

    int w = 0, h = 0;
    if (!getSize(sdlWindow, &w, &h) || w <= 0 || h <= 0) {
      if (desc.imageExtent.width && desc.imageExtent.height) {
        w = desc.imageExtent.width;
        h = desc.imageExtent.height;
      } else {
        Logger::warn("Presenter: autoRegisterFromSdl: window size is zero");
        return nullptr;
      }
    }

    DXVKOhosWindowHandle handle = 0;
    if (DXVKOhosRegisterWindow(static_cast<OHNativeWindow*>(nativeWindow),
        uint32_t(w), uint32_t(h), &handle) != DXVK_OHOS_WINDOW_OK) {
      Logger::warn("Presenter: autoRegisterFromSdl: DXVKOhosRegisterWindow failed");
      return nullptr;
    }

    Logger::info(str::format("Presenter: Auto-registered OHOS native window from SDL3: ",
      w, "x", h, " handle=", handle));

    return ohos::findWindow(handle);
  }
#endif

#if defined(DXVK_NATIVE_OHOS)
  static VkResult configureOhosNativeBuffers(OHNativeWindow* window, int32_t width, int32_t height) {
    if (!window || width <= 0 || height <= 0)
      return VK_ERROR_INITIALIZATION_FAILED;

    const int geometryStatus = OH_NativeWindow_NativeWindowHandleOpt(
      window, SET_BUFFER_GEOMETRY, width, height);
    if (geometryStatus != 0)
      return VK_ERROR_INITIALIZATION_FAILED;

    // XComponent/NativeWindow must match the RGBA present path. Leaving the
    // default format (often BGRA) while DXVK prefers RGBA swaps red and yellow.
    const int formatStatus = OH_NativeWindow_NativeWindowHandleOpt(
      window, SET_FORMAT, static_cast<int32_t>(NATIVEBUFFER_PIXEL_FMT_RGBA_8888));
    if (formatStatus != 0)
      return VK_ERROR_INITIALIZATION_FAILED;

    int32_t actualFormat = -1;
    OH_NativeWindow_NativeWindowHandleOpt(window, GET_FORMAT, &actualFormat);
    Logger::info(str::format(
      "Presenter: OpenHarmony native buffer ", width, "x", height,
      " format=", actualFormat, " rgba8888=",
      static_cast<int32_t>(NATIVEBUFFER_PIXEL_FMT_RGBA_8888)));
    return VK_SUCCESS;
  }
#endif

  template<typename T>
  static uint64_t winehuaHandleValue(T handle) {
#if VK_USE_64_BIT_PTR_DEFINES
    return reinterpret_cast<uintptr_t>(handle);
#else
    return uint64_t(handle);
#endif
  }

  Presenter::Presenter(
          HWND            window,
    const Rc<InstanceFn>& vki,
    const Rc<DeviceFn>&   vkd,
          PresenterDevice device,
    const PresenterDesc&  desc)
  : m_vki(vki), m_vkd(vkd), m_device(device), m_window(window) {
    // As of Wine 5.9, winevulkan provides this extension, but does
    // not filter the pNext chain for VkSwapchainCreateInfoKHR properly
    // before passing it to the Linux sude, which breaks RenderDoc.
    #if defined(DXVK_NATIVE_OHOS)
    m_device.features.fullScreenExclusive = false;
    m_nativeWindow = ohos::findWindow(reinterpret_cast<uintptr_t>(window));
    if (!m_nativeWindow)
      m_nativeWindow = autoRegisterFromSdl(window, desc);
    #else
    if (m_device.features.fullScreenExclusive && ::GetModuleHandle("winevulkan.dll")) {
      Logger::warn("winevulkan detected, disabling exclusive fullscreen support");
      m_device.features.fullScreenExclusive = false;
    }
    #endif

    try {
      #if defined(DXVK_NATIVE_OHOS)
      {
        ohos::WindowLease lease(m_nativeWindow);
        if (!lease.drawable())
          throw DxvkError("Native OpenHarmony window is not drawable");
        const auto bufferExtent = desc.imageExtent.width && desc.imageExtent.height
          ? desc.imageExtent : VkExtent2D { lease.width(), lease.height() };
        if (configureOhosNativeBuffers(
              static_cast<OHNativeWindow*>(lease.nativeWindow()),
              static_cast<int32_t>(bufferExtent.width),
              static_cast<int32_t>(bufferExtent.height)) != VK_SUCCESS)
          throw DxvkError("Failed to configure OpenHarmony native buffer geometry/format");
        Logger::info(str::format(
          "Presenter: OpenHarmony buffer geometry: display=",
          lease.width(), "x", lease.height(), " buffer=",
          bufferExtent.width, "x", bufferExtent.height));
      }
      #endif

      if (createSurface() != VK_SUCCESS)
        throw DxvkError("Failed to create surface");

      if (recreateSwapChain(desc) != VK_SUCCESS)
        throw DxvkError("Failed to create swap chain");
    } catch (...) {
      // A throwing constructor has no destructor to clean partial WSI state.
      destroySwapchain();
      destroySurface();
      throw;
    }
  }

  
  Presenter::~Presenter() {
    destroySwapchain();
    destroySurface();
  }


  PresenterInfo Presenter::info() const {
    return m_info;
  }


  PresenterImage Presenter::getImage(uint32_t index) const {
    return m_images.at(index);
  }


  VkResult Presenter::acquireNextImage(PresenterSync& sync, uint32_t& index, bool nonBlocking) {
    #if defined(DXVK_NATIVE_OHOS)
    ohos::WindowLease lease(m_nativeWindow);
    const auto windowStatus = checkNativeWindow(lease);
    if (windowStatus != VK_SUCCESS)
      return windowStatus;
    #endif

    sync = m_semaphores.at(m_frameIndex);

    // Don't acquire more than one image at a time
    if (m_acquireStatus == VK_NOT_READY) {
      m_acquireStatus = m_vkd->vkAcquireNextImageKHR(m_vkd->device(),
        m_swapchain,
        #if defined(DXVK_NATIVE_OHOS)
        nonBlocking ? 0 : 16000000, // Finite waits allow surface retirement.
        #else
        std::numeric_limits<uint64_t>::max(),
        #endif
        sync.acquire, VK_NULL_HANDLE, &m_imageIndex);
    }

    #if defined(DXVK_NATIVE_OHOS)
    if (m_acquireStatus == VK_TIMEOUT || m_acquireStatus == VK_NOT_READY) {
      const auto status = m_acquireStatus;
      m_acquireStatus = VK_NOT_READY;
      return status;
    }
    #endif
    
    if (m_acquireStatus != VK_SUCCESS && m_acquireStatus != VK_SUBOPTIMAL_KHR)
      return m_acquireStatus;
    
    index = m_imageIndex;
    #if defined(DXVK_NATIVE_OHOS)
    // Reacquiring an image orders reuse of that image's present semaphore.
    // A CPU frame index is not sufficient to prove presentation completion.
    sync.present = m_semaphores.at(m_imageIndex).present;
    #endif
    return m_acquireStatus;
  }


  VkResult Presenter::presentImage() {
    #if defined(DXVK_NATIVE_OHOS)
    // This runs on the submission thread, so validate here, not only at the
    // D3D11 entry point. Retirement waits for the actual Vulkan call to finish.
    std::optional<ohos::WindowLease> lease(std::in_place, m_nativeWindow);
    const auto windowStatus = checkNativeWindow(*lease);
    if (windowStatus != VK_SUCCESS)
      return windowStatus;
    if (m_acquireStatus != VK_SUCCESS && m_acquireStatus != VK_SUBOPTIMAL_KHR)
      return VK_NOT_READY;
    #endif

    PresenterSync sync = m_semaphores.at(m_frameIndex);
    #if defined(DXVK_NATIVE_OHOS)
    sync.present = m_semaphores.at(m_imageIndex).present;
    #endif

    VkPresentInfoKHR info;
    info.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.pNext              = nullptr;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores    = &sync.present;
    info.swapchainCount     = 1;
    info.pSwapchains        = &m_swapchain;
    info.pImageIndices      = &m_imageIndex;
    info.pResults           = nullptr;

    VkResult status = m_vkd->vkQueuePresentKHR(m_device.queue, &info);

    #if defined(DXVK_NATIVE_OHOS)
    // Never re-present an acquired image after an error. The frontend drains
    // the GPU and recreates the swapchain for out-of-date/surface-lost errors.
    m_acquireStatus = status < 0 ? status : VK_NOT_READY;
    #endif

    if (status != VK_SUCCESS && status != VK_SUBOPTIMAL_KHR)
      return status;

    m_frameIndex += 1;
    m_frameIndex %= m_semaphores.size();

    #if defined(DXVK_NATIVE_OHOS)
    // Do not hold the lifecycle lease while throttling or pre-acquiring.
    // Acquire is performed with a finite timeout at the next frame boundary.
    lease.reset();
    #else
    // Try to acquire next image already, in order to hide
    // potential delays from the application thread.
    sync = m_semaphores.at(m_frameIndex);

    m_acquireStatus = m_vkd->vkAcquireNextImageKHR(m_vkd->device(),
      m_swapchain, std::numeric_limits<uint64_t>::max(),
      sync.acquire, VK_NULL_HANDLE, &m_imageIndex);
    #endif

    bool vsync = m_info.presentMode == VK_PRESENT_MODE_FIFO_KHR
              || m_info.presentMode == VK_PRESENT_MODE_FIFO_RELAXED_KHR;

    m_fpsLimiter.delay(vsync);
    return status;
  }

  
  VkResult Presenter::recreateSwapChain(const PresenterDesc& desc) {
    #if defined(DXVK_NATIVE_OHOS)
    ohos::WindowLease lease(m_nativeWindow);
    if (!lease.active())
      return VK_ERROR_SURFACE_LOST_KHR;
    #endif

    const bool replaceSwapchain = m_swapchain != VK_NULL_HANDLE;
    if (replaceSwapchain)
      destroySwapchain();

    #if defined(DXVK_NATIVE_OHOS)
    if (!lease.drawable()) {
      m_info = { };
      m_windowRevision = lease.revision();
      return VK_SUCCESS;
    }
    {
      const VkExtent2D bufferExtent = desc.imageExtent.width && desc.imageExtent.height
        ? desc.imageExtent : VkExtent2D { lease.width(), lease.height() };
      if (configureOhosNativeBuffers(
            static_cast<OHNativeWindow*>(lease.nativeWindow()),
            static_cast<int32_t>(bufferExtent.width),
            static_cast<int32_t>(bufferExtent.height)) != VK_SUCCESS)
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    #endif

    // Query surface capabilities. Some properties might
    // have changed, including the size limits and supported
    // present modes, so we'll just query everything again.
    VkSurfaceCapabilitiesKHR        caps;
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR>   modes;

    VkResult status;
    
    if ((status = m_vki->vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        m_device.adapter, m_surface, &caps)) != VK_SUCCESS) {
      if (status == VK_ERROR_SURFACE_LOST_KHR) {
        // Recreate the surface and try again.
        if (m_surface)
          destroySurface();
        #if defined(DXVK_NATIVE_OHOS)
        status = createNativeSurface(lease.nativeWindow());
        #else
        status = createSurface();
        #endif
        if (status != VK_SUCCESS)
          return status;
        status = m_vki->vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
            m_device.adapter, m_surface, &caps);
      }
      if (status != VK_SUCCESS)
        return status;
    }

    if ((status = getSupportedFormats(formats, desc)) != VK_SUCCESS)
      return status;

    if ((status = getSupportedPresentModes(modes, desc)) != VK_SUCCESS)
      return status;

    if (formats.empty() || modes.empty())
      return VK_ERROR_INITIALIZATION_FAILED;

    // Select actual swap chain properties and create swap chain
    m_info.format       = pickFormat(formats.size(), formats.data(), desc.numFormats, desc.formats);
    m_info.presentMode  = pickPresentMode(modes.size(), modes.data(), desc.numPresentModes, desc.presentModes);
    m_info.imageExtent  = pickImageExtent(caps, desc.imageExtent);
    m_info.imageCount   = pickImageCount(caps, m_info.presentMode, desc.imageCount);

    if (!m_info.imageExtent.width || !m_info.imageExtent.height) {
      m_info.imageCount = 0;
      m_info.format     = { VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
      return VK_SUCCESS;
    }

    VkSurfaceFullScreenExclusiveInfoEXT fullScreenInfo;
    fullScreenInfo.sType            = VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT;
    fullScreenInfo.pNext            = nullptr;
    fullScreenInfo.fullScreenExclusive = desc.fullScreenExclusive;

    VkSwapchainCreateInfoKHR swapInfo;
    swapInfo.sType                  = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapInfo.pNext                  = nullptr;
    swapInfo.flags                  = 0;
    swapInfo.surface                = m_surface;
    swapInfo.minImageCount          = m_info.imageCount;
    swapInfo.imageFormat            = m_info.format.format;
    swapInfo.imageColorSpace        = m_info.format.colorSpace;
    swapInfo.imageExtent            = m_info.imageExtent;
    swapInfo.imageArrayLayers       = 1;
    swapInfo.imageUsage             = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                    | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    swapInfo.imageSharingMode       = VK_SHARING_MODE_EXCLUSIVE;
    swapInfo.queueFamilyIndexCount  = 0;
    swapInfo.pQueueFamilyIndices    = nullptr;
    swapInfo.preTransform           = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    swapInfo.compositeAlpha         = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapInfo.presentMode            = m_info.presentMode;
    swapInfo.clipped                = VK_TRUE;
    swapInfo.oldSwapchain           = VK_NULL_HANDLE;

    #if defined(DXVK_NATIVE_OHOS)
    // The blitter needs color-attachment usage. Transfer-dst is optional here.
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
    swapInfo.imageUsage &= caps.supportedUsageFlags;
    swapInfo.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
      ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
    if (!caps.supportedCompositeAlpha)
      return VK_ERROR_INITIALIZATION_FAILED;
    const auto alphaFlags = caps.supportedCompositeAlpha;
    swapInfo.compositeAlpha = static_cast<VkCompositeAlphaFlagBitsKHR>(
      alphaFlags & (~alphaFlags + 1u));
    #endif

    if (m_device.features.fullScreenExclusive)
      swapInfo.pNext = &fullScreenInfo;

    Logger::info(str::format(
      "Presenter: Actual swap chain properties:"
      "\n  Format:       ", m_info.format.format,
      "\n  Present mode: ", m_info.presentMode,
      "\n  Buffer size:  ", m_info.imageExtent.width, "x", m_info.imageExtent.height,
      "\n  Image count:  ", m_info.imageCount,
      "\n  Alpha mode:   ", swapInfo.compositeAlpha,
      "\n  Exclusive FS: ", desc.fullScreenExclusive));
    
    if ((status = m_vkd->vkCreateSwapchainKHR(m_vkd->device(),
        &swapInfo, nullptr, &m_swapchain)) != VK_SUCCESS)
      return status;
    
    // Acquire images and create views
    std::vector<VkImage> images;

    if ((status = getSwapImages(images)) != VK_SUCCESS)
      return status;
    
    // Update actual image count
    m_info.imageCount = images.size();
    m_images.resize(m_info.imageCount);

    for (uint32_t i = 0; i < m_info.imageCount; i++) {
      m_images[i].image = images[i];

      if (winehuaPresentImageTraceEnabled()) {
        Logger::info(str::format(
          "WineHuaPresentImage: layer=dxvk event=image-map swapchain=0x",
          std::hex, winehuaHandleValue(m_swapchain),
          " index=", std::dec, i,
          " image=0x", std::hex, winehuaHandleValue(images[i])));
      }

      VkImageViewCreateInfo viewInfo;
      viewInfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
      viewInfo.pNext    = nullptr;
      viewInfo.flags    = 0;
      viewInfo.image    = images[i];
      viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
      viewInfo.format   = m_info.format.format;
      viewInfo.components = VkComponentMapping {
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
      viewInfo.subresourceRange = {
        VK_IMAGE_ASPECT_COLOR_BIT,
        0, 1, 0, 1 };
      
      if ((status = m_vkd->vkCreateImageView(m_vkd->device(),
          &viewInfo, nullptr, &m_images[i].view)) != VK_SUCCESS)
        return status;
    }

    // Create one set of semaphores per swap image
    m_semaphores.resize(m_info.imageCount);

    for (uint32_t i = 0; i < m_semaphores.size(); i++) {
      VkSemaphoreCreateInfo semInfo;
      semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
      semInfo.pNext = nullptr;
      semInfo.flags = 0;

      if ((status = m_vkd->vkCreateSemaphore(m_vkd->device(),
          &semInfo, nullptr, &m_semaphores[i].acquire)) != VK_SUCCESS)
        return status;

      if ((status = m_vkd->vkCreateSemaphore(m_vkd->device(),
          &semInfo, nullptr, &m_semaphores[i].present)) != VK_SUCCESS)
        return status;
    }
    
    // Invalidate indices
    m_imageIndex = 0;
    m_frameIndex = 0;
    m_acquireStatus = VK_NOT_READY;
    #if defined(DXVK_NATIVE_OHOS)
    m_windowRevision = lease.revision();
    #endif
    return VK_SUCCESS;
  }


  void Presenter::setFrameRateLimit(double frameRate) {
    m_fpsLimiter.setTargetFrameRate(frameRate);
  }


  void Presenter::setFrameRateLimiterRefreshRate(double refreshRate) {
    m_fpsLimiter.setDisplayRefreshRate(refreshRate);
  }


  VkResult Presenter::getSupportedFormats(std::vector<VkSurfaceFormatKHR>& formats, const PresenterDesc& desc) {
    uint32_t numFormats = 0;

    VkSurfaceFullScreenExclusiveInfoEXT fullScreenInfo;
    fullScreenInfo.sType = VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT;
    fullScreenInfo.pNext = nullptr;
    fullScreenInfo.fullScreenExclusive = desc.fullScreenExclusive;

    VkPhysicalDeviceSurfaceInfo2KHR surfaceInfo;
    surfaceInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR;
    surfaceInfo.pNext = &fullScreenInfo;
    surfaceInfo.surface = m_surface;

    VkResult status;
    
    if (m_device.features.fullScreenExclusive) {
      status = m_vki->vkGetPhysicalDeviceSurfaceFormats2KHR(
        m_device.adapter, &surfaceInfo, &numFormats, nullptr);
    } else {
      status = m_vki->vkGetPhysicalDeviceSurfaceFormatsKHR(
        m_device.adapter, m_surface, &numFormats, nullptr);
    }

    if (status != VK_SUCCESS)
      return status;
    
    formats.resize(numFormats);

    if (m_device.features.fullScreenExclusive) {
      std::vector<VkSurfaceFormat2KHR> tmpFormats(numFormats, 
        { VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR, nullptr, VkSurfaceFormatKHR() });

      status = m_vki->vkGetPhysicalDeviceSurfaceFormats2KHR(
        m_device.adapter, &surfaceInfo, &numFormats, tmpFormats.data());

      for (uint32_t i = 0; i < numFormats; i++)
        formats[i] = tmpFormats[i].surfaceFormat;
    } else {
      status = m_vki->vkGetPhysicalDeviceSurfaceFormatsKHR(
        m_device.adapter, m_surface, &numFormats, formats.data());
    }

    return status;
  }

  
  VkResult Presenter::getSupportedPresentModes(std::vector<VkPresentModeKHR>& modes, const PresenterDesc& desc) {
    uint32_t numModes = 0;

    VkSurfaceFullScreenExclusiveInfoEXT fullScreenInfo;
    fullScreenInfo.sType = VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT;
    fullScreenInfo.pNext = nullptr;
    fullScreenInfo.fullScreenExclusive = desc.fullScreenExclusive;

    VkPhysicalDeviceSurfaceInfo2KHR surfaceInfo;
    surfaceInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR;
    surfaceInfo.pNext = &fullScreenInfo;
    surfaceInfo.surface = m_surface;

    VkResult status;

    if (m_device.features.fullScreenExclusive) {
      status = m_vki->vkGetPhysicalDeviceSurfacePresentModes2EXT(
        m_device.adapter, &surfaceInfo, &numModes, nullptr);
    } else {
      status = m_vki->vkGetPhysicalDeviceSurfacePresentModesKHR(
        m_device.adapter, m_surface, &numModes, nullptr);
    }

    if (status != VK_SUCCESS)
      return status;
    
    modes.resize(numModes);

    if (m_device.features.fullScreenExclusive) {
      status = m_vki->vkGetPhysicalDeviceSurfacePresentModes2EXT(
        m_device.adapter, &surfaceInfo, &numModes, modes.data());
    } else {
      status = m_vki->vkGetPhysicalDeviceSurfacePresentModesKHR(
        m_device.adapter, m_surface, &numModes, modes.data());
    }

    return status;
  }


  VkResult Presenter::getSwapImages(std::vector<VkImage>& images) {
    uint32_t imageCount = 0;

    VkResult status = m_vkd->vkGetSwapchainImagesKHR(
      m_vkd->device(), m_swapchain, &imageCount, nullptr);
    
    if (status != VK_SUCCESS)
      return status;
    
    images.resize(imageCount);

    return m_vkd->vkGetSwapchainImagesKHR(
      m_vkd->device(), m_swapchain, &imageCount, images.data());
  }


  VkSurfaceFormatKHR Presenter::pickFormat(
          uint32_t                  numSupported,
    const VkSurfaceFormatKHR*       pSupported,
          uint32_t                  numDesired,
    const VkSurfaceFormatKHR*       pDesired) {
    if (numDesired > 0) {
      // If the implementation allows us to freely choose
      // the format, we'll just use the preferred format.
      if (numSupported == 1 && pSupported[0].format == VK_FORMAT_UNDEFINED)
        return pDesired[0];
      
      // If the preferred format is explicitly listed in
      // the array of supported surface formats, use it
      for (uint32_t i = 0; i < numDesired; i++) {
        for (uint32_t j = 0; j < numSupported; j++) {
          if (pSupported[j].format     == pDesired[i].format
           && pSupported[j].colorSpace == pDesired[i].colorSpace)
            return pSupported[j];
        }
      }

      // If that didn't work, we'll fall back to a format
      // which has similar properties to the preferred one
      DxvkFormatFlags prefFlags = imageFormatInfo(pDesired[0].format)->flags;

      for (uint32_t j = 0; j < numSupported; j++) {
        auto currFlags = imageFormatInfo(pSupported[j].format)->flags;

        if ((currFlags & DxvkFormatFlag::ColorSpaceSrgb)
         == (prefFlags & DxvkFormatFlag::ColorSpaceSrgb))
          return pSupported[j];
      }
    }
    
    // Otherwise, fall back to the first supported format
    return pSupported[0];
  }


  VkPresentModeKHR Presenter::pickPresentMode(
          uint32_t                  numSupported,
    const VkPresentModeKHR*         pSupported,
          uint32_t                  numDesired,
    const VkPresentModeKHR*         pDesired) {
    // Just pick the first desired and supported mode
    for (uint32_t i = 0; i < numDesired; i++) {
      for (uint32_t j = 0; j < numSupported; j++) {
        if (pSupported[j] == pDesired[i])
          return pSupported[j];
      }
    }
    
    // Guaranteed to be available
    return VK_PRESENT_MODE_FIFO_KHR;
  }


  VkExtent2D Presenter::pickImageExtent(
    const VkSurfaceCapabilitiesKHR& caps,
          VkExtent2D                desired) {
    if (caps.currentExtent.width != std::numeric_limits<uint32_t>::max())
      return caps.currentExtent;
    
    VkExtent2D actual;
    actual.width  = clamp(desired.width,  caps.minImageExtent.width,  caps.maxImageExtent.width);
    actual.height = clamp(desired.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    return actual;
  }


  uint32_t Presenter::pickImageCount(
    const VkSurfaceCapabilitiesKHR& caps,
          VkPresentModeKHR          presentMode,
          uint32_t                  desired) {
    uint32_t count = caps.minImageCount;
    
    if (presentMode != VK_PRESENT_MODE_IMMEDIATE_KHR)
      count = caps.minImageCount + 1;
    
    if (count < desired)
      count = desired;
    
    if (count > caps.maxImageCount && caps.maxImageCount != 0)
      count = caps.maxImageCount;
    
    return count;
  }


  VkResult Presenter::createSurface() {
    #if defined(DXVK_NATIVE_OHOS)
    ohos::WindowLease lease(m_nativeWindow);
    return createNativeSurface(lease.nativeWindow());
    #else
    HINSTANCE instance = reinterpret_cast<HINSTANCE>(
      GetWindowLongPtr(m_window, GWLP_HINSTANCE));
    
    VkWin32SurfaceCreateInfoKHR info;
    info.sType      = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    info.pNext      = nullptr;
    info.flags      = 0;
    info.hinstance  = instance;
    info.hwnd       = m_window;
    
    VkResult status = m_vki->vkCreateWin32SurfaceKHR(
      m_vki->instance(), &info, nullptr, &m_surface);
    
    if (status != VK_SUCCESS)
      return status;
    
    VkBool32 supportStatus = VK_FALSE;

    if ((status = m_vki->vkGetPhysicalDeviceSurfaceSupportKHR(m_device.adapter,
        m_device.queueFamily, m_surface, &supportStatus)) != VK_SUCCESS)
      return status;
    
    if (!supportStatus) {
      destroySurface();
      return VK_ERROR_OUT_OF_HOST_MEMORY; // just abuse this
    }

    return VK_SUCCESS;
    #endif
  }


  #if defined(DXVK_NATIVE_OHOS)
  VkResult Presenter::createNativeSurface(void* window) {
    if (!window)
      return VK_ERROR_SURFACE_LOST_KHR;
    if (!m_vki->vkCreateSurfaceOHOS)
      return VK_ERROR_EXTENSION_NOT_PRESENT;

    VkSurfaceCreateInfoOHOS info = { };
    // VK_STRUCTURE_TYPE_SURFACE_CREATE_INFO_OHOS in the pinned NDK. The
    // legacy core enum predates it; the SDK ABI test checks this value.
    info.sType = static_cast<VkStructureType>(1000685000);
    info.window = static_cast<OHNativeWindow*>(window);
    auto status = m_vki->vkCreateSurfaceOHOS(m_vki->instance(), &info, nullptr, &m_surface);
    if (status != VK_SUCCESS)
      return status;

    VkBool32 supported = VK_FALSE;
    status = m_vki->vkGetPhysicalDeviceSurfaceSupportKHR(m_device.adapter,
      m_device.queueFamily, m_surface, &supported);
    if (status != VK_SUCCESS || !supported) {
      destroySurface();
      return status != VK_SUCCESS ? status : VK_ERROR_INITIALIZATION_FAILED;
    }
    return VK_SUCCESS;
  }


  VkResult Presenter::checkNativeWindow(const ohos::WindowLease& lease) const {
    if (!lease.active())
      return VK_ERROR_SURFACE_LOST_KHR;
    if (!lease.drawable() || lease.revision() != m_windowRevision || !m_swapchain)
      return VK_ERROR_OUT_OF_DATE_KHR;
    return VK_SUCCESS;
  }
  #endif


  void Presenter::destroySwapchain() {
    for (const auto& img : m_images)
      m_vkd->vkDestroyImageView(m_vkd->device(), img.view, nullptr);
    
    for (const auto& sem : m_semaphores) {
      m_vkd->vkDestroySemaphore(m_vkd->device(), sem.acquire, nullptr);
      m_vkd->vkDestroySemaphore(m_vkd->device(), sem.present, nullptr);
    }

    m_vkd->vkDestroySwapchainKHR(m_vkd->device(), m_swapchain, nullptr);

    m_images.clear();
    m_semaphores.clear();

    m_swapchain = VK_NULL_HANDLE;
    m_info.imageCount = 0;
    m_acquireStatus = VK_NOT_READY;
  }


  void Presenter::destroySurface() {
    if (m_surface)
      m_vki->vkDestroySurfaceKHR(m_vki->instance(), m_surface, nullptr);
    m_surface = VK_NULL_HANDLE;
  }

}
