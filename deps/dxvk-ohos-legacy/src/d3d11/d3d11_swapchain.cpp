#include "d3d11_context_imm.h"
#include "d3d11_device.h"
#include "d3d11_swapchain.h"

#if defined(DXVK_NATIVE_OHOS)
#include "../dxgi/dxgi_ohos_swapchain.h"
#include "../wsi/ohos_present_policy.h"
#include "../dxvk/dxvk_ohos_memory_stats.h"
#include "../dxvk/dxvk_winehua_submit_stats.h"
#include "../util/util_ohos_perf.h"

#include <array>
#include <atomic>
#include <chrono>
#endif

#if defined(DXVK_NATIVE_OHOS)
namespace {

  struct G9PerformanceCounters {
    std::atomic<uint64_t> sequence { 0 };
    std::atomic<uint32_t> fpsMilli { 0 };
    std::atomic<uint32_t> averageFrameUs { 0 };
    std::atomic<uint32_t> p95FrameUs { 0 };
    std::atomic<uint32_t> gpuLoadPermille { 0 };
    std::atomic<uint32_t> submissionsMilli { 0 };
    std::atomic<uint32_t> averageSyncUs { 0 };
    std::atomic<uint32_t> averageFrameWaitUs { 0 };
    std::atomic<uint32_t> averageAcquireUs { 0 };
    std::atomic<uint32_t> averageCsWaitUs { 0 };
    std::atomic<uint32_t> averageQueueWaitUs { 0 };
    std::atomic<uint32_t> averageVkPresentUs { 0 };
    std::atomic<uint32_t> averageCommandSubmitUs { 0 };
    std::atomic<uint32_t> maxQueueDepth { 0 };
    std::atomic<uint32_t> renderWidth { 0 };
    std::atomic<uint32_t> renderHeight { 0 };
    std::atomic<uint32_t> surfaceWidth { 0 };
    std::atomic<uint32_t> surfaceHeight { 0 };
    std::atomic<uint32_t> presentMode { UINT32_MAX };
    std::atomic<uint32_t> imageCount { 0 };
  } g9Performance;

}

extern "C" DXVK_OHOS_API int32_t DXVKOhosGetPerformanceStats(
    DXVKOhosPerformanceStats* stats) {
  if (!stats || stats->size != sizeof(*stats))
    return DXVK_OHOS_WINDOW_INVALID_ARGUMENT;

  DXVKOhosPerformanceStats current = {};
  current.size = sizeof(current);
  current.version = 2;
  current.fpsMilli = g9Performance.fpsMilli.load(std::memory_order_relaxed);
  current.averageFrameUs = g9Performance.averageFrameUs.load(std::memory_order_relaxed);
  current.p95FrameUs = g9Performance.p95FrameUs.load(std::memory_order_relaxed);
  current.gpuLoadPermille = g9Performance.gpuLoadPermille.load(std::memory_order_relaxed);
  current.submissionsMilli = g9Performance.submissionsMilli.load(std::memory_order_relaxed);
  current.averageSyncUs = g9Performance.averageSyncUs.load(std::memory_order_relaxed);
  current.averageFrameWaitUs = g9Performance.averageFrameWaitUs.load(std::memory_order_relaxed);
  current.averageAcquireUs = g9Performance.averageAcquireUs.load(std::memory_order_relaxed);
  current.averageCsWaitUs = g9Performance.averageCsWaitUs.load(std::memory_order_relaxed);
  current.averageQueueWaitUs = g9Performance.averageQueueWaitUs.load(std::memory_order_relaxed);
  current.averageVkPresentUs = g9Performance.averageVkPresentUs.load(std::memory_order_relaxed);
  current.averageCommandSubmitUs = g9Performance.averageCommandSubmitUs.load(std::memory_order_relaxed);
  current.maxQueueDepth = g9Performance.maxQueueDepth.load(std::memory_order_relaxed);
  current.renderWidth = g9Performance.renderWidth.load(std::memory_order_relaxed);
  current.renderHeight = g9Performance.renderHeight.load(std::memory_order_relaxed);
  current.surfaceWidth = g9Performance.surfaceWidth.load(std::memory_order_relaxed);
  current.surfaceHeight = g9Performance.surfaceHeight.load(std::memory_order_relaxed);
  current.presentMode = g9Performance.presentMode.load(std::memory_order_relaxed);
  current.imageCount = g9Performance.imageCount.load(std::memory_order_relaxed);
  current.sequence = g9Performance.sequence.load(std::memory_order_acquire);
  *stats = current;
  return DXVK_OHOS_WINDOW_OK;
}
#endif

namespace dxvk {

  static uint16_t MapGammaControlPoint(float x) {
    if (x < 0.0f) x = 0.0f;
    if (x > 1.0f) x = 1.0f;
    return uint16_t(65535.0f * x);
  }


  D3D11SwapChain::D3D11SwapChain(
          D3D11DXGIDevice*        pContainer,
          D3D11Device*            pDevice,
          HWND                    hWnd,
    const DXGI_SWAP_CHAIN_DESC1*  pDesc)
  : m_dxgiDevice(pContainer),
    m_parent    (pDevice),
    m_window    (hWnd),
    m_desc      (*pDesc),
    m_device    (pDevice->GetDXVKDevice()),
    m_context   (m_device->createContext()),
    m_frameLatencyCap(pDevice->GetOptions()->maxFrameLatency) {
    try {
      CreateFrameLatencyEvent();

      if (!pDevice->GetOptions()->deferSurfaceCreation)
        CreatePresenter();
    
      CreateBackBuffer();
      CreateBlitter();
      CreateHud();
    } catch (...) {
      m_device->waitForIdle();
      DestroyFrameLatencyEvent();
      throw;
    }
  }


  D3D11SwapChain::~D3D11SwapChain() {
    m_device->waitForSubmission(&m_presentStatus);
    m_device->waitForIdle();

    #if defined(DXVK_NATIVE_OHOS)
    // The blit context retains image views/framebuffer state. Release it
    // before Presenter destroys the images owned by the Vulkan swapchain.
    m_context = nullptr;
    m_imageViews.clear();
    m_presenter = nullptr;
    #endif
    
    DestroyFrameLatencyEvent();
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::QueryInterface(
          REFIID                  riid,
          void**                  ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    InitReturnPtr(ppvObject);

    if (riid == __uuidof(IUnknown)
     || riid == __uuidof(IDXGIVkSwapChain)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    Logger::warn("D3D11SwapChain::QueryInterface: Unknown interface query");
    return E_NOINTERFACE;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::GetDesc(
          DXGI_SWAP_CHAIN_DESC1*    pDesc) {
    *pDesc = m_desc;
    return S_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::GetAdapter(
          REFIID                    riid,
          void**                    ppvObject) {
    return m_dxgiDevice->GetParent(riid, ppvObject);
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::GetDevice(
          REFIID                    riid,
          void**                    ppDevice) {
    return m_dxgiDevice->QueryInterface(riid, ppDevice);
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::GetImage(
          UINT                      BufferId,
          REFIID                    riid,
          void**                    ppBuffer) {
    InitReturnPtr(ppBuffer);

    if (BufferId > 0) {
      Logger::err("D3D11: GetImage: BufferId > 0 not supported");
      return DXGI_ERROR_UNSUPPORTED;
    }

    return m_backBuffer->QueryInterface(riid, ppBuffer);
  }


  UINT STDMETHODCALLTYPE D3D11SwapChain::GetImageIndex() {
    return 0;
  }


  UINT STDMETHODCALLTYPE D3D11SwapChain::GetFrameLatency() {
    return m_frameLatency;
  }


  HANDLE STDMETHODCALLTYPE D3D11SwapChain::GetFrameLatencyEvent() {
    return m_frameLatencyEvent;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::ChangeProperties(
    const DXGI_SWAP_CHAIN_DESC1*  pDesc) {

    if (!pDesc) return E_INVALIDARG;
    #if defined(DXVK_NATIVE_OHOS)
    const auto oldDesc = m_desc;
    const bool oldDirty = m_dirty;
    const auto oldBuffer = m_backBuffer;
    const auto oldImage = m_swapImage;
    const auto oldView = m_swapImageView;
    const auto oldContext = m_context;
    try {
      m_context = m_device->createContext();
      m_desc = *pDesc;
      CreateBackBuffer();
      m_dirty = true;
      return S_OK;
    } catch (...) {
      m_desc = oldDesc;
      m_dirty = oldDirty;
      m_backBuffer = oldBuffer;
      m_swapImage = oldImage;
      m_swapImageView = oldView;
      m_context = oldContext;
      throw;
    }
    #else

    m_dirty |= m_desc.Format      != pDesc->Format
            || m_desc.Width       != pDesc->Width
            || m_desc.Height      != pDesc->Height
            || m_desc.BufferCount != pDesc->BufferCount
            || m_desc.Flags       != pDesc->Flags;

    m_desc = *pDesc;
    CreateBackBuffer();
    return S_OK;
    #endif
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::SetPresentRegion(
    const RECT*                     pRegion) {
    // TODO implement
    return E_NOTIMPL;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::SetGammaControl(
          UINT                      NumControlPoints,
    const DXGI_RGB*                 pControlPoints) {
    bool isIdentity = true;

    if (NumControlPoints > 1) {
      std::array<DxvkGammaCp, 1025> cp;

      if (NumControlPoints > cp.size())
        return E_INVALIDARG;
      
      for (uint32_t i = 0; i < NumControlPoints; i++) {
        uint16_t identity = MapGammaControlPoint(float(i) / float(NumControlPoints - 1));

        cp[i].r = MapGammaControlPoint(pControlPoints[i].Red);
        cp[i].g = MapGammaControlPoint(pControlPoints[i].Green);
        cp[i].b = MapGammaControlPoint(pControlPoints[i].Blue);
        cp[i].a = 0;

        isIdentity &= cp[i].r == identity
                   && cp[i].g == identity
                   && cp[i].b == identity;
      }

      if (!isIdentity)
        m_blitter->setGammaRamp(NumControlPoints, cp.data());
    }

    if (isIdentity)
      m_blitter->setGammaRamp(0, nullptr);

    return S_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::SetFrameLatency(
          UINT                      MaxLatency) {
    if (MaxLatency == 0 || MaxLatency > DXGI_MAX_SWAP_CHAIN_BUFFERS)
      return DXGI_ERROR_INVALID_CALL;

    #if !defined(DXVK_NATIVE_OHOS)
    if (m_frameLatencyEvent) {
      // Windows DXGI does not seem to handle the case where the new maximum
      // latency is less than the current value, and some games relying on
      // this behaviour will hang if we attempt to decrement the semaphore.
      // Thus, only increment the semaphore as necessary.
      if (MaxLatency > m_frameLatency)
        ReleaseSemaphore(m_frameLatencyEvent, MaxLatency - m_frameLatency, nullptr);
    }
    #endif

    m_frameLatency = MaxLatency;
    return S_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::Present(
          UINT                      SyncInterval,
          UINT                      PresentFlags,
    const DXGI_PRESENT_PARAMETERS*  pPresentParameters) {
    #if defined(DXVK_NATIVE_OHOS)
    try {
      if (PresentFlags & ~(DXGI_PRESENT_TEST | DXGI_PRESENT_DO_NOT_WAIT))
        return DXGI_ERROR_UNSUPPORTED;
      if (pPresentParameters && (pPresentParameters->DirtyRectsCount
       || pPresentParameters->pScrollRect || pPresentParameters->pScrollOffset))
        return DXGI_ERROR_UNSUPPORTED;
      if (m_device->getDeviceStatus() != VK_SUCCESS)
        return DXGI_ERROR_DEVICE_REMOVED;
      DXVKOhosWindowInfo window = { };
      const auto windowStatus = ohos::windowInfo(m_window, window);
      if (FAILED(windowStatus)) return windowStatus;
      if (!window.width || !window.height) return DXGI_STATUS_OCCLUDED;
      if (PresentFlags & DXGI_PRESENT_TEST) return S_OK;

      const auto options = m_parent->GetOptions();
      if (options->syncInterval >= 0) SyncInterval = options->syncInterval;

      /* Bounded GPU-memory attribution: every few seconds report what DXVK's
       * own allocator holds versus what the driver reports for the heap, so a
       * runaway footprint can be attributed without an external profiler.  It
       * queries the driver's heap state, so it follows the port's telemetry
       * switch. */
      if (winehuaTelemetryEnabled()) {
        static std::chrono::steady_clock::time_point sLastReport;
        const auto now = std::chrono::steady_clock::now();
        if (sLastReport == std::chrono::steady_clock::time_point()
         || now - sLastReport >= std::chrono::seconds(5)) {
          sLastReport = now;
          DxvkAdapterMemoryInfo heapInfo = m_device->adapter()->getMemoryHeapInfo();
          Logger::info(str::format("WineHuaMemory: imagesMiB=",
            uint64_t(ohosImageBytes().load(std::memory_order_relaxed) >> 20),
            " images=", uint64_t(ohosImageCount().load(std::memory_order_relaxed)),
            " largeImagesMiB=",
            uint64_t(ohosLargeImageBytes().load(std::memory_order_relaxed) >> 20),
            " largeImages=", uint64_t(ohosLargeImageCount().load(std::memory_order_relaxed)),
            " buffersMiB=",
            uint64_t(ohosBufferBytes().load(std::memory_order_relaxed) >> 20),
            " buffers=", uint64_t(ohosBufferCount().load(std::memory_order_relaxed))));
          for (uint32_t i = 0; i < heapInfo.heapCount; i++) {
            DxvkMemoryStats stats = m_device->getMemoryStats(i);
            Logger::info(str::format("WineHuaMemory: heap=", i,
              " flags=", uint32_t(heapInfo.heaps[i].heapFlags),
              " dxvkAllocatedMiB=", uint64_t(stats.memoryAllocated >> 20),
              " dxvkUsedMiB=", uint64_t(stats.memoryUsed >> 20),
              " driverUsageMiB=", uint64_t(heapInfo.heaps[i].memoryAllocated >> 20),
              " driverBudgetMiB=", uint64_t(heapInfo.heaps[i].memoryBudget >> 20)));
          }
        }
      }
      const bool vsync = SyncInterval != 0;
      m_dirty |= vsync != m_vsync;
      m_vsync = vsync;
      if (m_presenter == nullptr) CreatePresenter();
      if (!m_presenter->hasSwapChain() || m_dirty) {
        const auto rebuilt = RecreateSwapChain(m_vsync);
        if (rebuilt != VK_SUCCESS) return ohos::presentResult(rebuilt);
        m_dirty = false;
      }
      return PresentImage(SyncInterval, PresentFlags);
    } catch (const std::bad_alloc&) {
      return E_OUTOFMEMORY;
    } catch (const DxvkError& error) {
      Logger::err(error.message());
      return DXGI_ERROR_DRIVER_INTERNAL_ERROR;
    }
    #else
    auto options = m_parent->GetOptions();

    if (options->syncInterval >= 0)
      SyncInterval = options->syncInterval;

    if (!(PresentFlags & DXGI_PRESENT_TEST)) {
      bool vsync = SyncInterval != 0;

      m_dirty |= vsync != m_vsync;
      m_vsync  = vsync;
    }

    if (m_presenter == nullptr)
      CreatePresenter();

    HRESULT hr = S_OK;

    if (!m_presenter->hasSwapChain()) {
      RecreateSwapChain(m_vsync);
      m_dirty = false;
    }

    if (!m_presenter->hasSwapChain())
      hr = DXGI_STATUS_OCCLUDED;

    if (m_device->getDeviceStatus() != VK_SUCCESS)
      hr = DXGI_ERROR_DEVICE_RESET;

    if ((PresentFlags & DXGI_PRESENT_TEST) || hr != S_OK)
      return hr;

    if (std::exchange(m_dirty, false))
      RecreateSwapChain(m_vsync);
    
    try {
      hr = PresentImage(SyncInterval, PresentFlags);
    } catch (const DxvkError& e) {
      Logger::err(e.message());
      hr = E_FAIL;
    }

    return hr;
    #endif
  }


  void STDMETHODCALLTYPE D3D11SwapChain::NotifyModeChange(
          BOOL                      Windowed,
    const DXGI_MODE_DESC*           pDisplayMode) {
    if (Windowed || !pDisplayMode) {
      // Display modes aren't meaningful in windowed mode
      m_displayRefreshRate = 0.0;
    } else {
      DXGI_RATIONAL rate = pDisplayMode->RefreshRate;
      m_displayRefreshRate = double(rate.Numerator) / double(rate.Denominator);
    }

    if (m_presenter != nullptr)
      m_presenter->setFrameRateLimiterRefreshRate(m_displayRefreshRate);
  }


  HRESULT D3D11SwapChain::PresentImage(UINT SyncInterval, UINT PresentFlags) {
    #if defined(DXVK_NATIVE_OHOS)
    const auto g9NowUs = [] {
      return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    };
    const uint64_t g9EnterUs = g9NowUs();
    uint64_t g9SyncUs = 0;
    uint64_t g9FrameWaitUs = 0;
    uint64_t g9AcquireUs = 0;
    uint64_t g9SubmitUs = 0;
    uint64_t g9CsWaitUs = 0;
    uint64_t g9CsToQueueUs = 0;
    uint64_t g9QueueWaitUs = 0;
    uint64_t g9QueueLockWaitUs = 0;
    uint64_t g9VkPresentUs = 0;
    uint64_t g9PresentWakeUs = 0;
    uint64_t g9CommandSubmitUs = 0;
    uint64_t g9CommandSubmitMaxUs = 0;
    uint32_t g9CommandSubmitCount = 0;
    uint32_t g9QueueDepth = 0;
    uint32_t g9PresentMode = UINT32_MAX;
    uint32_t g9ImageCount = 0;
    #endif

    Com<ID3D11DeviceContext> deviceContext = nullptr;
    m_parent->GetImmediateContext(&deviceContext);

    // Flush pending rendering commands before
    auto immediateContext = static_cast<D3D11ImmediateContext*>(deviceContext.ptr());
    immediateContext->Flush();
    #if defined(DXVK_NATIVE_OHOS)
    const uint64_t g9FlushDoneUs = g9NowUs();
    #endif

    // Bump our frame id.
    #if !defined(DXVK_NATIVE_OHOS)
    ++m_frameId;
    #endif
    
    for (uint32_t i = 0; i < SyncInterval || i < 1; i++) {
      #if defined(DXVK_NATIVE_OHOS)
      if ((PresentFlags & DXGI_PRESENT_DO_NOT_WAIT) && m_presentStatus.result.load() == VK_NOT_READY)
        return DXGI_ERROR_WAS_STILL_DRAWING;
      #endif
      #if defined(DXVK_NATIVE_OHOS)
      const uint64_t g9SyncBeginUs = g9NowUs();
      #endif
      const auto synchronized = SynchronizePresent();
      #if defined(DXVK_NATIVE_OHOS)
      const uint64_t g9SyncDoneUs = g9NowUs();
      g9SyncUs += g9SyncDoneUs - g9SyncBeginUs;
      const uint64_t g9RequestedUs = m_presentStatus.presentRequestedUs.load();
      const uint64_t g9CsBeginUs = m_presentStatus.csBeginUs.load();
      const uint64_t g9QueueEnqueuedUs = m_presentStatus.queueEnqueuedUs.load();
      const uint64_t g9QueueDequeuedUs = m_presentStatus.queueDequeuedUs.load();
      const uint64_t g9QueueLockUs = m_presentStatus.queueLockAcquiredUs.load();
      const uint64_t g9DriverDoneUs = m_presentStatus.driverDoneUs.load();
      if (g9RequestedUs && g9CsBeginUs >= g9RequestedUs)
        g9CsWaitUs += g9CsBeginUs - g9RequestedUs;
      if (g9CsBeginUs && g9QueueEnqueuedUs >= g9CsBeginUs)
        g9CsToQueueUs += g9QueueEnqueuedUs - g9CsBeginUs;
      if (g9QueueEnqueuedUs && g9QueueDequeuedUs >= g9QueueEnqueuedUs)
        g9QueueWaitUs += g9QueueDequeuedUs - g9QueueEnqueuedUs;
      if (g9QueueDequeuedUs && g9QueueLockUs >= g9QueueDequeuedUs)
        g9QueueLockWaitUs += g9QueueLockUs - g9QueueDequeuedUs;
      if (g9QueueLockUs && g9DriverDoneUs >= g9QueueLockUs)
        g9VkPresentUs += g9DriverDoneUs - g9QueueLockUs;
      if (g9DriverDoneUs && g9SyncDoneUs >= g9DriverDoneUs)
        g9PresentWakeUs += g9SyncDoneUs - g9DriverDoneUs;
      g9CommandSubmitUs += m_presentStatus.submitTotalUsBeforePresent.load();
      g9CommandSubmitMaxUs = std::max(g9CommandSubmitMaxUs,
        m_presentStatus.submitMaxUsBeforePresent.load());
      g9CommandSubmitCount += m_presentStatus.submitCountBeforePresent.load();
      g9QueueDepth = std::max(g9QueueDepth,
        m_presentStatus.queueDepthAtEnqueue.load());
      if (synchronized != VK_SUCCESS) return ohos::presentResult(synchronized);
      #endif

      if (!m_presenter->hasSwapChain())
        return DXGI_STATUS_OCCLUDED;

      // Presentation semaphores and WSI swap chain image
      vk::PresenterInfo info = m_presenter->info();
      vk::PresenterSync sync;

      uint32_t imageIndex = 0;

      #if defined(DXVK_NATIVE_OHOS)
      // The acquire semaphore is frame-indexed. Its previous GPU wait must
      // finish before it can be passed to vkAcquireNextImageKHR again.
      const bool nonBlocking = PresentFlags & DXGI_PRESENT_DO_NOT_WAIT;
      const auto inFlight = std::min(info.imageCount, GetActualFrameLatency());
      const uint64_t g9FrameWaitBeginUs = g9NowUs();
      if (inFlight && m_frameId >= inFlight) {
        const auto ready = WaitForNativeFrame(m_frameId - inFlight + 1, nonBlocking);
        if (ready != S_OK) return ready;
      }
      g9FrameWaitUs += g9NowUs() - g9FrameWaitBeginUs;
      const uint64_t g9AcquireBeginUs = g9NowUs();
      const auto status = ohos::acquireForPresent(!nonBlocking,
        [&] { return m_presenter->acquireNextImage(sync, imageIndex, nonBlocking); },
        [&] { return RecreateSwapChain(m_vsync); },
        [] { std::this_thread::sleep_for(std::chrono::milliseconds(1)); });
      g9AcquireUs += g9NowUs() - g9AcquireBeginUs;
      if (status != VK_SUCCESS && status != VK_SUBOPTIMAL_KHR)
        return ohos::presentResult(status);
      info = m_presenter->info();
      g9PresentMode = static_cast<uint32_t>(info.presentMode);
      g9ImageCount = info.imageCount;
      /* Report the swapchain state that actually limits how far the CPU may run
       * ahead: the configured frame latency is only a request, and the image
       * count can cap it. */
      {
        WinehuaPresentState& state = winehuaPresentState();
        state.imageCount.store(info.imageCount, std::memory_order_relaxed);
        state.presentMode.store(static_cast<uint32_t>(info.presentMode),
          std::memory_order_relaxed);
        state.configuredLatency.store(m_frameLatencyCap, std::memory_order_relaxed);
        state.actualLatency.store(GetActualFrameLatency(), std::memory_order_relaxed);
      }
      winehuaPresentTick();
      // Count only frames actually submitted; timeouts must not leave holes
      // in the frame-latency fence's sequence.
      ++m_frameId;
      #else
      VkResult status = m_presenter->acquireNextImage(sync, imageIndex);
      while (status != VK_SUCCESS && status != VK_SUBOPTIMAL_KHR) {
        RecreateSwapChain(m_vsync);

        if (!m_presenter->hasSwapChain())
          return DXGI_STATUS_OCCLUDED;
        
        info = m_presenter->info();
        status = m_presenter->acquireNextImage(sync, imageIndex);
      }
      #endif

      // Resolve back buffer if it is multisampled. We
      // only have to do it only for the first frame.
      #if defined(DXVK_NATIVE_OHOS)
      const uint64_t g9SubmitBeginUs = g9NowUs();
      #endif
      m_context->beginRecording(
        m_device->createCommandList());
      
      m_blitter->presentImage(m_context.ptr(),
        m_imageViews.at(imageIndex), VkRect2D(),
        m_swapImageView, VkRect2D());

      if (m_hud != nullptr)
        m_hud->render(m_context, info.format, info.imageExtent);
      
      #if !defined(DXVK_NATIVE_OHOS)
      if (i + 1 >= SyncInterval)
      #endif
        m_context->signal(m_frameLatencySignal, m_frameId);

      const bool lastPresent = i + 1 >= std::max(SyncInterval, 1u);
      SubmitPresent(immediateContext, sync, i,
        lastPresent ? m_frameId + 1 : 0);
      #if defined(DXVK_NATIVE_OHOS)
      g9SubmitUs += g9NowUs() - g9SubmitBeginUs;
      #endif
    }

    #if !defined(DXVK_NATIVE_OHOS)
    SyncFrameLatency();
    #else
    const uint64_t g9ExitUs = g9NowUs();
    const uint64_t g9FlushUs = g9FlushDoneUs - g9EnterUs;
    const uint64_t g9TotalUs = g9ExitUs - g9EnterUs;
    static uint64_t g9RateStartUs = 0;
    static uint64_t g9Frames = 0;
    static uint64_t g9TotalAccumUs = 0;
    static uint64_t g9FlushAccumUs = 0;
    static uint64_t g9SyncAccumUs = 0;
    static uint64_t g9FrameWaitAccumUs = 0;
    static uint64_t g9AcquireAccumUs = 0;
    static uint64_t g9SubmitAccumUs = 0;
    static uint64_t g9CsWaitAccumUs = 0;
    static uint64_t g9CsToQueueAccumUs = 0;
    static uint64_t g9QueueWaitAccumUs = 0;
    static uint64_t g9QueueLockWaitAccumUs = 0;
    static uint64_t g9VkPresentAccumUs = 0;
    static uint64_t g9PresentWakeAccumUs = 0;
    static uint64_t g9CommandSubmitAccumUs = 0;
    static uint64_t g9CommandSubmitMaxAccumUs = 0;
    static uint64_t g9CommandSubmitCountAccum = 0;
    static uint32_t g9MaxQueueDepth = 0;
    static uint64_t g9MaxTotalUs = 0;
    static uint64_t g9MaxSyncUs = 0;
    static uint64_t g9MaxFrameWaitUs = 0;
    static uint64_t g9MaxAcquireUs = 0;
    static uint64_t g9MaxSubmitUs = 0;
    static uint64_t g9MaxCsWaitUs = 0;
    static uint64_t g9MaxQueueWaitUs = 0;
    static uint64_t g9MaxVkPresentUs = 0;
    static uint64_t g9Over50 = 0;
    static uint64_t g9HudStartUs = 0;
    static uint64_t g9HudPreviousFrameUs = 0;
    static uint64_t g9HudFrames = 0;
    static uint64_t g9HudSubmissions = 0;
    static uint64_t g9HudSyncUs = 0;
    static uint64_t g9HudFrameWaitUs = 0;
    static uint64_t g9HudAcquireUs = 0;
    static uint64_t g9HudCsWaitUs = 0;
    static uint64_t g9HudQueueWaitUs = 0;
    static uint64_t g9HudVkPresentUs = 0;
    static uint64_t g9HudCommandSubmitUs = 0;
    static uint64_t g9HudCommandSubmitCount = 0;
    static uint32_t g9HudMaxQueueDepth = 0;
    static uint64_t g9HudPreviousGpuIdleUs = 0;
    static std::array<uint32_t, 128> g9HudFrameIntervals = {};
    static uint32_t g9HudFrameIntervalCount = 0;
    if (!g9RateStartUs) g9RateStartUs = g9EnterUs;
    ++g9Frames;
    g9TotalAccumUs += g9TotalUs;
    g9FlushAccumUs += g9FlushUs;
    g9SyncAccumUs += g9SyncUs;
    g9FrameWaitAccumUs += g9FrameWaitUs;
    g9AcquireAccumUs += g9AcquireUs;
    g9SubmitAccumUs += g9SubmitUs;
    g9CsWaitAccumUs += g9CsWaitUs;
    g9CsToQueueAccumUs += g9CsToQueueUs;
    g9QueueWaitAccumUs += g9QueueWaitUs;
    g9QueueLockWaitAccumUs += g9QueueLockWaitUs;
    g9VkPresentAccumUs += g9VkPresentUs;
    g9PresentWakeAccumUs += g9PresentWakeUs;
    g9CommandSubmitAccumUs += g9CommandSubmitUs;
    g9CommandSubmitMaxAccumUs = std::max(g9CommandSubmitMaxAccumUs,
      g9CommandSubmitMaxUs);
    g9CommandSubmitCountAccum += g9CommandSubmitCount;
    g9MaxQueueDepth = std::max(g9MaxQueueDepth, g9QueueDepth);
    g9MaxTotalUs = std::max(g9MaxTotalUs, g9TotalUs);
    g9MaxSyncUs = std::max(g9MaxSyncUs, g9SyncUs);
    g9MaxFrameWaitUs = std::max(g9MaxFrameWaitUs, g9FrameWaitUs);
    g9MaxAcquireUs = std::max(g9MaxAcquireUs, g9AcquireUs);
    g9MaxSubmitUs = std::max(g9MaxSubmitUs, g9SubmitUs);
    g9MaxCsWaitUs = std::max(g9MaxCsWaitUs, g9CsWaitUs);
    g9MaxQueueWaitUs = std::max(g9MaxQueueWaitUs, g9QueueWaitUs);
    g9MaxVkPresentUs = std::max(g9MaxVkPresentUs, g9VkPresentUs);
    if (g9TotalUs >= 50000) ++g9Over50;
    if (!g9HudStartUs) {
      g9HudStartUs = g9EnterUs;
      g9HudPreviousFrameUs = g9EnterUs;
      g9HudPreviousGpuIdleUs = m_device->getStatCounters()
        .getCtr(DxvkStatCounter::GpuIdleTicks);
    }
    const uint64_t g9HudIntervalUs = g9EnterUs - g9HudPreviousFrameUs;
    g9HudPreviousFrameUs = g9EnterUs;
    if (ohosperf::enabled() && g9HudIntervalUs)
      ohosperf::record(DXVK_OHOS_PERF_PRESENT, m_frameId,
        uint32_t(std::min<uint64_t>(g9HudIntervalUs, UINT32_MAX)),
        g9TotalUs, m_device->getStatCounters().getCtr(DxvkStatCounter::GpuIdleTicks),
        g9QueueWaitUs, g9AcquireUs, g9CommandSubmitUs, g9CommandSubmitCount);
    if (g9HudIntervalUs && g9HudIntervalUs <= UINT32_MAX &&
        g9HudFrameIntervalCount < g9HudFrameIntervals.size())
      g9HudFrameIntervals[g9HudFrameIntervalCount++] =
        static_cast<uint32_t>(g9HudIntervalUs);
    ++g9HudFrames;
    g9HudSubmissions += g9CommandSubmitCount;
    g9HudSyncUs += g9SyncUs;
    g9HudFrameWaitUs += g9FrameWaitUs;
    g9HudAcquireUs += g9AcquireUs;
    g9HudCsWaitUs += g9CsWaitUs;
    g9HudQueueWaitUs += g9QueueWaitUs;
    g9HudVkPresentUs += g9VkPresentUs;
    g9HudCommandSubmitUs += g9CommandSubmitUs;
    g9HudCommandSubmitCount += g9CommandSubmitCount;
    g9HudMaxQueueDepth = std::max(g9HudMaxQueueDepth, g9QueueDepth);
    const uint64_t g9HudElapsedUs = g9ExitUs - g9HudStartUs;
    if (g9HudElapsedUs >= 1000000 && g9HudFrames) {
      auto sortedIntervals = g9HudFrameIntervals;
      std::sort(sortedIntervals.begin(),
        sortedIntervals.begin() + g9HudFrameIntervalCount);
      const uint32_t g9HudP95Us = g9HudFrameIntervalCount
        ? sortedIntervals[((g9HudFrameIntervalCount - 1) * 95) / 100]
        : static_cast<uint32_t>(g9HudElapsedUs / g9HudFrames);
      const uint64_t g9HudGpuIdleUs = m_device->getStatCounters()
        .getCtr(DxvkStatCounter::GpuIdleTicks);
      const uint64_t g9HudIdleDeltaUs = g9HudGpuIdleUs >= g9HudPreviousGpuIdleUs
        ? g9HudGpuIdleUs - g9HudPreviousGpuIdleUs : 0;
      const uint64_t g9HudBusyUs = g9HudElapsedUs > g9HudIdleDeltaUs
        ? g9HudElapsedUs - g9HudIdleDeltaUs : 0;
      g9Performance.fpsMilli.store(static_cast<uint32_t>(
        g9HudFrames * 1000000000ull / g9HudElapsedUs), std::memory_order_relaxed);
      g9Performance.averageFrameUs.store(static_cast<uint32_t>(
        g9HudElapsedUs / g9HudFrames), std::memory_order_relaxed);
      g9Performance.p95FrameUs.store(g9HudP95Us, std::memory_order_relaxed);
      g9Performance.gpuLoadPermille.store(static_cast<uint32_t>(
        std::min<uint64_t>(1000, g9HudBusyUs * 1000 / g9HudElapsedUs)),
        std::memory_order_relaxed);
      g9Performance.submissionsMilli.store(static_cast<uint32_t>(
        g9HudSubmissions * 1000 / g9HudFrames), std::memory_order_relaxed);
      g9Performance.averageSyncUs.store(static_cast<uint32_t>(
        std::min<uint64_t>(UINT32_MAX, g9HudSyncUs / g9HudFrames)),
        std::memory_order_relaxed);
      g9Performance.averageFrameWaitUs.store(static_cast<uint32_t>(
        std::min<uint64_t>(UINT32_MAX, g9HudFrameWaitUs / g9HudFrames)),
        std::memory_order_relaxed);
      g9Performance.averageAcquireUs.store(static_cast<uint32_t>(
        std::min<uint64_t>(UINT32_MAX, g9HudAcquireUs / g9HudFrames)),
        std::memory_order_relaxed);
      g9Performance.averageCsWaitUs.store(static_cast<uint32_t>(
        std::min<uint64_t>(UINT32_MAX, g9HudCsWaitUs / g9HudFrames)),
        std::memory_order_relaxed);
      g9Performance.averageQueueWaitUs.store(static_cast<uint32_t>(
        std::min<uint64_t>(UINT32_MAX, g9HudQueueWaitUs / g9HudFrames)),
        std::memory_order_relaxed);
      g9Performance.averageVkPresentUs.store(static_cast<uint32_t>(
        std::min<uint64_t>(UINT32_MAX, g9HudVkPresentUs / g9HudFrames)),
        std::memory_order_relaxed);
      g9Performance.averageCommandSubmitUs.store(static_cast<uint32_t>(
        std::min<uint64_t>(UINT32_MAX, g9HudCommandSubmitCount
          ? g9HudCommandSubmitUs / g9HudCommandSubmitCount : 0)),
        std::memory_order_relaxed);
      g9Performance.maxQueueDepth.store(g9HudMaxQueueDepth,
        std::memory_order_relaxed);
      g9Performance.renderWidth.store(m_desc.Width, std::memory_order_relaxed);
      g9Performance.renderHeight.store(m_desc.Height, std::memory_order_relaxed);
      DXVKOhosWindowInfo g9Window = {};
      if (SUCCEEDED(ohos::windowInfo(m_window, g9Window))) {
        g9Performance.surfaceWidth.store(g9Window.width,
          std::memory_order_relaxed);
        g9Performance.surfaceHeight.store(g9Window.height,
          std::memory_order_relaxed);
      }
      g9Performance.presentMode.store(g9PresentMode, std::memory_order_relaxed);
      g9Performance.imageCount.store(g9ImageCount, std::memory_order_relaxed);
      g9Performance.sequence.fetch_add(1, std::memory_order_release);
      g9HudStartUs = g9ExitUs;
      g9HudFrames = 0;
      g9HudSubmissions = 0;
      g9HudSyncUs = 0;
      g9HudFrameWaitUs = 0;
      g9HudAcquireUs = 0;
      g9HudCsWaitUs = 0;
      g9HudQueueWaitUs = 0;
      g9HudVkPresentUs = 0;
      g9HudCommandSubmitUs = 0;
      g9HudCommandSubmitCount = 0;
      g9HudMaxQueueDepth = 0;
      g9HudPreviousGpuIdleUs = g9HudGpuIdleUs;
      g9HudFrameIntervalCount = 0;
    }
    if (g9ExitUs - g9RateStartUs >= 5000000) {
      Logger::info(str::format(
        "G9_DXVK_PRESENT_PHASE frames=", g9Frames,
        " totalMs=", (g9ExitUs - g9RateStartUs) / 1000,
        " avgFlushUs=", g9FlushAccumUs / g9Frames,
        " avgSyncUs=", g9SyncAccumUs / g9Frames,
        " avgFrameWaitUs=", g9FrameWaitAccumUs / g9Frames,
        " avgAcquireUs=", g9AcquireAccumUs / g9Frames,
        " avgSubmitUs=", g9SubmitAccumUs / g9Frames,
        " avgCsWaitUs=", g9CsWaitAccumUs / g9Frames,
        " avgCsToQueueUs=", g9CsToQueueAccumUs / g9Frames,
        " avgQueueWaitUs=", g9QueueWaitAccumUs / g9Frames,
        " avgQueueLockWaitUs=", g9QueueLockWaitAccumUs / g9Frames,
        " avgVkPresentUs=", g9VkPresentAccumUs / g9Frames,
        " avgPresentWakeUs=", g9PresentWakeAccumUs / g9Frames,
        " commandSubmitCount=", g9CommandSubmitCountAccum,
        " avgCommandSubmitUs=", g9CommandSubmitCountAccum
          ? g9CommandSubmitAccumUs / g9CommandSubmitCountAccum : 0,
        " maxCommandSubmitUs=", g9CommandSubmitMaxAccumUs,
        " maxQueueDepth=", g9MaxQueueDepth,
        " maxTotalMs=", g9MaxTotalUs / 1000,
        " maxSyncMs=", g9MaxSyncUs / 1000,
        " maxFrameWaitMs=", g9MaxFrameWaitUs / 1000,
        " maxAcquireMs=", g9MaxAcquireUs / 1000,
        " maxSubmitMs=", g9MaxSubmitUs / 1000,
        " maxCsWaitMs=", g9MaxCsWaitUs / 1000,
        " maxQueueWaitMs=", g9MaxQueueWaitUs / 1000,
        " maxVkPresentMs=", g9MaxVkPresentUs / 1000,
        " over50=", g9Over50));
      g9RateStartUs = g9ExitUs;
      g9Frames = 0;
      g9TotalAccumUs = 0;
      g9FlushAccumUs = 0;
      g9SyncAccumUs = 0;
      g9FrameWaitAccumUs = 0;
      g9AcquireAccumUs = 0;
      g9SubmitAccumUs = 0;
      g9CsWaitAccumUs = 0;
      g9CsToQueueAccumUs = 0;
      g9QueueWaitAccumUs = 0;
      g9QueueLockWaitAccumUs = 0;
      g9VkPresentAccumUs = 0;
      g9PresentWakeAccumUs = 0;
      g9CommandSubmitAccumUs = 0;
      g9CommandSubmitMaxAccumUs = 0;
      g9CommandSubmitCountAccum = 0;
      g9MaxQueueDepth = 0;
      g9MaxTotalUs = 0;
      g9MaxSyncUs = 0;
      g9MaxFrameWaitUs = 0;
      g9MaxAcquireUs = 0;
      g9MaxSubmitUs = 0;
      g9MaxCsWaitUs = 0;
      g9MaxQueueWaitUs = 0;
      g9MaxVkPresentUs = 0;
      g9Over50 = 0;
    }
    #endif
    return S_OK;
  }


  #if defined(DXVK_NATIVE_OHOS)
  HRESULT D3D11SwapChain::WaitForNativeFrame(uint64_t frame, bool nonBlocking) {
    if (m_frameLatencySignal->value() >= frame) return S_OK;
    if (nonBlocking) return DXGI_ERROR_WAS_STILL_DRAWING;
    while (!m_frameLatencySignal->waitFor(frame, std::chrono::milliseconds(16))) {
      if (m_device->getDeviceStatus() != VK_SUCCESS) return DXGI_ERROR_DEVICE_REMOVED;
      DXVKOhosWindowInfo window = { };
      const auto status = ohos::windowInfo(m_window, window);
      if (FAILED(status)) return status;
      if (!window.width || !window.height) return DXGI_STATUS_OCCLUDED;
    }
    return S_OK;
  }
  #endif


  void D3D11SwapChain::SubmitPresent(
          D3D11ImmediateContext*  pContext,
    const vk::PresenterSync&      Sync,
          uint32_t                FrameId,
          uint64_t                NextFrameId) {
    auto lock = pContext->LockContext();

    // Present from CS thread so that we don't
    // have to synchronize with it first.
    #if defined(DXVK_NATIVE_OHOS)
    m_presentStatus.csBeginUs = 0;
    m_presentStatus.queueEnqueuedUs = 0;
    m_presentStatus.queueDequeuedUs = 0;
    m_presentStatus.queueLockAcquiredUs = 0;
    m_presentStatus.driverDoneUs = 0;
    m_presentStatus.submitTotalUsBeforePresent = 0;
    m_presentStatus.submitMaxUsBeforePresent = 0;
    m_presentStatus.submitCountBeforePresent = 0;
    m_presentStatus.queueDepthAtEnqueue = 0;
    m_presentStatus.presentRequestedUs = DxvkSubmitStatus::nowUs();
    #endif
    m_presentStatus.result = VK_NOT_READY;

    pContext->EmitCs([this,
      cFrameId     = FrameId,
      cNextFrameId = NextFrameId,
      cSync        = Sync,
      cHud         = m_hud,
      cCommandList = m_context->endRecording()
    ] (DxvkContext* ctx) {
      #if defined(DXVK_NATIVE_OHOS)
      m_presentStatus.csBeginUs = DxvkSubmitStatus::nowUs();
      #endif
      m_device->submitCommandList(cCommandList,
        cSync.acquire, cSync.present);

      if (cHud != nullptr && !cFrameId)
        cHud->update();

      m_device->presentImage(m_presenter, &m_presentStatus);

      if (cNextFrameId)
        ctx->winehuaFrameBoundary(cNextFrameId);
    });

    pContext->FlushCsChunk();
  }


  VkResult D3D11SwapChain::SynchronizePresent() {
    // Recreate swap chain if the previous present call failed
    VkResult status = m_device->waitForSubmission(&m_presentStatus);
    
    #if defined(DXVK_NATIVE_OHOS)
    if (status == VK_ERROR_DEVICE_LOST) return status;
    if (status != VK_SUCCESS && status != VK_SUBOPTIMAL_KHR
     && status != VK_ERROR_OUT_OF_DATE_KHR && status != VK_ERROR_SURFACE_LOST_KHR)
      return status;
    #endif
    return status != VK_SUCCESS ? RecreateSwapChain(m_vsync) : VK_SUCCESS;
  }


  VkResult D3D11SwapChain::RecreateSwapChain(BOOL Vsync) {
    // Ensure that we can safely destroy the swap chain
    m_device->waitForSubmission(&m_presentStatus);
    m_device->waitForIdle();

    #if defined(DXVK_NATIVE_OHOS)
    m_context = m_device->createContext();
    m_imageViews.clear();
    #endif
    m_presentStatus.result = VK_SUCCESS;

    vk::PresenterDesc presenterDesc;
    presenterDesc.imageExtent     = { m_desc.Width, m_desc.Height };
    presenterDesc.imageCount      = PickImageCount(m_desc.BufferCount + 1);
    presenterDesc.numFormats      = PickFormats(m_desc.Format, presenterDesc.formats);
    presenterDesc.numPresentModes = PickPresentModes(Vsync, presenterDesc.presentModes);
    presenterDesc.fullScreenExclusive = PickFullscreenMode();

    const auto status = m_presenter->recreateSwapChain(presenterDesc);
    #if defined(DXVK_NATIVE_OHOS)
    if (status != VK_SUCCESS) return status;
    #else
    if (status != VK_SUCCESS)
      throw DxvkError("D3D11SwapChain: Failed to recreate swap chain");
    #endif
    
    CreateRenderTargetViews();
    return VK_SUCCESS;
  }


  void D3D11SwapChain::CreateFrameLatencyEvent() {
    m_frameLatencySignal = new sync::CallbackFence(m_frameId);

    #if !defined(DXVK_NATIVE_OHOS)
    if (m_desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)
      m_frameLatencyEvent = CreateSemaphore(nullptr, m_frameLatency, DXGI_MAX_SWAP_CHAIN_BUFFERS, nullptr);
    #endif
  }


  void D3D11SwapChain::CreatePresenter() {
    DxvkDeviceQueue graphicsQueue = m_device->queues().graphics;

    vk::PresenterDevice presenterDevice;
    presenterDevice.queueFamily   = graphicsQueue.queueFamily;
    presenterDevice.queue         = graphicsQueue.queueHandle;
    presenterDevice.adapter       = m_device->adapter()->handle();
    presenterDevice.features.fullScreenExclusive = m_device->extensions().extFullScreenExclusive;

    vk::PresenterDesc presenterDesc;
    presenterDesc.imageExtent     = { m_desc.Width, m_desc.Height };
    presenterDesc.imageCount      = PickImageCount(m_desc.BufferCount + 1);
    presenterDesc.numFormats      = PickFormats(m_desc.Format, presenterDesc.formats);
    presenterDesc.numPresentModes = PickPresentModes(false, presenterDesc.presentModes);
    presenterDesc.fullScreenExclusive = PickFullscreenMode();

    m_presenter = new vk::Presenter(m_window,
      m_device->adapter()->vki(),
      m_device->vkd(),
      presenterDevice,
      presenterDesc);
    
    m_presenter->setFrameRateLimit(m_parent->GetOptions()->maxFrameRate);
    m_presenter->setFrameRateLimiterRefreshRate(m_displayRefreshRate);

    CreateRenderTargetViews();
  }


  void D3D11SwapChain::CreateRenderTargetViews() {
    vk::PresenterInfo info = m_presenter->info();

    m_imageViews.clear();
    m_imageViews.resize(info.imageCount);

    DxvkImageCreateInfo imageInfo;
    imageInfo.type        = VK_IMAGE_TYPE_2D;
    imageInfo.format      = info.format.format;
    imageInfo.flags       = 0;
    imageInfo.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.extent      = { info.imageExtent.width, info.imageExtent.height, 1 };
    imageInfo.numLayers   = 1;
    imageInfo.mipLevels   = 1;
    imageInfo.usage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    imageInfo.stages      = 0;
    imageInfo.access      = 0;
    imageInfo.tiling      = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.layout      = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    imageInfo.shared      = VK_TRUE;

    DxvkImageViewCreateInfo viewInfo;
    viewInfo.type         = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format       = info.format.format;
    viewInfo.usage        = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    viewInfo.aspect       = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.minLevel     = 0;
    viewInfo.numLevels    = 1;
    viewInfo.minLayer     = 0;
    viewInfo.numLayers    = 1;

    for (uint32_t i = 0; i < info.imageCount; i++) {
      VkImage imageHandle = m_presenter->getImage(i).image;
      
      Rc<DxvkImage> image = new DxvkImage(
        m_device.ptr(), imageInfo, imageHandle);

      m_imageViews[i] = new DxvkImageView(
        m_device->vkd(), image, viewInfo);
    }
  }


  void D3D11SwapChain::CreateBackBuffer() {
    // Explicitly destroy current swap image before
    // creating a new one to free up resources
    m_swapImage         = nullptr;
    m_swapImageView     = nullptr;
    m_backBuffer        = nullptr;

    // Create new back buffer
    D3D11_COMMON_TEXTURE_DESC desc;
    desc.Width              = std::max(m_desc.Width,  1u);
    desc.Height             = std::max(m_desc.Height, 1u);
    desc.Depth              = 1;
    desc.MipLevels          = 1;
    desc.ArraySize          = 1;
    desc.Format             = m_desc.Format;
    desc.SampleDesc         = m_desc.SampleDesc;
    desc.Usage              = D3D11_USAGE_DEFAULT;
    desc.BindFlags          = 0;
    desc.CPUAccessFlags     = 0;
    desc.MiscFlags          = 0;
    desc.TextureLayout      = D3D11_TEXTURE_LAYOUT_UNDEFINED;

    if (m_desc.BufferUsage & DXGI_USAGE_RENDER_TARGET_OUTPUT)
      desc.BindFlags |= D3D11_BIND_RENDER_TARGET;

    if (m_desc.BufferUsage & DXGI_USAGE_SHADER_INPUT)
      desc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;

    if (m_desc.BufferUsage & DXGI_USAGE_UNORDERED_ACCESS)
      desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    
    if (m_desc.Flags & DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE)
      desc.MiscFlags |= D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
    
    DXGI_USAGE dxgiUsage = DXGI_USAGE_BACK_BUFFER;

    if (m_desc.SwapEffect == DXGI_SWAP_EFFECT_DISCARD
     || m_desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD)
      dxgiUsage |= DXGI_USAGE_DISCARD_ON_PRESENT;

    m_backBuffer = new D3D11Texture2D(m_parent, this, &desc, dxgiUsage);
    m_swapImage = GetCommonTexture(m_backBuffer.ptr())->GetImage();

    // Create an image view that allows the
    // image to be bound as a shader resource.
    DxvkImageViewCreateInfo viewInfo;
    viewInfo.type       = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format     = m_swapImage->info().format;
    viewInfo.usage      = VK_IMAGE_USAGE_SAMPLED_BIT;
    viewInfo.aspect     = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.minLevel   = 0;
    viewInfo.numLevels  = 1;
    viewInfo.minLayer   = 0;
    viewInfo.numLayers  = 1;
    m_swapImageView = m_device->createImageView(m_swapImage, viewInfo);
    
    // Initialize the image so that we can use it. Clearing
    // to black prevents garbled output for the first frame.
    VkImageSubresourceRange subresources;
    subresources.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    subresources.baseMipLevel   = 0;
    subresources.levelCount     = 1;
    subresources.baseArrayLayer = 0;
    subresources.layerCount     = 1;

    m_context->beginRecording(
      m_device->createCommandList());
    
    m_context->initImage(m_swapImage,
      subresources, VK_IMAGE_LAYOUT_UNDEFINED);

    m_device->submitCommandList(
      m_context->endRecording(),
      VK_NULL_HANDLE,
      VK_NULL_HANDLE);
  }


  void D3D11SwapChain::CreateBlitter() {
#if defined(DXVK_NATIVE_OHOS)
    // OHOS can expose only INHERIT composite alpha. HWND swap chains still
    // require DXGI_ALPHA_MODE_IGNORE, regardless of the window's blend mode.
    const bool forceOpaque = m_desc.AlphaMode == DXGI_ALPHA_MODE_IGNORE;
    m_blitter = new DxvkSwapchainBlitter(m_device, forceOpaque);
    Logger::info(str::format("DXVK OHOS present: ignoreSourceAlpha=", forceOpaque ? 1 : 0));
#else
    m_blitter = new DxvkSwapchainBlitter(m_device);
#endif
  }


  void D3D11SwapChain::CreateHud() {
    m_hud = hud::Hud::createHud(m_device);

    if (m_hud != nullptr)
      m_hud->addItem<hud::HudClientApiItem>("api", 1, GetApiName());
  }


  void D3D11SwapChain::DestroyFrameLatencyEvent() {
    #if !defined(DXVK_NATIVE_OHOS)
    CloseHandle(m_frameLatencyEvent);
    #endif
  }


  void D3D11SwapChain::SyncFrameLatency() {
    // Wait for the sync event so that we respect the maximum frame latency
    m_frameLatencySignal->wait(m_frameId - GetActualFrameLatency());

    #if !defined(DXVK_NATIVE_OHOS)
    if (m_frameLatencyEvent) {
      m_frameLatencySignal->setCallback(m_frameId, [cFrameLatencyEvent = m_frameLatencyEvent] () {
        ReleaseSemaphore(cFrameLatencyEvent, 1, nullptr);
      });
    }
    #endif
  }


  uint32_t D3D11SwapChain::GetActualFrameLatency() {
    uint32_t maxFrameLatency = m_frameLatency;

    if (!(m_desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT))
      m_dxgiDevice->GetMaximumFrameLatency(&maxFrameLatency);

    if (m_frameLatencyCap)
      maxFrameLatency = std::min(maxFrameLatency, m_frameLatencyCap);

    maxFrameLatency = std::min(maxFrameLatency, m_desc.BufferCount + 1);
    return maxFrameLatency;
  }


  uint32_t D3D11SwapChain::PickFormats(
          DXGI_FORMAT               Format,
          VkSurfaceFormatKHR*       pDstFormats) {
    uint32_t n = 0;

    switch (Format) {
      default:
        Logger::warn(str::format("D3D11SwapChain: Unexpected format: ", m_desc.Format));
      [[fallthrough]];
      
      case DXGI_FORMAT_R8G8B8A8_UNORM:
      case DXGI_FORMAT_B8G8R8A8_UNORM: {
        pDstFormats[n++] = { VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
        pDstFormats[n++] = { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
      } break;
      
      case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: {
        pDstFormats[n++] = { VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
        pDstFormats[n++] = { VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
      } break;
      
      case DXGI_FORMAT_R10G10B10A2_UNORM: {
        pDstFormats[n++] = { VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
        pDstFormats[n++] = { VK_FORMAT_A2R10G10B10_UNORM_PACK32, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
      } break;
      
      case DXGI_FORMAT_R16G16B16A16_FLOAT: {
        pDstFormats[n++] = { VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
      } break;
    }

    return n;
  }


  uint32_t D3D11SwapChain::PickPresentModes(
          BOOL                      Vsync,
          VkPresentModeKHR*         pDstModes) {
    uint32_t n = 0;

    if (Vsync) {
      if (m_parent->GetOptions()->tearFree == Tristate::False)
        pDstModes[n++] = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
      pDstModes[n++] = VK_PRESENT_MODE_FIFO_KHR;
    } else {
      if (m_parent->GetOptions()->tearFree != Tristate::True)
        pDstModes[n++] = VK_PRESENT_MODE_IMMEDIATE_KHR;
      pDstModes[n++] = VK_PRESENT_MODE_MAILBOX_KHR;
    }

    return n;
  }


  uint32_t D3D11SwapChain::PickImageCount(
          UINT                      Preferred) {
    int32_t option = m_parent->GetOptions()->numBackBuffers;
    return option > 0 ? uint32_t(option) : uint32_t(Preferred);
  }


  VkFullScreenExclusiveEXT D3D11SwapChain::PickFullscreenMode() {
    return m_desc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH
      ? VK_FULL_SCREEN_EXCLUSIVE_ALLOWED_EXT
      : VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT;
  }


  std::string D3D11SwapChain::GetApiName() const {
    Com<IDXGIDXVKDevice> device;
    m_parent->QueryInterface(__uuidof(IDXGIDXVKDevice), reinterpret_cast<void**>(&device));

    uint32_t apiVersion = device->GetAPIVersion();
    uint32_t featureLevel = m_parent->GetFeatureLevel();

    uint32_t flHi = (featureLevel >> 12);
    uint32_t flLo = (featureLevel >> 8) & 0x7;

    return str::format("D3D", apiVersion, " FL", flHi, "_", flLo);
  }

}
