#include "util_monitor.h"
#include "util_string.h"

#include "./log/log.h"

#include <cstdio>
#include <cstdlib>

namespace dxvk {

#if defined(DXVK_NATIVE_OHOS)

  // OHOS has no Win32 display APIs. The engine exports the real display size
  // (from SDL_GetDesktopDisplayMode) as CSGO_OHOS_DISPLAY="WxH" before loading
  // us; fall back to a sane landscape default when absent.
  static void OhosDisplaySize(UINT* pWidth, UINT* pHeight) {
    UINT w = 1280, h = 720;
    if (const char* env = ::getenv("CSGO_OHOS_DISPLAY")) {
      unsigned ew = 0, eh = 0;
      if (::sscanf(env, "%ux%u", &ew, &eh) == 2 && ew > 0 && eh > 0) {
        w = ew;
        h = eh;
      }
    }
    if (pWidth)
      *pWidth = w;
    if (pHeight)
      *pHeight = h;
  }

  HMONITOR GetDefaultMonitor() {
    return (HMONITOR)(uintptr_t)1;
  }

  BOOL SetMonitorDisplayMode(HMONITOR, DEVMODEW*) {
    return FALSE;
  }

  BOOL GetMonitorDisplayMode(HMONITOR, DWORD modeIndex, DEVMODEW* pMode) {
    if (!pMode)
      return FALSE;
    // OHOS 单显示器：只报一个原生模式。modeIndex 递增枚举时必须返回 FALSE
    // 终止调用方的 while 循环（CacheModes 依赖它结束，否则 100% CPU 无限转）
    if (modeIndex != 0 && modeIndex != ENUM_CURRENT_SETTINGS && modeIndex != ENUM_REGISTRY_SETTINGS)
      return FALSE;
    UINT w = 0, h = 0;
    OhosDisplaySize(&w, &h);
    pMode->dmPelsWidth  = w;
    pMode->dmPelsHeight = h;
    pMode->dmDisplayFrequency = 60;
    fprintf(stderr, "CSGO_TRACE: dxvk GetMonitorDisplayMode idx=%u -> %ux%u\n", modeIndex, w, h);
    return TRUE;
  }

  BOOL RestoreMonitorDisplayMode() {
    return TRUE;
  }

  void GetWindowClientSize(HWND, UINT* pWidth, UINT* pHeight) {
    OhosDisplaySize(pWidth, pHeight);
  }

  void GetMonitorClientSize(HMONITOR, UINT* pWidth, UINT* pHeight) {
    OhosDisplaySize(pWidth, pHeight);
  }

  void GetMonitorRect(HMONITOR, RECT* pRect) {
    UINT w = 0, h = 0;
    OhosDisplaySize(&w, &h);
    if (pRect)
      *pRect = RECT { 0, 0, (LONG)w, (LONG)h };
  }

#else
  
  HMONITOR GetDefaultMonitor() {
    return ::MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
  }


  BOOL SetMonitorDisplayMode(
          HMONITOR                hMonitor,
          DEVMODEW*               pMode) {
    ::MONITORINFOEXW monInfo;
    monInfo.cbSize = sizeof(monInfo);

    if (!::GetMonitorInfoW(hMonitor, reinterpret_cast<MONITORINFO*>(&monInfo))) {
      Logger::err("Failed to query monitor info");
      return E_FAIL;
    }

    Logger::info(str::format("Setting display mode: ",
      pMode->dmPelsWidth, "x", pMode->dmPelsHeight, "@",
      pMode->dmDisplayFrequency));

    DEVMODEW curMode = { };
    curMode.dmSize = sizeof(curMode);

    if (GetMonitorDisplayMode(hMonitor, ENUM_CURRENT_SETTINGS, &curMode)) {
      bool eq = curMode.dmPelsWidth  == pMode->dmPelsWidth
             && curMode.dmPelsHeight == pMode->dmPelsHeight
             && curMode.dmBitsPerPel == pMode->dmBitsPerPel;

      if (pMode->dmFields & DM_DISPLAYFREQUENCY)
        eq &= curMode.dmDisplayFrequency == pMode->dmDisplayFrequency;

      if (eq)
        return true;
    }

    LONG status = ::ChangeDisplaySettingsExW(monInfo.szDevice,
      pMode, nullptr, CDS_FULLSCREEN, nullptr);

    if (status != DISP_CHANGE_SUCCESSFUL) {
      pMode->dmFields &= ~DM_DISPLAYFREQUENCY;

      status = ::ChangeDisplaySettingsExW(monInfo.szDevice,
        pMode, nullptr, CDS_FULLSCREEN, nullptr);
    }

    return status == DISP_CHANGE_SUCCESSFUL;
  }


  BOOL GetMonitorDisplayMode(
          HMONITOR                hMonitor,
          DWORD                   modeNum,
          DEVMODEW*               pMode) {
    ::MONITORINFOEXW monInfo;
    monInfo.cbSize = sizeof(monInfo);

    if (!::GetMonitorInfoW(hMonitor, reinterpret_cast<MONITORINFO*>(&monInfo))) {
      Logger::err("Failed to query monitor info");
      return false;
    }

    return ::EnumDisplaySettingsW(monInfo.szDevice, modeNum, pMode);
  }


  BOOL CALLBACK RestoreMonitorDisplayModeCallback(
          HMONITOR                hMonitor,
          HDC                     hDC,
          LPRECT                  pRect,
          LPARAM                  pUserdata) {
    auto success = reinterpret_cast<bool*>(pUserdata);

    DEVMODEW devMode = { };
    devMode.dmSize = sizeof(devMode);

    if (!GetMonitorDisplayMode(hMonitor, ENUM_REGISTRY_SETTINGS, &devMode)) {
      *success = false;
      return false;
    }

    Logger::info(str::format("Restoring display mode: ",
      devMode.dmPelsWidth, "x", devMode.dmPelsHeight, "@",
      devMode.dmDisplayFrequency));

    if (!SetMonitorDisplayMode(hMonitor, &devMode)) {
      *success = false;
      return false;
    }

    return true;
  }


  BOOL RestoreMonitorDisplayMode() {
    bool success = true;
    bool result = ::EnumDisplayMonitors(nullptr, nullptr,
      &RestoreMonitorDisplayModeCallback,
      reinterpret_cast<LPARAM>(&success));

    return result && success;
  }


  void GetWindowClientSize(
          HWND                    hWnd,
          UINT*                   pWidth,
          UINT*                   pHeight) {
    RECT rect = { };
    ::GetClientRect(hWnd, &rect);
    
    if (pWidth)
      *pWidth = rect.right - rect.left;
    
    if (pHeight)
      *pHeight = rect.bottom - rect.top;
  }


  void GetMonitorClientSize(
          HMONITOR                hMonitor,
          UINT*                   pWidth,
          UINT*                   pHeight) {
    ::MONITORINFOEXW monInfo;
    monInfo.cbSize = sizeof(monInfo);

    if (!::GetMonitorInfoW(hMonitor, reinterpret_cast<MONITORINFO*>(&monInfo))) {
      Logger::err("Failed to query monitor info");
      return;
    }
    
    auto rect = monInfo.rcMonitor;

    if (pWidth)
      *pWidth = rect.right - rect.left;
    
    if (pHeight)
      *pHeight = rect.bottom - rect.top;
  }


  void GetMonitorRect(
          HMONITOR                hMonitor,
          RECT*                   pRect) {
    ::MONITORINFOEXW monInfo;
    monInfo.cbSize = sizeof(monInfo);

    if (!::GetMonitorInfoW(hMonitor, reinterpret_cast<MONITORINFO*>(&monInfo))) {
      Logger::err("Failed to query monitor info");
      return;
    }

    *pRect = monInfo.rcMonitor;
  }

#endif

}
