#pragma once

#include "dxgi_include.h"
#include "../../include/native/ohos/dxvk_native_ohos.h"
#include "../vulkan/vulkan_loader.h"

namespace dxvk::ohos {

  inline HRESULT windowInfo(HWND window, DXVKOhosWindowInfo& info) {
    return DXVKOhosGetWindowInfo(reinterpret_cast<uintptr_t>(window), &info) == DXVK_OHOS_WINDOW_OK
      ? S_OK : DXGI_ERROR_INVALID_CALL;
  }

  inline HRESULT normalizeSwapchainDesc(HWND window, DXGI_SWAP_CHAIN_DESC1& desc) {
    DXVKOhosWindowInfo info = { };
    const auto status = windowInfo(window, info);
    if (FAILED(status)) return status;
    if (!desc.Width) desc.Width = info.width;
    if (!desc.Height) desc.Height = info.height;
    if (!desc.Width || !desc.Height || desc.Width > 16384 || desc.Height > 16384
     || !desc.BufferCount || desc.BufferCount > DXGI_MAX_SWAP_CHAIN_BUFFERS
     || !desc.SampleDesc.Count)
      return DXGI_ERROR_INVALID_CALL;
    // ArkUI owns display mode and window placement. Win32 waitable handles,
    // GDI, protected/composition buffers cannot be silently advertised here.
    constexpr UINT supportedFlags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    if (desc.Stereo || (desc.Flags & ~supportedFlags)
     || desc.Scaling != DXGI_SCALING_STRETCH
     || (desc.AlphaMode != DXGI_ALPHA_MODE_IGNORE && desc.AlphaMode != DXGI_ALPHA_MODE_UNSPECIFIED))
      return DXGI_ERROR_UNSUPPORTED;
    switch (desc.SwapEffect) {
      case DXGI_SWAP_EFFECT_DISCARD:
      case DXGI_SWAP_EFFECT_SEQUENTIAL:
        break;
      case DXGI_SWAP_EFFECT_FLIP_DISCARD:
      case DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL:
        if (desc.BufferCount < 2 || desc.SampleDesc.Count != 1)
          return DXGI_ERROR_INVALID_CALL;
        break;
      default: return DXGI_ERROR_INVALID_CALL;
    }
    return S_OK;
  }

  inline HRESULT presentResult(VkResult status) {
    switch (status) {
      case VK_SUCCESS:
      case VK_SUBOPTIMAL_KHR: return S_OK;
      case VK_TIMEOUT:
      case VK_NOT_READY: return DXGI_ERROR_WAS_STILL_DRAWING;
      case VK_ERROR_OUT_OF_DATE_KHR: return DXGI_STATUS_OCCLUDED;
      case VK_ERROR_SURFACE_LOST_KHR: return DXGI_ERROR_INVALID_CALL;
      case VK_ERROR_DEVICE_LOST: return DXGI_ERROR_DEVICE_REMOVED;
      case VK_ERROR_OUT_OF_HOST_MEMORY:
      case VK_ERROR_OUT_OF_DEVICE_MEMORY: return E_OUTOFMEMORY;
      default: return DXGI_ERROR_DRIVER_INTERNAL_ERROR;
    }
  }

}
