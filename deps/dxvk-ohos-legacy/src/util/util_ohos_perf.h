#pragma once

#if defined(DXVK_NATIVE_OHOS)

#include "../../include/native/ohos/dxvk_native_ohos.h"

#include <cstdlib>
#include <ctime>
#include <sys/syscall.h>
#include <unistd.h>

extern "C" DXVK_OHOS_API void DXVKOhosSubmitPerfEvent(
  const DXVKOhosPerfEvent* event);

namespace dxvk::ohosperf {

  inline bool enabled() {
    static const bool active = [] {
      const char* value = std::getenv("GTAV_OHOS_PERF_TRACE");
      return value && value[0] == '1' && value[1] == '\0';
    }();
    return active;
  }

  inline uint64_t nowNs() {
    timespec now = {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return uint64_t(now.tv_sec) * 1000000000ull + uint64_t(now.tv_nsec);
  }

  inline uint32_t elapsedUs(uint64_t startNs) {
    const uint64_t value = (nowNs() - startNs) / 1000ull;
    return uint32_t(value > UINT32_MAX ? UINT32_MAX : value);
  }

  inline void record(uint32_t type, uint64_t frameId, uint32_t durationUs,
      uint64_t a = 0, uint64_t b = 0, uint64_t c = 0, uint64_t d = 0,
      uint64_t e = 0, uint64_t f = 0, uint32_t flags = 0) {
    if (!enabled()) return;
    DXVKOhosPerfEvent event = {};
    event.monotonicNs = nowNs();
    event.frameId = frameId;
    event.type = type;
    event.threadId = uint32_t(syscall(SYS_gettid));
    event.durationUs = durationUs;
    event.flags = flags;
    event.a = a;
    event.b = b;
    event.c = c;
    event.d = d;
    event.e = e;
    event.f = f;
    DXVKOhosSubmitPerfEvent(&event);
  }
}

#endif
