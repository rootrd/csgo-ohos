#include "util_string.h"

#if defined(DXVK_NATIVE_OHOS)
#include <algorithm>
#include <codecvt>
#include <locale>
#endif

namespace dxvk::str {
  std::string fromws(const WCHAR *ws) {
#if defined(DXVK_NATIVE_OHOS)
    if (!ws)
      return "";

    try {
      std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
      return converter.to_bytes(ws);
    } catch (const std::range_error&) {
      return "";
    }
#else
    size_t len = ::WideCharToMultiByte(CP_UTF8,
      0, ws, -1, nullptr, 0, nullptr, nullptr);

    if (len <= 1)
      return "";

    len -= 1;

    std::string result;
    result.resize(len);
    ::WideCharToMultiByte(CP_UTF8, 0, ws, -1,
      &result.at(0), len, nullptr, nullptr);
    return result;
#endif
  }


  void tows(const char* mbs, WCHAR* wcs, size_t wcsLen) {
#if defined(DXVK_NATIVE_OHOS)
    if (!wcs || !wcsLen)
      return;

    try {
      std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
      std::wstring result = converter.from_bytes(mbs ? mbs : "");
      size_t length = std::min(result.size(), wcsLen - 1);
      std::copy_n(result.data(), length, wcs);
      wcs[length] = L'\0';
    } catch (const std::range_error&) {
      wcs[0] = L'\0';
    }
#else
    ::MultiByteToWideChar(
      CP_UTF8, 0, mbs, -1,
      wcs, wcsLen);
#endif
  }

  std::wstring tows(const char* mbs) {
#if defined(DXVK_NATIVE_OHOS)
    if (!mbs)
      return L"";

    try {
      std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
      return converter.from_bytes(mbs);
    } catch (const std::range_error&) {
      return L"";
    }
#else
    size_t len = ::MultiByteToWideChar(CP_UTF8,
      0, mbs, -1, nullptr, 0);
    
    if (len <= 1)
      return L"";

    len -= 1;

    std::wstring result;
    result.resize(len);
    ::MultiByteToWideChar(CP_UTF8, 0, mbs, -1,
      &result.at(0), len);
    return result;
#endif
  }

}
