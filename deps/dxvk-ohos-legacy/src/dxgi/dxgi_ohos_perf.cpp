#include "../util/util_ohos_perf.h"

#if defined(DXVK_NATIVE_OHOS)

#include <algorithm>
#include <array>
#include <mutex>

namespace {
struct PerfStore {
  static constexpr uint32_t Capacity = 16384;
  std::mutex mutex;
  std::array<DXVKOhosPerfEvent, Capacity> events = {};
  uint32_t head = 0;
  uint32_t count = 0;
  uint64_t nextSequence = 0;
  uint64_t dropped = 0;
  uint64_t lastFrameId = 0;
};

PerfStore& store() {
  static PerfStore state;
  return state;
}
}

extern "C" DXVK_OHOS_API void DXVKOhosSubmitPerfEvent(
    const DXVKOhosPerfEvent* event) {
  if (!event || !dxvk::ohosperf::enabled()) return;
  auto& state = store();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (event->type == DXVK_OHOS_PERF_PRESENT && event->frameId)
    state.lastFrameId = event->frameId;
  DXVKOhosPerfEvent entry = *event;
  entry.sequence = ++state.nextSequence;
  if (!entry.frameId) entry.frameId = state.lastFrameId;
  if (state.count == PerfStore::Capacity) {
    ++state.dropped;
    return;
  }
  state.events[(state.head + state.count) % PerfStore::Capacity] = entry;
  ++state.count;
}

extern "C" DXVK_OHOS_API void DXVKOhosRecordPerfEvent(
    uint32_t type, uint64_t frameId, uint32_t durationUs,
    uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
  dxvk::ohosperf::record(type, frameId, durationUs, a, b, c, d);
}

extern "C" DXVK_OHOS_API int32_t DXVKOhosReadPerfEvents(
    DXVKOhosPerfReadout* readout) {
  if (!readout || readout->size != sizeof(*readout) ||
      !readout->events || !readout->capacity)
    return DXVK_OHOS_WINDOW_INVALID_ARGUMENT;
  auto& state = store();
  std::lock_guard<std::mutex> lock(state.mutex);
  readout->version = 1;
  readout->count = std::min(readout->capacity, state.count);
  readout->dropped = state.dropped;
  for (uint32_t index = 0; index < readout->count; ++index)
    readout->events[index] = state.events[(state.head + index) % PerfStore::Capacity];
  state.head = (state.head + readout->count) % PerfStore::Capacity;
  state.count -= readout->count;
  return DXVK_OHOS_WINDOW_OK;
}

#endif
