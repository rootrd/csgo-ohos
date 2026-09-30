#ifndef DXVK_NATIVE_OHOS_H
#define DXVK_NATIVE_OHOS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NativeWindow OHNativeWindow;
typedef uint64_t DXVKOhosWindowHandle;

enum DXVKOhosWindowStatus {
  DXVK_OHOS_WINDOW_OK = 0,
  DXVK_OHOS_WINDOW_INVALID_ARGUMENT = -1,
  DXVK_OHOS_WINDOW_UNAVAILABLE = -2,
  DXVK_OHOS_WINDOW_OUT_OF_MEMORY = -3
};

typedef struct DXVKOhosWindowInfo {
  uint32_t width;
  uint32_t height;
  uint64_t revision;
} DXVKOhosWindowInfo;

typedef struct DXVKOhosStateCacheStats {
  uint32_t size;
  uint32_t version;
  uint64_t instances;
  uint64_t filesRead;
  uint64_t entriesRead;
  uint64_t entriesWritten;
} DXVKOhosStateCacheStats;

typedef struct DXVKOhosPerformanceStats {
  uint32_t size;
  uint32_t version;
  uint64_t sequence;
  uint32_t fpsMilli;
  uint32_t averageFrameUs;
  uint32_t p95FrameUs;
  uint32_t gpuLoadPermille;
  uint32_t submissionsMilli;
  uint32_t averageSyncUs;
  uint32_t averageFrameWaitUs;
  uint32_t averageAcquireUs;
  uint32_t averageCsWaitUs;
  uint32_t averageQueueWaitUs;
  uint32_t averageVkPresentUs;
  uint32_t averageCommandSubmitUs;
  uint32_t maxQueueDepth;
  uint32_t renderWidth;
  uint32_t renderHeight;
  uint32_t surfaceWidth;
  uint32_t surfaceHeight;
  uint32_t presentMode;
  uint32_t imageCount;
} DXVKOhosPerformanceStats;

enum DXVKOhosPerfEventType {
  DXVK_OHOS_PERF_GAME_FRAME = 1,
  DXVK_OHOS_PERF_PRESENT = 2,
  DXVK_OHOS_PERF_TEXTURE_INIT = 3,
  DXVK_OHOS_PERF_SHADER_COMPILE = 4,
  DXVK_OHOS_PERF_PIPELINE_CREATE = 5,
  DXVK_OHOS_PERF_COMMAND_SUBMIT = 6,
  DXVK_OHOS_PERF_RESOURCE_READ = 7,
  DXVK_OHOS_PERF_NATIVE_PASS = 8,
  DXVK_OHOS_PERF_QUEUE_ENQUEUE = 9,
  DXVK_OHOS_PERF_SYNC_WAIT = 10,
  DXVK_OHOS_PERF_GAME_UPDATE = 11,
  DXVK_OHOS_PERF_RENDER_CPU = 12,
  DXVK_OHOS_PERF_GPU_FRAME = 13,
  DXVK_OHOS_PERF_TEXTURE_RESOURCE = 14,
  DXVK_OHOS_PERF_RENDER_THREAD_WAIT = 15,
  DXVK_OHOS_PERF_CONTEXT_LOCK_WAIT = 16,
  DXVK_OHOS_PERF_SAFE_MODE_WORK = 17,
  DXVK_OHOS_PERF_REMOVE_ALL_REFS = 18,
  DXVK_OHOS_PERF_GPU_FENCE_WAIT = 19,
  DXVK_OHOS_PERF_PENDING_BUFFER_UPDATES = 20,
  DXVK_OHOS_PERF_GAME_LOGIC_UPDATE = 21,
  DXVK_OHOS_PERF_DRAWLIST_BUILD = 22,
  DXVK_OHOS_PERF_CONTROL_THREAD_WAIT = 23,
  DXVK_OHOS_PERF_AUDIO_WAIT = 24,
  DXVK_OHOS_PERF_AUDIO_SEMA_WAIT = 25,
  DXVK_OHOS_PERF_AUDIO_POST_WORK = 26,
  DXVK_OHOS_PERF_AUDIO_MIXER_COMMAND_WAIT = 27,
  DXVK_OHOS_PERF_AUDIO_WORKER_UPDATE = 28,
  DXVK_OHOS_PERF_AUDIO_WORKER_REQUEST = 29,
  DXVK_OHOS_PERF_AUDIO_WORKER_WAKE = 30,
  DXVK_OHOS_PERF_AUDIO_WORKER_DONE = 31
};

typedef struct DXVKOhosPerfEvent {
  uint64_t sequence;
  uint64_t monotonicNs;
  uint64_t frameId;
  uint32_t type;
  uint32_t threadId;
  uint32_t durationUs;
  uint32_t flags;
  uint64_t a;
  uint64_t b;
  uint64_t c;
  uint64_t d;
  uint64_t e;
  uint64_t f;
} DXVKOhosPerfEvent;

typedef struct DXVKOhosPerfReadout {
  uint32_t size;
  uint32_t version;
  uint32_t capacity;
  uint32_t count;
  uint64_t dropped;
  DXVKOhosPerfEvent* events;
} DXVKOhosPerfReadout;

#define DXVK_OHOS_API __attribute__((visibility("default")))

/* Register during the XComponent surface-created callback, while window is
 * valid. DXVK retains one native-object reference. The returned value is an
 * opaque, non-reused ID, not an OHNativeWindow pointer. On ARM64 it will be
 * passed as the native DXGI OutputWindow/ HWND value by the WSI frontend.
 * A native window may have only one live registration.
 */
DXVK_OHOS_API int32_t DXVKOhosRegisterWindow(
  OHNativeWindow* window, uint32_t width, uint32_t height,
  DXVKOhosWindowHandle* handle);

/* Call from surface-changed. Zero width or height suspends presentation.
 * A size change advances revision and requires swapchain recreation.
 */
DXVK_OHOS_API int32_t DXVKOhosResizeWindow(
  DXVKOhosWindowHandle handle, uint32_t width, uint32_t height);

/* Call before returning from surface-destroyed. Retires the ID and waits for
 * any in-progress WSI lease to finish. Subsequent lookups/operations fail.
 * The native reference remains held until existing Presenter owners release
 * their surfaces. A newly created surface must get a new registration.
 * Lifecycle functions must not be called from inside a WSI lease.
 */
DXVK_OHOS_API int32_t DXVKOhosUnregisterWindow(DXVKOhosWindowHandle handle);

DXVK_OHOS_API int32_t DXVKOhosGetWindowInfo(
  DXVKOhosWindowHandle handle, DXVKOhosWindowInfo* info);

/* Process-lifetime, read-only telemetry for deterministic cache validation.
 * It does not expose Vulkan handles or alter cache policy.
 */
DXVK_OHOS_API int32_t DXVKOhosGetStateCacheStats(
  DXVKOhosStateCacheStats* stats);

/* Rolling one-second telemetry from the active D3D11 present path. GPU load
 * uses DXVK's queue-idle counter and therefore represents this process's GPU
 * workload, not device-wide utilization. This call is read-only and lock-free.
 */
DXVK_OHOS_API int32_t DXVKOhosGetPerformanceStats(
  DXVKOhosPerformanceStats* stats);

/* Optional bounded trace. a-d are event-specific counters; zero means unknown.
 * The read call drains at most capacity events without waiting for the GPU. */
DXVK_OHOS_API int32_t DXVKOhosReadPerfEvents(DXVKOhosPerfReadout* readout);
DXVK_OHOS_API void DXVKOhosRecordPerfEvent(uint32_t type, uint64_t frameId,
  uint32_t durationUs, uint64_t a, uint64_t b, uint64_t c, uint64_t d);

/* Test-only, same-process fault injection. The call is rejected unless
 * DXVK_OHOS_TEST_DEVICE_LOST=1. It marks the supplied DXVK-backed D3D11
 * device as lost so the normal GetDeviceRemovedReason path can be tested.
 */
DXVK_OHOS_API int32_t DXVKOhosInjectDeviceLost(void* d3d11Device);

/* Opt-in, one-shot scene diagnostics. Called on the game's render thread
 * after its first world frame; no effect unless DXVK_OHOS_CAPTURE_SCENE=1.
 */
DXVK_OHOS_API void DXVKOhosCaptureScene(void);

#ifdef __cplusplus
}
#endif

#endif
