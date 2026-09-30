#pragma once

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace dxvk::ohos {

  using WindowHandle = uint64_t;
  using NativeRetain = int32_t (*)(void*);
  using NativeRelease = void (*)(void*);

  // One immutable native-object identity; revision changes on resize, while
  // retirement is permanent. A new surface always receives a new handle.
  class WindowState {
  public:
    WindowState(void* window, uint32_t width, uint32_t height, NativeRelease release)
    : m_window(window), m_width(width), m_height(height), m_release(release) { }

    ~WindowState() {
      m_release(m_window);
    }

    bool active() const {
      return m_active.load(std::memory_order_acquire);
    }

  private:
    friend class WindowLease;
    friend class WindowRegistry;

    std::mutex m_mutex;
    std::atomic<bool> m_active { true };
    void* const m_window;
    uint32_t m_width;
    uint32_t m_height;
    uint64_t m_revision = 1;
    NativeRelease const m_release;
  };

  // Hold this across a Vulkan WSI operation. Retirement first prevents new
  // operations, then waits for this lock before returning to XComponent.
  class WindowLease {
  public:
    explicit WindowLease(std::shared_ptr<WindowState> state)
    : m_state(std::move(state)),
      m_lock(m_state ? std::unique_lock<std::mutex>(m_state->m_mutex)
                     : std::unique_lock<std::mutex>()) { }

    WindowLease(WindowLease&&) = default;
    WindowLease& operator=(WindowLease&&) = delete;
    WindowLease(const WindowLease&) = delete;
    WindowLease& operator=(const WindowLease&) = delete;

    bool active() const { return m_state && m_state->active(); }
    bool drawable() const { return active() && width() && height(); }
    void* nativeWindow() const { return active() ? m_state->m_window : nullptr; }
    uint32_t width() const { return m_state ? m_state->m_width : 0; }
    uint32_t height() const { return m_state ? m_state->m_height : 0; }
    uint64_t revision() const { return m_state ? m_state->m_revision : 0; }

  private:
    // Order matters: unlock before releasing the last native-object owner.
    std::shared_ptr<WindowState> m_state;
    std::unique_lock<std::mutex> m_lock;
  };

  class WindowRegistry {
  public:
    WindowRegistry(NativeRetain retain, NativeRelease release)
    : m_retain(retain), m_release(release) { }

    WindowHandle add(void* window, uint32_t width, uint32_t height) {
      if (!window || !width || !height)
        return 0;

      std::lock_guard<std::mutex> lock(m_mutex);
      if (!m_next || m_nativeWindows.count(window))
        return 0;
      if (m_retain(window) != 0)
        return 0;

      std::shared_ptr<WindowState> state;
      try {
        state = std::make_shared<WindowState>(window, width, height, m_release);
      } catch (...) {
        m_release(window);
        throw;
      }

      const WindowHandle handle = m_next++;
      m_windows.emplace(handle, state);
      try {
        m_nativeWindows.emplace(window, handle);
      } catch (...) {
        m_windows.erase(handle);
        throw;
      }
      return handle;
    }

    std::shared_ptr<WindowState> lookup(WindowHandle handle) const {
      std::lock_guard<std::mutex> lock(m_mutex);
      const auto entry = m_windows.find(handle);
      return entry != m_windows.end() ? entry->second : nullptr;
    }

    bool resize(WindowHandle handle, uint32_t width, uint32_t height) {
      auto state = lookup(handle);
      if (!state)
        return false;
      std::lock_guard<std::mutex> lock(state->m_mutex);
      if (!state->active())
        return false;
      if (state->m_width != width || state->m_height != height) {
        // Do not permit a revision wrap to make an old swapchain current.
        if (state->m_revision == std::numeric_limits<uint64_t>::max())
          return false;
        state->m_width = width;
        state->m_height = height;
        ++state->m_revision;
      }
      return true;
    }

    bool remove(WindowHandle handle) {
      std::shared_ptr<WindowState> state;
      {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto entry = m_windows.find(handle);
        if (entry == m_windows.end())
          return false;
        state = entry->second;
        state->m_active.store(false, std::memory_order_release);
        m_windows.erase(entry);
      }
      // Never wait on a WSI operation while holding the registry mutex.
      std::lock_guard<std::mutex> finishOperation(state->m_mutex);
      // Keep the native identity reserved while the old operation drains.
      // Otherwise a concurrent add could start a second generation on it.
      std::lock_guard<std::mutex> lock(m_mutex);
      m_nativeWindows.erase(state->m_window);
      return true;
    }

  private:
    NativeRetain const m_retain;
    NativeRelease const m_release;
    mutable std::mutex m_mutex;
    WindowHandle m_next = 1;
    std::unordered_map<WindowHandle, std::shared_ptr<WindowState>> m_windows;
    std::unordered_map<void*, WindowHandle> m_nativeWindows;
  };

  // Defined once in libdxvk_dxgi. D3D11/Presenter must use that same registry.
  std::shared_ptr<WindowState> findWindow(WindowHandle handle);

}
