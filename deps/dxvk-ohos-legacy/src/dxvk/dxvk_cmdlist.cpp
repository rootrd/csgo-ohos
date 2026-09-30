#include "dxvk_cmdlist.h"
#include "dxvk_device.h"
#include "dxvk_winehua_submit_stats.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <limits>
#if defined(DXVK_NATIVE_OHOS)
#include <unistd.h>
#define _getpid getpid
#else
#include <process.h>
#endif

namespace dxvk {

  static std::atomic<uint64_t> g_winehuaRecordingId = { 0 };
  static std::atomic<uint64_t> g_winehuaMappedFlushLists = { 0 };
  static std::atomic<uint64_t> g_winehuaMappedFlushQueuedRanges = { 0 };
  static std::atomic<uint64_t> g_winehuaMappedFlushEmittedRanges = { 0 };
  static std::atomic<uint64_t> g_winehuaMappedFlushCalls = { 0 };
  static std::atomic<uint64_t> g_winehuaMappedFlushQueuedBytes = { 0 };
  static std::atomic<uint64_t> g_winehuaMappedFlushEmittedBytes = { 0 };
  static std::atomic<uint64_t> g_winehuaMappedFlushWholeRanges = { 0 };
  static std::atomic<uint64_t> g_winehuaMappedFlushFailures = { 0 };

#if defined(DXVK_NATIVE_OHOS)
  struct G9SubmitPhaseWindow {
    uint64_t reportStartUs = 0;
    uint64_t mappedFlushUs = 0;
    uint64_t mappedFlushMaxUs = 0;
    uint64_t queueSubmitUs = 0;
    uint64_t queueSubmitMaxUs = 0;
    uint64_t submissions = 0;
  };

  static thread_local G9SubmitPhaseWindow g_g9SubmitPhase;
  static thread_local uint64_t g_g9CurrentQueueSubmitUs = 0;

  static uint64_t g9SteadyClockUs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
  }

  static void g9RecordSubmitPhase(uint64_t mappedFlushUs, uint64_t queueSubmitUs) {
    auto& phase = g_g9SubmitPhase;
    const uint64_t now = g9SteadyClockUs();
    if (!phase.reportStartUs)
      phase.reportStartUs = now;
    phase.mappedFlushUs += mappedFlushUs;
    phase.mappedFlushMaxUs = std::max(phase.mappedFlushMaxUs, mappedFlushUs);
    phase.queueSubmitUs += queueSubmitUs;
    phase.queueSubmitMaxUs = std::max(phase.queueSubmitMaxUs, queueSubmitUs);
    phase.submissions += 1;

    if (now - phase.reportStartUs >= 5000000u) {
      Logger::info(str::format(
        "G9_DXVK_SUBMIT_PHASE submissions=", phase.submissions,
        " avgMappedFlushUs=", phase.submissions ? phase.mappedFlushUs / phase.submissions : 0,
        " maxMappedFlushUs=", phase.mappedFlushMaxUs,
        " avgVkQueueSubmitUs=", phase.submissions ? phase.queueSubmitUs / phase.submissions : 0,
        " maxVkQueueSubmitUs=", phase.queueSubmitMaxUs));
      phase = G9SubmitPhaseWindow();
    }
  }
#endif
    
  DxvkCommandList::DxvkCommandList(DxvkDevice* device)
  : m_device        (device),
    m_vkd           (device->vkd()),
    m_vki           (device->instance()->vki()),
    m_cmdBuffersUsed(0),
    m_descriptorPoolTracker(device) {
    const auto& graphicsQueue = m_device->queues().graphics;
    const auto& transferQueue = m_device->queues().transfer;

    VkFenceCreateInfo fenceInfo;
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.pNext = nullptr;
    fenceInfo.flags = 0;
    
    if (m_vkd->vkCreateFence(m_vkd->device(), &fenceInfo, nullptr, &m_fence) != VK_SUCCESS)
      throw DxvkError("DxvkCommandList: Failed to create fence");
    
    VkCommandPoolCreateInfo poolInfo;
    poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.pNext            = nullptr;
    poolInfo.flags            = 0;
    poolInfo.queueFamilyIndex = graphicsQueue.queueFamily;
    
    if (m_vkd->vkCreateCommandPool(m_vkd->device(), &poolInfo, nullptr, &m_graphicsPool) != VK_SUCCESS)
      throw DxvkError("DxvkCommandList: Failed to create graphics command pool");
    
    if (m_device->hasDedicatedTransferQueue()) {
      poolInfo.queueFamilyIndex = transferQueue.queueFamily;

      if (m_vkd->vkCreateCommandPool(m_vkd->device(), &poolInfo, nullptr, &m_transferPool) != VK_SUCCESS)
        throw DxvkError("DxvkCommandList: Failed to create transfer command pool");
    }
    
    VkCommandBufferAllocateInfo cmdInfoGfx;
    cmdInfoGfx.sType             = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdInfoGfx.pNext             = nullptr;
    cmdInfoGfx.commandPool       = m_graphicsPool;
    cmdInfoGfx.level             = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdInfoGfx.commandBufferCount = 1;
    
    VkCommandBufferAllocateInfo cmdInfoDma;
    cmdInfoDma.sType             = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdInfoDma.pNext             = nullptr;
    cmdInfoDma.commandPool       = m_transferPool ? m_transferPool : m_graphicsPool;
    cmdInfoDma.level             = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdInfoDma.commandBufferCount = 1;
    
    if (m_vkd->vkAllocateCommandBuffers(m_vkd->device(), &cmdInfoGfx, &m_execBuffer) != VK_SUCCESS
     || m_vkd->vkAllocateCommandBuffers(m_vkd->device(), &cmdInfoGfx, &m_initBuffer) != VK_SUCCESS
     || m_vkd->vkAllocateCommandBuffers(m_vkd->device(), &cmdInfoDma, &m_sdmaBuffer) != VK_SUCCESS)
      throw DxvkError("DxvkCommandList: Failed to allocate command buffer");
    
    if (m_device->hasDedicatedTransferQueue()) {
      VkSemaphoreCreateInfo semInfo;
      semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
      semInfo.pNext = nullptr;
      semInfo.flags = 0;

      if (m_vkd->vkCreateSemaphore(m_vkd->device(), &semInfo, nullptr, &m_sdmaSemaphore) != VK_SUCCESS)
        throw DxvkError("DxvkCommandList: Failed to create semaphore");
    }
  }
  
  
  DxvkCommandList::~DxvkCommandList() {
    this->reset();

    m_vkd->vkDestroySemaphore(m_vkd->device(), m_sdmaSemaphore, nullptr);
    
    m_vkd->vkDestroyCommandPool(m_vkd->device(), m_graphicsPool, nullptr);
    m_vkd->vkDestroyCommandPool(m_vkd->device(), m_transferPool, nullptr);
    
    m_vkd->vkDestroyFence(m_vkd->device(), m_fence, nullptr);
  }
  
  
  VkResult DxvkCommandList::submit(
          VkSemaphore     waitSemaphore,
          VkSemaphore     wakeSemaphore) {
    const auto& graphics = m_device->queues().graphics;
    const auto& transfer = m_device->queues().transfer;

#if defined(DXVK_NATIVE_OHOS)
    g_g9CurrentQueueSubmitUs = 0;
    const uint64_t mappedFlushStartUs = g9SteadyClockUs();
#endif
    const VkResult mappedFlushResult = flushWineHuaMappedFlushes();
    if (mappedFlushResult != VK_SUCCESS) {
#if defined(DXVK_NATIVE_OHOS)
      g9RecordSubmitPhase(g9SteadyClockUs() - mappedFlushStartUs, 0);
#endif
      return mappedFlushResult;
    }
#if defined(DXVK_NATIVE_OHOS)
    const uint64_t mappedFlushUs = g9SteadyClockUs() - mappedFlushStartUs;
#endif

    m_submission.reset();

    if (m_cmdBuffersUsed.test(DxvkCmdBuffer::SdmaBuffer)) {
      m_submission.cmdBuffers.push_back(m_sdmaBuffer);

      if (m_device->hasDedicatedTransferQueue()) {
        m_submission.addWakeSemaphore(m_sdmaSemaphore, 0);
        VkResult status = submitToQueue(transfer.queueHandle, VK_NULL_HANDLE, m_submission);

        if (status != VK_SUCCESS) {
#if defined(DXVK_NATIVE_OHOS)
          g9RecordSubmitPhase(mappedFlushUs, g_g9CurrentQueueSubmitUs);
#endif
          return status;
        }

        m_submission.reset();
        m_submission.addWaitSemaphore(m_sdmaSemaphore, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
      }
    }

    if (m_cmdBuffersUsed.test(DxvkCmdBuffer::InitBuffer))
      m_submission.cmdBuffers.push_back(m_initBuffer);
    if (m_cmdBuffersUsed.test(DxvkCmdBuffer::ExecBuffer))
      m_submission.cmdBuffers.push_back(m_execBuffer);

    if (winehuaCameraTraceEnabled() && !m_winehuaFrames.empty()) {
      std::string frames = "[";
      for (size_t i = 0; i < m_winehuaFrames.size(); i++) {
        if (i)
          frames += ',';
        frames += str::format(m_winehuaFrames[i]);
      }
      frames += ']';

      Logger::info(str::format(
        "WineHuaDxvkSubmit: winPid=", _getpid(),
        " recording=", m_winehuaRecordingId,
        " execCmd=0x", std::hex, reinterpret_cast<uintptr_t>(m_execBuffer),
        " frameCount=", std::dec, m_winehuaFrames.size(),
        " frames=", frames));
    }

    if (winehuaPresentImageTraceEnabled() && m_winehuaPresentCopyValid) {
      Logger::info(str::format(
        "WineHuaPresentCopy: layer=dxvk event=submit",
        " frame=", m_winehuaPresentCopyFrame,
        " recording=", m_winehuaRecordingId,
        " execCmd=0x", std::hex,
          reinterpret_cast<uintptr_t>(m_execBuffer),
        " sourceImage=0x", m_winehuaPresentCopySourceImage,
        " destinationImage=0x", m_winehuaPresentCopyDestinationImage,
        " destinationIndex=", std::dec,
          m_winehuaPresentCopyDestinationIndex,
        " sourceSamples=", uint32_t(m_winehuaPresentCopySourceSamples)));
    }
    
    if (waitSemaphore)
      m_submission.addWaitSemaphore(waitSemaphore, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

    if (wakeSemaphore)
      m_submission.addWakeSemaphore(wakeSemaphore, 0);
    
    for (const auto& entry : m_waitSemaphores) {
      m_submission.addWaitSemaphore(entry.fence->handle(), entry.value, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    }

    for (const auto& entry : m_signalSemaphores) {
      m_submission.addWakeSemaphore(entry.fence->handle(), entry.value);
    }

    const VkResult result = submitToQueue(graphics.queueHandle, m_fence, m_submission);
#if defined(DXVK_NATIVE_OHOS)
    g9RecordSubmitPhase(mappedFlushUs, g_g9CurrentQueueSubmitUs);
#endif
    return result;
  }


  VkResult DxvkCommandList::queueWineHuaMappedFlush(
          Rc<DxvkResource>          resource,
    const VkMappedMemoryRange&      range) {
    if (!range.size)
      return VK_SUCCESS;

    const auto rangeEnd = [] (const VkMappedMemoryRange& value) {
      if (value.size == VK_WHOLE_SIZE
       || value.size > std::numeric_limits<VkDeviceSize>::max() - value.offset)
        return std::numeric_limits<VkDeviceSize>::max();
      return value.offset + value.size;
    };

    if (!m_winehuaMappedFlushes.empty()) {
      WineHuaMappedFlush& previous = m_winehuaMappedFlushes.back();
      if (previous.resource.ptr() == resource.ptr()
       && previous.range.memory == range.memory) {
        const VkDeviceSize previousEnd = rangeEnd(previous.range);
        const VkDeviceSize nextEnd = rangeEnd(range);
        if (range.offset <= previousEnd && previous.range.offset <= nextEnd) {
          const VkDeviceSize mergedBegin = std::min(previous.range.offset, range.offset);
          const VkDeviceSize mergedEnd = std::max(previousEnd, nextEnd);
          previous.range.offset = mergedBegin;
          previous.range.size = mergedEnd == std::numeric_limits<VkDeviceSize>::max()
            ? VK_WHOLE_SIZE
            : mergedEnd - mergedBegin;
          return VK_SUCCESS;
        }
      }
    }

    WineHuaMappedFlush pending;
    pending.resource = std::move(resource);
    pending.range = range;
    pending.range.pNext = nullptr;
    m_winehuaMappedFlushes.push_back(std::move(pending));
    return VK_SUCCESS;
  }


  VkResult DxvkCommandList::flushWineHuaMappedFlushes() {
    if (m_winehuaMappedFlushes.empty())
      return VK_SUCCESS;

    const bool collectStats = winehuaBatchMappedFlushStats();
    const uint64_t queuedRangeCount = m_winehuaMappedFlushes.size();
    uint64_t emittedRangeCount = 0;
    uint64_t flushCallCount = 0;
    uint64_t queuedBytes = 0;
    uint64_t emittedBytes = 0;
    uint64_t wholeRangeCount = 0;

    const auto memoryLess = [] (VkDeviceMemory a, VkDeviceMemory b) {
      return std::less<VkDeviceMemory>()(a, b);
    };

    std::sort(m_winehuaMappedFlushes.begin(), m_winehuaMappedFlushes.end(),
      [&memoryLess] (const WineHuaMappedFlush& a,
                     const WineHuaMappedFlush& b) {
        if (memoryLess(a.range.memory, b.range.memory))
          return true;
        if (memoryLess(b.range.memory, a.range.memory))
          return false;
        return a.range.offset < b.range.offset;
      });

    const auto rangeEnd = [] (const VkMappedMemoryRange& range) {
      if (range.size == VK_WHOLE_SIZE
       || range.size > std::numeric_limits<VkDeviceSize>::max() - range.offset)
        return std::numeric_limits<VkDeviceSize>::max();
      return range.offset + range.size;
    };

    constexpr uint32_t MaxRangesPerCall = 256;
    std::array<VkMappedMemoryRange, MaxRangesPerCall> ranges;
    uint32_t rangeCount = 0;

    const auto flushRanges = [&] () {
      if (!rangeCount)
        return VK_SUCCESS;

      if (collectStats) {
        emittedRangeCount += rangeCount;
        flushCallCount += 1;
        for (uint32_t i = 0; i < rangeCount; i++) {
          if (ranges[i].size == VK_WHOLE_SIZE)
            wholeRangeCount += 1;
          else
            emittedBytes += ranges[i].size;
        }
      }

      const VkResult result = m_vkd->vkFlushMappedMemoryRanges(
        m_vkd->device(), rangeCount, ranges.data());
      rangeCount = 0;
      return result;
    };

    for (const auto& entry : m_winehuaMappedFlushes) {
      const auto& range = entry.range;
      if (!range.size)
        continue;

      if (collectStats && range.size != VK_WHOLE_SIZE)
        queuedBytes += range.size;

      if (rangeCount) {
        auto& previous = ranges[rangeCount - 1];
        const bool sameMemory = !memoryLess(previous.memory, range.memory)
                             && !memoryLess(range.memory, previous.memory);
        const VkDeviceSize previousEnd = rangeEnd(previous);
        if (sameMemory && range.offset <= previousEnd) {
          const VkDeviceSize mergedEnd = std::max(previousEnd, rangeEnd(range));
          previous.size = mergedEnd == std::numeric_limits<VkDeviceSize>::max()
            ? VK_WHOLE_SIZE
            : mergedEnd - previous.offset;
          continue;
        }
      }

      if (rangeCount == MaxRangesPerCall) {
        const VkResult result = flushRanges();
        if (result != VK_SUCCESS) {
          if (collectStats)
            g_winehuaMappedFlushFailures.fetch_add(1);
          return result;
        }
      }

      ranges[rangeCount++] = range;
    }

    const VkResult result = flushRanges();
    if (collectStats) {
      if (result != VK_SUCCESS)
        g_winehuaMappedFlushFailures.fetch_add(1);

      const uint64_t lists =
        g_winehuaMappedFlushLists.fetch_add(1) + 1;
      const uint64_t queuedRanges =
        g_winehuaMappedFlushQueuedRanges.fetch_add(queuedRangeCount) +
        queuedRangeCount;
      const uint64_t emittedRanges =
        g_winehuaMappedFlushEmittedRanges.fetch_add(emittedRangeCount) +
        emittedRangeCount;
      const uint64_t calls =
        g_winehuaMappedFlushCalls.fetch_add(flushCallCount) + flushCallCount;
      const uint64_t totalQueuedBytes =
        g_winehuaMappedFlushQueuedBytes.fetch_add(queuedBytes) + queuedBytes;
      const uint64_t totalEmittedBytes =
        g_winehuaMappedFlushEmittedBytes.fetch_add(emittedBytes) + emittedBytes;
      const uint64_t wholeRanges =
        g_winehuaMappedFlushWholeRanges.fetch_add(wholeRangeCount) +
        wholeRangeCount;

      if (lists <= 8 || !(lists % 120)) {
        Logger::info(str::format(
          "WineHuaMappedFlushPerf: lists=", lists,
          " queued_ranges=", queuedRanges,
          " emitted_ranges=", emittedRanges,
          " calls=", calls,
          " queued_bytes=", totalQueuedBytes,
          " emitted_bytes=", totalEmittedBytes,
          " whole_ranges=", wholeRanges,
          " failures=", g_winehuaMappedFlushFailures.load()));
      }
    }

    return result;
  }
  
  
  VkResult DxvkCommandList::synchronize() {
    VkResult status = VK_TIMEOUT;
    
    while (status == VK_TIMEOUT) {
      status = m_vkd->vkWaitForFences(
        m_vkd->device(), 1, &m_fence, VK_FALSE,
        1'000'000'000ull);
    }
    
    return status;
  }
  
  
  void DxvkCommandList::beginRecording() {
    VkCommandBufferBeginInfo info;
    info.sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    info.pNext            = nullptr;
    info.flags            = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    info.pInheritanceInfo = nullptr;
    
    if ((m_graphicsPool && m_vkd->vkResetCommandPool(m_vkd->device(), m_graphicsPool, 0) != VK_SUCCESS)
     || (m_transferPool && m_vkd->vkResetCommandPool(m_vkd->device(), m_transferPool, 0) != VK_SUCCESS))
      Logger::err("DxvkCommandList: Failed to reset command buffer");
    
    if (m_vkd->vkBeginCommandBuffer(m_execBuffer, &info) != VK_SUCCESS
     || m_vkd->vkBeginCommandBuffer(m_initBuffer, &info) != VK_SUCCESS
     || m_vkd->vkBeginCommandBuffer(m_sdmaBuffer, &info) != VK_SUCCESS)
      Logger::err("DxvkCommandList: Failed to begin command buffer");
    
    if (m_vkd->vkResetFences(m_vkd->device(), 1, &m_fence) != VK_SUCCESS)
      Logger::err("DxvkCommandList: Failed to reset fence");
    
    // Unconditionally mark the exec buffer as used. There
    // is virtually no use case where this isn't correct.
    m_cmdBuffersUsed = DxvkCmdBuffer::ExecBuffer;

    m_winehuaFrames.clear();
    m_winehuaPresentCopyValid = false;
    m_winehuaRecordingId = (winehuaCameraTraceEnabled()
                          || winehuaPresentImageTraceEnabled())
      ? g_winehuaRecordingId.fetch_add(1, std::memory_order_relaxed) + 1
      : 0;
  }
  
  
  void DxvkCommandList::endRecording() {
    if (m_vkd->vkEndCommandBuffer(m_execBuffer) != VK_SUCCESS
     || m_vkd->vkEndCommandBuffer(m_initBuffer) != VK_SUCCESS
     || m_vkd->vkEndCommandBuffer(m_sdmaBuffer) != VK_SUCCESS)
      Logger::err("DxvkCommandList::endRecording: Failed to record command buffer");
  }
  
  
  void DxvkCommandList::reset() {
    // Free resources and other objects
    // that are no longer in use
    m_resources.reset();

    // Recycle heavy Vulkan objects
    m_descriptorPoolTracker.reset();

    // Return buffer memory slices
    m_bufferTracker.reset();

    // Return query and event handles
    m_gpuQueryTracker.reset();
    m_gpuEventTracker.reset();

    // Less important stuff
    m_signalTracker.reset();
    m_statCounters.reset();

    m_winehuaMappedFlushes.clear();

    m_waitSemaphores.clear();
    m_signalSemaphores.clear();
  }


  VkResult DxvkCommandList::submitToQueue(
          VkQueue               queue,
          VkFence               fence,
    const DxvkQueueSubmission&  info) {
    VkTimelineSemaphoreSubmitInfoKHR timelineInfo = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO_KHR };
    timelineInfo.waitSemaphoreValueCount    = info.waitValues.size();
    timelineInfo.pWaitSemaphoreValues       = info.waitValues.data();
    timelineInfo.signalSemaphoreValueCount  = info.wakeValues.size();
    timelineInfo.pSignalSemaphoreValues     = info.wakeValues.data();

    VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submitInfo.waitSemaphoreCount   = info.waitSync.size();
    submitInfo.pWaitSemaphores      = info.waitSync.data();
    submitInfo.pWaitDstStageMask    = info.waitMask.data();
    submitInfo.commandBufferCount   = info.cmdBuffers.size();
    submitInfo.pCommandBuffers      = info.cmdBuffers.data();
    submitInfo.signalSemaphoreCount = info.wakeSync.size();
    submitInfo.pSignalSemaphores    = info.wakeSync.data();

    if (m_device->features().khrTimelineSemaphore.timelineSemaphore)
      submitInfo.pNext = &timelineInfo;
    
#if defined(DXVK_NATIVE_OHOS)
    const uint64_t queueSubmitStartUs = g9SteadyClockUs();
#endif
    const VkResult result = m_vkd->vkQueueSubmit(queue, 1, &submitInfo, fence);
#if defined(DXVK_NATIVE_OHOS)
    g_g9CurrentQueueSubmitUs += g9SteadyClockUs() - queueSubmitStartUs;
    winehuaRecordSubmit(WinehuaSubmitReason::CommandList);
#endif
    return result;
  }
  
  void DxvkCommandList::cmdBeginDebugUtilsLabel(VkDebugUtilsLabelEXT *pLabelInfo) {
    m_vki->vkCmdBeginDebugUtilsLabelEXT(m_execBuffer, pLabelInfo);
  }

  void DxvkCommandList::cmdEndDebugUtilsLabel() {
    m_vki->vkCmdEndDebugUtilsLabelEXT(m_execBuffer);
  }

  void DxvkCommandList::cmdInsertDebugUtilsLabel(VkDebugUtilsLabelEXT *pLabelInfo) {
    m_vki->vkCmdInsertDebugUtilsLabelEXT(m_execBuffer, pLabelInfo);
  }
}
