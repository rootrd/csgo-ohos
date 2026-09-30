#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <numeric>

#if defined(__linux__) || defined(DXVK_NATIVE_OHOS)
#include <unistd.h>
#include <limits.h>
#endif

#if defined(DXVK_NATIVE_OHOS)
#include <qos/qos.h>
#endif

#include "util_env.h"
#include "./log/log.h"

#include "./com/com_include.h"

namespace dxvk::env {

#if defined(DXVK_NATIVE_OHOS)
  static bool getOhosThreadQos(const std::string& name, QoS_Level& level) {
    if (name == "dxvk-cs" || name == "dxvk-submit") {
      level = QOS_USER_INTERACTIVE;
      return true;
    }

    if (name == "dxvk-queue" || name == "dxvk-shader") {
      level = QOS_USER_INITIATED;
      return true;
    }

    if (name == "dxvk-writer") {
      level = QOS_UTILITY;
      return true;
    }

    return false;
  }


  static void setOhosThreadQos(const std::string& name) {
    const char* mobileScheduler = std::getenv("GTAV_OHOS_MOBILE_SCHEDULER");
    if (mobileScheduler == nullptr || std::strcmp(mobileScheduler, "1") != 0)
      return;

    QoS_Level requested = QOS_DEFAULT;
    if (!getOhosThreadQos(name, requested))
      return;

    if (OH_QoS_SetThreadQoS(requested) != 0) {
      Logger::warn(str::format(
        "DXVK_OHOS_THREAD_QOS name=", name,
        " requested=", int32_t(requested),
        " status=set-failed"));
      return;
    }

    QoS_Level actual = QOS_DEFAULT;
    if (OH_QoS_GetThreadQoS(&actual) != 0) {
      Logger::warn(str::format(
        "DXVK_OHOS_THREAD_QOS name=", name,
        " requested=", int32_t(requested),
        " status=query-failed"));
      return;
    }

    Logger::info(str::format(
      "DXVK_OHOS_THREAD_QOS name=", name,
      " requested=", int32_t(requested),
      " actual=", int32_t(actual),
      " status=active"));
  }
#endif

  std::string getEnvVar(const char* name) {
#ifdef _WIN32
    std::vector<WCHAR> result;
    result.resize(MAX_PATH + 1);

    DWORD len = ::GetEnvironmentVariableW(str::tows(name).c_str(), result.data(), MAX_PATH);
    result.resize(len);

    return str::fromws(result.data());
#else
    const char* result = std::getenv(name);
    return result ? result : "";
#endif
  }


  size_t matchFileExtension(const std::string& name, const char* ext) {
    auto pos = name.find_last_of('.');

    if (pos == std::string::npos)
      return pos;

    bool matches = std::accumulate(name.begin() + pos + 1, name.end(), true,
      [&ext] (bool current, char a) {
        if (a >= 'A' && a <= 'Z')
          a += 'a' - 'A';
        return current && *ext && a == *(ext++);
      });

    return matches ? pos : std::string::npos;
  }


  std::string getExeName() {
    std::string fullPath = getExePath();
    auto n = fullPath.find_last_of(env::PlatformDirSlash);

    return (n != std::string::npos)
      ? fullPath.substr(n + 1)
      : fullPath;
  }


  std::string getExeBaseName() {
    auto exeName = getExeName();
#ifdef _WIN32
    auto extp = matchFileExtension(exeName, "exe");

    if (extp != std::string::npos)
      exeName.erase(extp);
#endif

    return exeName;
  }


  std::string getExePath() {
#if defined(_WIN32)
    std::vector<WCHAR> exePath;
    exePath.resize(MAX_PATH + 1);

    DWORD len = ::GetModuleFileNameW(NULL, exePath.data(), MAX_PATH);
    exePath.resize(len);

    return str::fromws(exePath.data());
#elif defined(__linux__) || defined(DXVK_NATIVE_OHOS)
    std::array<char, PATH_MAX> exePath = {};

    const ssize_t count = readlink("/proc/self/exe", exePath.data(), exePath.size());

    return count > 0
      ? std::string(exePath.data(), static_cast<size_t>(count))
      : std::string();
#else
    return std::string();
#endif
  }


  void setThreadName(const std::string& name) {
#ifdef _WIN32
    using SetThreadDescriptionProc = HRESULT (WINAPI *) (HANDLE, PCWSTR);

    static auto proc = reinterpret_cast<SetThreadDescriptionProc>(
      ::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"));

    if (proc != nullptr) {
      auto wideName = std::vector<WCHAR>(name.length() + 1);
      str::tows(name.c_str(), wideName.data(), wideName.size());
      (*proc)(::GetCurrentThread(), wideName.data());
    }
#else
    std::array<char, 16> posixName = {};
    dxvk::str::strlcpy(posixName.data(), name.c_str(), 16);
    ::pthread_setname_np(pthread_self(), posixName.data());
#if defined(DXVK_NATIVE_OHOS)
    setOhosThreadQos(name);
#endif
#endif
  }


  bool createDirectory(const std::string& path) {
#ifdef _WIN32
    WCHAR widePath[MAX_PATH];
    str::tows(path.c_str(), widePath);
    return !!CreateDirectoryW(widePath, nullptr);
#else
    // DXVK treats the state-cache path as optional. On OHOS, unlike Win32
    // CreateDirectoryW, std::filesystem throws for an empty path, so avoid
    // making device creation depend on an optional cache directory.
    if (path.empty())
      return false;

    std::error_code error;
    if (std::filesystem::create_directories(path, error))
      return true;

    return !error && std::filesystem::is_directory(path, error);
#endif
  }

}
