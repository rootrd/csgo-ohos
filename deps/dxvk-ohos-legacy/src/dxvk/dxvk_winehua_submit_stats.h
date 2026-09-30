#pragma once

#include <atomic>
#include <chrono>
#include <ctime>
#include <thread>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <mutex>

#include "../util/util_string.h"
#include "dxvk_winehua_trace.h"

namespace dxvk {

  /**
   * \brief Submit accounting for the performance investigation
   *
   * The D3D11 CPU side was observed to issue between three and fourteen
   * physical queue submissions per frame, with the GPU queue barely ever
   * waiting, so the frames are limited by CPU work and by the granularity of
   * the CPU to GPU hand-off rather than by the GPU itself.  Classifying where
   * those submissions come from is the prerequisite for merging any of them:
   * only redundant submissions may be merged, never the ones the CPU has to
   * observe (present, explicit flush, map/readback, query results, fences).
   *
   * The counters are process-wide and reported every kReportInterval
   * submissions, together with a mirrored copy next to the shader dumps, which
   * the launcher points at a directory the host can read back.
   */
  enum class WinehuaSubmitReason : uint32_t {
    CommandList = 0,   // DxvkCommandList::submit
    FlushCommandList,  // DxvkContext::flushCommandList
    ReasonCount,
  };

  /**
   * \brief Query path accounting
   *
   * GTA V polls occlusion queries every frame.  A GetData that finds the result
   * not ready makes DXVK flush and submit again, so the same query can produce
   * several physical submissions per frame; counting begin/end/getdata and the
   * flushes they trigger is what separates redundant submissions from the ones
   * the CPU genuinely has to observe.
   */
  struct WinehuaQueryCounters {
    std::atomic<uint64_t> begin { 0 };
    std::atomic<uint64_t> end { 0 };
    std::atomic<uint64_t> getData { 0 };
    std::atomic<uint64_t> getDataPending { 0 };
  };

  inline WinehuaQueryCounters& winehuaQueryCounters() {
    static WinehuaQueryCounters counters;
    return counters;
  }

  /**
   * \brief Swapchain state reported alongside the counters
   *
   * The configured frame latency is only a request: the D3D11 buffer count the
   * application asked for and the number of swap images actually created can
   * both cap it, so the effective value has to be reported too.
   */
  struct WinehuaPresentState {
    std::atomic<uint32_t> imageCount { 0 };
    std::atomic<uint32_t> presentMode { UINT32_MAX };
    std::atomic<int32_t>  configuredLatency { 0 };
    std::atomic<int32_t>  actualLatency { 0 };
  };

  inline WinehuaPresentState& winehuaPresentState() {
    static WinehuaPresentState state;
    return state;
  }

  /**
   * \brief Long frame attribution
   *
   * The frame time series contains multi-second outliers while the GPU queue is
   * idle, so the cost is on the CPU side of one single frame.  Counting the
   * events that can block a frame (pipeline compilation, uploads, submissions,
   * query polling) and reporting the deltas whenever a frame exceeds the
   * threshold says which of them the stall belongs to, instead of guessing.
   */
  struct WinehuaHitchCounters {
    std::atomic<uint64_t> pipelineCompiles { 0 };
    std::atomic<uint64_t> pipelineCompileUs { 0 };
  };

  inline WinehuaHitchCounters& winehuaHitchCounters() {
    static WinehuaHitchCounters counters;
    return counters;
  }

  inline uint64_t winehuaNowUs() {
    struct timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000ull + uint64_t(ts.tv_nsec) / 1000ull;
  }

  /**
   * \brief Streaming chain ledger
   *
   * In-game frames of 100-260 ms occur roughly every ten seconds with no
   * pipeline activity at all, so the remaining candidates are the streaming
   * chain: reading resource data, re-encoding BC into the device format, then
   * creating images and uploading.  The counters below account for that chain
   * per frame, so a long frame states where its milliseconds went instead of
   * leaving the layer to be guessed.
   */
  struct WinehuaChainCounters {
    std::atomic<uint64_t> transcodeCalls { 0 };
    std::atomic<uint64_t> transcodeBytes { 0 };
    std::atomic<uint64_t> transcodeUs { 0 };
    std::atomic<uint64_t> transcodeMaxUs { 0 };
  };

  inline WinehuaChainCounters& winehuaChainCounters() {
    static WinehuaChainCounters counters;
    return counters;
  }

  inline void winehuaRecordTranscode(uint64_t durationUs, uint64_t bytes) {
    WinehuaChainCounters& counters = winehuaChainCounters();
    counters.transcodeCalls.fetch_add(1, std::memory_order_relaxed);
    counters.transcodeBytes.fetch_add(bytes, std::memory_order_relaxed);
    counters.transcodeUs.fetch_add(durationUs, std::memory_order_relaxed);
    uint64_t previous = counters.transcodeMaxUs.load(std::memory_order_relaxed);
    while (durationUs > previous &&
           !counters.transcodeMaxUs.compare_exchange_weak(previous, durationUs,
             std::memory_order_relaxed)) {
    }
  }

  /* Frame boundary ledger: deltas since the previous present, reported whenever
   * a frame is long enough to matter. */
  inline void winehuaChainTick(uint64_t frameUs) {
    if (!winehuaTelemetryEnabled())
      return;
    constexpr uint64_t kReportThresholdUs = 50000;
    static std::atomic<uint64_t> lastCalls { 0 };
    static std::atomic<uint64_t> lastBytes { 0 };
    static std::atomic<uint64_t> lastUs { 0 };
    static std::atomic<uint32_t> reports { 0 };

    WinehuaChainCounters& counters = winehuaChainCounters();
    const uint64_t calls = counters.transcodeCalls.load(std::memory_order_relaxed);
    const uint64_t bytes = counters.transcodeBytes.load(std::memory_order_relaxed);
    const uint64_t us = counters.transcodeUs.load(std::memory_order_relaxed);
    const uint64_t callDelta = calls - lastCalls.exchange(calls, std::memory_order_relaxed);
    const uint64_t byteDelta = bytes - lastBytes.exchange(bytes, std::memory_order_relaxed);
    const uint64_t usDelta = us - lastUs.exchange(us, std::memory_order_relaxed);

    if (frameUs < kReportThresholdUs || (callDelta == 0 && usDelta == 0))
      return;
    if (reports.fetch_add(1, std::memory_order_relaxed) >= 128)
      return;

    std::string line = str::format("[G9-Chain] frameUs=", frameUs,
      " transcodeCalls=", callDelta,
      " transcodeMiB=", byteDelta / (1024 * 1024),
      " transcodeUs=", usDelta,
      " transcodeMaxUs=", counters.transcodeMaxUs.load(std::memory_order_relaxed));
    Logger::info(line);
    const char* dumpPath = std::getenv("DXVK_SHADER_DUMP_PATH");
    if (!dumpPath || !dumpPath[0])
      return;
    static std::mutex chainMutex;
    std::lock_guard<std::mutex> lock(chainMutex);
    std::ofstream record(
      str::tows(str::format(dumpPath, "/g9-chain.log").c_str()).c_str(),
      std::ios_base::app);
    record << line << std::endl;
  }

  /* State cache pre-compilation queue: how much work was queued and how much of
   * it had finished when the frame was measured.  A large pending count at the
   * moment of a stall means the workers simply had not caught up, which is a
   * different fix from the render thread creating the pipelines itself. */
  inline std::atomic<uint64_t>& winehuaStateCacheQueued() {
    static std::atomic<uint64_t> queued { 0 };
    return queued;
  }

  inline std::atomic<uint64_t>& winehuaStateCacheCompleted() {
    static std::atomic<uint64_t> completed { 0 };
    return completed;
  }

  inline void winehuaRecordPipelineCompile(uint64_t durationUs) {
    WinehuaHitchCounters& counters = winehuaHitchCounters();
    counters.pipelineCompiles.fetch_add(1, std::memory_order_relaxed);
    counters.pipelineCompileUs.fetch_add(durationUs, std::memory_order_relaxed);
  }

  /**
   * \brief Pipeline creation attribution
   *
   * A single frame was observed creating 1043 pipelines while spending only a
   * fraction of the stall inside DXVK's own compilation step, which means the
   * time went into the driver's pipeline creation.  That leaves two very
   * different explanations with opposite fixes: the render thread creating them
   * synchronously on first use, or the state-cache workers pre-compiling them in
   * parallel and contending on a driver that serialises internally.  Recording
   * the origin, the thread and the outside-the-lock wall time separates them.
   */
  enum class WinehuaPipelineOrigin : uint32_t {
    FirstUseSync = 0,
    StateCacheWorker,
    OriginCount,
  };

  inline const char* winehuaPipelineOriginName(WinehuaPipelineOrigin origin) {
    return origin == WinehuaPipelineOrigin::StateCacheWorker
      ? "stateCacheWorker" : "firstUseSync";
  }

  /* Set once on each state-cache worker thread so creations can be attributed. */
  inline bool& winehuaStateCacheWorkerFlag() {
    static thread_local bool isWorker = false;
    return isWorker;
  }

  inline void winehuaRecordPipelineOrigin(WinehuaPipelineOrigin origin,
                                          uint64_t wallUs) {
    if (!winehuaTelemetryEnabled())
      return;
    struct OriginStat {
      std::atomic<uint64_t> count { 0 };
      std::atomic<uint64_t> wallSumUs { 0 };
      std::atomic<uint64_t> wallMaxUs { 0 };
    };
    static OriginStat stats[uint32_t(WinehuaPipelineOrigin::OriginCount)];
    static std::atomic<uint32_t> workersActive { 0 };
    static std::atomic<uint32_t> workersActiveMax { 0 };
    static std::atomic<uint64_t> created { 0 };
    static std::atomic<uint64_t> lastReported { 0 };

    OriginStat& stat = stats[uint32_t(origin)];
    stat.count.fetch_add(1, std::memory_order_relaxed);
    stat.wallSumUs.fetch_add(wallUs, std::memory_order_relaxed);
    uint64_t previousMax = stat.wallMaxUs.load(std::memory_order_relaxed);
    while (wallUs > previousMax &&
           !stat.wallMaxUs.compare_exchange_weak(previousMax, wallUs,
             std::memory_order_relaxed)) {
    }

    if (origin == WinehuaPipelineOrigin::StateCacheWorker) {
      const uint32_t active = workersActive.fetch_add(1, std::memory_order_relaxed) + 1;
      uint32_t peak = workersActiveMax.load(std::memory_order_relaxed);
      while (active > peak &&
             !workersActiveMax.compare_exchange_weak(peak, active,
               std::memory_order_relaxed)) {
      }
      workersActive.fetch_sub(1, std::memory_order_relaxed);
    }

    const uint64_t total = created.fetch_add(1, std::memory_order_relaxed) + 1;
    uint64_t reported = lastReported.load(std::memory_order_relaxed);
    if (total < reported + 256)
      return;
    if (!lastReported.compare_exchange_strong(reported, total,
          std::memory_order_relaxed)) {
      return;
    }

    std::string line = str::format("[G9-Pipe] newPipelines=", total,
      " firstUseSync=", stats[0].count.load(std::memory_order_relaxed),
      " stateCacheWorker=", stats[1].count.load(std::memory_order_relaxed),
      " firstUseWallSumUs=", stats[0].wallSumUs.load(std::memory_order_relaxed),
      " firstUseWallMaxUs=", stats[0].wallMaxUs.load(std::memory_order_relaxed),
      " workerWallSumUs=", stats[1].wallSumUs.load(std::memory_order_relaxed),
      " workerWallMaxUs=", stats[1].wallMaxUs.load(std::memory_order_relaxed),
      " workersActiveMax=", workersActiveMax.load(std::memory_order_relaxed),
      " stateCacheQueued=", winehuaStateCacheQueued().load(std::memory_order_relaxed),
      " stateCacheCompleted=", winehuaStateCacheCompleted().load(std::memory_order_relaxed),
      " stateCachePending=", winehuaStateCacheQueued().load(std::memory_order_relaxed)
        - winehuaStateCacheCompleted().load(std::memory_order_relaxed));
    Logger::info(line);
    const char* dumpPath = std::getenv("DXVK_SHADER_DUMP_PATH");
    if (!dumpPath || !dumpPath[0])
      return;
    static std::mutex pipeMutex;
    std::lock_guard<std::mutex> lock(pipeMutex);
    std::ofstream record(
      str::tows(str::format(dumpPath, "/g9-pipeline-perf.log").c_str()).c_str(),
      std::ios_base::app);
    record << line << std::endl;
  }

  /* Called once per present; logs the events attributed to a suspiciously long
   * frame together with the frame duration. */
  inline void winehuaPresentTick() {
    /* Loading warm-up gate.
     *
     * The state cache queues about a thousand pipeline states when a scene is
     * first entered, and the measured serialised creation rate is roughly eight
     * milliseconds per pipeline with only one worker ever active, so the queue
     * takes tens of seconds to drain.  Frames that arrive while it drains wait
     * behind it.  Holding a present for a short, bounded time while a large
     * batch is in flight moves that wait into the screen that is already
     * showing a spinner instead of into gameplay.
     *
     * Bounded and ablatable: GTAV_OHOS_PIPELINE_WARMUP=0 disables it, the gate
     * only engages while more than kPendingThreshold entries are outstanding,
     * and it never spends more than kBudgetUs in total. */
    {
      static std::atomic<uint64_t> gateBudgetUs { 0 };
      constexpr uint64_t kPendingThreshold = 64;
      constexpr uint64_t kBudgetUs = 10000000;
      constexpr uint64_t kSliceUs = 2000;
      const char* gate = std::getenv("GTAV_OHOS_PIPELINE_WARMUP");
      const bool enabled = !gate || gate[0] != '0';
      uint64_t waitedUs = 0;
      while (enabled &&
             gateBudgetUs.load(std::memory_order_relaxed) < kBudgetUs) {
        const uint64_t queued = winehuaStateCacheQueued().load(std::memory_order_relaxed);
        const uint64_t completed = winehuaStateCacheCompleted().load(std::memory_order_relaxed);
        if (queued <= completed ||
            queued - completed < kPendingThreshold)
          break;
        std::this_thread::sleep_for(std::chrono::microseconds(kSliceUs));
        gateBudgetUs.fetch_add(kSliceUs, std::memory_order_relaxed);
        waitedUs += kSliceUs;
      }
      if (waitedUs) {
        static std::atomic<uint32_t> gateReports { 0 };
        if (gateReports.fetch_add(1, std::memory_order_relaxed) < 8) {
          Logger::info(str::format("[G9-Warmup] gate waitedUs=", waitedUs,
            " pending=", winehuaStateCacheQueued().load(std::memory_order_relaxed)
              - winehuaStateCacheCompleted().load(std::memory_order_relaxed),
            " budgetLeftUs=", kBudgetUs - gateBudgetUs.load(std::memory_order_relaxed)));
        }
      }
    }

    /* The warm-up gate above is behaviour and stays; everything below only
     * produces the hitch and transcode ledger. */
    if (!winehuaTelemetryEnabled())
      return;

    static std::atomic<uint64_t> lastPresentUs { 0 };
    static std::atomic<uint64_t> lastCompiles { 0 };
    static std::atomic<uint64_t> lastCompileUs { 0 };
    static std::atomic<uint64_t> hitchReports { 0 };
    constexpr uint64_t kHitchThresholdUs = 100000;

    static std::mutex timedMutex;
    struct timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const uint64_t nowUs = uint64_t(ts.tv_sec) * 1000000ull
      + uint64_t(ts.tv_nsec) / 1000ull;

    const uint64_t previous = lastPresentUs.exchange(nowUs, std::memory_order_relaxed);
    if (previous != 0)
      winehuaChainTick(nowUs - previous);
    if (previous == 0 || nowUs - previous < kHitchThresholdUs)
      return;
    if (hitchReports.fetch_add(1, std::memory_order_relaxed) >= 64)
      return;

    const WinehuaHitchCounters& counters = winehuaHitchCounters();
    const uint64_t compiles = counters.pipelineCompiles.load(std::memory_order_relaxed);
    const uint64_t compileUs = counters.pipelineCompileUs.load(std::memory_order_relaxed);
    const uint64_t compileDelta = compiles - lastCompiles.exchange(compiles, std::memory_order_relaxed);
    const uint64_t compileUsDelta = compileUs - lastCompileUs.exchange(compileUs, std::memory_order_relaxed);

    std::string line = str::format("[G9-Hitch] frameUs=", nowUs - previous,
      " pipelines=", compileDelta,
      " pipelineCompileUs=", compileUsDelta,
      " submits=", 0);
    Logger::info(line);
    const char* dumpPath = std::getenv("DXVK_SHADER_DUMP_PATH");
    if (!dumpPath || !dumpPath[0])
      return;
    std::lock_guard<std::mutex> lock(timedMutex);
    std::ofstream record(
      str::tows(str::format(dumpPath, "/g9-hitch.log").c_str()).c_str(),
      std::ios_base::app);
    record << line << std::endl;
  }

  inline const char* winehuaSubmitReasonName(WinehuaSubmitReason reason) {
    switch (reason) {
      case WinehuaSubmitReason::FlushCommandList: return "flush";
      default:                                    return "cmdbuf";
    }
  }

  inline void winehuaRecordSubmit(WinehuaSubmitReason reason) {
    if (!winehuaTelemetryEnabled())
      return;
    static std::atomic<uint64_t> counts[uint32_t(WinehuaSubmitReason::ReasonCount)] = {};
    static std::atomic<uint64_t> reported { 0 };
    constexpr uint64_t kReportInterval = 512;

    auto& counter = counts[uint32_t(reason)];
    const uint64_t value = counter.fetch_add(1, std::memory_order_relaxed) + 1;
    uint64_t previous = reported.load(std::memory_order_relaxed);
    if (value < previous + kReportInterval)
      return;
    if (!reported.compare_exchange_strong(previous, value, std::memory_order_relaxed))
      return;

    std::string line = str::format("[G9-Submit] submits=",
      counts[0].load(std::memory_order_relaxed)
        + counts[1].load(std::memory_order_relaxed));
    for (uint32_t i = 0; i < uint32_t(WinehuaSubmitReason::ReasonCount); ++i)
      line += str::format(" ", winehuaSubmitReasonName(WinehuaSubmitReason(i)), "=",
        counts[i].load(std::memory_order_relaxed));

    const WinehuaQueryCounters& queries = winehuaQueryCounters();
    line += str::format(" queryBegin=", queries.begin.load(std::memory_order_relaxed),
      " queryEnd=", queries.end.load(std::memory_order_relaxed),
      " queryGetData=", queries.getData.load(std::memory_order_relaxed),
      " queryPending=", queries.getDataPending.load(std::memory_order_relaxed));

    const WinehuaPresentState& present = winehuaPresentState();
    line += str::format(" swapImages=", present.imageCount.load(std::memory_order_relaxed),
      " presentMode=", present.presentMode.load(std::memory_order_relaxed),
      " configuredLatency=", present.configuredLatency.load(std::memory_order_relaxed),
      " actualLatency=", present.actualLatency.load(std::memory_order_relaxed));

    Logger::info(line);
    const char* dumpPath = std::getenv("DXVK_SHADER_DUMP_PATH");
    if (!dumpPath || !dumpPath[0])
      return;
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    std::ofstream record(
      str::tows(str::format(dumpPath, "/g9-submit.log").c_str()).c_str(),
      std::ios_base::app);
    record << line << std::endl;
  }

}
