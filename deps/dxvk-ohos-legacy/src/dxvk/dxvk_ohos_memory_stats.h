#pragma once

#include <atomic>
#include <cstdint>

/* Process-local accounting of the resources DXVK keeps alive.  The OHOS port
 * needs this because the system reclaims a process by its graphics-memory
 * footprint, and the driver-side numbers alone cannot tell whether the bulk is
 * textures, render targets or buffers. */
namespace dxvk {

  inline std::atomic<uint64_t>& ohosImageBytes() {
    static std::atomic<uint64_t> value { 0 };
    return value;
  }

  inline std::atomic<uint64_t>& ohosImageCount() {
    static std::atomic<uint64_t> value { 0 };
    return value;
  }

  inline std::atomic<uint64_t>& ohosLargeImageBytes() {
    static std::atomic<uint64_t> value { 0 };
    return value;
  }

  inline std::atomic<uint64_t>& ohosLargeImageCount() {
    static std::atomic<uint64_t> value { 0 };
    return value;
  }

  inline std::atomic<uint64_t>& ohosBufferBytes() {
    static std::atomic<uint64_t> value { 0 };
    return value;
  }

  inline std::atomic<uint64_t>& ohosBufferCount() {
    static std::atomic<uint64_t> value { 0 };
    return value;
  }

  /* Images of this size or larger are counted separately: a handful of
   * oversized render targets or streamed textures dominate the footprint. */
  constexpr uint64_t OhosLargeImageThreshold = 16ull << 20;

  inline void ohosAccumulateImage(uint64_t bytes) {
    ohosImageBytes().fetch_add(bytes, std::memory_order_relaxed);
    ohosImageCount().fetch_add(1, std::memory_order_relaxed);
    if (bytes >= OhosLargeImageThreshold) {
      ohosLargeImageBytes().fetch_add(bytes, std::memory_order_relaxed);
      ohosLargeImageCount().fetch_add(1, std::memory_order_relaxed);
    }
  }

  inline void ohosReleaseImage(uint64_t bytes) {
    ohosImageBytes().fetch_sub(bytes, std::memory_order_relaxed);
    ohosImageCount().fetch_sub(1, std::memory_order_relaxed);
    if (bytes >= OhosLargeImageThreshold) {
      ohosLargeImageBytes().fetch_sub(bytes, std::memory_order_relaxed);
      ohosLargeImageCount().fetch_sub(1, std::memory_order_relaxed);
    }
  }

  inline void ohosAccumulateBuffer(uint64_t bytes) {
    ohosBufferBytes().fetch_add(bytes, std::memory_order_relaxed);
    ohosBufferCount().fetch_add(1, std::memory_order_relaxed);
  }

  inline void ohosReleaseBuffer(uint64_t bytes) {
    ohosBufferBytes().fetch_sub(bytes, std::memory_order_relaxed);
    ohosBufferCount().fetch_sub(1, std::memory_order_relaxed);
  }

}
