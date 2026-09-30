#include <native_window/external_window.h>

#include <new>

#include "../../include/native/ohos/dxvk_native_ohos.h"
#include "../dxvk/dxvk_state_cache.h"
#include "../wsi/ohos_window_registry.h"

namespace {

  // NDK native-object ref/unref are documented as non-thread-safe.
  std::mutex nativeObjectMutex;

  int32_t retainNativeWindow(void* window) {
    std::lock_guard<std::mutex> lock(nativeObjectMutex);
    return OH_NativeWindow_NativeObjectReference(window);
  }

  void releaseNativeWindow(void* window) {
    std::lock_guard<std::mutex> lock(nativeObjectMutex);
    OH_NativeWindow_NativeObjectUnreference(window);
  }

  dxvk::ohos::WindowRegistry windows(retainNativeWindow, releaseNativeWindow);

}

namespace dxvk::ohos {

  std::shared_ptr<WindowState> findWindow(WindowHandle handle) {
    return windows.lookup(handle);
  }

}

extern "C" {

  int32_t DXVKOhosRegisterWindow(OHNativeWindow* window,
      uint32_t width, uint32_t height, DXVKOhosWindowHandle* handle) {
    if (!handle)
      return DXVK_OHOS_WINDOW_INVALID_ARGUMENT;
    *handle = 0;
    if (!window || !width || !height)
      return DXVK_OHOS_WINDOW_INVALID_ARGUMENT;
    try {
      *handle = windows.add(window, width, height);
      return *handle ? DXVK_OHOS_WINDOW_OK : DXVK_OHOS_WINDOW_UNAVAILABLE;
    } catch (const std::bad_alloc&) {
      return DXVK_OHOS_WINDOW_OUT_OF_MEMORY;
    } catch (...) {
      return DXVK_OHOS_WINDOW_UNAVAILABLE;
    }
  }

  int32_t DXVKOhosResizeWindow(DXVKOhosWindowHandle handle,
      uint32_t width, uint32_t height) {
    try {
      return windows.resize(handle, width, height)
        ? DXVK_OHOS_WINDOW_OK : DXVK_OHOS_WINDOW_UNAVAILABLE;
    } catch (...) {
      return DXVK_OHOS_WINDOW_UNAVAILABLE;
    }
  }

  int32_t DXVKOhosUnregisterWindow(DXVKOhosWindowHandle handle) {
    try {
      return windows.remove(handle)
        ? DXVK_OHOS_WINDOW_OK : DXVK_OHOS_WINDOW_UNAVAILABLE;
    } catch (...) {
      return DXVK_OHOS_WINDOW_UNAVAILABLE;
    }
  }

  int32_t DXVKOhosGetWindowInfo(DXVKOhosWindowHandle handle,
      DXVKOhosWindowInfo* info) {
    if (!info)
      return DXVK_OHOS_WINDOW_INVALID_ARGUMENT;
    *info = {};
    try {
      dxvk::ohos::WindowLease lease(windows.lookup(handle));
      if (!lease.active())
        return DXVK_OHOS_WINDOW_UNAVAILABLE;
      *info = { lease.width(), lease.height(), lease.revision() };
      return DXVK_OHOS_WINDOW_OK;
    } catch (...) {
      return DXVK_OHOS_WINDOW_UNAVAILABLE;
    }
  }

  int32_t DXVKOhosGetStateCacheStats(DXVKOhosStateCacheStats* stats) {
    if (!stats || stats->size != sizeof(*stats))
      return DXVK_OHOS_WINDOW_INVALID_ARGUMENT;
    const auto current = dxvk::getStateCacheStats();
    *stats = { sizeof(*stats), 1, current.instances, current.filesRead,
      current.entriesRead, current.entriesWritten };
    return DXVK_OHOS_WINDOW_OK;
  }

}
