#include <algorithm>
#include <cstring>
#include <fstream>
#if defined(DXVK_NATIVE_OHOS)
#include <unistd.h>
#define _getpid getpid
#else
#include <process.h>
#endif
#include <string>
#include <vector>
#include <utility>

#include "dxvk_device.h"
#include "dxvk_context.h"
#include "dxvk_winehua_trace.h"
#include "dxvk_winehua_submit_stats.h"
#include "../dxbc/dxbc_util.h"
#include "../util/util_string.h"

namespace dxvk {

  static bool winehuaTraceDepthFormat(VkFormat format) {
    return format == VK_FORMAT_D16_UNORM
        || format == VK_FORMAT_X8_D24_UNORM_PACK32
        || format == VK_FORMAT_D32_SFLOAT
        || format == VK_FORMAT_D16_UNORM_S8_UINT
        || format == VK_FORMAT_D24_UNORM_S8_UINT
        || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
  }

  static bool winehuaTraceDepthView(const Rc<DxvkImageView>& view) {
    return view != nullptr && winehuaTraceDepthFormat(view->imageInfo().format);
  }
  
  DxvkContext::DxvkContext(const Rc<DxvkDevice>& device)
  : m_device      (device),
    m_common      (&device->m_objects),
    m_sdmaAcquires(DxvkCmdBuffer::SdmaBuffer),
    m_sdmaBarriers(DxvkCmdBuffer::SdmaBuffer),
    m_initBarriers(DxvkCmdBuffer::InitBuffer),
    m_execAcquires(DxvkCmdBuffer::ExecBuffer),
    m_execBarriers(DxvkCmdBuffer::ExecBuffer),
    m_gfxBarriers (DxvkCmdBuffer::ExecBuffer),
    m_queryManager(m_common->queryPool()),
    m_staging     (device, StagingBufferSize) {
    if (m_device->features().extRobustness2.nullDescriptor)
      m_features.set(DxvkContextFeature::NullDescriptors);
    if (m_device->features().extExtendedDynamicState.extendedDynamicState)
      m_features.set(DxvkContextFeature::ExtendedDynamicState);

    // Init framebuffer info with default render pass in case
    // the app does not explicitly bind any render targets
    m_state.om.framebufferInfo = makeFramebufferInfo(m_state.om.renderTargets);
  }
  
  
  DxvkContext::~DxvkContext() {
    
  }
  
  
  void DxvkContext::beginRecording(const Rc<DxvkCommandList>& cmdList) {
    m_cmd = cmdList;
    m_cmd->beginRecording();

    // Mark all resources as untracked
    m_vbTracked.clear();
    m_rcTracked.clear();
    
    // The current state of the internal command buffer is
    // undefined, so we have to bind and set up everything
    // before any draw or dispatch command is recorded.
    m_flags.clr(
      DxvkContextFlag::GpRenderPassBound,
      DxvkContextFlag::GpXfbActive);
    
    m_flags.set(
      DxvkContextFlag::GpDirtyFramebuffer,
      DxvkContextFlag::GpDirtyPipeline,
      DxvkContextFlag::GpDirtyPipelineState,
      DxvkContextFlag::GpDirtyResources,
      DxvkContextFlag::GpDirtyVertexBuffers,
      DxvkContextFlag::GpDirtyIndexBuffer,
      DxvkContextFlag::GpDirtyXfbBuffers,
      DxvkContextFlag::GpDirtyBlendConstants,
      DxvkContextFlag::GpDirtyStencilRef,
      DxvkContextFlag::GpDirtyViewport,
      DxvkContextFlag::GpDirtyDepthBias,
      DxvkContextFlag::GpDirtyDepthBounds,
      DxvkContextFlag::CpDirtyPipeline,
      DxvkContextFlag::CpDirtyPipelineState,
      DxvkContextFlag::CpDirtyResources,
      DxvkContextFlag::DirtyDrawBuffer);
  }
  
  
  Rc<DxvkCommandList> DxvkContext::endRecording() {
    this->spillRenderPass(true);
    this->flushSharedImages();

    m_sdmaBarriers.recordCommands(m_cmd);
    m_initBarriers.recordCommands(m_cmd);
    m_execBarriers.recordCommands(m_cmd);

    m_cmd->endRecording();
    return std::exchange(m_cmd, nullptr);
  }


  void DxvkContext::flushCommandList() {
    winehuaRecordSubmit(WinehuaSubmitReason::FlushCommandList);
    m_device->submitCommandList(
      this->endRecording(),
      VK_NULL_HANDLE,
      VK_NULL_HANDLE);
    
    this->beginRecording(
      m_device->createCommandList());
  }


  VkResult DxvkContext::flushMappedBuffer(
    const Rc<DxvkBuffer>&          buffer,
    const DxvkBufferSliceHandle&   slice) {
    return buffer->flushMappedSlice(slice, m_cmd.operator->());
  }


  VkResult DxvkContext::flushMappedImage(
    const Rc<DxvkImage>&           image,
          VkDeviceSize             offset,
          VkDeviceSize             length) {
    return image->flushMappedRange(offset, length, m_cmd.operator->());
  }


  void DxvkContext::winehuaTracePresentCopy(
          uint64_t              frameId,
          uint32_t              destinationIndex,
          VkImage               sourceImage,
          VkImage               destinationImage,
          VkSampleCountFlagBits sourceSamples) {
    if (winehuaPresentImageTraceEnabled() && m_cmd != nullptr) {
      m_cmd->winehuaTracePresentCopy(
        frameId, destinationIndex, sourceImage,
        destinationImage, sourceSamples);
    }
  }


  void DxvkContext::winehuaFrameBoundary(uint64_t nextFrameId) {
#if defined(DXVK_NATIVE_OHOS)
    if (winehuaSceneCaptureEnabled()
     && winehuaSceneCaptureRequested().exchange(false, std::memory_order_acquire)
     && winehuaSceneCaptureFrame().load(std::memory_order_relaxed) == UINT64_MAX) {
      // Allow an already queued present to retire before sampling a complete
      // world frame. The request originates after the game's first scene draw.
      winehuaSceneCaptureFrame().store(nextFrameId + 1, std::memory_order_relaxed);
      Logger::info(str::format("WineHuaSceneCapture: selected frame=", nextFrameId + 1));
    }
#endif
    if (winehuaRenderTargetDumpEnabled()) {
      if (m_winehuaFrameId == winehuaRenderTargetDumpFrame())
        this->winehuaWriteRenderTargetDumps();
      else
        m_winehuaRenderTargetDumps.clear();

      if (m_winehuaFrameId == winehuaRenderTargetDumpFrame())
        this->winehuaWriteGeometryDumps();
      else {
        m_winehuaGeometryDumps.clear();
        m_winehuaGeometryStateJson.clear();
      }

      if (nextFrameId == winehuaRenderTargetDumpFrame()) {
        Logger::info(str::format(
          "WineHuaRenderTargetDump: armed frame=", nextFrameId,
          " maxAttachments=", winehuaRenderTargetDumpMaxAttachments(),
          " maxBytes=", winehuaRenderTargetDumpMaxBytes(),
          " sampled=", winehuaRenderTargetDumpSampledEnabled() ? 1 : 0,
          " path=", winehuaRenderTargetDumpPath()));
      }
    }

    if (winehuaDrawTraceEnabled()
     && nextFrameId == winehuaRenderTargetDumpFrame()) {
      Logger::info(str::format(
        "WineHuaDraw: armed frame=", nextFrameId,
        " pass=", winehuaDrawTracePass(),
        " secondPass=", winehuaDrawTraceSecondPass(),
        " maxDraws=", winehuaDrawTraceMaxDraws(),
        " draw=", winehuaRenderTargetDumpDraw(),
        " fs=", winehuaRenderTargetDumpFragmentShader(),
        " indexCount=", winehuaRenderTargetDumpIndexCount(),
        " firstIndex=", winehuaRenderTargetDumpFirstIndex(),
        " vertexOffset=", winehuaRenderTargetDumpVertexOffset()));
    }

    m_winehuaFrameId = nextFrameId;
    m_winehuaPassId = 0;
    m_winehuaActivePassId = 0;
    m_winehuaFrameDrawId = 0;
    m_winehuaPassDrawId = 0;
    m_winehuaLastPassDrawId = UINT32_MAX;
    m_winehuaTraceDrawsEmitted = 0;
    m_winehuaTargetDrawCaptured = false;
    m_winehuaSecondTargetDrawCaptured = false;
    m_winehuaDumpBytes = 0;
    m_winehuaGeometryBytes = 0;
    m_winehuaGeometryStateJson.clear();
    m_winehuaLastGraphicsResourceViews = "[]";
    m_winehuaLastGraphicsImages.clear();
  }


  void DxvkContext::winehuaTraceDraw(
    const char*         drawType,
    const std::string&  arguments) {
    if (winehuaCameraTraceEnabled()
     && m_winehuaCameraTraceFrame != m_winehuaFrameId
     && m_winehuaActivePassId == 0) {
      bool emitted = false;
      for (const auto& binding : m_winehuaGraphicsBindings) {
        if (binding.descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
         && binding.descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
          continue;

        if (!binding.bufferSlice.defined())
          continue;

        const DxvkBufferSliceHandle slice = binding.bufferSlice.getSliceHandle();
        if (!slice.mapPtr || !slice.length)
          continue;

        const uint64_t byteCount = std::min<uint64_t>(slice.length, 4096u);
        const auto* bytes = reinterpret_cast<const uint8_t*>(slice.mapPtr);
        uint64_t contentHash = 1469598103934665603ull;
        for (uint64_t i = 0; i < byteCount; i++) {
          contentHash ^= bytes[i];
          contentHash *= 1099511628211ull;
        }

        std::string words = "[";
        const uint32_t wordCount = uint32_t(
          std::min<uint64_t>(byteCount, 64u) / sizeof(uint32_t));
        for (uint32_t i = 0; i < wordCount; i++) {
          uint32_t word = 0;
          std::memcpy(&word, bytes + i * sizeof(uint32_t), sizeof(word));
          if (i)
            words += ',';
          words += str::format("0x", std::hex, word);
        }
        words += ']';

        Logger::info(str::format(
          "WineHuaUbo: winPid=", _getpid(),
          " recording=", m_cmd->winehuaRecordingId(),
          " frame=", m_winehuaFrameId,
          " pass=", m_winehuaActivePassId,
          " guestCmd=0x", std::hex,
            reinterpret_cast<uintptr_t>(m_cmd->winehuaExecBuffer()),
          " descriptorSet=0x", m_gpSet,
          " binding=", binding.binding,
          " resourceSlot=", binding.resourceSlot,
          " descriptorType=", binding.descriptorType,
          " stages=0x", std::hex, binding.stages,
          " writtenHandle=0x", binding.buffer.buffer,
          " writtenBaseOffset=", std::dec, binding.buffer.offset,
          " writtenRange=", binding.buffer.range,
          " dynamicOffsetBound=", binding.dynamicOffsetBound ? 1 : 0,
          " dynamicOffset=", binding.dynamicOffset,
          " sliceHandle=0x", std::hex, slice.handle,
          " sliceOffset=", std::dec, slice.offset,
          " sliceLength=", slice.length,
          " bytes=", byteCount,
          " hash=0x", std::hex, contentHash,
          " words=", words));
        emitted = true;
      }

      if (emitted) {
        m_cmd->winehuaTraceFrame(m_winehuaFrameId);
        m_winehuaCameraTraceFrame = m_winehuaFrameId;
      }
    }

    if (!winehuaDrawTraceEnabled()
     || m_winehuaFrameId != winehuaRenderTargetDumpFrame())
      return;

    const uint32_t frameDrawId = m_winehuaFrameDrawId++;
    const uint32_t passDrawId = m_winehuaPassDrawId++;
    m_winehuaLastPassDrawId = passDrawId;
    const uint32_t selectedPass = winehuaDrawTracePass();

    if ((selectedPass != UINT32_MAX && m_winehuaActivePassId != selectedPass)
     || m_winehuaTraceDrawsEmitted >= winehuaDrawTraceMaxDraws())
      return;

    m_winehuaTraceDrawsEmitted++;

    const std::string vertexShader = m_state.gp.shaders.vs != nullptr
      ? m_state.gp.shaders.vs->debugName() : std::string();
    const std::string fragmentShader = m_state.gp.shaders.fs != nullptr
      ? m_state.gp.shaders.fs->debugName() : std::string();

    std::string pushWords = "[]";
    uint32_t pushOffset = 0;
    uint32_t pushSize = 0;
    if (m_state.gp.pipeline != nullptr && m_state.gp.pipeline->layout() != nullptr) {
      const VkPushConstantRange range = m_state.gp.pipeline->layout()->pushConstRange();
      pushOffset = range.offset;
      pushSize = range.size;
      if (range.size) {
        pushWords = "[";
        const uint32_t wordCount = std::min(range.size, 64u) / sizeof(uint32_t);
        for (uint32_t i = 0; i < wordCount; i++) {
          uint32_t word = 0;
          std::memcpy(&word,
            &m_state.pc.data[range.offset + i * sizeof(uint32_t)],
            sizeof(word));
          if (i)
            pushWords += ',';
          pushWords += str::format("0x", std::hex, word);
        }
        pushWords += ']';
      }
    }

    Logger::info(str::format(
      "WineHuaDraw: frame=", m_winehuaFrameId,
      " pass=", m_winehuaActivePassId,
      " draw=", passDrawId,
      " frameDraw=", frameDrawId,
      " type=", drawType,
      " ", arguments,
      " pipeline=0x", std::hex, m_gpActivePipeline,
      " descriptorSet=0x", m_gpSet,
      " descriptorUpdate=", std::dec, m_winehuaDescriptorUpdateSerial,
      " descriptorBind=", m_winehuaDescriptorBindSerial,
      " vs=", vertexShader,
      " fs=", fragmentShader,
      " viewport=", m_state.vp.viewports[0].x, ",",
                       m_state.vp.viewports[0].y, ",",
                       m_state.vp.viewports[0].width, ",",
                       m_state.vp.viewports[0].height,
      " scissor=", m_state.vp.scissorRects[0].offset.x, ",",
                      m_state.vp.scissorRects[0].offset.y, ",",
                      m_state.vp.scissorRects[0].extent.width, ",",
                      m_state.vp.scissorRects[0].extent.height,
      " pushOffset=", pushOffset,
      " pushSize=", pushSize,
      " pushWords=", pushWords));

    for (const auto& binding : m_winehuaGraphicsBindings) {
      const auto& current = m_rc[binding.resourceSlot];

      if (binding.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER
       || binding.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
       || binding.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
       || binding.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
        Rc<DxvkSampler> currentSampler = current.sampler;
        uint32_t currentSamplerSlot = binding.resourceSlot;

        if (binding.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
          const uint32_t stageSlot = binding.resourceSlot % DxbcStageBindingCount;
          if (currentSampler == nullptr
           && stageSlot >= DxbcResourceBindingIndex
           && stageSlot < DxbcResourceBindingIndex + DxbcResourceBindingCount) {
            currentSamplerSlot = binding.resourceSlot
              - (DxbcResourceBindingIndex - DxbcSamplerBindingIndex);
            currentSampler = m_rc[currentSamplerSlot].sampler;
          }
        }

        const Rc<DxvkImageView>& currentView = current.imageView;
        const VkImageView currentViewHandle = currentView != nullptr
          ? currentView->handle(binding.viewType) : VK_NULL_HANDLE;
        const VkSampler currentSamplerHandle = currentSampler != nullptr
          ? currentSampler->handle() : VK_NULL_HANDLE;
        const uint64_t writtenViewCookie = binding.imageView != nullptr
          ? binding.imageView->cookie() : 0;
        const uint64_t currentViewCookie = currentView != nullptr
          ? currentView->cookie() : 0;
        const VkImage writtenImage = binding.imageView != nullptr
          ? binding.imageView->imageHandle() : VK_NULL_HANDLE;
        const VkImage currentImage = currentView != nullptr
          ? currentView->imageHandle() : VK_NULL_HANDLE;

        Logger::info(str::format(
          "WineHuaDrawBinding: frame=", m_winehuaFrameId,
          " pass=", m_winehuaActivePassId,
          " draw=", passDrawId,
          " binding=", binding.binding,
          " resourceSlot=", binding.resourceSlot,
          " samplerSlot=", currentSamplerSlot,
          " descriptorType=", binding.descriptorType,
          " stages=0x", std::hex, binding.stages,
          " viewType=", std::dec, binding.viewType,
          " writtenImageView=0x", std::hex, binding.image.imageView,
          " writtenSampler=0x", binding.image.sampler,
          " writtenLayout=", std::dec, binding.image.imageLayout,
          " writtenViewCookie=", writtenViewCookie,
          " writtenImage=0x", std::hex, writtenImage,
          " currentImageView=0x", currentViewHandle,
          " currentSampler=0x", currentSamplerHandle,
          " currentViewCookie=", std::dec, currentViewCookie,
          " currentImage=0x", std::hex, currentImage,
          " imageIdentityMatch=", std::dec,
            binding.image.imageView == currentViewHandle ? 1 : 0,
          " samplerIdentityMatch=",
            binding.image.sampler == currentSamplerHandle ? 1 : 0,
          " compareEnable=", binding.sampler != nullptr
            ? binding.sampler->compareToDepth() : VK_FALSE,
          " compareOp=", binding.sampler != nullptr
            ? binding.sampler->compareOp() : VK_COMPARE_OP_NEVER,
          " currentCompareEnable=", currentSampler != nullptr
            ? currentSampler->compareToDepth() : VK_FALSE,
          " currentCompareOp=", currentSampler != nullptr
            ? currentSampler->compareOp() : VK_COMPARE_OP_NEVER,
          " imageFormat=", binding.imageView != nullptr
            ? binding.imageView->imageInfo().format : VK_FORMAT_UNDEFINED,
          " viewFormat=", binding.imageView != nullptr
            ? binding.imageView->info().format : VK_FORMAT_UNDEFINED,
          " aspect=0x", std::hex, binding.imageView != nullptr
            ? binding.imageView->info().aspect : 0,
          " baseMip=", std::dec, binding.imageView != nullptr
            ? binding.imageView->info().minLevel : 0,
          " mipCount=", binding.imageView != nullptr
            ? binding.imageView->info().numLevels : 0,
          " baseLayer=", binding.imageView != nullptr
            ? binding.imageView->info().minLayer : 0,
          " layerCount=", binding.imageView != nullptr
            ? binding.imageView->info().numLayers : 0));
      } else if (binding.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
              || binding.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
              || binding.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
        const DxvkBufferSliceHandle writtenSlice = binding.bufferSlice.defined()
          ? binding.bufferSlice.getSliceHandle() : DxvkBufferSliceHandle();
        const DxvkBufferSliceHandle currentSlice = current.bufferSlice.defined()
          ? current.bufferSlice.getSliceHandle() : DxvkBufferSliceHandle();
        const VkDeviceSize effectiveOffset = binding.buffer.offset
          + (binding.dynamicOffsetBound ? binding.dynamicOffset : 0);

        std::string words = "[]";
        uint64_t contentHash = 1469598103934665603ull;
        uint64_t hashBytes = 0;
        bool mapped = false;
        if (binding.bufferSlice.defined()
         && (binding.bufferSlice.buffer()->memFlags()
           & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
         && writtenSlice.mapPtr != nullptr) {
          mapped = true;
          hashBytes = std::min<uint64_t>(writtenSlice.length, 4096u);
          const auto* bytes = reinterpret_cast<const uint8_t*>(writtenSlice.mapPtr);
          for (uint64_t i = 0; i < hashBytes; i++) {
            contentHash ^= bytes[i];
            contentHash *= 1099511628211ull;
          }

          words = "[";
          const uint32_t wordCount = uint32_t(
            std::min<VkDeviceSize>(writtenSlice.length, 64u) / sizeof(uint32_t));
          for (uint32_t i = 0; i < wordCount; i++) {
            uint32_t word = 0;
            std::memcpy(&word, bytes + i * sizeof(uint32_t), sizeof(word));
            if (i)
              words += ',';
            words += str::format("0x", std::hex, word);
          }
          words += ']';
        }

        Logger::info(str::format(
          "WineHuaDrawBuffer: frame=", m_winehuaFrameId,
          " pass=", m_winehuaActivePassId,
          " draw=", passDrawId,
          " binding=", binding.binding,
          " resourceSlot=", binding.resourceSlot,
          " descriptorType=", binding.descriptorType,
          " stages=0x", std::hex, binding.stages,
          " writtenHandle=0x", binding.buffer.buffer,
          " writtenBaseOffset=", std::dec, binding.buffer.offset,
          " writtenRange=", binding.buffer.range,
          " dynamicOffsetBound=", binding.dynamicOffsetBound ? 1 : 0,
          " dynamicOffset=", binding.dynamicOffset,
          " effectiveOffset=", effectiveOffset,
          " sliceHandle=0x", std::hex, writtenSlice.handle,
          " sliceOffset=", std::dec, writtenSlice.offset,
          " sliceLength=", writtenSlice.length,
          " currentHandle=0x", std::hex, currentSlice.handle,
          " currentOffset=", std::dec, currentSlice.offset,
          " currentLength=", currentSlice.length,
          " identityMatch=", writtenSlice.handle == currentSlice.handle
            && writtenSlice.offset == currentSlice.offset
            && writtenSlice.length == currentSlice.length ? 1 : 0,
          " mapped=", mapped ? 1 : 0,
          " hashBytes=", hashBytes,
          " contentHash=0x", std::hex, contentHash,
          " words=", words));
      } else {
        Logger::info(str::format(
          "WineHuaDrawBinding: frame=", m_winehuaFrameId,
          " pass=", m_winehuaActivePassId,
          " draw=", passDrawId,
          " binding=", binding.binding,
          " resourceSlot=", binding.resourceSlot,
          " descriptorType=", binding.descriptorType,
          " stages=0x", std::hex, binding.stages,
          " texelBuffer=0x", binding.texelBuffer));
      }
    }
  }


  void DxvkContext::winehuaCaptureTargetDraw(
    bool indexed,
    uint32_t count,
    uint32_t first,
    int32_t vertexOffset,
    uint32_t instanceCount,
    uint32_t firstInstance) {
    if (!winehuaTargetDrawCaptureEnabled()
     || m_winehuaFrameId != winehuaRenderTargetDumpFrame())
      return;

    const uint32_t selectedPass = winehuaDrawTracePass();
    const uint32_t secondPass = winehuaDrawTraceSecondPass();
    const bool matchesPrimary = selectedPass == UINT32_MAX
      || m_winehuaActivePassId == selectedPass;
    const bool matchesSecond = secondPass != UINT32_MAX
      && m_winehuaActivePassId == secondPass;
    if (!matchesPrimary && !matchesSecond)
      return;
    if ((matchesSecond && m_winehuaSecondTargetDrawCaptured)
     || (!matchesSecond && m_winehuaTargetDrawCaptured))
      return;

    const char* targetFragmentShader = winehuaRenderTargetDumpFragmentShader();
    const std::string vertexShader = m_state.gp.shaders.vs != nullptr
      ? m_state.gp.shaders.vs->debugName() : std::string();
    const std::string fragmentShader = m_state.gp.shaders.fs != nullptr
      ? m_state.gp.shaders.fs->debugName() : std::string();
    if (targetFragmentShader[0]) {
      const uint64_t targetCount = winehuaRenderTargetDumpIndexCount();
      const uint64_t targetFirst = winehuaRenderTargetDumpFirstIndex();
      const int64_t targetVertexOffset = winehuaRenderTargetDumpVertexOffset();
      if (!indexed
       || fragmentShader != targetFragmentShader
       || (targetCount != UINT64_MAX && count != targetCount)
       || (targetFirst != UINT64_MAX && first != targetFirst)
       || (targetVertexOffset != INT64_MIN && vertexOffset != targetVertexOffset))
        return;
    } else if (m_winehuaLastPassDrawId != winehuaRenderTargetDumpDraw()) {
      return;
    }

    if (matchesSecond)
      m_winehuaSecondTargetDrawCaptured = true;
    else
      m_winehuaTargetDrawCaptured = true;
    Logger::info(str::format(
      "WineHuaDrawCapture: frame=", m_winehuaFrameId,
      " pass=", m_winehuaActivePassId,
      " draw=", m_winehuaLastPassDrawId,
      " indexed=", indexed ? 1 : 0,
      " count=", count,
      " first=", first,
      " vertexOffset=", vertexOffset,
      " instanceCount=", instanceCount,
      " firstInstance=", firstInstance,
      " vs=", vertexShader,
      " fs=", fragmentShader,
      " ending render pass for diagnostic capture"));
    const VkPipeline capturedPipeline = m_gpActivePipeline;
    const VkDescriptorSet capturedDescriptorSet = m_gpSet;
    this->spillRenderPass(false);

    const auto& pipelineState = m_state.gp.state;
    const auto& viewport = m_state.vp.viewports[0];
    const auto& scissor = m_state.vp.scissorRects[0];
    const auto frontStencil = pipelineState.dsFront.state();
    const auto backStencil = pipelineState.dsBack.state();
    const VkSampleCountFlags sampleCount = pipelineState.ms.sampleCount()
      ? pipelineState.ms.sampleCount() : pipelineState.rs.sampleCount();
    const VkPushConstantRange pushRange = m_state.gp.pipeline != nullptr
      && m_state.gp.pipeline->layout() != nullptr
      ? m_state.gp.pipeline->layout()->pushConstRange()
      : VkPushConstantRange { };

    std::string stateJson = str::format(
      "{\"frame\":", m_winehuaFrameId,
      ",\"pass\":", m_winehuaActivePassId,
      ",\"draw\":", m_winehuaLastPassDrawId,
      ",\"kind\":\"pipeline-state\"",
      ",\"pipeline\":", uint64_t(capturedPipeline),
      ",\"descriptorSet\":", uint64_t(capturedDescriptorSet),
      ",\"vertexShader\":\"", vertexShader, "\"",
      ",\"fragmentShader\":\"", fragmentShader, "\"",
      ",\"inputAssembly\":{\"topology\":", uint32_t(pipelineState.ia.primitiveTopology()),
      ",\"primitiveRestart\":", pipelineState.ia.primitiveRestart() ? "true" : "false",
      ",\"patchVertexCount\":", pipelineState.ia.patchVertexCount(), "}",
      ",\"rasterizer\":{\"depthClipEnable\":", pipelineState.rs.depthClipEnable() ? "true" : "false",
      ",\"depthBiasEnable\":", pipelineState.rs.depthBiasEnable() ? "true" : "false",
      ",\"polygonMode\":", uint32_t(pipelineState.rs.polygonMode()),
      ",\"cullMode\":", uint32_t(pipelineState.rs.cullMode()),
      ",\"frontFace\":", uint32_t(pipelineState.rs.frontFace()),
      ",\"viewportCount\":", pipelineState.rs.viewportCount(), "}",
      ",\"multisample\":{\"sampleCount\":", uint32_t(sampleCount),
      ",\"sampleMask\":", pipelineState.ms.sampleMask(),
      ",\"alphaToCoverage\":", pipelineState.ms.enableAlphaToCoverage() ? "true" : "false", "}",
      ",\"depthStencil\":{\"depthTest\":", pipelineState.ds.enableDepthTest() ? "true" : "false",
      ",\"depthWrite\":", pipelineState.ds.enableDepthWrite() ? "true" : "false",
      ",\"depthBoundsTest\":", pipelineState.ds.enableDepthBoundsTest() ? "true" : "false",
      ",\"stencilTest\":", pipelineState.ds.enableStencilTest() ? "true" : "false",
      ",\"depthCompareOp\":", uint32_t(pipelineState.ds.depthCompareOp()),
      ",\"front\":[", uint32_t(frontStencil.failOp), ",", uint32_t(frontStencil.passOp),
      ",", uint32_t(frontStencil.depthFailOp), ",", uint32_t(frontStencil.compareOp),
      ",", frontStencil.compareMask, ",", frontStencil.writeMask, ",", m_state.dyn.stencilReference, "]",
      ",\"back\":[", uint32_t(backStencil.failOp), ",", uint32_t(backStencil.passOp),
      ",", uint32_t(backStencil.depthFailOp), ",", uint32_t(backStencil.compareOp),
      ",", backStencil.compareMask, ",", backStencil.writeMask, ",", m_state.dyn.stencilReference, "]}",
      ",\"viewport\":[", viewport.x, ",", viewport.y, ",", viewport.width, ",", viewport.height,
      ",", viewport.minDepth, ",", viewport.maxDepth, "]",
      ",\"scissor\":[", scissor.offset.x, ",", scissor.offset.y, ",",
      scissor.extent.width, ",", scissor.extent.height, "]",
      ",\"dynamic\":{\"depthBias\":[", m_state.dyn.depthBias.depthBiasConstant, ",",
      m_state.dyn.depthBias.depthBiasClamp, ",", m_state.dyn.depthBias.depthBiasSlope, "]",
      ",\"depthBounds\":[", m_state.dyn.depthBounds.minDepthBounds, ",",
      m_state.dyn.depthBounds.maxDepthBounds, "]",
      ",\"blendConstants\":[", m_state.dyn.blendConstants.r, ",", m_state.dyn.blendConstants.g,
      ",", m_state.dyn.blendConstants.b, ",", m_state.dyn.blendConstants.a, "]}",
      ",\"outputMerger\":{\"logicOpEnable\":", pipelineState.om.enableLogicOp() ? "true" : "false",
      ",\"logicOp\":", uint32_t(pipelineState.om.logicOp()),
      ",\"colorFormats\":[");

    for (uint32_t i = 0; i < MaxNumRenderTargets; i++) {
      if (i)
        stateJson += ',';
      const auto& target = m_state.om.renderTargets.color[i];
      stateJson += str::format(target.view != nullptr
        ? uint32_t(target.view->info().format) : uint32_t(VK_FORMAT_UNDEFINED));
    }

    const auto& depthTarget = m_state.om.renderTargets.depth;
    stateJson += str::format(
      "],\"depthFormat\":", depthTarget.view != nullptr
        ? uint32_t(depthTarget.view->info().format) : uint32_t(VK_FORMAT_UNDEFINED),
      ",\"blendAttachments\":[");
    for (uint32_t i = 0; i < MaxNumRenderTargets; i++) {
      if (i)
        stateJson += ',';
      const auto blend = pipelineState.omBlend[i].state();
      stateJson += str::format(
        "{\"blendEnable\":", blend.blendEnable ? "true" : "false",
        ",\"srcColor\":", uint32_t(blend.srcColorBlendFactor),
        ",\"dstColor\":", uint32_t(blend.dstColorBlendFactor),
        ",\"colorOp\":", uint32_t(blend.colorBlendOp),
        ",\"srcAlpha\":", uint32_t(blend.srcAlphaBlendFactor),
        ",\"dstAlpha\":", uint32_t(blend.dstAlphaBlendFactor),
        ",\"alphaOp\":", uint32_t(blend.alphaBlendOp),
        ",\"writeMask\":", uint32_t(blend.colorWriteMask),
        ",\"swizzle\":[", pipelineState.omSwizzle[i].rIndex(), ",",
        pipelineState.omSwizzle[i].gIndex(), ",", pipelineState.omSwizzle[i].bIndex(), ",",
        pipelineState.omSwizzle[i].aIndex(), "]}");
    }
    stateJson += "]}";

    stateJson += ",\"vertexBindings\":[";
    for (uint32_t i = 0; i < pipelineState.il.bindingCount(); i++) {
      if (i)
        stateJson += ',';
      const auto& binding = pipelineState.ilBindings[i];
      const uint32_t sourceBinding = binding.binding();
      stateJson += str::format(
        "{\"pipelineBinding\":", i,
        ",\"sourceBinding\":", sourceBinding,
        ",\"pipelineStride\":", binding.stride(),
        ",\"boundStride\":", m_state.vi.vertexStrides[sourceBinding],
        ",\"inputRate\":", uint32_t(binding.inputRate()),
        ",\"divisor\":", binding.divisor(), "}");
    }
    stateJson += "],\"vertexAttributes\":[";
    for (uint32_t i = 0; i < pipelineState.il.attributeCount(); i++) {
      if (i)
        stateJson += ',';
      const auto& attribute = pipelineState.ilAttributes[i];
      stateJson += str::format(
        "{\"location\":", attribute.location(),
        ",\"sourceBinding\":", attribute.binding(),
        ",\"format\":", uint32_t(attribute.format()),
        ",\"offset\":", attribute.offset(), "}");
    }
    stateJson += "],\"descriptorBindings\":[";
    for (size_t i = 0; i < m_winehuaGraphicsBindings.size(); i++) {
      if (i)
        stateJson += ',';
      const auto& binding = m_winehuaGraphicsBindings[i];
      stateJson += str::format(
        "{\"binding\":", binding.binding,
        ",\"resourceSlot\":", binding.resourceSlot,
        ",\"type\":", uint32_t(binding.descriptorType),
        ",\"stages\":", uint32_t(binding.stages),
        ",\"viewType\":", uint32_t(binding.viewType), "}");
    }
    stateJson += "],\"samplers\":[";
    bool firstSampler = true;
    for (const auto& binding : m_winehuaGraphicsBindings) {
      if (binding.sampler == nullptr)
        continue;
      if (!firstSampler)
        stateJson += ',';
      const auto& sampler = binding.sampler->info();
      const auto& border = binding.sampler->customBorderColor();
      stateJson += str::format(
        "{\"binding\":", binding.binding,
        ",\"resourceSlot\":", binding.resourceSlot,
        ",\"flags\":", uint32_t(sampler.flags),
        ",\"magFilter\":", uint32_t(sampler.magFilter),
        ",\"minFilter\":", uint32_t(sampler.minFilter),
        ",\"mipmapMode\":", uint32_t(sampler.mipmapMode),
        ",\"addressModeU\":", uint32_t(sampler.addressModeU),
        ",\"addressModeV\":", uint32_t(sampler.addressModeV),
        ",\"addressModeW\":", uint32_t(sampler.addressModeW),
        ",\"mipLodBias\":", sampler.mipLodBias,
        ",\"anisotropyEnable\":", sampler.anisotropyEnable ? "true" : "false",
        ",\"maxAnisotropy\":", sampler.maxAnisotropy,
        ",\"compareEnable\":", sampler.compareEnable ? "true" : "false",
        ",\"compareOp\":", uint32_t(sampler.compareOp),
        ",\"minLod\":", sampler.minLod,
        ",\"maxLod\":", sampler.maxLod,
        ",\"borderColor\":", uint32_t(sampler.borderColor),
        ",\"customBorder\":[", border.float32[0], ",", border.float32[1], ",",
        border.float32[2], ",", border.float32[3], "]",
        ",\"unnormalizedCoordinates\":", sampler.unnormalizedCoordinates ? "true" : "false", "}");
      firstSampler = false;
    }
    stateJson += "],\"specializationConstants\":[";
    for (uint32_t i = 0; i < MaxNumSpecConstants; i++) {
      if (i)
        stateJson += ',';
      stateJson += str::format(pipelineState.sc.specConstants[i]);
    }
    stateJson += str::format(
      "],\"pushConstantOffset\":", pushRange.offset,
      ",\"pushConstantSize\":", pushRange.size,
      ",\"pushConstantWords\":[");
    const uint32_t pushWordCount = std::min(pushRange.size, 64u) / sizeof(uint32_t);
    for (uint32_t i = 0; i < pushWordCount; i++) {
      if (i)
        stateJson += ',';
      uint32_t word = 0;
      std::memcpy(&word, &m_state.pc.data[pushRange.offset + i * sizeof(uint32_t)], sizeof(word));
      stateJson += str::format(word);
    }
    stateJson += "]}";
    m_winehuaGeometryStateJson += stateJson;
    m_winehuaGeometryStateJson += '\n';

    const uint64_t maxBytes = winehuaGeometryDumpMaxBytes();
    const uint32_t indexSize = indexed
      ? (m_state.vi.indexType == VK_INDEX_TYPE_UINT16 ? 2u
       : m_state.vi.indexType == VK_INDEX_TYPE_UINT32 ? 4u : 0u)
      : 0u;

    auto captureBuffer = [&](const char* kind, uint32_t binding,
                             uint32_t resourceSlot,
                             VkDescriptorType descriptorType,
                             VkShaderStageFlags stages,
                             VkDeviceSize dynamicOffset,
                             const DxvkBufferSlice& source,
                             VkDeviceSize relativeOffset,
                             VkDeviceSize requestedLength,
                             uint32_t stride) {
      if (!source.defined() || !requestedLength
       || relativeOffset >= source.length()
       || m_winehuaGeometryBytes >= maxBytes)
        return;

      const VkDeviceSize available = source.length() - relativeOffset;
      const VkDeviceSize remaining = maxBytes - m_winehuaGeometryBytes;
      const VkDeviceSize length = std::min(
        std::min(available, requestedLength), remaining);
      if (!length)
        return;

      DxvkBufferCreateInfo info = { };
      info.size = length;
      info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT
                 | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
      info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
      info.access = VK_ACCESS_TRANSFER_WRITE_BIT;

      auto dumpBuffer = m_device->createBuffer(info,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
        | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      auto sourceSlice = source.getSliceHandle(relativeOffset, length);
      auto dumpSlice = dumpBuffer->getSliceHandle();

      if (m_execBarriers.isBufferDirty(sourceSlice, DxvkAccess::Read))
        m_execBarriers.recordCommands(m_cmd);

      VkBufferCopy region;
      region.srcOffset = sourceSlice.offset;
      region.dstOffset = dumpSlice.offset;
      region.size = length;
      m_cmd->cmdCopyBuffer(DxvkCmdBuffer::ExecBuffer,
        sourceSlice.handle, dumpSlice.handle, 1, &region);

      m_execBarriers.accessBuffer(sourceSlice,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_TRANSFER_READ_BIT,
        source.bufferInfo().stages,
        source.bufferInfo().access);
      m_cmd->trackResource<DxvkAccess::Read>(source.buffer());
      m_cmd->trackResource<DxvkAccess::Write>(dumpBuffer);

      WineHuaGeometryDump dump;
      dump.kind = kind;
      dump.frameId = m_winehuaFrameId;
      dump.passId = m_winehuaActivePassId;
      dump.drawId = m_winehuaLastPassDrawId;
      dump.binding = binding;
      dump.resourceSlot = resourceSlot;
      dump.descriptorType = descriptorType;
      dump.stages = stages;
      dump.indexed = indexed;
      dump.indexType = m_state.vi.indexType;
      dump.count = count;
      dump.first = first;
      dump.vertexOffset = vertexOffset;
      dump.instanceCount = instanceCount;
      dump.firstInstance = firstInstance;
      dump.dynamicOffset = dynamicOffset;
      dump.stride = stride;
      dump.vertexShader = vertexShader;
      dump.fragmentShader = fragmentShader;
      dump.sourceHandle = sourceSlice.handle;
      dump.sourceOffset = sourceSlice.offset;
      dump.sourceLength = sourceSlice.length;
      dump.dataSize = length;
      dump.buffer = std::move(dumpBuffer);
      m_winehuaGeometryBytes += length;
      m_winehuaGeometryDumps.push_back(std::move(dump));

      Logger::info(str::format(
        "WineHuaGeometryCapture: frame=", m_winehuaFrameId,
        " pass=", m_winehuaActivePassId,
        " draw=", m_winehuaLastPassDrawId,
        " kind=", kind,
        " binding=", binding,
        " resourceSlot=", resourceSlot,
        " descriptorType=", descriptorType,
        " stages=0x", std::hex, stages,
        " source=0x", std::hex, sourceSlice.handle,
        " offset=", std::dec, sourceSlice.offset,
        " bytes=", length,
        " total=", m_winehuaGeometryBytes));
    };

    if (indexed && indexSize) {
      const auto& indexBuffer = m_state.vi.indexBuffer;
      const VkDeviceSize relativeOffset = VkDeviceSize(first) * indexSize;
      const VkDeviceSize requestedLength = VkDeviceSize(count) * indexSize;
      captureBuffer("index", UINT32_MAX, UINT32_MAX,
        VK_DESCRIPTOR_TYPE_MAX_ENUM, 0, 0, indexBuffer,
        relativeOffset, requestedLength, indexSize);
    }

    for (uint32_t i = 0; i < m_state.gp.state.il.bindingCount(); i++) {
      const uint32_t binding = m_state.gp.state.ilBindings[i].binding();
      const auto& vertexBuffer = m_state.vi.vertexBuffers[binding];
      captureBuffer("vertex", binding, UINT32_MAX,
        VK_DESCRIPTOR_TYPE_MAX_ENUM, 0, 0, vertexBuffer, 0,
        vertexBuffer.defined() ? vertexBuffer.length() : 0,
        m_state.vi.vertexStrides[binding]);
    }

    for (const auto& binding : m_winehuaGraphicsBindings) {
      if (binding.descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
       && binding.descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
       && binding.descriptorType != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
        continue;

      const VkDeviceSize dynamicOffset = binding.dynamicOffsetBound
        ? binding.dynamicOffset : 0;
      captureBuffer("descriptor-buffer", binding.binding,
        binding.resourceSlot, binding.descriptorType, binding.stages,
        dynamicOffset, binding.bufferSlice, dynamicOffset,
        binding.bufferSlice.defined()
          && dynamicOffset < binding.bufferSlice.length()
          ? binding.bufferSlice.length() - dynamicOffset : 0,
        0);
    }
  }


  void DxvkContext::winehuaWriteGeometryDumps() {
    if (m_winehuaGeometryDumps.empty())
      return;

    const std::string basePath = winehuaRenderTargetDumpPath();
    const std::string frameName = str::format("frame-", m_winehuaFrameId);
    const std::string metadataPath = str::format(
      basePath, "/", frameName, "-geometry.jsonl");
    std::ofstream metadata(
      str::tows(metadataPath.c_str()).c_str(),
      std::ios_base::binary | std::ios_base::trunc);

    if (!metadata) {
      Logger::err(str::format(
        "WineHuaGeometryCapture: cannot open metadata path=", metadataPath));
      m_winehuaGeometryDumps.clear();
      m_winehuaGeometryStateJson.clear();
      return;
    }

    if (!m_winehuaGeometryStateJson.empty())
      metadata << m_winehuaGeometryStateJson;

    uint32_t filesWritten = 0;
    for (auto& dump : m_winehuaGeometryDumps) {
      if (dump.buffer == nullptr || !dump.dataSize)
        continue;

      const auto slice = dump.buffer->getSliceHandle();
      m_device->waitForResource(dump.buffer, DxvkAccess::Write);
      dump.buffer->invalidateMappedSlice(slice);

      const std::string bindingSuffix = dump.binding == UINT32_MAX
        ? std::string()
        : str::format("-", dump.binding);
      const std::string fileName = str::format(
        frameName, "-pass-", dump.passId,
        "-draw-", dump.drawId, "-", dump.kind,
        bindingSuffix, ".bin");
      const std::string dataPath = str::format(basePath, "/", fileName);
      std::ofstream data(
        str::tows(dataPath.c_str()).c_str(),
        std::ios_base::binary | std::ios_base::trunc);
      bool dataWritten = false;
      if (data) {
        data.write(reinterpret_cast<const char*>(slice.mapPtr), dump.dataSize);
        dataWritten = data.good();
        if (dataWritten)
          filesWritten++;
      }

      metadata
        << "{\"frame\":" << dump.frameId
        << ",\"pass\":" << dump.passId
        << ",\"draw\":" << dump.drawId
        << ",\"kind\":\"" << dump.kind << "\""
        << ",\"binding\":" << dump.binding
        << ",\"resourceSlot\":" << dump.resourceSlot
        << ",\"descriptorType\":" << uint32_t(dump.descriptorType)
        << ",\"stages\":" << dump.stages
        << ",\"indexed\":" << (dump.indexed ? "true" : "false")
        << ",\"indexType\":" << uint32_t(dump.indexType)
        << ",\"count\":" << dump.count
        << ",\"first\":" << dump.first
        << ",\"vertexOffset\":" << dump.vertexOffset
        << ",\"instanceCount\":" << dump.instanceCount
        << ",\"firstInstance\":" << dump.firstInstance
        << ",\"dynamicOffset\":" << dump.dynamicOffset
        << ",\"stride\":" << dump.stride
        << ",\"vertexShader\":\"" << dump.vertexShader << "\""
        << ",\"fragmentShader\":\"" << dump.fragmentShader << "\""
        << ",\"sourceHandle\":" << uint64_t(dump.sourceHandle)
        << ",\"sourceOffset\":" << dump.sourceOffset
        << ",\"sourceLength\":" << dump.sourceLength
        << ",\"bytes\":" << dump.dataSize
        << ",\"file\":\"" << (dataWritten ? fileName : "")
        << "\"}\n";
    }

    Logger::info(str::format(
      "WineHuaGeometryCapture: completed frame=", m_winehuaFrameId,
      " records=", m_winehuaGeometryDumps.size(),
      " files=", filesWritten,
      " bytes=", m_winehuaGeometryBytes,
      " metadata=", metadataPath));
    m_winehuaGeometryDumps.clear();
    m_winehuaGeometryStateJson.clear();
  }


  void DxvkContext::winehuaWriteRenderTargetDumps() {
    const std::string basePath = winehuaRenderTargetDumpPath();
    const std::string frameName = str::format("frame-", m_winehuaFrameId);
    const std::string metadataPath = str::format(basePath, "/", frameName, ".jsonl");
    std::ofstream metadata(
      str::tows(metadataPath.c_str()).c_str(),
      std::ios_base::binary | std::ios_base::trunc);

    if (!metadata) {
      Logger::err(str::format(
        "WineHuaRenderTargetDump: cannot open metadata path=", metadataPath));
      m_winehuaRenderTargetDumps.clear();
      return;
    }

    uint32_t filesWritten = 0;
    for (auto& dump : m_winehuaRenderTargetDumps) {
      std::string fileName;
      bool dataWritten = false;

      if (dump.buffer != nullptr && dump.dataSize) {
        const auto slice = dump.buffer->getSliceHandle();
        m_device->waitForResource(dump.buffer, DxvkAccess::Write);
        dump.buffer->invalidateMappedSlice(slice);

        const std::string mipSuffix = dump.kind == "sampled" && dump.baseMip
          ? str::format("-mip-", dump.baseMip) : std::string();
        fileName = str::format(
          frameName, "-pass-", dump.passId,
          "-", dump.kind, "-", dump.attachmentId, mipSuffix, ".bin");
        const std::string dataPath = str::format(basePath, "/", fileName);
        std::ofstream data(
          str::tows(dataPath.c_str()).c_str(),
          std::ios_base::binary | std::ios_base::trunc);

        if (data) {
          data.write(reinterpret_cast<const char*>(slice.mapPtr), dump.dataSize);
          dataWritten = data.good();
          if (dataWritten)
            filesWritten++;
        }

        if (!dataWritten) {
          Logger::err(str::format(
            "WineHuaRenderTargetDump: cannot write data path=", dataPath));
        }
      }

      metadata
        << "{\"frame\":" << dump.frameId
        << ",\"pass\":" << dump.passId
        << ",\"attachment\":" << dump.attachmentId
        << ",\"kind\":\"" << dump.kind << "\""
        << ",\"colorIndex\":" << dump.colorIndex
        << ",\"descriptorBinding\":" << dump.descriptorBinding
        << ",\"resourceSlot\":" << dump.resourceSlot
        << ",\"viewCookie\":" << dump.viewCookie
        << ",\"imageHandle\":" << uint64_t(dump.imageHandle)
        << ",\"imageFormat\":" << uint32_t(dump.imageFormat)
        << ",\"viewFormat\":" << uint32_t(dump.viewFormat)
        << ",\"aspect\":" << uint32_t(dump.aspect)
        << ",\"extent\":[" << dump.extent.width << ','
                              << dump.extent.height << ','
                              << dump.extent.depth << ']'
        << ",\"viewport\":[" << dump.viewport.x << ','
                                 << dump.viewport.y << ','
                                 << dump.viewport.width << ','
                                 << dump.viewport.height << ','
                                 << dump.viewport.minDepth << ','
                                 << dump.viewport.maxDepth << ']'
        << ",\"scissor\":[" << dump.scissor.offset.x << ','
                                << dump.scissor.offset.y << ','
                                << dump.scissor.extent.width << ','
                                << dump.scissor.extent.height << ']'
        << ",\"baseMip\":" << dump.baseMip
        << ",\"baseLayer\":" << dump.baseLayer
        << ",\"layerCount\":" << dump.layerCount
        << ",\"layout\":" << uint32_t(dump.layout)
        << ",\"loadLayout\":" << uint32_t(dump.loadLayout)
        << ",\"loadOp\":" << uint32_t(dump.loadOp)
        << ",\"storeLayout\":" << uint32_t(dump.storeLayout)
        << ",\"rowPitch\":" << dump.rowPitch
        << ",\"slicePitch\":" << dump.slicePitch
        << ",\"bytes\":" << dump.dataSize
        << ",\"vertexShader\":\"" << dump.vertexShader << "\""
        << ",\"fragmentShader\":\"" << dump.fragmentShader << "\""
        << ",\"resourceViews\":" << dump.resourceViews
        << ",\"file\":\"" << (dataWritten ? fileName : "") << "\""
        << "}\n";
    }

    Logger::info(str::format(
      "WineHuaRenderTargetDump: completed frame=", m_winehuaFrameId,
      " records=", m_winehuaRenderTargetDumps.size(),
      " files=", filesWritten,
      " metadata=", metadataPath));
    m_winehuaRenderTargetDumps.clear();
  }
  
  
  void DxvkContext::beginQuery(const Rc<DxvkGpuQuery>& query) {
    m_queryManager.enableQuery(m_cmd, query);
  }


  void DxvkContext::endQuery(const Rc<DxvkGpuQuery>& query) {
    m_queryManager.disableQuery(m_cmd, query);
  }
  
  
  void DxvkContext::bindRenderTargets(
    const DxvkRenderTargets&    targets) {
    const auto& oldDepth = m_state.om.renderTargets.depth.view;
    const auto& newDepth = targets.depth.view;
    if (winehuaSampleTraceEnabled()
     && (winehuaTraceDepthView(oldDepth) || winehuaTraceDepthView(newDepth))) {
      winehuaSampleTrace(str::format(
        "depth-target-bind oldCookie=", oldDepth != nullptr ? oldDepth->cookie() : 0,
        " oldImage=0x", std::hex, oldDepth != nullptr ? oldDepth->imageHandle() : VK_NULL_HANDLE,
        " newCookie=", std::dec, newDepth != nullptr ? newDepth->cookie() : 0,
        " newImage=0x", std::hex, newDepth != nullptr ? newDepth->imageHandle() : VK_NULL_HANDLE,
        " newLayout=", std::dec, targets.depth.layout));
    }

    // Set up default render pass ops
    m_state.om.renderTargets = targets;
    
    this->resetRenderPassOps(
      m_state.om.renderTargets,
      m_state.om.renderPassOps);

    if (!m_state.om.framebufferInfo.hasTargets(targets)) {
      // Create a new framebuffer object next
      // time we start rendering something
      m_flags.set(DxvkContextFlag::GpDirtyFramebuffer);
    } else {
      // Don't redundantly spill the render pass if
      // the same render targets are bound again
      m_flags.clr(DxvkContextFlag::GpDirtyFramebuffer);
    }
  }
  
  
  void DxvkContext::bindDrawBuffers(
    const DxvkBufferSlice&      argBuffer,
    const DxvkBufferSlice&      cntBuffer) {
    m_state.id.argBuffer = argBuffer;
    m_state.id.cntBuffer = cntBuffer;

    m_flags.set(DxvkContextFlag::DirtyDrawBuffer);
  }


  void DxvkContext::bindIndexBuffer(
    const DxvkBufferSlice&      buffer,
          VkIndexType           indexType) {
    if (!m_state.vi.indexBuffer.matchesBuffer(buffer))
      m_vbTracked.clr(MaxNumVertexBindings);

    m_state.vi.indexBuffer = buffer;
    m_state.vi.indexType   = indexType;

    m_flags.set(DxvkContextFlag::GpDirtyIndexBuffer);
  }
  
  
  void DxvkContext::bindResourceBuffer(
          uint32_t              slot,
    const DxvkBufferSlice&      buffer) {
    bool needsUpdate = !m_rc[slot].bufferSlice.matchesBuffer(buffer);

    if (likely(needsUpdate))
      m_rcTracked.clr(slot);
    else
      needsUpdate = m_rc[slot].bufferSlice.length() != buffer.length();

    if (likely(needsUpdate)) {
      m_flags.set(
        DxvkContextFlag::CpDirtyResources,
        DxvkContextFlag::GpDirtyResources);
    } else {
      m_flags.set(
        DxvkContextFlag::CpDirtyDescriptorBinding,
        DxvkContextFlag::GpDirtyDescriptorBinding);
    }

    m_rc[slot].bufferSlice = buffer;
  }
  
  
  void DxvkContext::bindResourceView(
          uint32_t              slot,
    const Rc<DxvkImageView>&    imageView,
    const Rc<DxvkBufferView>&   bufferView) {
    m_rc[slot].imageView   = imageView;
    m_rc[slot].bufferView  = bufferView;
    m_rc[slot].bufferSlice = bufferView != nullptr
      ? bufferView->slice()
      : DxvkBufferSlice();
    m_rcTracked.clr(slot);

    m_flags.set(
      DxvkContextFlag::CpDirtyResources,
      DxvkContextFlag::GpDirtyResources);
  }
  
  
  void DxvkContext::bindResourceSampler(
          uint32_t              slot,
    const Rc<DxvkSampler>&      sampler) {
    m_rc[slot].sampler = sampler;
    m_rcTracked.clr(slot);

    m_flags.set(
      DxvkContextFlag::CpDirtyResources,
      DxvkContextFlag::GpDirtyResources);
  }
  
  
  void DxvkContext::bindShader(
          VkShaderStageFlagBits stage,
    const Rc<DxvkShader>&       shader) {
    Rc<DxvkShader>* shaderStage;
    
    switch (stage) {
      case VK_SHADER_STAGE_VERTEX_BIT:                  shaderStage = &m_state.gp.shaders.vs;  break;
      case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:    shaderStage = &m_state.gp.shaders.tcs; break;
      case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: shaderStage = &m_state.gp.shaders.tes; break;
      case VK_SHADER_STAGE_GEOMETRY_BIT:                shaderStage = &m_state.gp.shaders.gs;  break;
      case VK_SHADER_STAGE_FRAGMENT_BIT:                shaderStage = &m_state.gp.shaders.fs;  break;
      case VK_SHADER_STAGE_COMPUTE_BIT:                 shaderStage = &m_state.cp.shaders.cs;  break;
      default: return;
    }
    
    *shaderStage = shader;

    if (stage == VK_SHADER_STAGE_COMPUTE_BIT) {
      m_flags.set(
        DxvkContextFlag::CpDirtyPipeline,
        DxvkContextFlag::CpDirtyPipelineState,
        DxvkContextFlag::CpDirtyResources);
    } else {
      m_flags.set(
        DxvkContextFlag::GpDirtyPipeline,
        DxvkContextFlag::GpDirtyPipelineState,
        DxvkContextFlag::GpDirtyResources);
    }
  }
  
  
  void DxvkContext::bindVertexBuffer(
          uint32_t              binding,
    const DxvkBufferSlice&      buffer,
          uint32_t              stride) {
    if (!m_state.vi.vertexBuffers[binding].matchesBuffer(buffer))
      m_vbTracked.clr(binding);

    m_state.vi.vertexBuffers[binding] = buffer;
    m_flags.set(DxvkContextFlag::GpDirtyVertexBuffers);
    
    if (unlikely(m_state.vi.vertexStrides[binding] != stride)) {
      m_state.vi.vertexStrides[binding] = stride;
      m_flags.set(DxvkContextFlag::GpDirtyPipelineState);
    }
  }
  
  
  void DxvkContext::bindXfbBuffer(
          uint32_t              binding,
    const DxvkBufferSlice&      buffer,
    const DxvkBufferSlice&      counter) {
    if (!m_state.xfb.buffers [binding].matches(buffer)
     || !m_state.xfb.counters[binding].matches(counter)) {
      m_state.xfb.buffers [binding] = buffer;
      m_state.xfb.counters[binding] = counter;
      
      m_flags.set(DxvkContextFlag::GpDirtyXfbBuffers);
    }
  }


  void DxvkContext::blitImage(
    const Rc<DxvkImage>&        dstImage,
    const VkComponentMapping&   dstMapping,
    const Rc<DxvkImage>&        srcImage,
    const VkComponentMapping&   srcMapping,
    const VkImageBlit&          region,
          VkFilter              filter) {
    this->spillRenderPass(true);
    this->prepareImage(m_execBarriers, dstImage, vk::makeSubresourceRange(region.dstSubresource));
    this->prepareImage(m_execBarriers, srcImage, vk::makeSubresourceRange(region.srcSubresource));

    auto mapping = util::resolveSrcComponentMapping(dstMapping, srcMapping);

    bool canUseFb = (srcImage->info().usage & VK_IMAGE_USAGE_SAMPLED_BIT)
                 && (dstImage->info().usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
                 && ((dstImage->info().flags & VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT)
                  || (dstImage->info().type != VK_IMAGE_TYPE_3D));

    bool useFb = dstImage->info().sampleCount != VK_SAMPLE_COUNT_1_BIT
              || !util::isIdentityMapping(mapping);

    if (!useFb) {
      this->blitImageHw(
        dstImage, srcImage,
        region, filter);
    } else if (canUseFb) {
      this->blitImageFb(
        dstImage, srcImage,
        region, mapping, filter);
    } else {
      Logger::err("DxvkContext: Unsupported blit operation");
    }
  }


  void DxvkContext::changeImageLayout(
    const Rc<DxvkImage>&        image,
          VkImageLayout         layout) {
    if (image->info().layout != layout) {
      this->spillRenderPass(true);

      VkImageSubresourceRange subresources = image->getAvailableSubresources();

      this->prepareImage(m_execBarriers, image, subresources);

      if (m_execBarriers.isImageDirty(image, subresources, DxvkAccess::Write))
        m_execBarriers.recordCommands(m_cmd);

      m_execBarriers.accessImage(image, subresources,
        image->info().layout,
        image->info().stages, 0,
        layout,
        image->info().stages,
        image->info().access);

      image->setLayout(layout);

      m_cmd->trackResource<DxvkAccess::Write>(image);
    }
  }


  void DxvkContext::clearBuffer(
    const Rc<DxvkBuffer>&       buffer,
          VkDeviceSize          offset,
          VkDeviceSize          length,
          uint32_t              value) {
    bool replaceBuffer = this->tryInvalidateDeviceLocalBuffer(buffer, length);
    auto bufferSlice = buffer->getSliceHandle(offset, align(length, sizeof(uint32_t)));

    if (!replaceBuffer) {
      this->spillRenderPass(true);
    
      if (m_execBarriers.isBufferDirty(bufferSlice, DxvkAccess::Write))
        m_execBarriers.recordCommands(m_cmd);
    }

    DxvkCmdBuffer cmdBuffer = replaceBuffer
      ? DxvkCmdBuffer::InitBuffer
      : DxvkCmdBuffer::ExecBuffer;

    if (length > sizeof(value)) {
      m_cmd->cmdFillBuffer(cmdBuffer,
        bufferSlice.handle,
        bufferSlice.offset,
        bufferSlice.length,
        value);
    } else {
      m_cmd->cmdUpdateBuffer(cmdBuffer,
        bufferSlice.handle,
        bufferSlice.offset,
        bufferSlice.length,
        &value);
    }

    auto& barriers = replaceBuffer
      ? m_initBarriers
      : m_execBarriers;

    barriers.accessBuffer(bufferSlice,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      buffer->info().stages,
      buffer->info().access);

    m_cmd->trackResource<DxvkAccess::Write>(buffer);
  }
  
  
  void DxvkContext::clearBufferView(
    const Rc<DxvkBufferView>&   bufferView,
          VkDeviceSize          offset,
          VkDeviceSize          length,
          VkClearColorValue     value) {
    this->spillRenderPass(true);
    this->unbindComputePipeline();

    // The view range might have been invalidated, so
    // we need to make sure the handle is up to date
    bufferView->updateView();

    auto bufferSlice = bufferView->getSliceHandle();

    if (m_execBarriers.isBufferDirty(bufferSlice, DxvkAccess::Write))
      m_execBarriers.recordCommands(m_cmd);
    
    // Query pipeline objects to use for this clear operation
    DxvkMetaClearPipeline pipeInfo = m_common->metaClear().getClearBufferPipeline(
      imageFormatInfo(bufferView->info().format)->flags);
    
    // Create a descriptor set pointing to the view
    VkBufferView viewObject = bufferView->handle();
    
    VkDescriptorSet descriptorSet = allocateDescriptorSet(pipeInfo.dsetLayout);
    
    VkWriteDescriptorSet descriptorWrite;
    descriptorWrite.sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrite.pNext            = nullptr;
    descriptorWrite.dstSet           = descriptorSet;
    descriptorWrite.dstBinding       = 0;
    descriptorWrite.dstArrayElement  = 0;
    descriptorWrite.descriptorCount  = 1;
    descriptorWrite.descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
    descriptorWrite.pImageInfo       = nullptr;
    descriptorWrite.pBufferInfo      = nullptr;
    descriptorWrite.pTexelBufferView = &viewObject;
    m_cmd->updateDescriptorSets(1, &descriptorWrite);
    
    // Prepare shader arguments
    DxvkMetaClearArgs pushArgs = { };
    pushArgs.clearValue = value;
    pushArgs.offset = VkOffset3D {  int32_t(offset), 0, 0 };
    pushArgs.extent = VkExtent3D { uint32_t(length), 1, 1 };
    
    VkExtent3D workgroups = util::computeBlockCount(
      pushArgs.extent, pipeInfo.workgroupSize);
    
    m_cmd->cmdBindPipeline(
      VK_PIPELINE_BIND_POINT_COMPUTE,
      pipeInfo.pipeline);
    m_cmd->cmdBindDescriptorSet(
      VK_PIPELINE_BIND_POINT_COMPUTE,
      pipeInfo.pipeLayout, descriptorSet,
      0, nullptr);
    m_cmd->cmdPushConstants(
      pipeInfo.pipeLayout,
      VK_SHADER_STAGE_COMPUTE_BIT,
      0, sizeof(pushArgs), &pushArgs);
    m_cmd->cmdDispatch(
      workgroups.width,
      workgroups.height,
      workgroups.depth);
    
    m_execBarriers.accessBuffer(bufferSlice,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_WRITE_BIT,
      bufferView->bufferInfo().stages,
      bufferView->bufferInfo().access);
    
    m_cmd->trackResource<DxvkAccess::None>(bufferView);
    m_cmd->trackResource<DxvkAccess::Write>(bufferView->buffer());
  }
  
  
  void DxvkContext::clearRenderTarget(
    const Rc<DxvkImageView>&    imageView,
          VkImageAspectFlags    clearAspects,
          VkClearValue          clearValue) {
    // Make sure the color components are ordered correctly
    if (clearAspects & VK_IMAGE_ASPECT_COLOR_BIT) {
      clearValue.color = util::swizzleClearColor(clearValue.color,
        util::invertComponentMapping(imageView->info().swizzle));
    }
    
    // Check whether the render target view is an attachment
    // of the current framebuffer and is included entirely.
    // If not, we need to create a temporary framebuffer.
    int32_t attachmentIndex = -1;
    
    if (m_state.om.framebufferInfo.isFullSize(imageView))
      attachmentIndex = m_state.om.framebufferInfo.findAttachment(imageView);

    if (winehuaSampleTraceEnabled() && winehuaTraceDepthView(imageView)) {
      winehuaSampleTrace(str::format(
        "depth-clear-request cookie=", imageView->cookie(),
        " image=0x", std::hex, imageView->imageHandle(),
        " view=0x", imageView->handle(),
        " imageFormat=", std::dec, imageView->imageInfo().format,
        " viewFormat=", imageView->info().format,
        " aspect=0x", std::hex, clearAspects,
        " baseMip=", std::dec, imageView->info().minLevel,
        " mipCount=", imageView->info().numLevels,
        " baseLayer=", imageView->info().minLayer,
        " layerCount=", imageView->info().numLayers,
        " depth=", clearValue.depthStencil.depth,
        " stencil=", clearValue.depthStencil.stencil,
        " attachmentIndex=", attachmentIndex,
        " renderPassBound=", m_flags.test(DxvkContextFlag::GpRenderPassBound) ? 1 : 0));
    }

    if (attachmentIndex < 0) {
      // Suspend works here because we'll end up with one of these scenarios:
      // 1) The render pass gets ended for good, in which case we emit barriers
      // 2) The clear gets folded into render pass ops, so the layout is correct
      // 3) The clear gets executed separately, in which case updateFramebuffer
      //    will indirectly emit barriers for the given render target.
      // If there is overlap, we need to explicitly transition affected attachments.
      this->spillRenderPass(true);
      this->prepareImage(m_execBarriers, imageView->image(), imageView->subresources(), false);
    } else if (!m_state.om.framebufferInfo.isWritable(attachmentIndex, clearAspects)) {
      // We cannot inline clears if the clear aspects are not writable
      this->spillRenderPass(true);
    }

    if (m_flags.test(DxvkContextFlag::GpRenderPassBound)) {
      uint32_t colorIndex = std::max(0, m_state.om.framebufferInfo.getColorAttachmentIndex(attachmentIndex));

      VkClearAttachment clearInfo;
      clearInfo.aspectMask      = clearAspects;
      clearInfo.colorAttachment = colorIndex;
      clearInfo.clearValue      = clearValue;

      VkClearRect clearRect;
      clearRect.rect.offset.x       = 0;
      clearRect.rect.offset.y       = 0;
      clearRect.rect.extent.width   = imageView->mipLevelExtent(0).width;
      clearRect.rect.extent.height  = imageView->mipLevelExtent(0).height;
      clearRect.baseArrayLayer      = 0;
      clearRect.layerCount          = imageView->info().numLayers;

      m_cmd->cmdClearAttachments(1, &clearInfo, 1, &clearRect);
    } else
      this->deferClear(imageView, clearAspects, clearValue);
  }
  
  
  void DxvkContext::clearImageView(
    const Rc<DxvkImageView>&    imageView,
          VkOffset3D            offset,
          VkExtent3D            extent,
          VkImageAspectFlags    aspect,
          VkClearValue          value) {
    const VkImageUsageFlags viewUsage = imageView->info().usage;

    if (aspect & VK_IMAGE_ASPECT_COLOR_BIT) {
      value.color = util::swizzleClearColor(value.color,
        util::invertComponentMapping(imageView->info().swizzle));
    }
    
    if (viewUsage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))
      this->clearImageViewFb(imageView, offset, extent, aspect, value);
    else if (viewUsage & VK_IMAGE_USAGE_STORAGE_BIT)
      this->clearImageViewCs(imageView, offset, extent, value);
  }
  
  
  void DxvkContext::copyBuffer(
    const Rc<DxvkBuffer>&       dstBuffer,
          VkDeviceSize          dstOffset,
    const Rc<DxvkBuffer>&       srcBuffer,
          VkDeviceSize          srcOffset,
          VkDeviceSize          numBytes) {
    // When overwriting small buffers, we can allocate a new slice in order to
    // avoid suspending the current render pass or inserting barriers. The source
    // buffer must be read-only since otherwise we cannot schedule the copy early.
    bool srcIsReadOnly = DxvkBarrierSet::getAccessTypes(srcBuffer->info().access) == DxvkAccess::Read;
    bool replaceBuffer = srcIsReadOnly && this->tryInvalidateDeviceLocalBuffer(dstBuffer, numBytes);

    auto srcSlice = srcBuffer->getSliceHandle(srcOffset, numBytes);
    auto dstSlice = dstBuffer->getSliceHandle(dstOffset, numBytes);

    if (!replaceBuffer) {
      this->spillRenderPass(true);

      if (m_execBarriers.isBufferDirty(srcSlice, DxvkAccess::Read)
       || m_execBarriers.isBufferDirty(dstSlice, DxvkAccess::Write))
        m_execBarriers.recordCommands(m_cmd);
    }

    DxvkCmdBuffer cmdBuffer = replaceBuffer
      ? DxvkCmdBuffer::InitBuffer
      : DxvkCmdBuffer::ExecBuffer;

    VkBufferCopy bufferRegion;
    bufferRegion.srcOffset = srcSlice.offset;
    bufferRegion.dstOffset = dstSlice.offset;
    bufferRegion.size      = dstSlice.length;

    m_cmd->cmdCopyBuffer(cmdBuffer,
      srcSlice.handle, dstSlice.handle, 1, &bufferRegion);

    auto& barriers = replaceBuffer
      ? m_initBarriers
      : m_execBarriers;

    barriers.accessBuffer(srcSlice,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_READ_BIT,
      srcBuffer->info().stages,
      srcBuffer->info().access);

    barriers.accessBuffer(dstSlice,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      dstBuffer->info().stages,
      dstBuffer->info().access);

    m_cmd->trackResource<DxvkAccess::Write>(dstBuffer);
    m_cmd->trackResource<DxvkAccess::Read>(srcBuffer);
  }
  
  
  void DxvkContext::copyBufferRegion(
    const Rc<DxvkBuffer>&       dstBuffer,
          VkDeviceSize          dstOffset,
          VkDeviceSize          srcOffset,
          VkDeviceSize          numBytes) {
    VkDeviceSize loOvl = std::max(dstOffset, srcOffset);
    VkDeviceSize hiOvl = std::min(dstOffset, srcOffset) + numBytes;

    if (hiOvl > loOvl) {
      DxvkBufferCreateInfo bufInfo;
      bufInfo.size    = numBytes;
      bufInfo.usage   = VK_BUFFER_USAGE_TRANSFER_DST_BIT
                      | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
      bufInfo.stages  = VK_PIPELINE_STAGE_TRANSFER_BIT;
      bufInfo.access  = VK_ACCESS_TRANSFER_WRITE_BIT
                      | VK_ACCESS_TRANSFER_READ_BIT;

      auto tmpBuffer = m_device->createBuffer(
        bufInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      
      VkDeviceSize tmpOffset = 0;
      
      this->copyBuffer(tmpBuffer, tmpOffset, dstBuffer, srcOffset, numBytes);
      this->copyBuffer(dstBuffer, dstOffset, tmpBuffer, tmpOffset, numBytes);
    } else {
      this->copyBuffer(dstBuffer, dstOffset, dstBuffer, srcOffset, numBytes);
    }
  }


  void DxvkContext::copyBufferToImage(
    const Rc<DxvkImage>&        dstImage,
          VkImageSubresourceLayers dstSubresource,
          VkOffset3D            dstOffset,
          VkExtent3D            dstExtent,
    const Rc<DxvkBuffer>&       srcBuffer,
          VkDeviceSize          srcOffset,
          VkDeviceSize          rowAlignment,
          VkDeviceSize          sliceAlignment) {
    this->spillRenderPass(true);
    this->prepareImage(m_execBarriers, dstImage, vk::makeSubresourceRange(dstSubresource));

    auto srcSlice = srcBuffer->getSliceHandle(srcOffset, 0);

    // We may copy to only one aspect at a time, but pipeline
    // barriers need to have all available aspect bits set
    auto dstFormatInfo = dstImage->formatInfo();

    auto dstSubresourceRange = vk::makeSubresourceRange(dstSubresource);
    dstSubresourceRange.aspectMask = dstFormatInfo->aspectMask;
    
    if (m_execBarriers.isImageDirty(dstImage, dstSubresourceRange, DxvkAccess::Write)
     || m_execBarriers.isBufferDirty(srcSlice, DxvkAccess::Read))
      m_execBarriers.recordCommands(m_cmd);

    // Initialize the image if the entire subresource is covered
    VkImageLayout dstImageLayoutInitial  = dstImage->info().layout;
    VkImageLayout dstImageLayoutTransfer = dstImage->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    if (dstImage->isFullSubresource(dstSubresource, dstExtent))
      dstImageLayoutInitial = VK_IMAGE_LAYOUT_UNDEFINED;

    if (dstImageLayoutTransfer != dstImageLayoutInitial) {
      m_execAcquires.accessImage(
        dstImage, dstSubresourceRange,
        dstImageLayoutInitial,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
        dstImageLayoutTransfer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT);
    }
      
    m_execAcquires.recordCommands(m_cmd);

    this->copyImageBufferData<true>(DxvkCmdBuffer::ExecBuffer, dstImage, dstSubresource,
      dstOffset, dstExtent, dstImageLayoutTransfer, srcSlice, rowAlignment, sliceAlignment);

    m_execBarriers.accessImage(
      dstImage, dstSubresourceRange,
      dstImageLayoutTransfer,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      dstImage->info().layout,
      dstImage->info().stages,
      dstImage->info().access);

    m_execBarriers.accessBuffer(srcSlice,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_READ_BIT,
      srcBuffer->info().stages,
      srcBuffer->info().access);
    
    m_cmd->trackResource<DxvkAccess::Write>(dstImage);
    m_cmd->trackResource<DxvkAccess::Read>(srcBuffer);
  }
  
  
  void DxvkContext::copyImage(
    const Rc<DxvkImage>&        dstImage,
          VkImageSubresourceLayers dstSubresource,
          VkOffset3D            dstOffset,
    const Rc<DxvkImage>&        srcImage,
          VkImageSubresourceLayers srcSubresource,
          VkOffset3D            srcOffset,
          VkExtent3D            extent) {
    this->spillRenderPass(true);

    if (this->copyImageClear(dstImage, dstSubresource, dstOffset, extent, srcImage, srcSubresource))
      return;

    this->prepareImage(m_execBarriers, dstImage, vk::makeSubresourceRange(dstSubresource));
    this->prepareImage(m_execBarriers, srcImage, vk::makeSubresourceRange(srcSubresource));

    bool useFb = dstSubresource.aspectMask != srcSubresource.aspectMask;

    if (m_device->perfHints().preferFbDepthStencilCopy) {
      useFb |= (dstSubresource.aspectMask == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
            && (dstImage->info().usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
            && (srcImage->info().usage & VK_IMAGE_USAGE_SAMPLED_BIT);
    }

    if (!useFb) {
      this->copyImageHw(
        dstImage, dstSubresource, dstOffset,
        srcImage, srcSubresource, srcOffset,
        extent);
    } else {
      this->copyImageFb(
        dstImage, dstSubresource, dstOffset,
        srcImage, srcSubresource, srcOffset,
        extent);
    }
  }
  
  
  void DxvkContext::copyImageRegion(
    const Rc<DxvkImage>&        dstImage,
          VkImageSubresourceLayers dstSubresource,
          VkOffset3D            dstOffset,
          VkOffset3D            srcOffset,
          VkExtent3D            extent) {
    VkOffset3D loOvl = {
      std::max(dstOffset.x, srcOffset.x),
      std::max(dstOffset.y, srcOffset.y),
      std::max(dstOffset.z, srcOffset.z) };
    
    VkOffset3D hiOvl = {
      std::min(dstOffset.x, srcOffset.x) + int32_t(extent.width),
      std::min(dstOffset.y, srcOffset.y) + int32_t(extent.height),
      std::min(dstOffset.z, srcOffset.z) + int32_t(extent.depth) };
    
    bool overlap = hiOvl.x > loOvl.x
                && hiOvl.y > loOvl.y
                && hiOvl.z > loOvl.z;
    
    if (overlap) {
      DxvkImageCreateInfo imgInfo;
      imgInfo.type          = dstImage->info().type;
      imgInfo.format        = dstImage->info().format;
      imgInfo.flags         = 0;
      imgInfo.sampleCount   = dstImage->info().sampleCount;
      imgInfo.extent        = extent;
      imgInfo.numLayers     = dstSubresource.layerCount;
      imgInfo.mipLevels     = 1;
      imgInfo.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT
                            | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
      imgInfo.stages        = VK_PIPELINE_STAGE_TRANSFER_BIT;
      imgInfo.access        = VK_ACCESS_TRANSFER_WRITE_BIT
                            | VK_ACCESS_TRANSFER_READ_BIT;
      imgInfo.tiling        = dstImage->info().tiling;
      imgInfo.layout        = VK_IMAGE_LAYOUT_GENERAL;

      auto tmpImage = m_device->createImage(
        imgInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      
      VkImageSubresourceLayers tmpSubresource;
      tmpSubresource.aspectMask     = dstSubresource.aspectMask;
      tmpSubresource.mipLevel       = 0;
      tmpSubresource.baseArrayLayer = 0;
      tmpSubresource.layerCount     = dstSubresource.layerCount;

      VkOffset3D tmpOffset = { 0, 0, 0 };

      this->copyImage(
        tmpImage, tmpSubresource, tmpOffset,
        dstImage, dstSubresource, srcOffset,
        extent);
      
      this->copyImage(
        dstImage, dstSubresource, dstOffset,
        tmpImage, tmpSubresource, tmpOffset,
        extent);
    } else {
      this->copyImage(
        dstImage, dstSubresource, dstOffset,
        dstImage, dstSubresource, srcOffset,
        extent);
    }
  }


  void DxvkContext::copyImageToBuffer(
    const Rc<DxvkBuffer>&       dstBuffer,
          VkDeviceSize          dstOffset,
          VkDeviceSize          rowAlignment,
          VkDeviceSize          sliceAlignment,
    const Rc<DxvkImage>&        srcImage,
          VkImageSubresourceLayers srcSubresource,
          VkOffset3D            srcOffset,
          VkExtent3D            srcExtent) {
    this->spillRenderPass(true);
    this->prepareImage(m_execBarriers, srcImage, vk::makeSubresourceRange(srcSubresource));

    auto dstSlice = dstBuffer->getSliceHandle(dstOffset, 0);

    // We may copy to only one aspect of a depth-stencil image,
    // but pipeline barriers need to have all aspect bits set
    auto srcFormatInfo = srcImage->formatInfo();

    auto srcSubresourceRange = vk::makeSubresourceRange(srcSubresource);
    srcSubresourceRange.aspectMask = srcFormatInfo->aspectMask;
    
    if (m_execBarriers.isImageDirty(srcImage, srcSubresourceRange, DxvkAccess::Write)
     || m_execBarriers.isBufferDirty(dstSlice, DxvkAccess::Write))
      m_execBarriers.recordCommands(m_cmd);

    // Select a suitable image layout for the transfer op
    VkImageLayout srcImageLayoutTransfer = srcImage->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    
    m_execAcquires.accessImage(
      srcImage, srcSubresourceRange,
      srcImage->info().layout,
      VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
      srcImageLayoutTransfer,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_READ_BIT);

    m_execAcquires.recordCommands(m_cmd);
    
    this->copyImageBufferData<false>(DxvkCmdBuffer::ExecBuffer, srcImage, srcSubresource,
      srcOffset, srcExtent, srcImageLayoutTransfer, dstSlice, rowAlignment, sliceAlignment);
    
    m_execBarriers.accessImage(
      srcImage, srcSubresourceRange,
      srcImageLayoutTransfer,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_READ_BIT,
      srcImage->info().layout,
      srcImage->info().stages,
      srcImage->info().access);

    m_execBarriers.accessBuffer(dstSlice,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      dstBuffer->info().stages,
      dstBuffer->info().access);
    
    m_cmd->trackResource<DxvkAccess::Write>(dstBuffer);
    m_cmd->trackResource<DxvkAccess::Read>(srcImage);
  }


  void DxvkContext::copyDepthStencilImageToPackedBuffer(
    const Rc<DxvkBuffer>&       dstBuffer,
          VkDeviceSize          dstBufferOffset,
          VkOffset2D            dstOffset,
          VkExtent2D            dstExtent,
    const Rc<DxvkImage>&        srcImage,
          VkImageSubresourceLayers srcSubresource,
          VkOffset2D            srcOffset,
          VkExtent2D            srcExtent,
          VkFormat              format) {
    this->spillRenderPass(true);
    this->prepareImage(m_execBarriers, srcImage, vk::makeSubresourceRange(srcSubresource));

    this->unbindComputePipeline();

    // Retrieve compute pipeline for the given format
    auto pipeInfo = m_common->metaPack().getPackPipeline(format);

    if (!pipeInfo.pipeHandle)
      return;
    
    // Create one depth view and one stencil view
    DxvkImageViewCreateInfo dViewInfo;
    dViewInfo.type       = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    dViewInfo.format     = srcImage->info().format;
    dViewInfo.usage      = VK_IMAGE_USAGE_SAMPLED_BIT;
    dViewInfo.aspect     = VK_IMAGE_ASPECT_DEPTH_BIT;
    dViewInfo.minLevel   = srcSubresource.mipLevel;
    dViewInfo.numLevels  = 1;
    dViewInfo.minLayer   = srcSubresource.baseArrayLayer;
    dViewInfo.numLayers  = srcSubresource.layerCount;

    DxvkImageViewCreateInfo sViewInfo = dViewInfo;
    sViewInfo.aspect     = VK_IMAGE_ASPECT_STENCIL_BIT;
    
    Rc<DxvkImageView> dView = m_device->createImageView(srcImage, dViewInfo);
    Rc<DxvkImageView> sView = m_device->createImageView(srcImage, sViewInfo);

    // Create a descriptor set for the pack operation
    VkImageLayout layout = srcImage->pickLayout(VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);

    DxvkMetaPackDescriptors descriptors;
    descriptors.dstBuffer  = dstBuffer->getDescriptor(dstBufferOffset, VK_WHOLE_SIZE).buffer;
    descriptors.srcDepth   = dView->getDescriptor(VK_IMAGE_VIEW_TYPE_2D_ARRAY, layout).image;
    descriptors.srcStencil = sView->getDescriptor(VK_IMAGE_VIEW_TYPE_2D_ARRAY, layout).image;

    VkDescriptorSet dset = allocateDescriptorSet(pipeInfo.dsetLayout);
    m_cmd->updateDescriptorSetWithTemplate(dset, pipeInfo.dsetTemplate, &descriptors);

    // Since this is a meta operation, the image may be
    // in a different layout and we have to transition it
    auto subresourceRange = vk::makeSubresourceRange(srcSubresource);

    if (m_execBarriers.isImageDirty(srcImage, subresourceRange, DxvkAccess::Write))
      m_execBarriers.recordCommands(m_cmd);
    
    if (srcImage->info().layout != layout) {
      m_execAcquires.accessImage(
        srcImage, subresourceRange,
        srcImage->info().layout,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
        layout,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT);
      
      m_execAcquires.recordCommands(m_cmd);
    }

    // Execute the actual pack operation
    DxvkMetaPackArgs args;
    args.srcOffset = srcOffset;
    args.srcExtent = srcExtent;
    args.dstOffset = dstOffset;
    args.dstExtent = dstExtent;

    m_cmd->cmdBindPipeline(
      VK_PIPELINE_BIND_POINT_COMPUTE,
      pipeInfo.pipeHandle);
    
    m_cmd->cmdBindDescriptorSet(
      VK_PIPELINE_BIND_POINT_COMPUTE,
      pipeInfo.pipeLayout, dset,
      0, nullptr);
    
    m_cmd->cmdPushConstants(
      pipeInfo.pipeLayout,
      VK_SHADER_STAGE_COMPUTE_BIT,
      0, sizeof(args), &args);
    
    m_cmd->cmdDispatch(
      (srcExtent.width  + 7) / 8,
      (srcExtent.height + 7) / 8,
      srcSubresource.layerCount);
    
    m_execBarriers.accessImage(
      srcImage, subresourceRange, layout,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_READ_BIT,
      srcImage->info().layout,
      srcImage->info().stages,
      srcImage->info().access);
    
    m_execBarriers.accessBuffer(
      dstBuffer->getSliceHandle(),
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_WRITE_BIT,
      dstBuffer->info().stages,
      dstBuffer->info().access);

    m_cmd->trackResource<DxvkAccess::None>(dView);
    m_cmd->trackResource<DxvkAccess::None>(sView);

    m_cmd->trackResource<DxvkAccess::Write>(dstBuffer);
    m_cmd->trackResource<DxvkAccess::Read>(srcImage);
  }
  
  
  void DxvkContext::copyPackedBufferImage(
    const Rc<DxvkBuffer>&       dstBuffer,
          VkDeviceSize          dstBufferOffset,
          VkOffset3D            dstOffset,
          VkExtent3D            dstSize,
    const Rc<DxvkBuffer>&       srcBuffer,
          VkDeviceSize          srcBufferOffset,
          VkOffset3D            srcOffset,
          VkExtent3D            srcSize,
          VkExtent3D            extent,
          VkDeviceSize          elementSize) {
    this->spillRenderPass(true);
    this->unbindComputePipeline();

    auto dstBufferSlice = dstBuffer->getSliceHandle(dstBufferOffset, elementSize * util::flattenImageExtent(dstSize));
    auto srcBufferSlice = srcBuffer->getSliceHandle(srcBufferOffset, elementSize * util::flattenImageExtent(srcSize));

    if (m_execBarriers.isBufferDirty(dstBufferSlice, DxvkAccess::Write)
     || m_execBarriers.isBufferDirty(srcBufferSlice, DxvkAccess::Read))
      m_execBarriers.recordCommands(m_cmd);

    // We'll use texel buffer views with an appropriately
    // sized integer format to perform the copy
    VkFormat format = VK_FORMAT_UNDEFINED;

    switch (elementSize) {
      case  1: format = VK_FORMAT_R8_UINT; break;
      case  2: format = VK_FORMAT_R16_UINT; break;
      case  4: format = VK_FORMAT_R32_UINT; break;
      case  8: format = VK_FORMAT_R32G32_UINT; break;
      case 12: format = VK_FORMAT_R32G32B32_UINT; break;
      case 16: format = VK_FORMAT_R32G32B32A32_UINT; break;
    }

    if (!format) {
      Logger::err(str::format("DxvkContext: copyPackedBufferImage: Unsupported element size ", elementSize));
      return;
    }

    DxvkBufferViewCreateInfo viewInfo;
    viewInfo.format = format;
    viewInfo.rangeOffset = dstBufferOffset;
    viewInfo.rangeLength = dstBufferSlice.length;
    Rc<DxvkBufferView> dstView = m_device->createBufferView(dstBuffer, viewInfo);

    viewInfo.rangeOffset = srcBufferOffset;
    viewInfo.rangeLength = srcBufferSlice.length;
    Rc<DxvkBufferView> srcView;

    if (srcBuffer == dstBuffer
     && srcBufferSlice.offset < dstBufferSlice.offset + dstBufferSlice.length
     && srcBufferSlice.offset + srcBufferSlice.length > dstBufferSlice.offset) {
      // Create temporary copy in case of overlapping regions
      DxvkBufferCreateInfo bufferInfo;
      bufferInfo.size   = srcBufferSlice.length;
      bufferInfo.usage  = VK_BUFFER_USAGE_TRANSFER_DST_BIT
                        | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
      bufferInfo.stages = VK_PIPELINE_STAGE_TRANSFER_BIT
                        | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      bufferInfo.access = VK_ACCESS_TRANSFER_WRITE_BIT
                        | VK_ACCESS_SHADER_READ_BIT;
      Rc<DxvkBuffer> tmpBuffer = m_device->createBuffer(bufferInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

      auto tmpBufferSlice = tmpBuffer->getSliceHandle();

      VkBufferCopy copyRegion;
      copyRegion.srcOffset = srcBufferSlice.offset;
      copyRegion.dstOffset = tmpBufferSlice.offset;
      copyRegion.size = tmpBufferSlice.length;

      m_cmd->cmdCopyBuffer(DxvkCmdBuffer::ExecBuffer,
        srcBufferSlice.handle, tmpBufferSlice.handle,
        1, &copyRegion);

      emitMemoryBarrier(0,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT);

      viewInfo.rangeOffset = 0;
      srcView = m_device->createBufferView(tmpBuffer, viewInfo);

      m_cmd->trackResource<DxvkAccess::Write>(tmpBuffer);
    } else {
      srcView = m_device->createBufferView(srcBuffer, viewInfo);
    }

    auto pipeInfo = m_common->metaCopy().getCopyBufferImagePipeline();
    VkDescriptorSet descriptorSet = allocateDescriptorSet(pipeInfo.dsetLayout);

    std::array<VkWriteDescriptorSet, 2> descriptorWrites;

    std::array<std::pair<VkDescriptorType, VkBufferView>, 2> descriptorInfos = {{
      { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, dstView->handle() },
      { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, srcView->handle() },
    }};

    for (uint32_t i = 0; i < descriptorWrites.size(); i++) {
      auto write = &descriptorWrites[i];
      auto info = &descriptorInfos[i];

      write->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write->pNext = nullptr;
      write->dstSet = descriptorSet;
      write->dstBinding = i;
      write->dstArrayElement = 0;
      write->descriptorCount = 1;
      write->descriptorType = info->first;
      write->pImageInfo = nullptr;
      write->pBufferInfo = nullptr;
      write->pTexelBufferView = &info->second;
    }

    m_cmd->updateDescriptorSets(descriptorWrites.size(), descriptorWrites.data());

    DxvkCopyBufferImageArgs args = { };
    args.dstOffset = dstOffset;
    args.srcOffset = srcOffset;
    args.extent = extent;
    args.dstSize = { dstSize.width, dstSize.height };
    args.srcSize = { srcSize.width, srcSize.height };

    m_cmd->cmdBindPipeline(
      VK_PIPELINE_BIND_POINT_COMPUTE,
      pipeInfo.pipeHandle);
    
    m_cmd->cmdBindDescriptorSet(
      VK_PIPELINE_BIND_POINT_COMPUTE,
      pipeInfo.pipeLayout, descriptorSet,
      0, nullptr);
    
    m_cmd->cmdPushConstants(
      pipeInfo.pipeLayout,
      VK_SHADER_STAGE_COMPUTE_BIT,
      0, sizeof(args), &args);
    
    m_cmd->cmdDispatch(
      (extent.width  + 7) / 8,
      (extent.height + 7) / 8,
      extent.depth);
    
    m_execBarriers.accessBuffer(
      dstView->getSliceHandle(),
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_WRITE_BIT,
      dstBuffer->info().stages,
      dstBuffer->info().access);

    m_execBarriers.accessBuffer(
      srcView->getSliceHandle(),
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_READ_BIT,
      srcBuffer->info().stages,
      srcBuffer->info().access);

    // Track all involved resources
    m_cmd->trackResource<DxvkAccess::Write>(dstBuffer);
    m_cmd->trackResource<DxvkAccess::Read>(srcBuffer);

    m_cmd->trackResource<DxvkAccess::None>(dstView);
    m_cmd->trackResource<DxvkAccess::None>(srcView);
  }


  void DxvkContext::copyPackedBufferToDepthStencilImage(
    const Rc<DxvkImage>&        dstImage,
          VkImageSubresourceLayers dstSubresource,
          VkOffset2D            dstOffset,
          VkExtent2D            dstExtent,
    const Rc<DxvkBuffer>&       srcBuffer,
          VkDeviceSize          srcBufferOffset,
          VkOffset2D            srcOffset,
          VkExtent2D            srcExtent,
          VkFormat              format) {
    this->spillRenderPass(true);
    this->prepareImage(m_execBarriers, dstImage, vk::makeSubresourceRange(dstSubresource));

    this->unbindComputePipeline();

    if (m_execBarriers.isBufferDirty(srcBuffer->getSliceHandle(), DxvkAccess::Read)
     || m_execBarriers.isImageDirty(dstImage, vk::makeSubresourceRange(dstSubresource), DxvkAccess::Write))
      m_execBarriers.recordCommands(m_cmd);
    
    // Retrieve compute pipeline for the given format
    auto pipeInfo = m_common->metaPack().getUnpackPipeline(dstImage->info().format, format);

    if (!pipeInfo.pipeHandle) {
      Logger::err(str::format(
        "DxvkContext: copyPackedBufferToDepthStencilImage: Unhandled formats"
        "\n  dstFormat = ", dstImage->info().format,
        "\n  srcFormat = ", format));
      return;
    }
    
    // Pick depth and stencil data formats
    VkFormat dataFormatD = VK_FORMAT_UNDEFINED;
    VkFormat dataFormatS = VK_FORMAT_UNDEFINED;

    const std::array<std::tuple<VkFormat, VkFormat, VkFormat>, 2> formats = {{
      { VK_FORMAT_D24_UNORM_S8_UINT,  VK_FORMAT_R32_UINT,   VK_FORMAT_R8_UINT },
      { VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R8_UINT },
    }};

    for (const auto& e : formats) {
      if (std::get<0>(e) == dstImage->info().format) {
        dataFormatD = std::get<1>(e);
        dataFormatS = std::get<2>(e);
      }
    }

    // Create temporary buffer for depth/stencil data
    VkDeviceSize pixelCount = dstExtent.width * dstExtent.height * dstSubresource.layerCount;
    VkDeviceSize dataSizeD = align(pixelCount * imageFormatInfo(dataFormatD)->elementSize, 256);
    VkDeviceSize dataSizeS = align(pixelCount * imageFormatInfo(dataFormatS)->elementSize, 256);

    DxvkBufferCreateInfo tmpBufferInfo;
    tmpBufferInfo.size    = dataSizeD + dataSizeS;
    tmpBufferInfo.usage   = VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT
                          | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    tmpBufferInfo.stages  = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                          | VK_PIPELINE_STAGE_TRANSFER_BIT;
    tmpBufferInfo.access  = VK_ACCESS_SHADER_WRITE_BIT
                          | VK_ACCESS_TRANSFER_READ_BIT;
    
    auto tmpBuffer = m_device->createBuffer(tmpBufferInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    // Create formatted buffer views
    DxvkBufferViewCreateInfo tmpViewInfoD;
    tmpViewInfoD.format      = dataFormatD;
    tmpViewInfoD.rangeOffset = 0;
    tmpViewInfoD.rangeLength = dataSizeD;

    DxvkBufferViewCreateInfo tmpViewInfoS;
    tmpViewInfoS.format      = dataFormatS;
    tmpViewInfoS.rangeOffset = dataSizeD;
    tmpViewInfoS.rangeLength = dataSizeS;

    auto tmpBufferViewD = m_device->createBufferView(tmpBuffer, tmpViewInfoD);
    auto tmpBufferViewS = m_device->createBufferView(tmpBuffer, tmpViewInfoS);

    // Create descriptor set for the unpack operation
    DxvkMetaUnpackDescriptors descriptors;
    descriptors.dstDepth   = tmpBufferViewD->handle();
    descriptors.dstStencil = tmpBufferViewS->handle();
    descriptors.srcBuffer  = srcBuffer->getDescriptor(srcBufferOffset, VK_WHOLE_SIZE).buffer;

    VkDescriptorSet dset = allocateDescriptorSet(pipeInfo.dsetLayout);
    m_cmd->updateDescriptorSetWithTemplate(dset, pipeInfo.dsetTemplate, &descriptors);

    // Unpack the source buffer to temporary buffers
    DxvkMetaPackArgs args;
    args.srcOffset = srcOffset;
    args.srcExtent = srcExtent;
    args.dstOffset = VkOffset2D { 0, 0 };
    args.dstExtent = dstExtent;

    m_cmd->cmdBindPipeline(
      VK_PIPELINE_BIND_POINT_COMPUTE,
      pipeInfo.pipeHandle);
    
    m_cmd->cmdBindDescriptorSet(
      VK_PIPELINE_BIND_POINT_COMPUTE,
      pipeInfo.pipeLayout, dset,
      0, nullptr);
    
    m_cmd->cmdPushConstants(
      pipeInfo.pipeLayout,
      VK_SHADER_STAGE_COMPUTE_BIT,
      0, sizeof(args), &args);
    
    m_cmd->cmdDispatch(
      (dstExtent.width + 63) / 64,
      dstExtent.height,
      dstSubresource.layerCount);
    
    m_execBarriers.accessBuffer(
      tmpBuffer->getSliceHandle(),
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_WRITE_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_READ_BIT);

    m_execBarriers.accessBuffer(
      srcBuffer->getSliceHandle(),
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_READ_BIT,
      srcBuffer->info().stages,
      srcBuffer->info().access);
    
    // Prepare image for the data transfer operation
    VkOffset3D dstOffset3D = { dstOffset.x,     dstOffset.y,      0 };
    VkExtent3D dstExtent3D = { dstExtent.width, dstExtent.height, 1 };

    VkImageLayout initialImageLayout = dstImage->info().layout;

    if (dstImage->isFullSubresource(dstSubresource, dstExtent3D))
      initialImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    m_execBarriers.accessImage(
      dstImage, vk::makeSubresourceRange(dstSubresource),
      initialImageLayout,
      dstImage->info().stages,
      dstImage->info().access,
      dstImage->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT);

    m_execBarriers.recordCommands(m_cmd);

    // Copy temporary buffer data to depth-stencil image
    VkImageSubresourceLayers dstSubresourceD = dstSubresource;
    dstSubresourceD.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;

    VkImageSubresourceLayers dstSubresourceS = dstSubresource;
    dstSubresourceS.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;

    std::array<VkBufferImageCopy, 2> copyRegions = {{
      { tmpBufferViewD->info().rangeOffset, 0, 0, dstSubresourceD, dstOffset3D, dstExtent3D },
      { tmpBufferViewS->info().rangeOffset, 0, 0, dstSubresourceS, dstOffset3D, dstExtent3D },
    }};

    m_cmd->cmdCopyBufferToImage(DxvkCmdBuffer::ExecBuffer,
      tmpBuffer->getSliceHandle().handle,
      dstImage->handle(),
      dstImage->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
      copyRegions.size(),
      copyRegions.data());
    
    m_execBarriers.accessImage(
      dstImage, vk::makeSubresourceRange(dstSubresource),
      dstImage->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      dstImage->info().layout,
      dstImage->info().stages,
      dstImage->info().access);

    // Track all involved resources
    m_cmd->trackResource<DxvkAccess::Write>(dstImage);
    m_cmd->trackResource<DxvkAccess::Read>(srcBuffer);

    m_cmd->trackResource<DxvkAccess::None>(tmpBufferViewD);
    m_cmd->trackResource<DxvkAccess::None>(tmpBufferViewS);
  }


  void DxvkContext::discardBuffer(
    const Rc<DxvkBuffer>&       buffer) {
    if (buffer->memFlags() & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
      return;

    if (m_execBarriers.isBufferDirty(buffer->getSliceHandle(), DxvkAccess::Write))
      this->invalidateBuffer(buffer, buffer->allocSlice());
  }


  void DxvkContext::discardImageView(
    const Rc<DxvkImageView>&      imageView,
          VkImageAspectFlags      discardAspects) {
    VkImageUsageFlags viewUsage = imageView->info().usage;

    // Ignore non-render target views since there's likely no good use case for
    // discarding those. Also, force reinitialization even if the image is bound
    // as a render target, which may have niche use cases for depth buffers.
    if (viewUsage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
      this->spillRenderPass(true);
      this->deferDiscard(imageView, discardAspects);
    }
  }


  void DxvkContext::dispatch(
          uint32_t x,
          uint32_t y,
          uint32_t z) {
    winehuaFlowTrace(str::format(
      "dispatch request x=", x,
      " y=", y,
      " z=", z));

    if (this->commitComputeState()) {
      this->commitComputeInitBarriers();

      m_queryManager.beginQueries(m_cmd,
        VK_QUERY_TYPE_PIPELINE_STATISTICS);
      
      m_cmd->cmdDispatch(x, y, z);
      
      m_queryManager.endQueries(m_cmd,
        VK_QUERY_TYPE_PIPELINE_STATISTICS);
      
      this->commitComputePostBarriers();
    }
    
    m_cmd->addStatCtr(DxvkStatCounter::CmdDispatchCalls, 1);
  }
  
  
  void DxvkContext::dispatchIndirect(
          VkDeviceSize      offset) {
    auto bufferSlice = m_state.id.argBuffer.getSliceHandle(
      offset, sizeof(VkDispatchIndirectCommand));

    if (m_execBarriers.isBufferDirty(bufferSlice, DxvkAccess::Read))
      m_execBarriers.recordCommands(m_cmd);
    
    if (this->commitComputeState()) {
      this->commitComputeInitBarriers();

      m_queryManager.beginQueries(m_cmd,
        VK_QUERY_TYPE_PIPELINE_STATISTICS);
      
      m_cmd->cmdDispatchIndirect(
        bufferSlice.handle,
        bufferSlice.offset);
      
      m_queryManager.endQueries(m_cmd,
        VK_QUERY_TYPE_PIPELINE_STATISTICS);
      
      this->commitComputePostBarriers();

      m_execBarriers.accessBuffer(bufferSlice,
        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
        VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
        m_state.id.argBuffer.bufferInfo().stages,
        m_state.id.argBuffer.bufferInfo().access);
      
      this->trackDrawBuffer();
    }
    
    m_cmd->addStatCtr(DxvkStatCounter::CmdDispatchCalls, 1);
  }
  
  
  void DxvkContext::draw(
          uint32_t vertexCount,
          uint32_t instanceCount,
          uint32_t firstVertex,
          uint32_t firstInstance) {
    winehuaFlowTrace(str::format(
      "draw request vertices=", vertexCount,
      " instances=", instanceCount));

    if (this->commitGraphicsState<false, false>()) {
      if (unlikely(winehuaDrawTraceEnabled() || winehuaCameraTraceEnabled()))
        this->winehuaTraceDraw("draw", str::format(
          "vertexCount=", vertexCount,
          " instanceCount=", instanceCount,
          " firstVertex=", firstVertex,
          " firstInstance=", firstInstance));

      const auto& blend = m_state.gp.state.omBlend[0];
      bool singleColorTarget = m_state.om.renderTargets.color[0].view != nullptr;
      for (uint32_t i = 1; i < MaxNumRenderTargets; i++)
        singleColorTarget &= m_state.om.renderTargets.color[i].view == nullptr;

      const bool emulateDualSource =
        !m_device->features().core.features.dualSrcBlend
        && singleColorTarget
        && m_state.gp.shaders.fs != nullptr
        && (m_state.gp.shaders.fs->info().outputMask & 0x2u)
        && !m_state.gp.flags.any(
          DxvkGraphicsPipelineFlag::HasTransformFeedback,
          DxvkGraphicsPipelineFlag::HasStorageDescriptors)
        && !m_state.gp.state.om.enableLogicOp()
        && !m_state.gp.state.ds.enableDepthTest()
        && !m_state.gp.state.ds.enableStencilTest()
        && blend.blendEnable()
        && blend.srcColorBlendFactor() == VK_BLEND_FACTOR_ONE
        && blend.dstColorBlendFactor() == VK_BLEND_FACTOR_SRC1_COLOR
        && blend.colorBlendOp() == VK_BLEND_OP_ADD
        && blend.srcAlphaBlendFactor() == VK_BLEND_FACTOR_ONE
        && blend.dstAlphaBlendFactor() == VK_BLEND_FACTOR_SRC1_ALPHA
        && blend.alphaBlendOp() == VK_BLEND_OP_ADD;

      bool emulated = false;
      if (unlikely(emulateDualSource)) {
        const WineHuaDualSrcMode dualSrcMode = winehuaDualSrcMode();
        const bool runSecondary = dualSrcMode != WineHuaDualSrcMode::PrimaryReplace
                               && dualSrcMode != WineHuaDualSrcMode::PrimaryAdd;
        const bool runPrimary = dualSrcMode != WineHuaDualSrcMode::SecondaryReplace
                             && dualSrcMode != WineHuaDualSrcMode::SecondaryMultiply;
        const bool replaceSecondary =
          dualSrcMode == WineHuaDualSrcMode::SecondaryReplace;
        const bool replacePrimary =
          dualSrcMode == WineHuaDualSrcMode::PrimaryReplace;
        const VkColorComponentFlags rgbWriteMask = blend.colorWriteMask()
          & (VK_COLOR_COMPONENT_R_BIT
           | VK_COLOR_COMPONENT_G_BIT
           | VK_COLOR_COMPONENT_B_BIT);

        if (rgbWriteMask) {
          DxvkGraphicsPipelineStateInfo secondaryState = m_state.gp.state;
          DxvkGraphicsPipelineStateInfo primaryState = m_state.gp.state;

          secondaryState.ds = DxvkDsInfo(
            m_state.gp.state.ds.enableDepthTest(),
            VK_FALSE,
            m_state.gp.state.ds.enableDepthBoundsTest(),
            m_state.gp.state.ds.enableStencilTest(),
            m_state.gp.state.ds.depthCompareOp());
          secondaryState.omBlend[0] = DxvkOmAttachmentBlend(
            VK_TRUE,
            replaceSecondary ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO,
            replaceSecondary ? VK_BLEND_FACTOR_ZERO : VK_BLEND_FACTOR_SRC_COLOR,
            VK_BLEND_OP_ADD,
            VK_BLEND_FACTOR_ZERO,
            VK_BLEND_FACTOR_ONE,
            VK_BLEND_OP_ADD,
            rgbWriteMask);
          primaryState.omBlend[0] = DxvkOmAttachmentBlend(
            VK_TRUE,
            VK_BLEND_FACTOR_ONE,
            replacePrimary ? VK_BLEND_FACTOR_ZERO : VK_BLEND_FACTOR_ONE,
            VK_BLEND_OP_ADD,
            VK_BLEND_FACTOR_ZERO,
            VK_BLEND_FACTOR_ONE,
            VK_BLEND_OP_ADD,
            rgbWriteMask);

          const DxvkRenderPass* renderPass =
            m_state.om.framebufferInfo.renderPass();
          const VkPipeline secondaryPipeline = runSecondary
            ? m_state.gp.pipeline->getPipelineHandle(
                secondaryState, renderPass, true)
            : VK_NULL_HANDLE;
          const VkPipeline primaryPipeline = runPrimary
            ? m_state.gp.pipeline->getPipelineHandle(
                primaryState, renderPass, false)
            : VK_NULL_HANDLE;

          if ((!runSecondary || secondaryPipeline)
           && (!runPrimary || primaryPipeline)) {
            static std::atomic<uint32_t> logged { 0 };
            if (logged.fetch_add(1, std::memory_order_relaxed) == 0) {
              Logger::info(str::format(
                "WineHuaDualSrcEmulation: mode=",
                winehuaDualSrcModeName(dualSrcMode), " fs=",
                m_state.gp.shaders.fs->debugName()));
            }

            if (runSecondary) {
              m_cmd->cmdBindPipeline(
                VK_PIPELINE_BIND_POINT_GRAPHICS, secondaryPipeline);
              m_cmd->cmdDraw(
                vertexCount, instanceCount,
                firstVertex, firstInstance);
            }
            if (runPrimary) {
              m_cmd->cmdBindPipeline(
                VK_PIPELINE_BIND_POINT_GRAPHICS, primaryPipeline);
              m_cmd->cmdDraw(
                vertexCount, instanceCount,
                firstVertex, firstInstance);
            }
            emulated = true;

            m_gpActivePipeline = VK_NULL_HANDLE;
            m_flags.set(DxvkContextFlag::GpDirtyPipelineState);
          }
        }
      }

      if (!emulated) {
        m_cmd->cmdDraw(
          vertexCount, instanceCount,
          firstVertex, firstInstance);
      }
      if (unlikely(winehuaTargetDrawCaptureEnabled()))
        this->winehuaCaptureTargetDraw(false, vertexCount, firstVertex, 0,
          instanceCount, firstInstance);
    }
    
    m_cmd->addStatCtr(DxvkStatCounter::CmdDrawCalls, 1);
  }
  
  
  void DxvkContext::drawIndirect(
          VkDeviceSize      offset,
          uint32_t          count,
          uint32_t          stride) {
    if (this->commitGraphicsState<false, true>()) {
      auto descriptor = m_state.id.argBuffer.getDescriptor();

      if (unlikely(winehuaDrawTraceEnabled() || winehuaCameraTraceEnabled()))
        this->winehuaTraceDraw("drawIndirect", str::format(
          "buffer=0x", std::hex, descriptor.buffer.buffer,
          " offset=", std::dec, descriptor.buffer.offset + offset,
          " count=", count,
          " stride=", stride));
      
      m_cmd->cmdDrawIndirect(
        descriptor.buffer.buffer,
        descriptor.buffer.offset + offset,
        count, stride);
      if (unlikely(winehuaTargetDrawCaptureEnabled()))
        this->winehuaCaptureTargetDraw(false, 0, 0, 0, 0, 0);
    }
    
    m_cmd->addStatCtr(DxvkStatCounter::CmdDrawCalls, 1);
  }
  
  
  void DxvkContext::drawIndirectCount(
          VkDeviceSize      offset,
          VkDeviceSize      countOffset,
          uint32_t          maxCount,
          uint32_t          stride) {
    if (this->commitGraphicsState<false, true>()) {
      auto argDescriptor = m_state.id.argBuffer.getDescriptor();
      auto cntDescriptor = m_state.id.cntBuffer.getDescriptor();

      if (unlikely(winehuaDrawTraceEnabled() || winehuaCameraTraceEnabled()))
        this->winehuaTraceDraw("drawIndirectCount", str::format(
          "argBuffer=0x", std::hex, argDescriptor.buffer.buffer,
          " argOffset=", std::dec, argDescriptor.buffer.offset + offset,
          " countBuffer=0x", std::hex, cntDescriptor.buffer.buffer,
          " countOffset=", std::dec, cntDescriptor.buffer.offset + countOffset,
          " maxCount=", maxCount,
          " stride=", stride));
      
      m_cmd->cmdDrawIndirectCount(
        argDescriptor.buffer.buffer,
        argDescriptor.buffer.offset + offset,
        cntDescriptor.buffer.buffer,
        cntDescriptor.buffer.offset + countOffset,
        maxCount, stride);
      if (unlikely(winehuaTargetDrawCaptureEnabled()))
        this->winehuaCaptureTargetDraw(false, 0, 0, 0, 0, 0);
    }
    
    m_cmd->addStatCtr(DxvkStatCounter::CmdDrawCalls, 1);
  }
  
  
  void DxvkContext::drawIndexed(
          uint32_t indexCount,
          uint32_t instanceCount,
          uint32_t firstIndex,
          uint32_t vertexOffset,
          uint32_t firstInstance) {
    winehuaFlowTrace(str::format(
      "draw-indexed request indices=", indexCount,
      " instances=", instanceCount));

    if (this->commitGraphicsState<true, false>()) {
      if (unlikely(winehuaDrawTraceEnabled() || winehuaCameraTraceEnabled()))
        this->winehuaTraceDraw("drawIndexed", str::format(
          "indexCount=", indexCount,
          " instanceCount=", instanceCount,
          " firstIndex=", firstIndex,
          " vertexOffset=", vertexOffset,
          " firstInstance=", firstInstance));
      m_cmd->cmdDrawIndexed(
        indexCount, instanceCount,
        firstIndex, vertexOffset,
        firstInstance);
      if (unlikely(winehuaTargetDrawCaptureEnabled()))
        this->winehuaCaptureTargetDraw(
          true, indexCount, firstIndex, int32_t(vertexOffset),
          instanceCount, firstInstance);
    }
    
    m_cmd->addStatCtr(DxvkStatCounter::CmdDrawCalls, 1);
  }
  
  
  void DxvkContext::drawIndexedIndirect(
          VkDeviceSize      offset,
          uint32_t          count,
          uint32_t          stride) {
    if (this->commitGraphicsState<true, true>()) {
      auto descriptor = m_state.id.argBuffer.getDescriptor();

      if (unlikely(winehuaDrawTraceEnabled() || winehuaCameraTraceEnabled()))
        this->winehuaTraceDraw("drawIndexedIndirect", str::format(
          "buffer=0x", std::hex, descriptor.buffer.buffer,
          " offset=", std::dec, descriptor.buffer.offset + offset,
          " count=", count,
          " stride=", stride));
      
      m_cmd->cmdDrawIndexedIndirect(
        descriptor.buffer.buffer,
        descriptor.buffer.offset + offset,
        count, stride);
      if (unlikely(winehuaTargetDrawCaptureEnabled()))
        this->winehuaCaptureTargetDraw(true, 0, 0, 0, 0, 0);
    }
    
    m_cmd->addStatCtr(DxvkStatCounter::CmdDrawCalls, 1);
  }
  
  
  void DxvkContext::drawIndexedIndirectCount(
          VkDeviceSize      offset,
          VkDeviceSize      countOffset,
          uint32_t          maxCount,
          uint32_t          stride) {
    if (this->commitGraphicsState<true, true>()) {
      auto argDescriptor = m_state.id.argBuffer.getDescriptor();
      auto cntDescriptor = m_state.id.cntBuffer.getDescriptor();

      if (unlikely(winehuaDrawTraceEnabled() || winehuaCameraTraceEnabled()))
        this->winehuaTraceDraw("drawIndexedIndirectCount", str::format(
          "argBuffer=0x", std::hex, argDescriptor.buffer.buffer,
          " argOffset=", std::dec, argDescriptor.buffer.offset + offset,
          " countBuffer=0x", std::hex, cntDescriptor.buffer.buffer,
          " countOffset=", std::dec, cntDescriptor.buffer.offset + countOffset,
          " maxCount=", maxCount,
          " stride=", stride));
      
      m_cmd->cmdDrawIndexedIndirectCount(
        argDescriptor.buffer.buffer,
        argDescriptor.buffer.offset + offset,
        cntDescriptor.buffer.buffer,
        cntDescriptor.buffer.offset + countOffset,
        maxCount, stride);
      if (unlikely(winehuaTargetDrawCaptureEnabled()))
        this->winehuaCaptureTargetDraw(true, 0, 0, 0, 0, 0);
    }
    
    m_cmd->addStatCtr(DxvkStatCounter::CmdDrawCalls, 1);
  }
  
  
  void DxvkContext::drawIndirectXfb(
    const DxvkBufferSlice&  counterBuffer,
          uint32_t          counterDivisor,
          uint32_t          counterBias) {
    if (this->commitGraphicsState<false, false>()) {
      auto physSlice = counterBuffer.getSliceHandle();

      if (unlikely(winehuaDrawTraceEnabled() || winehuaCameraTraceEnabled()))
        this->winehuaTraceDraw("drawIndirectXfb", str::format(
          "counterBuffer=0x", std::hex, physSlice.handle,
          " counterOffset=", std::dec, physSlice.offset,
          " counterBias=", counterBias,
          " counterDivisor=", counterDivisor));

      m_cmd->cmdDrawIndirectVertexCount(1, 0,
        physSlice.handle,
        physSlice.offset,
        counterBias,
        counterDivisor);
      if (unlikely(winehuaTargetDrawCaptureEnabled()))
        this->winehuaCaptureTargetDraw(false, 0, 0, 0, 0, 0);
    }

    m_cmd->addStatCtr(DxvkStatCounter::CmdDrawCalls, 1);
  }


  void DxvkContext::emitRenderTargetReadbackBarrier() {
    if (m_flags.test(DxvkContextFlag::GpRenderPassBound)) {
      emitMemoryBarrier(VK_DEPENDENCY_BY_REGION_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT);
    }
  }


  void DxvkContext::initBuffer(
    const Rc<DxvkBuffer>&           buffer) {
    auto slice = buffer->getSliceHandle();

    m_cmd->cmdFillBuffer(DxvkCmdBuffer::InitBuffer,
      slice.handle, slice.offset,
      dxvk::align(slice.length, 4), 0);

    m_initBarriers.accessBuffer(slice,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      buffer->info().stages,
      buffer->info().access);

    m_cmd->trackResource<DxvkAccess::Write>(buffer);
  }


  void DxvkContext::initImage(
    const Rc<DxvkImage>&            image,
    const VkImageSubresourceRange&  subresources,
          VkImageLayout             initialLayout) {
    if (initialLayout == VK_IMAGE_LAYOUT_PREINITIALIZED) {
      m_initBarriers.accessImage(image, subresources,
        initialLayout, 0, 0,
        image->info().layout,
        image->info().stages,
        image->info().access);

      m_cmd->trackResource<DxvkAccess::None>(image);
    } else {
      VkImageLayout clearLayout = image->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

      m_execAcquires.accessImage(image, subresources,
        initialLayout, 0, 0, clearLayout,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT);
      m_execAcquires.recordCommands(m_cmd);

      auto formatInfo = image->formatInfo();

      if (formatInfo->flags.any(DxvkFormatFlag::BlockCompressed, DxvkFormatFlag::MultiPlane)) {
        for (auto aspects = formatInfo->aspectMask; aspects; ) {
          auto aspect = vk::getNextAspect(aspects);
          auto extent = image->mipLevelExtent(subresources.baseMipLevel);
          auto elementSize = formatInfo->elementSize;

          if (formatInfo->flags.test(DxvkFormatFlag::MultiPlane)) {
            auto plane = &formatInfo->planes[vk::getPlaneIndex(aspect)];
            extent.width  /= plane->blockSize.width;
            extent.height /= plane->blockSize.height;
            elementSize = plane->elementSize;
          }

          // Allocate enough staging buffer memory to fit one
          // single subresource, then dispatch multiple copies
          VkExtent3D blockCount = util::computeBlockCount(extent, formatInfo->blockSize);
          VkDeviceSize dataSize = util::flattenImageExtent(blockCount) * elementSize;

          auto zeroBuffer = createZeroBuffer(dataSize);
          auto zeroHandle = zeroBuffer->getSliceHandle();

          for (uint32_t level = 0; level < subresources.levelCount; level++) {
            VkOffset3D offset = VkOffset3D { 0, 0, 0 };
            VkExtent3D extent = image->mipLevelExtent(subresources.baseMipLevel + level);

            if (formatInfo->flags.test(DxvkFormatFlag::MultiPlane)) {
              auto plane = &formatInfo->planes[vk::getPlaneIndex(aspect)];
              extent.width  /= plane->blockSize.width;
              extent.height /= plane->blockSize.height;
            }

            for (uint32_t layer = 0; layer < subresources.layerCount; layer++) {
              VkBufferImageCopy region;
              region.bufferOffset       = zeroHandle.offset;
              region.bufferRowLength    = 0;
              region.bufferImageHeight  = 0;
              region.imageSubresource   = vk::makeSubresourceLayers(
                vk::pickSubresource(subresources, level, layer));
              region.imageSubresource.aspectMask = aspect;
              region.imageOffset        = offset;
              region.imageExtent        = extent;

              m_cmd->cmdCopyBufferToImage(DxvkCmdBuffer::ExecBuffer,
                zeroHandle.handle, image->handle(), clearLayout, 1, &region);
            }
          }

          m_cmd->trackResource<DxvkAccess::Read>(zeroBuffer);
        }
      } else {
        if (subresources.aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
          VkClearDepthStencilValue value = { };

          m_cmd->cmdClearDepthStencilImage(image->handle(),
            clearLayout, &value, 1, &subresources);
        } else {
          VkClearColorValue value = { };

          m_cmd->cmdClearColorImage(image->handle(),
            clearLayout, &value, 1, &subresources);
        }
      }

      m_execBarriers.accessImage(image, subresources,
        clearLayout,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        image->info().layout,
        image->info().stages,
        image->info().access);

      m_cmd->trackResource<DxvkAccess::Write>(image);
    }
  }
  
  
  void DxvkContext::generateMipmaps(
    const Rc<DxvkImageView>&        imageView,
          VkFilter                  filter) {
    if (imageView->info().numLevels <= 1)
      return;
    
    this->spillRenderPass(false);

    m_execBarriers.recordCommands(m_cmd);
    
    // Create the a set of framebuffers and image views
    const Rc<DxvkMetaMipGenRenderPass> mipGenerator
      = new DxvkMetaMipGenRenderPass(m_device->vkd(), imageView);
    
    // Common descriptor set properties that we use to
    // bind the source image view to the fragment shader
    VkDescriptorImageInfo descriptorImage;
    descriptorImage.sampler     = m_common->metaBlit().getSampler(filter);
    descriptorImage.imageView   = VK_NULL_HANDLE;
    descriptorImage.imageLayout = imageView->imageInfo().layout;
    
    VkWriteDescriptorSet descriptorWrite;
    descriptorWrite.sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrite.pNext            = nullptr;
    descriptorWrite.dstSet           = VK_NULL_HANDLE;
    descriptorWrite.dstBinding       = 0;
    descriptorWrite.dstArrayElement  = 0;
    descriptorWrite.descriptorCount  = 1;
    descriptorWrite.descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrite.pImageInfo       = &descriptorImage;
    descriptorWrite.pBufferInfo      = nullptr;
    descriptorWrite.pTexelBufferView = nullptr;
    
    // Common render pass info
    VkRenderPassBeginInfo passInfo;
    passInfo.sType            = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    passInfo.pNext            = nullptr;
    passInfo.renderPass       = mipGenerator->renderPass();
    passInfo.framebuffer      = VK_NULL_HANDLE;
    passInfo.renderArea       = VkRect2D { };
    passInfo.clearValueCount  = 0;
    passInfo.pClearValues     = nullptr;
    
    // Retrieve a compatible pipeline to use for rendering
    DxvkMetaBlitPipeline pipeInfo = m_common->metaBlit().getPipeline(
      mipGenerator->viewType(), imageView->info().format, VK_SAMPLE_COUNT_1_BIT);
    
    for (uint32_t i = 0; i < mipGenerator->passCount(); i++) {
      DxvkMetaBlitPass pass = mipGenerator->pass(i);
      
      // Width, height and layer count for the current pass
      VkExtent3D passExtent = mipGenerator->passExtent(i);
      
      // Create descriptor set with the current source view
      descriptorImage.imageView = pass.srcView;
      descriptorWrite.dstSet = allocateDescriptorSet(pipeInfo.dsetLayout);
      m_cmd->updateDescriptorSets(1, &descriptorWrite);
      
      // Set up viewport and scissor rect
      VkViewport viewport;
      viewport.x        = 0.0f;
      viewport.y        = 0.0f;
      viewport.width    = float(passExtent.width);
      viewport.height   = float(passExtent.height);
      viewport.minDepth = 0.0f;
      viewport.maxDepth = 1.0f;
      
      VkRect2D scissor;
      scissor.offset    = { 0, 0 };
      scissor.extent    = { passExtent.width, passExtent.height };
      
      // Set up render pass info
      passInfo.framebuffer = pass.framebuffer;
      passInfo.renderArea  = scissor;
      
      // Set up push constants
      DxvkMetaBlitPushConstants pushConstants = { };
      pushConstants.srcCoord0  = { 0.0f, 0.0f, 0.0f };
      pushConstants.srcCoord1  = { 1.0f, 1.0f, 1.0f };
      pushConstants.layerCount = passExtent.depth;
      
      m_cmd->cmdBeginRenderPass(&passInfo, VK_SUBPASS_CONTENTS_INLINE);
      m_cmd->cmdBindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeInfo.pipeHandle);
      m_cmd->cmdBindDescriptorSet(VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipeInfo.pipeLayout, descriptorWrite.dstSet, 0, nullptr);
      
      m_cmd->cmdSetViewport(0, 1, &viewport);
      m_cmd->cmdSetScissor (0, 1, &scissor);
      
      m_cmd->cmdPushConstants(
        pipeInfo.pipeLayout,
        VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(pushConstants),
        &pushConstants);
      
      m_cmd->cmdDraw(3, passExtent.depth, 0, 0);
      m_cmd->cmdEndRenderPass();
    }
    
    m_cmd->trackResource<DxvkAccess::None>(mipGenerator);
    m_cmd->trackResource<DxvkAccess::Write>(imageView->image());
  }
  
  
  void DxvkContext::invalidateBuffer(
    const Rc<DxvkBuffer>&           buffer,
    const DxvkBufferSliceHandle&    slice) {
    // Allocate new backing resource
    DxvkBufferSliceHandle prevSlice = buffer->rename(slice);
    if (winehuaSampleTraceEnabled()
     && (buffer->info().usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) {
      winehuaSampleTrace(str::format(
        "dynamic-cb-invalidate buffer=", buffer.operator->(),
        " prevHandle=0x", std::hex, prevSlice.handle,
        " prevOffset=", std::dec, prevSlice.offset,
        " nextHandle=0x", std::hex, slice.handle,
        " nextOffset=", std::dec, slice.offset,
        " words=0x", std::hex,
        reinterpret_cast<const uint32_t*>(slice.mapPtr)[0], ",0x",
        reinterpret_cast<const uint32_t*>(slice.mapPtr)[1], ",0x",
        reinterpret_cast<const uint32_t*>(slice.mapPtr)[2], ",0x",
        reinterpret_cast<const uint32_t*>(slice.mapPtr)[3]));
    }
    m_cmd->freeBufferSlice(buffer, prevSlice);
    
    // We also need to update all bindings that the buffer
    // may be bound to either directly or through views.
    VkBufferUsageFlags usage = buffer->info().usage &
      ~(VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT);

    if (usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) {
      m_flags.set(prevSlice.handle == slice.handle
        ? DxvkContextFlags(DxvkContextFlag::GpDirtyDescriptorBinding,
                           DxvkContextFlag::CpDirtyDescriptorBinding)
        : DxvkContextFlags(DxvkContextFlag::GpDirtyResources,
                           DxvkContextFlag::CpDirtyResources));
    }

    // Fast early-out for uniform buffers, very common
    if (likely(usage == VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT))
      return;
    
    if (usage & (VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT
               | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT
               | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
      m_flags.set(DxvkContextFlag::GpDirtyResources,
                  DxvkContextFlag::CpDirtyResources);
    }

    if (usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT)
      m_flags.set(DxvkContextFlag::GpDirtyIndexBuffer);
    
    if (usage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)
      m_flags.set(DxvkContextFlag::GpDirtyVertexBuffers);
    
    if (usage & VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT)
      m_flags.set(DxvkContextFlag::DirtyDrawBuffer);

    if (usage & VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT)
      m_flags.set(DxvkContextFlag::GpDirtyXfbBuffers);
  }


  void DxvkContext::pushConstants(
          uint32_t                  offset,
          uint32_t                  size,
    const void*                     data) {
    std::memcpy(&m_state.pc.data[offset], data, size);

    m_flags.set(DxvkContextFlag::DirtyPushConstants);
  }
  
  
  void DxvkContext::resolveImage(
    const Rc<DxvkImage>&            dstImage,
    const Rc<DxvkImage>&            srcImage,
    const VkImageResolve&           region,
          VkFormat                  format) {
    this->spillRenderPass(true);
    this->prepareImage(m_execBarriers, dstImage, vk::makeSubresourceRange(region.dstSubresource));
    this->prepareImage(m_execBarriers, srcImage, vk::makeSubresourceRange(region.srcSubresource));

    if (format == VK_FORMAT_UNDEFINED)
      format = srcImage->info().format;

    bool useFb = srcImage->info().format != format
              || dstImage->info().format != format;

    if (m_device->perfHints().preferFbResolve) {
      useFb |= (dstImage->info().usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
            && (srcImage->info().usage & VK_IMAGE_USAGE_SAMPLED_BIT);
    }

    if (!useFb) {
      this->resolveImageHw(
        dstImage, srcImage, region);
    } else {
      this->resolveImageFb(
        dstImage, srcImage, region, format,
        VK_RESOLVE_MODE_NONE_KHR,
        VK_RESOLVE_MODE_NONE_KHR);
    }
  }


  void DxvkContext::resolveDepthStencilImage(
    const Rc<DxvkImage>&            dstImage,
    const Rc<DxvkImage>&            srcImage,
    const VkImageResolve&           region,
          VkResolveModeFlagBitsKHR  depthMode,
          VkResolveModeFlagBitsKHR  stencilMode) {
    this->spillRenderPass(true);
    this->prepareImage(m_execBarriers, dstImage, vk::makeSubresourceRange(region.dstSubresource));
    this->prepareImage(m_execBarriers, srcImage, vk::makeSubresourceRange(region.srcSubresource));

    // Technically legal, but no-op
    if (!depthMode && !stencilMode)
      return;

    // Subsequent functions expect stencil mode to be None
    // if either of the images have no stencil aspect
    if (!(region.dstSubresource.aspectMask
        & region.srcSubresource.aspectMask
        & VK_IMAGE_ASPECT_STENCIL_BIT))
      stencilMode = VK_RESOLVE_MODE_NONE_KHR;

    // We can only use the depth-stencil resolve path if the
    // extension is supported, if we are resolving a full
    // subresource, and both images have the same format.
    bool useFb = !m_device->extensions().khrDepthStencilResolve
              || !dstImage->isFullSubresource(region.dstSubresource, region.extent)
              || !srcImage->isFullSubresource(region.srcSubresource, region.extent)
              || dstImage->info().format != srcImage->info().format;
    
    if (!useFb) {
      // Additionally, the given mode combination must be supported.
      const auto& properties = m_device->properties().khrDepthStencilResolve;

      useFb |= (properties.supportedDepthResolveModes   & depthMode)   != depthMode
            || (properties.supportedStencilResolveModes & stencilMode) != stencilMode;
      
      if (depthMode != stencilMode) {
        useFb |= (!depthMode || !stencilMode)
          ? !properties.independentResolveNone
          : !properties.independentResolve;
      }
    }

    if (useFb) {
      this->resolveImageFb(
        dstImage, srcImage, region, VK_FORMAT_UNDEFINED,
        depthMode, stencilMode);
    } else {
      this->resolveImageDs(
        dstImage, srcImage, region,
        depthMode, stencilMode);
    }
  }


  void DxvkContext::transformImage(
    const Rc<DxvkImage>&            dstImage,
    const VkImageSubresourceRange&  dstSubresources,
          VkImageLayout             srcLayout,
          VkImageLayout             dstLayout) {
    this->spillRenderPass(false);
    
    if (srcLayout != dstLayout) {
      m_execBarriers.recordCommands(m_cmd);

      m_execBarriers.accessImage(
        dstImage, dstSubresources,
        srcLayout,
        dstImage->info().stages,
        dstImage->info().access,
        dstLayout,
        dstImage->info().stages,
        dstImage->info().access);
      
      m_cmd->trackResource<DxvkAccess::Write>(dstImage);
    }
  }
  
  
  void DxvkContext::performClear(
    const Rc<DxvkImageView>&        imageView,
          int32_t                   attachmentIndex,
          VkImageAspectFlags        discardAspects,
          VkImageAspectFlags        clearAspects,
          VkClearValue              clearValue) {
    if (winehuaSampleTraceEnabled() && winehuaTraceDepthView(imageView)) {
      winehuaSampleTrace(str::format(
        "depth-clear-perform cookie=", imageView->cookie(),
        " image=0x", std::hex, imageView->imageHandle(),
        " view=0x", imageView->handle(),
        " clearAspects=0x", clearAspects,
        " discardAspects=0x", discardAspects,
        " depth=", std::dec, clearValue.depthStencil.depth,
        " stencil=", clearValue.depthStencil.stencil,
        " attachmentIndex=", attachmentIndex));
    }

    DxvkColorAttachmentOps colorOp;
    colorOp.loadOp        = VK_ATTACHMENT_LOAD_OP_LOAD;
    colorOp.loadLayout    = imageView->imageInfo().layout;
    colorOp.storeLayout   = imageView->imageInfo().layout;
    
    DxvkDepthAttachmentOps depthOp;
    depthOp.loadOpD       = VK_ATTACHMENT_LOAD_OP_LOAD;
    depthOp.loadOpS       = VK_ATTACHMENT_LOAD_OP_LOAD;
    depthOp.loadLayout    = imageView->imageInfo().layout;
    depthOp.storeLayout   = imageView->imageInfo().layout;
    
    if (clearAspects & VK_IMAGE_ASPECT_COLOR_BIT)
      colorOp.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    else if (discardAspects & VK_IMAGE_ASPECT_COLOR_BIT)
      colorOp.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    
    if (clearAspects & VK_IMAGE_ASPECT_DEPTH_BIT)
      depthOp.loadOpD = VK_ATTACHMENT_LOAD_OP_CLEAR;
    else if (discardAspects & VK_IMAGE_ASPECT_DEPTH_BIT)
      depthOp.loadOpD = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    
    if (clearAspects & VK_IMAGE_ASPECT_STENCIL_BIT)
      depthOp.loadOpS = VK_ATTACHMENT_LOAD_OP_CLEAR;
    else if (discardAspects & VK_IMAGE_ASPECT_DEPTH_BIT)
      depthOp.loadOpS = VK_ATTACHMENT_LOAD_OP_DONT_CARE;

    if (attachmentIndex >= 0 && !m_state.om.framebufferInfo.isWritable(attachmentIndex, clearAspects | discardAspects)) {
      // Do not fold the clear/discard into the render pass if any of the affected aspects
      // isn't writable. We can only hit this particular path when starting a render pass,
      // so we can safely manipulate load layouts here.
      int32_t colorIndex = m_state.om.framebufferInfo.getColorAttachmentIndex(attachmentIndex);
      VkImageLayout renderLayout = m_state.om.framebufferInfo.getAttachment(attachmentIndex).layout;

      if (colorIndex < 0) {
        depthOp.loadLayout = m_state.om.renderPassOps.depthOps.loadLayout;
        depthOp.storeLayout = renderLayout;
        m_state.om.renderPassOps.depthOps.loadLayout = renderLayout;
      } else {
        colorOp.loadLayout = m_state.om.renderPassOps.colorOps[colorIndex].loadLayout;
        colorOp.storeLayout = renderLayout;
        m_state.om.renderPassOps.colorOps[colorIndex].loadLayout = renderLayout;
      }

      attachmentIndex = -1;
    }

    bool is3D = imageView->imageInfo().type == VK_IMAGE_TYPE_3D;

    if ((clearAspects | discardAspects) == imageView->info().aspect && !is3D) {
      colorOp.loadLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
      depthOp.loadLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    
    if (attachmentIndex < 0) {
      if (winehuaSampleTraceEnabled() && winehuaTraceDepthView(imageView))
        winehuaSampleTrace("depth-clear-mode temporary-render-pass");

      if (m_execBarriers.isImageDirty(
          imageView->image(),
          imageView->imageSubresources(),
          DxvkAccess::Write))
        m_execBarriers.recordCommands(m_cmd);
      
      // Set up and bind a temporary framebuffer
      DxvkRenderTargets attachments;
      DxvkRenderPassOps ops;

      VkPipelineStageFlags clearStages = 0;
      VkAccessFlags        clearAccess = 0;
      
      if ((clearAspects | discardAspects) & VK_IMAGE_ASPECT_COLOR_BIT) {
        clearStages |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        clearAccess |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        attachments.color[0].view   = imageView;
        attachments.color[0].layout = imageView->pickLayout(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        
        ops.colorOps[0] = colorOp;
      } else {
        clearStages |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                    |  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        clearAccess |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        attachments.depth.view   = imageView;
        attachments.depth.layout = imageView->pickLayout(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        
        ops.depthOps = depthOp;
      }

      ops.barrier.srcStages = clearStages;
      ops.barrier.srcAccess = clearAccess;
      ops.barrier.dstStages = imageView->imageInfo().stages;
      ops.barrier.dstAccess = imageView->imageInfo().access;

      this->renderPassBindFramebuffer(makeFramebufferInfo(attachments), ops, 1, &clearValue);
      this->renderPassUnbindFramebuffer();
    } else {
      if (winehuaSampleTraceEnabled() && winehuaTraceDepthView(imageView))
        winehuaSampleTrace("depth-clear-mode folded-load-op");

      // Perform the operation when starting the next render pass
      if ((clearAspects | discardAspects) & VK_IMAGE_ASPECT_COLOR_BIT) {
        uint32_t colorIndex = m_state.om.framebufferInfo.getColorAttachmentIndex(attachmentIndex);

        m_state.om.renderPassOps.colorOps[colorIndex].loadOp = colorOp.loadOp;
        if (m_state.om.renderPassOps.colorOps[colorIndex].loadOp != VK_ATTACHMENT_LOAD_OP_LOAD && !is3D)
          m_state.om.renderPassOps.colorOps[colorIndex].loadLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        m_state.om.clearValues[attachmentIndex].color = clearValue.color;
      }
      
      if ((clearAspects | discardAspects) & VK_IMAGE_ASPECT_DEPTH_BIT) {
        m_state.om.renderPassOps.depthOps.loadOpD = depthOp.loadOpD;
        m_state.om.clearValues[attachmentIndex].depthStencil.depth = clearValue.depthStencil.depth;
      }
      
      if ((clearAspects | discardAspects) & VK_IMAGE_ASPECT_STENCIL_BIT) {
        m_state.om.renderPassOps.depthOps.loadOpS = depthOp.loadOpS;
        m_state.om.clearValues[attachmentIndex].depthStencil.stencil = clearValue.depthStencil.stencil;
      }

      if ((clearAspects | discardAspects) & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
        if (m_state.om.renderPassOps.depthOps.loadOpD != VK_ATTACHMENT_LOAD_OP_LOAD
         && m_state.om.renderPassOps.depthOps.loadOpS != VK_ATTACHMENT_LOAD_OP_LOAD)
          m_state.om.renderPassOps.depthOps.loadLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      }
    }
  }


  void DxvkContext::deferClear(
    const Rc<DxvkImageView>&        imageView,
          VkImageAspectFlags        clearAspects,
          VkClearValue              clearValue) {
    if (winehuaSampleTraceEnabled() && winehuaTraceDepthView(imageView)) {
      winehuaSampleTrace(str::format(
        "depth-clear-defer cookie=", imageView->cookie(),
        " image=0x", std::hex, imageView->imageHandle(),
        " clearAspects=0x", clearAspects,
        " depth=", std::dec, clearValue.depthStencil.depth,
        " pendingBefore=", m_deferredClears.size()));
    }

    for (auto& entry : m_deferredClears) {
      if (entry.imageView->matchesView(imageView)) {
        entry.imageView = imageView;
        entry.discardAspects &= ~clearAspects;
        entry.clearAspects |= clearAspects;

        if (clearAspects & VK_IMAGE_ASPECT_COLOR_BIT)
          entry.clearValue.color = clearValue.color;
        if (clearAspects & VK_IMAGE_ASPECT_DEPTH_BIT)
          entry.clearValue.depthStencil.depth = clearValue.depthStencil.depth;
        if (clearAspects & VK_IMAGE_ASPECT_STENCIL_BIT)
          entry.clearValue.depthStencil.stencil = clearValue.depthStencil.stencil;
        
        return;
      } else if (entry.imageView->checkSubresourceOverlap(imageView)) {
        this->spillRenderPass(false);
        break;
      }
    }

    m_deferredClears.push_back({ imageView, 0, clearAspects, clearValue });
  }


  void DxvkContext::deferDiscard(
    const Rc<DxvkImageView>&        imageView,
          VkImageAspectFlags        discardAspects) {
    for (auto& entry : m_deferredClears) {
      if (entry.imageView->matchesView(imageView)) {
        entry.imageView = imageView;
        entry.discardAspects |= discardAspects;
        entry.clearAspects &= ~discardAspects;
        return;
      } else if (entry.imageView->checkSubresourceOverlap(imageView)) {
        this->spillRenderPass(false);
        break;
      }
    }

    m_deferredClears.push_back({ imageView, discardAspects });
  }


  void DxvkContext::flushClears(
          bool                      useRenderPass) {
    for (const auto& clear : m_deferredClears) {
      int32_t attachmentIndex = -1;

      if (useRenderPass && m_state.om.framebufferInfo.isFullSize(clear.imageView))
        attachmentIndex = m_state.om.framebufferInfo.findAttachment(clear.imageView);

      this->performClear(clear.imageView, attachmentIndex,
        clear.discardAspects, clear.clearAspects, clear.clearValue);
    }

    m_deferredClears.clear();
  }


  void DxvkContext::flushSharedImages() {
    for (auto i = m_deferredClears.begin(); i != m_deferredClears.end(); ) {
      if (i->imageView->imageInfo().shared) {
        this->performClear(i->imageView, -1, i->discardAspects, i->clearAspects, i->clearValue);
        i = m_deferredClears.erase(i);
      } else {
        i++;
      }
    }

    this->transitionRenderTargetLayouts(m_execBarriers, true);
  }


  void DxvkContext::updateBuffer(
    const Rc<DxvkBuffer>&           buffer,
          VkDeviceSize              offset,
          VkDeviceSize              size,
    const void*                     data) {
    bool replaceBuffer = this->tryInvalidateDeviceLocalBuffer(buffer, size);
    auto bufferSlice = buffer->getSliceHandle(offset, size);

    if (!replaceBuffer) {
      this->spillRenderPass(true);
    
      if (m_execBarriers.isBufferDirty(bufferSlice, DxvkAccess::Write))
        m_execBarriers.recordCommands(m_cmd);
    }

    DxvkCmdBuffer cmdBuffer = replaceBuffer
      ? DxvkCmdBuffer::InitBuffer
      : DxvkCmdBuffer::ExecBuffer;

    m_cmd->cmdUpdateBuffer(cmdBuffer,
      bufferSlice.handle,
      bufferSlice.offset,
      bufferSlice.length,
      data);

    auto& barriers = replaceBuffer
      ? m_initBarriers
      : m_execBarriers;

    barriers.accessBuffer(bufferSlice,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      buffer->info().stages,
      buffer->info().access);

    m_cmd->trackResource<DxvkAccess::Write>(buffer);
  }
  
  
  void DxvkContext::updateDepthStencilImage(
    const Rc<DxvkImage>&            image,
    const VkImageSubresourceLayers& subresources,
          VkOffset2D                imageOffset,
          VkExtent2D                imageExtent,
    const void*                     data,
          VkDeviceSize              pitchPerRow,
          VkDeviceSize              pitchPerLayer,
          VkFormat                  format) {
    auto formatInfo = imageFormatInfo(format);
    
    VkExtent3D extent3D;
    extent3D.width  = imageExtent.width;
    extent3D.height = imageExtent.height;
    extent3D.depth  = subresources.layerCount;

    VkDeviceSize pixelCount = extent3D.width * extent3D.height * extent3D.depth;

    DxvkBufferCreateInfo tmpBufferInfo;
    tmpBufferInfo.size      = pixelCount * formatInfo->elementSize;
    tmpBufferInfo.usage     = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    tmpBufferInfo.stages    = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    tmpBufferInfo.access    = VK_ACCESS_SHADER_READ_BIT;

    auto tmpBuffer = m_device->createBuffer(tmpBufferInfo,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    util::packImageData(tmpBuffer->mapPtr(0), data,
      extent3D, formatInfo->elementSize,
      pitchPerRow, pitchPerLayer);
    if (winehuaFlushDynamicMapped())
      this->flushMappedBuffer(tmpBuffer, tmpBuffer->getSliceHandle());
    
    copyPackedBufferToDepthStencilImage(
      image, subresources, imageOffset, imageExtent,
      tmpBuffer, 0, VkOffset2D { 0, 0 }, imageExtent,
      format);
  }


  void DxvkContext::uploadBuffer(
    const Rc<DxvkBuffer>&           buffer,
    const void*                     data) {
    auto bufferSlice = buffer->getSliceHandle();

    auto stagingSlice = m_staging.alloc(CACHE_LINE_SIZE, bufferSlice.length);
    auto stagingHandle = stagingSlice.getSliceHandle();
    std::memcpy(stagingHandle.mapPtr, data, bufferSlice.length);
    if (winehuaFlushDynamicMapped())
      this->flushMappedBuffer(stagingSlice.buffer(), stagingHandle);

    VkBufferCopy region;
    region.srcOffset = stagingHandle.offset;
    region.dstOffset = bufferSlice.offset;
    region.size      = bufferSlice.length;

    m_cmd->cmdCopyBuffer(DxvkCmdBuffer::SdmaBuffer,
      stagingHandle.handle, bufferSlice.handle, 1, &region);

    m_sdmaBarriers.releaseBuffer(
      m_initBarriers, bufferSlice,
      m_device->queues().transfer.queueFamily,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      m_device->queues().graphics.queueFamily,
      buffer->info().stages,
      buffer->info().access);
    
    m_cmd->trackResource<DxvkAccess::Read>(stagingSlice.buffer());
    m_cmd->trackResource<DxvkAccess::Write>(buffer);
  }


  void DxvkContext::uploadImage(
    const Rc<DxvkImage>&            image,
    const VkImageSubresourceLayers& subresources,
    const void*                     data,
          VkDeviceSize              pitchPerRow,
          VkDeviceSize              pitchPerLayer) {
    VkOffset3D imageOffset = { 0, 0, 0 };
    VkExtent3D imageExtent = image->mipLevelExtent(subresources.mipLevel);

    DxvkCmdBuffer cmdBuffer = DxvkCmdBuffer::SdmaBuffer;
    DxvkBarrierSet* barriers = &m_sdmaAcquires;
    
    if (subresources.aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
      cmdBuffer = DxvkCmdBuffer::InitBuffer;
      barriers = &m_initBarriers;
    }

    // Discard previous subresource contents
    barriers->accessImage(image,
      vk::makeSubresourceRange(subresources),
      VK_IMAGE_LAYOUT_UNDEFINED, 0, 0,
      image->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT);

    barriers->recordCommands(m_cmd);

    this->copyImageHostData(cmdBuffer,
      image, subresources, imageOffset, imageExtent,
      data, pitchPerRow, pitchPerLayer);

    // Transfer ownership to graphics queue
    if (cmdBuffer == DxvkCmdBuffer::SdmaBuffer) {
      m_sdmaBarriers.releaseImage(m_initBarriers,
        image, vk::makeSubresourceRange(subresources),
        m_device->queues().transfer.queueFamily,
        image->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        m_device->queues().graphics.queueFamily,
        image->info().layout,
        image->info().stages,
        image->info().access);
    } else {
      barriers->accessImage(image,
        vk::makeSubresourceRange(subresources),
        image->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        image->info().layout,
        image->info().stages,
        image->info().access);
    }
    
    m_cmd->trackResource<DxvkAccess::Write>(image);
  }


  void DxvkContext::setViewports(
          uint32_t            viewportCount,
    const VkViewport*         viewports,
    const VkRect2D*           scissorRects) {
    if (m_state.gp.state.rs.viewportCount() != viewportCount) {
      m_state.gp.state.rs.setViewportCount(viewportCount);
      m_flags.set(DxvkContextFlag::GpDirtyPipelineState);
    }
    
    for (uint32_t i = 0; i < viewportCount; i++) {
      m_state.vp.viewports[i]    = viewports[i];
      m_state.vp.scissorRects[i] = scissorRects[i];
      
      // Vulkan viewports are not allowed to have a width or
      // height of zero, so we fall back to a dummy viewport
      // and instead set an empty scissor rect, which is legal.
      if (viewports[i].width == 0.0f || viewports[i].height == 0.0f) {
        m_state.vp.viewports[i] = VkViewport {
          0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f };
        m_state.vp.scissorRects[i] = VkRect2D {
          VkOffset2D { 0, 0 },
          VkExtent2D { 0, 0 } };
      }
    }
    
    m_flags.set(DxvkContextFlag::GpDirtyViewport);
  }
  
  
  void DxvkContext::setBlendConstants(
          DxvkBlendConstants  blendConstants) {
    if (m_state.dyn.blendConstants != blendConstants) {
      m_state.dyn.blendConstants = blendConstants;
      m_flags.set(DxvkContextFlag::GpDirtyBlendConstants);
    }
  }
  
  
  void DxvkContext::setDepthBias(
          DxvkDepthBias       depthBias) {
    if (m_state.dyn.depthBias != depthBias) {
      m_state.dyn.depthBias = depthBias;
      m_flags.set(DxvkContextFlag::GpDirtyDepthBias);
    }
  }


  void DxvkContext::setDepthBounds(
          DxvkDepthBounds     depthBounds) {
    if (m_state.dyn.depthBounds != depthBounds) {
      m_state.dyn.depthBounds = depthBounds;
      m_flags.set(DxvkContextFlag::GpDirtyDepthBounds);
    }

    if (m_state.gp.state.ds.enableDepthBoundsTest() != depthBounds.enableDepthBounds) {
      m_state.gp.state.ds.setEnableDepthBoundsTest(depthBounds.enableDepthBounds);
      m_flags.set(DxvkContextFlag::GpDirtyPipelineState);
    }
  }
  
  
  void DxvkContext::setStencilReference(
          uint32_t            reference) {
    if (m_state.dyn.stencilReference != reference) {
      m_state.dyn.stencilReference = reference;
      m_flags.set(DxvkContextFlag::GpDirtyStencilRef);
    }
  }
  
  
  void DxvkContext::setInputAssemblyState(const DxvkInputAssemblyState& ia) {
    m_state.gp.state.ia = DxvkIaInfo(
      ia.primitiveTopology,
      ia.primitiveRestart,
      ia.patchVertexCount);
    
    m_flags.set(DxvkContextFlag::GpDirtyPipelineState);
  }
  
  
  void DxvkContext::setInputLayout(
          uint32_t             attributeCount,
    const DxvkVertexAttribute* attributes,
          uint32_t             bindingCount,
    const DxvkVertexBinding*   bindings) {
    m_flags.set(
      DxvkContextFlag::GpDirtyPipelineState,
      DxvkContextFlag::GpDirtyVertexBuffers);
    
    for (uint32_t i = 0; i < attributeCount; i++) {
      m_state.gp.state.ilAttributes[i] = DxvkIlAttribute(
        attributes[i].location, attributes[i].binding,
        attributes[i].format,   attributes[i].offset);
    }
    
    for (uint32_t i = attributeCount; i < m_state.gp.state.il.attributeCount(); i++)
      m_state.gp.state.ilAttributes[i] = DxvkIlAttribute();
    
    for (uint32_t i = 0; i < bindingCount; i++) {
      m_state.gp.state.ilBindings[i] = DxvkIlBinding(
        bindings[i].binding, 0, bindings[i].inputRate,
        bindings[i].fetchRate);
    }
    
    for (uint32_t i = bindingCount; i < m_state.gp.state.il.bindingCount(); i++)
      m_state.gp.state.ilBindings[i] = DxvkIlBinding();
    
    m_state.gp.state.il = DxvkIlInfo(attributeCount, bindingCount);
  }
  
  
  void DxvkContext::setRasterizerState(const DxvkRasterizerState& rs) {
    m_state.gp.state.rs = DxvkRsInfo(
      rs.depthClipEnable,
      rs.depthBiasEnable,
      rs.polygonMode,
      rs.cullMode,
      rs.frontFace,
      m_state.gp.state.rs.viewportCount(),
      rs.sampleCount,
      rs.conservativeMode);

    m_flags.set(DxvkContextFlag::GpDirtyPipelineState);
  }
  
  
  void DxvkContext::setMultisampleState(const DxvkMultisampleState& ms) {
    m_state.gp.state.ms = DxvkMsInfo(
      m_state.gp.state.ms.sampleCount(),
      ms.sampleMask,
      ms.enableAlphaToCoverage);
    
    m_flags.set(DxvkContextFlag::GpDirtyPipelineState);
  }
  
  
  void DxvkContext::setDepthStencilState(const DxvkDepthStencilState& ds) {
    m_state.gp.state.ds = DxvkDsInfo(
      ds.enableDepthTest,
      ds.enableDepthWrite,
      m_state.gp.state.ds.enableDepthBoundsTest(),
      ds.enableStencilTest,
      ds.depthCompareOp);
    
    m_state.gp.state.dsFront = DxvkDsStencilOp(ds.stencilOpFront);
    m_state.gp.state.dsBack  = DxvkDsStencilOp(ds.stencilOpBack);
    
    m_flags.set(DxvkContextFlag::GpDirtyPipelineState);
  }
  
  
  void DxvkContext::setLogicOpState(const DxvkLogicOpState& lo) {
    m_state.gp.state.om = DxvkOmInfo(
      lo.enableLogicOp,
      lo.logicOp);
    
    m_flags.set(DxvkContextFlag::GpDirtyPipelineState);
  }
  
  
  void DxvkContext::setBlendMode(
          uint32_t            attachment,
    const DxvkBlendMode&      blendMode) {
    m_state.gp.state.omBlend[attachment] = DxvkOmAttachmentBlend(
      blendMode.enableBlending,
      blendMode.colorSrcFactor,
      blendMode.colorDstFactor,
      blendMode.colorBlendOp,
      blendMode.alphaSrcFactor,
      blendMode.alphaDstFactor,
      blendMode.alphaBlendOp,
      blendMode.writeMask);
    
    m_flags.set(DxvkContextFlag::GpDirtyPipelineState);
  }


  void DxvkContext::setSpecConstant(
          VkPipelineBindPoint pipeline,
          uint32_t            index,
          uint32_t            value) {
    auto& specConst = pipeline == VK_PIPELINE_BIND_POINT_GRAPHICS
      ? m_state.gp.state.sc.specConstants[index]
      : m_state.cp.state.sc.specConstants[index];
    
    if (specConst != value) {
      specConst = value;

      m_flags.set(pipeline == VK_PIPELINE_BIND_POINT_GRAPHICS
        ? DxvkContextFlag::GpDirtyPipelineState
        : DxvkContextFlag::CpDirtyPipelineState);
    }
  }
  
  
  void DxvkContext::setBarrierControl(DxvkBarrierControlFlags control) {
    m_barrierControl = control;
  }
  
  
  void DxvkContext::signalGpuEvent(const Rc<DxvkGpuEvent>& event) {
    this->spillRenderPass(true);
    
    DxvkGpuEventHandle handle = m_common->eventPool().allocEvent();

    m_cmd->cmdSetEvent(handle.event,
      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

    m_cmd->trackGpuEvent(event->reset(handle));
    m_cmd->trackResource<DxvkAccess::None>(event);
  }
  

  void DxvkContext::launchCuKernelNVX(
    const VkCuLaunchInfoNVX& nvxLaunchInfo,
    const std::vector<std::pair<Rc<DxvkBuffer>, DxvkAccessFlags>>& buffers,
    const std::vector<std::pair<Rc<DxvkImage>,  DxvkAccessFlags>>& images) {
    // The resources in the std::vectors above are called-out
    // explicitly in the API for barrier and tracking purposes
    // since they're being used bindlessly.
    this->spillRenderPass(true);

    VkPipelineStageFlags srcStages = 0;
    VkAccessFlags srcAccess = 0;

    for (auto& r : buffers) {
      srcStages |= r.first->info().stages;
      srcAccess |= r.first->info().access;
    }

    for (auto& r : images) {
      srcStages |= r.first->info().stages;
      srcAccess |= r.first->info().access;

      this->prepareImage(m_execBarriers, r.first, r.first->getAvailableSubresources());
    }

    m_execBarriers.accessMemory(srcStages, srcAccess,
      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    m_execBarriers.recordCommands(m_cmd);

    m_cmd->cmdLaunchCuKernel(nvxLaunchInfo);

    for (auto& r : buffers) {
      VkAccessFlags accessFlags = (r.second.test(DxvkAccess::Read) * VK_ACCESS_SHADER_READ_BIT)
                                | (r.second.test(DxvkAccess::Write) * VK_ACCESS_SHADER_WRITE_BIT);
      DxvkBufferSliceHandle bufferSlice = r.first->getSliceHandle();
      m_execBarriers.accessBuffer(bufferSlice,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        accessFlags,
        r.first->info().stages,
        r.first->info().access);
    }

    for (auto& r : images) {
      VkAccessFlags accessFlags = (r.second.test(DxvkAccess::Read) * VK_ACCESS_SHADER_READ_BIT)
                                | (r.second.test(DxvkAccess::Write) * VK_ACCESS_SHADER_WRITE_BIT);
      m_execBarriers.accessImage(r.first,
        r.first->getAvailableSubresources(),
        r.first->info().layout,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        accessFlags,
        r.first->info().layout,
        r.first->info().stages,
        r.first->info().access);
    }

    for (auto& r : images) {
      if (r.second.test(DxvkAccess::Read)) m_cmd->trackResource<DxvkAccess::Read>(r.first);
      if (r.second.test(DxvkAccess::Write)) m_cmd->trackResource<DxvkAccess::Write>(r.first);
    }

    for (auto& r : buffers) {
      if (r.second.test(DxvkAccess::Read)) m_cmd->trackResource<DxvkAccess::Read>(r.first);
      if (r.second.test(DxvkAccess::Write)) m_cmd->trackResource<DxvkAccess::Write>(r.first);
    }
  }
  
  
  void DxvkContext::writeTimestamp(const Rc<DxvkGpuQuery>& query) {
    m_queryManager.writeTimestamp(m_cmd, query);
  }


  void DxvkContext::signal(const Rc<sync::Signal>& signal, uint64_t value) {
    m_cmd->queueSignal(signal, value);
  }


  void DxvkContext::waitFence(const Rc<DxvkFence>& fence, uint64_t value) {
    m_cmd->waitFence(fence, value);
  }


  void DxvkContext::signalFence(const Rc<DxvkFence>& fence, uint64_t value) {
    m_cmd->signalFence(fence, value);
  }


  void DxvkContext::beginDebugLabel(VkDebugUtilsLabelEXT *label) {
    if (!m_device->instance()->extensions().extDebugUtils)
      return;

    m_cmd->cmdBeginDebugUtilsLabel(label);
  }

  void DxvkContext::endDebugLabel() {
    if (!m_device->instance()->extensions().extDebugUtils)
      return;

    m_cmd->cmdEndDebugUtilsLabel();
  }

  void DxvkContext::insertDebugLabel(VkDebugUtilsLabelEXT *label) {
    if (!m_device->instance()->extensions().extDebugUtils)
      return;

    m_cmd->cmdInsertDebugUtilsLabel(label);
  }
  
  
  void DxvkContext::blitImageFb(
    const Rc<DxvkImage>&        dstImage,
    const Rc<DxvkImage>&        srcImage,
    const VkImageBlit&          region,
    const VkComponentMapping&   mapping,
          VkFilter              filter) {
    auto dstSubresourceRange = vk::makeSubresourceRange(region.dstSubresource);
    auto srcSubresourceRange = vk::makeSubresourceRange(region.srcSubresource);

    if (m_execBarriers.isImageDirty(dstImage, dstSubresourceRange, DxvkAccess::Write)
     || m_execBarriers.isImageDirty(srcImage, srcSubresourceRange, DxvkAccess::Write))
      m_execBarriers.recordCommands(m_cmd);

    bool isDepthStencil = region.srcSubresource.aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);

    VkImageLayout srcLayout = srcImage->pickLayout(isDepthStencil
      ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
      : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    
    if (srcImage->info().layout != srcLayout) {
      m_execAcquires.accessImage(
        srcImage, srcSubresourceRange,
        srcImage->info().layout,
        srcImage->info().stages, 0,
        srcLayout,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT);
      
      m_execAcquires.recordCommands(m_cmd);
    }

    // Sort out image offsets so that dstOffset[0] points
    // to the top-left corner of the target area
    VkOffset3D srcOffsets[2] = { region.srcOffsets[0], region.srcOffsets[1] };
    VkOffset3D dstOffsets[2] = { region.dstOffsets[0], region.dstOffsets[1] };

    if (dstOffsets[0].x > dstOffsets[1].x) {
      std::swap(dstOffsets[0].x, dstOffsets[1].x);
      std::swap(srcOffsets[0].x, srcOffsets[1].x);
    }

    if (dstOffsets[0].y > dstOffsets[1].y) {
      std::swap(dstOffsets[0].y, dstOffsets[1].y);
      std::swap(srcOffsets[0].y, srcOffsets[1].y);
    }

    if (dstOffsets[0].z > dstOffsets[1].z) {
      std::swap(dstOffsets[0].z, dstOffsets[1].z);
      std::swap(srcOffsets[0].z, srcOffsets[1].z);
    }
    
    VkExtent3D dstExtent = {
      uint32_t(dstOffsets[1].x - dstOffsets[0].x),
      uint32_t(dstOffsets[1].y - dstOffsets[0].y),
      uint32_t(dstOffsets[1].z - dstOffsets[0].z) };

    // Begin render pass
    Rc<DxvkMetaBlitRenderPass> pass = new DxvkMetaBlitRenderPass(
      m_device, dstImage, srcImage, region, mapping);
    DxvkMetaBlitPass passObjects = pass->pass();

    VkExtent3D imageExtent = dstImage->mipLevelExtent(region.dstSubresource.mipLevel);

    VkRect2D renderArea;
    renderArea.offset         = VkOffset2D { 0, 0 };
    renderArea.extent         = VkExtent2D { imageExtent.width, imageExtent.height };

    VkRenderPassBeginInfo passInfo;
    passInfo.sType            = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    passInfo.pNext            = nullptr;
    passInfo.renderPass       = passObjects.renderPass;
    passInfo.framebuffer      = passObjects.framebuffer;
    passInfo.renderArea       = renderArea;
    passInfo.clearValueCount  = 0;
    passInfo.pClearValues     = nullptr;

    m_cmd->cmdBeginRenderPass(&passInfo, VK_SUBPASS_CONTENTS_INLINE);

    // Bind pipeline
    DxvkMetaBlitPipeline pipeInfo = m_common->metaBlit().getPipeline(
      pass->viewType(), dstImage->info().format, dstImage->info().sampleCount);

    m_cmd->cmdBindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeInfo.pipeHandle);

    // Set up viewport
    VkViewport viewport;
    viewport.x                = float(dstOffsets[0].x);
    viewport.y                = float(dstOffsets[0].y);
    viewport.width            = float(dstExtent.width);
    viewport.height           = float(dstExtent.height);
    viewport.minDepth         = 0.0f;
    viewport.maxDepth         = 1.0f;

    VkRect2D scissor;
    scissor.offset            = { dstOffsets[0].x, dstOffsets[0].y  };
    scissor.extent            = { dstExtent.width, dstExtent.height };

    m_cmd->cmdSetViewport(0, 1, &viewport);
    m_cmd->cmdSetScissor (0, 1, &scissor);

    // Bind source image view
    VkDescriptorImageInfo descriptorImage;
    descriptorImage.sampler     = m_common->metaBlit().getSampler(filter);
    descriptorImage.imageView   = passObjects.srcView;
    descriptorImage.imageLayout = srcLayout;
    
    VkWriteDescriptorSet descriptorWrite;
    descriptorWrite.sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrite.pNext            = nullptr;
    descriptorWrite.dstSet           = allocateDescriptorSet(pipeInfo.dsetLayout);
    descriptorWrite.dstBinding       = 0;
    descriptorWrite.dstArrayElement  = 0;
    descriptorWrite.descriptorCount  = 1;
    descriptorWrite.descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrite.pImageInfo       = &descriptorImage;
    descriptorWrite.pBufferInfo      = nullptr;
    descriptorWrite.pTexelBufferView = nullptr;

    m_cmd->updateDescriptorSets(1, &descriptorWrite);
    m_cmd->cmdBindDescriptorSet(VK_PIPELINE_BIND_POINT_GRAPHICS,
      pipeInfo.pipeLayout, descriptorWrite.dstSet, 0, nullptr);

    // Compute shader parameters for the operation
    VkExtent3D srcExtent = srcImage->mipLevelExtent(region.srcSubresource.mipLevel);

    DxvkMetaBlitPushConstants pushConstants = { };
    pushConstants.srcCoord0  = {
      float(srcOffsets[0].x) / float(srcExtent.width),
      float(srcOffsets[0].y) / float(srcExtent.height),
      float(srcOffsets[0].z) / float(srcExtent.depth) };
    pushConstants.srcCoord1  = {
      float(srcOffsets[1].x) / float(srcExtent.width),
      float(srcOffsets[1].y) / float(srcExtent.height),
      float(srcOffsets[1].z) / float(srcExtent.depth) };
    pushConstants.layerCount = pass->framebufferLayerCount();

    m_cmd->cmdPushConstants(
      pipeInfo.pipeLayout,
      VK_SHADER_STAGE_FRAGMENT_BIT,
      0, sizeof(pushConstants),
      &pushConstants);

    m_cmd->cmdDraw(3, pushConstants.layerCount, 0, 0);
    m_cmd->cmdEndRenderPass();

    // Add barriers and track image objects
    m_execBarriers.accessImage(dstImage,
      vk::makeSubresourceRange(region.dstSubresource),
      dstImage->info().layout,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      dstImage->info().layout,
      dstImage->info().stages,
      dstImage->info().access);
    
    m_execBarriers.accessImage(srcImage,
      vk::makeSubresourceRange(region.srcSubresource),
      srcLayout,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      VK_ACCESS_SHADER_READ_BIT,
      srcImage->info().layout,
      srcImage->info().stages,
      srcImage->info().access);

    m_cmd->trackResource<DxvkAccess::Write>(dstImage);
    m_cmd->trackResource<DxvkAccess::Read>(srcImage);
    m_cmd->trackResource<DxvkAccess::None>(pass);
  }


  void DxvkContext::blitImageHw(
    const Rc<DxvkImage>&        dstImage,
    const Rc<DxvkImage>&        srcImage,
    const VkImageBlit&          region,
          VkFilter              filter) {
    auto dstSubresourceRange = vk::makeSubresourceRange(region.dstSubresource);
    auto srcSubresourceRange = vk::makeSubresourceRange(region.srcSubresource);

    if (m_execBarriers.isImageDirty(dstImage, dstSubresourceRange, DxvkAccess::Write)
     || m_execBarriers.isImageDirty(srcImage, srcSubresourceRange, DxvkAccess::Write))
      m_execBarriers.recordCommands(m_cmd);

    // Prepare the two images for transfer ops if necessary
    auto dstLayout = dstImage->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    auto srcLayout = srcImage->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    if (dstImage->info().layout != dstLayout) {
      m_execAcquires.accessImage(
        dstImage, dstSubresourceRange,
        dstImage->info().layout,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
        dstLayout,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT);
    }

    if (srcImage->info().layout != srcLayout) {
      m_execAcquires.accessImage(
        srcImage, srcSubresourceRange,
        srcImage->info().layout,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
        srcLayout,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_TRANSFER_READ_BIT);
    }

    m_execAcquires.recordCommands(m_cmd);

    // Perform the blit operation
    m_cmd->cmdBlitImage(
      srcImage->handle(), srcLayout,
      dstImage->handle(), dstLayout,
      1, &region, filter);
    
    m_execBarriers.accessImage(
      dstImage, dstSubresourceRange, dstLayout,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      dstImage->info().layout,
      dstImage->info().stages,
      dstImage->info().access);
    
    m_execBarriers.accessImage(
      srcImage, srcSubresourceRange, srcLayout,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_READ_BIT,
      srcImage->info().layout,
      srcImage->info().stages,
      srcImage->info().access);

    m_cmd->trackResource<DxvkAccess::Write>(dstImage);
    m_cmd->trackResource<DxvkAccess::Read>(srcImage);
  }


  template<bool ToImage>
  void DxvkContext::copyImageBufferData(
          DxvkCmdBuffer         cmd,
    const Rc<DxvkImage>&        image,
    const VkImageSubresourceLayers& imageSubresource,
          VkOffset3D            imageOffset,
          VkExtent3D            imageExtent,
          VkImageLayout         imageLayout,
    const DxvkBufferSliceHandle& bufferSlice,
          VkDeviceSize          bufferRowAlignment,
          VkDeviceSize          bufferSliceAlignment) {
    auto formatInfo = image->formatInfo();
    auto layers = imageSubresource.layerCount;

    VkDeviceSize bufferOffset = bufferSlice.offset;

    // Do one copy region per layer in case the buffer memory layout is weird
    if (bufferSliceAlignment || formatInfo->flags.test(DxvkFormatFlag::MultiPlane))
      layers = 1;

    for (uint32_t i = 0; i < imageSubresource.layerCount; i += layers) {
      auto aspectOffset = bufferOffset;

      for (auto aspects = imageSubresource.aspectMask; aspects; ) {
        auto aspect = vk::getNextAspect(aspects);
        auto elementSize = formatInfo->elementSize;

        VkBufferImageCopy copyRegion = { };
        copyRegion.imageSubresource.aspectMask = aspect;
        copyRegion.imageSubresource.baseArrayLayer = imageSubresource.baseArrayLayer + i;
        copyRegion.imageSubresource.layerCount = layers;
        copyRegion.imageSubresource.mipLevel = imageSubresource.mipLevel;
        copyRegion.imageOffset = imageOffset;
        copyRegion.imageExtent = imageExtent;

        if (formatInfo->flags.test(DxvkFormatFlag::MultiPlane)) {
          auto plane = &formatInfo->planes[vk::getPlaneIndex(aspect)];
          copyRegion.imageOffset.x /= plane->blockSize.width;
          copyRegion.imageOffset.y /= plane->blockSize.height;
          copyRegion.imageExtent.width  /= plane->blockSize.width;
          copyRegion.imageExtent.height /= plane->blockSize.height;
          elementSize = plane->elementSize;
        }

        // Vulkan can't really express row pitch in the same way that client APIs
        // may expect, so we'll need to do some heroics here and hope that it works
        VkExtent3D blockCount = util::computeBlockCount(copyRegion.imageExtent, formatInfo->blockSize);
        VkDeviceSize rowPitch = blockCount.width * elementSize;

        if (bufferRowAlignment > elementSize)
          rowPitch = bufferRowAlignment >= rowPitch ? bufferRowAlignment : align(rowPitch, bufferRowAlignment);

        VkDeviceSize slicePitch = blockCount.height * rowPitch;

        if (image->info().type == VK_IMAGE_TYPE_3D && bufferSliceAlignment > elementSize)
          slicePitch = bufferSliceAlignment >= slicePitch ? bufferSliceAlignment : align(slicePitch, bufferSliceAlignment);

        copyRegion.bufferOffset      = aspectOffset;
        copyRegion.bufferRowLength   = formatInfo->blockSize.width * rowPitch / elementSize;
        copyRegion.bufferImageHeight = formatInfo->blockSize.height * slicePitch / rowPitch;

        // Perform the actual copy
        if constexpr (ToImage) {
          m_cmd->cmdCopyBufferToImage(cmd, bufferSlice.handle,
            image->handle(), imageLayout, 1, &copyRegion);
        } else {
          m_cmd->cmdCopyImageToBuffer(cmd, image->handle(), imageLayout,
            bufferSlice.handle, 1, &copyRegion);
        }

        aspectOffset += blockCount.depth * slicePitch;
      }

      // Advance to next layer. This is non-trivial for multi-plane formats
      // since plane data for each layer is expected to be packed.
      VkDeviceSize layerPitch = aspectOffset - bufferOffset;

      if (bufferSliceAlignment)
        layerPitch = bufferSliceAlignment >= layerPitch ? bufferSliceAlignment : align(layerPitch, bufferSliceAlignment);

      bufferOffset += layerPitch;
    }
  }


  void DxvkContext::copyImageHostData(
          DxvkCmdBuffer         cmd,
    const Rc<DxvkImage>&        image,
    const VkImageSubresourceLayers& imageSubresource,
          VkOffset3D            imageOffset,
          VkExtent3D            imageExtent,
    const void*                 hostData,
          VkDeviceSize          rowPitch,
          VkDeviceSize          slicePitch) {
    auto formatInfo = image->formatInfo();
    auto srcData = reinterpret_cast<const char*>(hostData);

    for (uint32_t i = 0; i < imageSubresource.layerCount; i++) {
      auto layerData = srcData + i * slicePitch;

      for (auto aspects = imageSubresource.aspectMask; aspects; ) {
        auto aspect = vk::getNextAspect(aspects);
        auto extent = imageExtent;

        VkDeviceSize elementSize = formatInfo->elementSize;

        if (formatInfo->flags.test(DxvkFormatFlag::MultiPlane)) {
          auto plane = &formatInfo->planes[vk::getPlaneIndex(aspect)];
          extent.width  /= plane->blockSize.width;
          extent.height /= plane->blockSize.height;
          elementSize = plane->elementSize;
        }

        auto blockCount = util::computeBlockCount(extent, formatInfo->blockSize);
        auto stagingSlice  = m_staging.alloc(CACHE_LINE_SIZE, elementSize * util::flattenImageExtent(blockCount));
        auto stagingHandle = stagingSlice.getSliceHandle();

        util::packImageData(stagingHandle.mapPtr, layerData,
          blockCount, elementSize, rowPitch, slicePitch);
        if (winehuaFlushDynamicMapped())
          this->flushMappedBuffer(stagingSlice.buffer(), stagingHandle);

        auto subresource = imageSubresource;
        subresource.aspectMask = aspect;

        this->copyImageBufferData<true>(cmd,
          image, subresource, imageOffset, imageExtent,
          image->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
          stagingHandle, 0, 0);

        layerData += blockCount.height * rowPitch;

        m_cmd->trackResource<DxvkAccess::Read>(stagingSlice.buffer());
      }
    }
  }


  void DxvkContext::clearImageViewFb(
    const Rc<DxvkImageView>&    imageView,
          VkOffset3D            offset,
          VkExtent3D            extent,
          VkImageAspectFlags    aspect,
          VkClearValue          value) {
    this->updateFramebuffer();

    // Find out if the render target view is currently bound,
    // so that we can avoid spilling the render pass if it is.
    int32_t attachmentIndex = -1;
    
    if (m_state.om.framebufferInfo.isFullSize(imageView))
      attachmentIndex = m_state.om.framebufferInfo.findAttachment(imageView);

    if (attachmentIndex >= 0 && !m_state.om.framebufferInfo.isWritable(attachmentIndex, aspect))
      attachmentIndex = -1;

    if (attachmentIndex < 0) {
      this->spillRenderPass(false);

      if (m_execBarriers.isImageDirty(
          imageView->image(),
          imageView->imageSubresources(),
          DxvkAccess::Write))
        m_execBarriers.recordCommands(m_cmd);
      
      // Set up a temporary framebuffer
      DxvkRenderTargets attachments;
      DxvkRenderPassOps ops;

      VkPipelineStageFlags clearStages = 0;
      VkAccessFlags        clearAccess = 0;
      
      if (imageView->info().aspect & VK_IMAGE_ASPECT_COLOR_BIT) {
        clearStages |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        clearAccess |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                    |  VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;

        attachments.color[0].view   = imageView;
        attachments.color[0].layout = imageView->pickLayout(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

        ops.colorOps[0].loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
        ops.colorOps[0].loadLayout  = imageView->imageInfo().layout;
        ops.colorOps[0].storeLayout = imageView->imageInfo().layout;
      } else {
        clearStages |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                    |  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        clearAccess |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                    |  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;

        attachments.depth.view   = imageView;
        attachments.depth.layout = imageView->pickLayout(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

        ops.depthOps.loadOpD     = VK_ATTACHMENT_LOAD_OP_LOAD;
        ops.depthOps.loadOpS     = VK_ATTACHMENT_LOAD_OP_LOAD;
        ops.depthOps.loadLayout  = imageView->imageInfo().layout;
        ops.depthOps.storeLayout = imageView->imageInfo().layout;
      }

      ops.barrier.srcStages = clearStages;
      ops.barrier.srcAccess = clearAccess;
      ops.barrier.dstStages = imageView->imageInfo().stages;
      ops.barrier.dstAccess = imageView->imageInfo().access;

      // We cannot leverage render pass clears
      // because we clear only part of the view
      this->renderPassBindFramebuffer(makeFramebufferInfo(attachments), ops, 0, nullptr);
    } else {
      // Make sure the render pass is active so
      // that we can actually perform the clear
      this->startRenderPass();
    }

    // Perform the actual clear operation
    VkClearAttachment clearInfo;
    clearInfo.aspectMask          = aspect;
    clearInfo.colorAttachment     = 0;
    clearInfo.clearValue          = value;

    if ((aspect & VK_IMAGE_ASPECT_COLOR_BIT) && (attachmentIndex >= 0))
      clearInfo.colorAttachment   = m_state.om.framebufferInfo.getColorAttachmentIndex(attachmentIndex);

    VkClearRect clearRect;
    clearRect.rect.offset.x       = offset.x;
    clearRect.rect.offset.y       = offset.y;
    clearRect.rect.extent.width   = extent.width;
    clearRect.rect.extent.height  = extent.height;
    clearRect.baseArrayLayer      = 0;
    clearRect.layerCount          = imageView->info().numLayers;

    m_cmd->cmdClearAttachments(1, &clearInfo, 1, &clearRect);

    // Unbind temporary framebuffer
    if (attachmentIndex < 0)
      this->renderPassUnbindFramebuffer();
  }

  
  void DxvkContext::clearImageViewCs(
    const Rc<DxvkImageView>&    imageView,
          VkOffset3D            offset,
          VkExtent3D            extent,
          VkClearValue          value) {
    this->spillRenderPass(false);
    this->unbindComputePipeline();
    
    if (m_execBarriers.isImageDirty(
          imageView->image(),
          imageView->imageSubresources(),
          DxvkAccess::Write))
      m_execBarriers.recordCommands(m_cmd);
    
    // Query pipeline objects to use for this clear operation
    DxvkMetaClearPipeline pipeInfo = m_common->metaClear().getClearImagePipeline(
      imageView->type(), imageFormatInfo(imageView->info().format)->flags);
    
    // Create a descriptor set pointing to the view
    VkDescriptorSet descriptorSet = allocateDescriptorSet(pipeInfo.dsetLayout);
    
    VkDescriptorImageInfo viewInfo;
    viewInfo.sampler      = VK_NULL_HANDLE;
    viewInfo.imageView    = imageView->handle();
    viewInfo.imageLayout  = imageView->imageInfo().layout;
    
    VkWriteDescriptorSet descriptorWrite;
    descriptorWrite.sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrite.pNext            = nullptr;
    descriptorWrite.dstSet           = descriptorSet;
    descriptorWrite.dstBinding       = 0;
    descriptorWrite.dstArrayElement  = 0;
    descriptorWrite.descriptorCount  = 1;
    descriptorWrite.descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    descriptorWrite.pImageInfo       = &viewInfo;
    descriptorWrite.pBufferInfo      = nullptr;
    descriptorWrite.pTexelBufferView = nullptr;
    m_cmd->updateDescriptorSets(1, &descriptorWrite);
    
    // Prepare shader arguments
    DxvkMetaClearArgs pushArgs = { };
    pushArgs.clearValue = value.color;
    pushArgs.offset = offset;
    pushArgs.extent = extent;
    
    VkExtent3D workgroups = util::computeBlockCount(
      pushArgs.extent, pipeInfo.workgroupSize);
    
    if (imageView->type() == VK_IMAGE_VIEW_TYPE_1D_ARRAY)
      workgroups.height = imageView->subresources().layerCount;
    else if (imageView->type() == VK_IMAGE_VIEW_TYPE_2D_ARRAY)
      workgroups.depth = imageView->subresources().layerCount;
    
    m_cmd->cmdBindPipeline(
      VK_PIPELINE_BIND_POINT_COMPUTE,
      pipeInfo.pipeline);
    m_cmd->cmdBindDescriptorSet(
      VK_PIPELINE_BIND_POINT_COMPUTE,
      pipeInfo.pipeLayout, descriptorSet,
      0, nullptr);
    m_cmd->cmdPushConstants(
      pipeInfo.pipeLayout,
      VK_SHADER_STAGE_COMPUTE_BIT,
      0, sizeof(pushArgs), &pushArgs);
    m_cmd->cmdDispatch(
      workgroups.width,
      workgroups.height,
      workgroups.depth);
    
    m_execBarriers.accessImage(
      imageView->image(),
      imageView->imageSubresources(),
      imageView->imageInfo().layout,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_WRITE_BIT,
      imageView->imageInfo().layout,
      imageView->imageInfo().stages,
      imageView->imageInfo().access);
    
    m_cmd->trackResource<DxvkAccess::None>(imageView);
    m_cmd->trackResource<DxvkAccess::Write>(imageView->image());
  }

  
  void DxvkContext::copyImageHw(
    const Rc<DxvkImage>&        dstImage,
          VkImageSubresourceLayers dstSubresource,
          VkOffset3D            dstOffset,
    const Rc<DxvkImage>&        srcImage,
          VkImageSubresourceLayers srcSubresource,
          VkOffset3D            srcOffset,
          VkExtent3D            extent) {
    auto dstSubresourceRange = vk::makeSubresourceRange(dstSubresource);
    auto srcSubresourceRange = vk::makeSubresourceRange(srcSubresource);

    auto dstFormatInfo = dstImage->formatInfo();

    if (m_execBarriers.isImageDirty(dstImage, dstSubresourceRange, DxvkAccess::Write)
     || m_execBarriers.isImageDirty(srcImage, srcSubresourceRange, DxvkAccess::Write))
      m_execBarriers.recordCommands(m_cmd);

    VkImageLayout dstImageLayout = dstImage->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageLayout srcImageLayout = srcImage->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkImageLayout dstInitImageLayout = dstImage->info().layout;

    if (dstImage->isFullSubresource(dstSubresource, extent))
      dstInitImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (dstImageLayout != dstInitImageLayout) {
      m_execAcquires.accessImage(
        dstImage, dstSubresourceRange,
        dstInitImageLayout,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
        dstImageLayout,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT);
    }

    if (srcImageLayout != srcImage->info().layout) {
      m_execAcquires.accessImage(
        srcImage, srcSubresourceRange,
        srcImage->info().layout,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
        srcImageLayout,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_TRANSFER_READ_BIT);
    }

    m_execAcquires.recordCommands(m_cmd);
    
    for (auto aspects = dstSubresource.aspectMask; aspects; ) {
      auto aspect = vk::getNextAspect(aspects);

      VkImageCopy imageRegion;
      imageRegion.srcSubresource = srcSubresource;
      imageRegion.srcSubresource.aspectMask = aspect;
      imageRegion.srcOffset      = srcOffset;
      imageRegion.dstSubresource = dstSubresource;
      imageRegion.dstSubresource.aspectMask = aspect;
      imageRegion.dstOffset      = dstOffset;
      imageRegion.extent         = extent;

      if (dstFormatInfo->flags.test(DxvkFormatFlag::MultiPlane)) {
        auto plane = &dstFormatInfo->planes[vk::getPlaneIndex(aspect)];
        imageRegion.srcOffset.x /= plane->blockSize.width;
        imageRegion.srcOffset.y /= plane->blockSize.height;
        imageRegion.dstOffset.x /= plane->blockSize.width;
        imageRegion.dstOffset.y /= plane->blockSize.height;
        imageRegion.extent.width  /= plane->blockSize.width;
        imageRegion.extent.height /= plane->blockSize.height;
      }

      m_cmd->cmdCopyImage(DxvkCmdBuffer::ExecBuffer,
        srcImage->handle(), srcImageLayout,
        dstImage->handle(), dstImageLayout,
        1, &imageRegion);
    }
    
    m_execBarriers.accessImage(
      dstImage, dstSubresourceRange,
      dstImageLayout,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      dstImage->info().layout,
      dstImage->info().stages,
      dstImage->info().access);

    m_execBarriers.accessImage(
      srcImage, srcSubresourceRange,
      srcImageLayout,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_READ_BIT,
      srcImage->info().layout,
      srcImage->info().stages,
      srcImage->info().access);
    
    m_cmd->trackResource<DxvkAccess::Write>(dstImage);
    m_cmd->trackResource<DxvkAccess::Read>(srcImage);
  }

  
  void DxvkContext::copyImageFb(
    const Rc<DxvkImage>&        dstImage,
          VkImageSubresourceLayers dstSubresource,
          VkOffset3D            dstOffset,
    const Rc<DxvkImage>&        srcImage,
          VkImageSubresourceLayers srcSubresource,
          VkOffset3D            srcOffset,
          VkExtent3D            extent) {
    auto dstSubresourceRange = vk::makeSubresourceRange(dstSubresource);
    auto srcSubresourceRange = vk::makeSubresourceRange(srcSubresource);
    
    if (m_execBarriers.isImageDirty(dstImage, dstSubresourceRange, DxvkAccess::Write)
     || m_execBarriers.isImageDirty(srcImage, srcSubresourceRange, DxvkAccess::Write))
      m_execBarriers.recordCommands(m_cmd);

    // Source image needs to be readable
    if (!(srcImage->info().usage & VK_IMAGE_USAGE_SAMPLED_BIT)) {
      Logger::err("DxvkContext: copyImageFb: Source image not readable");
      return;
    }

    // Render target format to use for this copy
    VkFormat viewFormat = m_common->metaCopy().getCopyDestinationFormat(
      dstSubresource.aspectMask,
      srcSubresource.aspectMask,
      srcImage->info().format);
    
    if (viewFormat == VK_FORMAT_UNDEFINED) {
      Logger::err("DxvkContext: copyImageFb: Unsupported format");
      return;
    }
    
    // We might have to transition the source image layout
    VkImageLayout srcLayout = (srcSubresource.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT)
      ? srcImage->pickLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
      : srcImage->pickLayout(VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    
    if (srcImage->info().layout != srcLayout) {
      m_execAcquires.accessImage(
        srcImage, srcSubresourceRange,
        srcImage->info().layout,
        srcImage->info().stages, 0,
        srcLayout,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT);
      
      m_execAcquires.recordCommands(m_cmd);
    }

    // In some cases, we may be able to render to the destination
    // image directly, which is faster than using a temporary image
    VkImageUsageFlagBits tgtUsage = (dstSubresource.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT)
      ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
      : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

    bool useDirectRender = (dstImage->isViewCompatible(viewFormat))
                        && (dstImage->info().usage & tgtUsage);
    
    // If needed, create a temporary render target for the copy
    Rc<DxvkImage>            tgtImage;
    VkImageSubresourceLayers tgtSubresource = dstSubresource;
    VkOffset3D               tgtOffset      = dstOffset;

    if (!useDirectRender) {
      DxvkImageCreateInfo info;
      info.type           = dstImage->info().type;
      info.format         = viewFormat;
      info.flags          = 0;
      info.sampleCount    = dstImage->info().sampleCount;
      info.extent         = extent;
      info.numLayers      = dstSubresource.layerCount;
      info.mipLevels      = 1;
      info.usage          = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | tgtUsage;
      info.stages         = VK_PIPELINE_STAGE_TRANSFER_BIT;
      info.access         = VK_ACCESS_TRANSFER_READ_BIT;
      info.tiling         = VK_IMAGE_TILING_OPTIMAL;
      info.layout         = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

      tgtImage = m_device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

      tgtSubresource.mipLevel       = 0;
      tgtSubresource.baseArrayLayer = 0;

      tgtOffset = { 0, 0, 0 };
    } else {
      tgtImage = dstImage;
    }
    
    // Create source and destination image views
    VkImageViewType viewType = dstImage->info().type == VK_IMAGE_TYPE_1D
      ? VK_IMAGE_VIEW_TYPE_1D_ARRAY
      : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    
    DxvkImageViewCreateInfo tgtViewInfo;
    tgtViewInfo.type      = viewType;
    tgtViewInfo.format    = viewFormat;
    tgtViewInfo.usage     = tgtUsage;
    tgtViewInfo.aspect    = tgtSubresource.aspectMask;
    tgtViewInfo.minLevel  = tgtSubresource.mipLevel;
    tgtViewInfo.numLevels = 1;
    tgtViewInfo.minLayer  = tgtSubresource.baseArrayLayer;
    tgtViewInfo.numLayers = tgtSubresource.layerCount;

    DxvkImageViewCreateInfo srcViewInfo;
    srcViewInfo.type      = viewType;
    srcViewInfo.format    = srcImage->info().format;
    srcViewInfo.usage     = VK_IMAGE_USAGE_SAMPLED_BIT;
    srcViewInfo.aspect    = srcSubresource.aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_COLOR_BIT);
    srcViewInfo.minLevel  = srcSubresource.mipLevel;
    srcViewInfo.numLevels = 1;
    srcViewInfo.minLayer  = srcSubresource.baseArrayLayer;
    srcViewInfo.numLayers = srcSubresource.layerCount;

    Rc<DxvkImageView> tgtImageView = m_device->createImageView(tgtImage, tgtViewInfo);
    Rc<DxvkImageView> srcImageView = m_device->createImageView(srcImage, srcViewInfo);
    Rc<DxvkImageView> srcStencilView;

    if (srcSubresource.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) {
      srcViewInfo.aspect = VK_IMAGE_ASPECT_STENCIL_BIT;
      srcStencilView = m_device->createImageView(srcImage, srcViewInfo);
    }

    // Create framebuffer and pipeline for the copy
    Rc<DxvkMetaCopyRenderPass> fb = new DxvkMetaCopyRenderPass(
      m_device->vkd(), tgtImageView, srcImageView, srcStencilView,
      tgtImage->isFullSubresource(tgtSubresource, extent));
    
    auto pipeInfo = m_common->metaCopy().getPipeline(
      viewType, viewFormat, tgtImage->info().sampleCount);
    
    VkDescriptorImageInfo descriptorImage;
    descriptorImage.sampler          = VK_NULL_HANDLE;
    descriptorImage.imageView        = srcImageView->handle();
    descriptorImage.imageLayout      = srcLayout;

    VkWriteDescriptorSet descriptorWrite;
    descriptorWrite.sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrite.pNext            = nullptr;
    descriptorWrite.dstBinding       = 0;
    descriptorWrite.dstArrayElement  = 0;
    descriptorWrite.descriptorCount  = 1;
    descriptorWrite.descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrite.pImageInfo       = &descriptorImage;
    descriptorWrite.pBufferInfo      = nullptr;
    descriptorWrite.pTexelBufferView = nullptr;
    
    descriptorWrite.dstSet = allocateDescriptorSet(pipeInfo.dsetLayout);
    m_cmd->updateDescriptorSets(1, &descriptorWrite);

    if (srcSubresource.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) {
      descriptorImage.imageView  = srcStencilView->handle();
      descriptorWrite.dstBinding = 1;
      m_cmd->updateDescriptorSets(1, &descriptorWrite);
    }
    
    VkViewport viewport;
    viewport.x        = float(tgtOffset.x);
    viewport.y        = float(tgtOffset.y);
    viewport.width    = float(extent.width);
    viewport.height   = float(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor;
    scissor.offset    = { tgtOffset.x, tgtOffset.y };
    scissor.extent    = { extent.width, extent.height };

    VkRenderPassBeginInfo info;
    info.sType              = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    info.pNext              = nullptr;
    info.renderPass         = fb->renderPass();
    info.framebuffer        = fb->framebuffer();
    info.renderArea.offset  = { 0, 0 };
    info.renderArea.extent  = {
      tgtImage->mipLevelExtent(tgtSubresource.mipLevel).width,
      tgtImage->mipLevelExtent(tgtSubresource.mipLevel).height };
    info.clearValueCount    = 0;
    info.pClearValues       = nullptr;

    // Perform the actual copy operation
    m_cmd->cmdBeginRenderPass(&info, VK_SUBPASS_CONTENTS_INLINE);
    m_cmd->cmdBindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeInfo.pipeHandle);
    m_cmd->cmdBindDescriptorSet(VK_PIPELINE_BIND_POINT_GRAPHICS,
      pipeInfo.pipeLayout, descriptorWrite.dstSet, 0, nullptr);

    m_cmd->cmdSetViewport(0, 1, &viewport);
    m_cmd->cmdSetScissor (0, 1, &scissor);

    VkOffset2D srcCoordOffset = {
      srcOffset.x - tgtOffset.x,
      srcOffset.y - tgtOffset.y };
    
    m_cmd->cmdPushConstants(pipeInfo.pipeLayout,
      VK_SHADER_STAGE_FRAGMENT_BIT,
      0, sizeof(srcCoordOffset),
      &srcCoordOffset);
    
    m_cmd->cmdDraw(3, tgtSubresource.layerCount, 0, 0);
    m_cmd->cmdEndRenderPass();

    if (srcLayout != srcImage->info().layout) {
      m_execBarriers.accessImage(
        srcImage, srcSubresourceRange, srcLayout,
        srcImage->info().stages,
        srcImage->info().access,
        srcImage->info().layout,
        srcImage->info().stages,
        srcImage->info().access);
    }

    m_cmd->trackResource<DxvkAccess::Write>(tgtImage);
    m_cmd->trackResource<DxvkAccess::Read>(srcImage);
    m_cmd->trackResource<DxvkAccess::None>(fb);
    
    // If necessary, copy the temporary image
    // to the original destination image
    if (!useDirectRender) {
      this->copyImageHw(
        dstImage, dstSubresource, dstOffset,
        tgtImage, tgtSubresource, tgtOffset,
        extent);
    }
  }


  bool DxvkContext::copyImageClear(
    const Rc<DxvkImage>&        dstImage,
          VkImageSubresourceLayers dstSubresource,
          VkOffset3D            dstOffset,
          VkExtent3D            dstExtent,
    const Rc<DxvkImage>&        srcImage,
          VkImageSubresourceLayers srcSubresource) {
    // If the source image has a pending deferred clear, we can
    // implement the copy by clearing the destination image to
    // the same clear value.
    const VkImageUsageFlags attachmentUsage
      = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
      | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

    if (!(dstImage->info().usage & attachmentUsage)
     || !(srcImage->info().usage & attachmentUsage))
      return false;

    // Ignore 3D images since those are complicated to handle
    if (dstImage->info().type == VK_IMAGE_TYPE_3D
     || srcImage->info().type == VK_IMAGE_TYPE_3D)
      return false;

    // Find a pending clear that overlaps with the source image
    const DxvkDeferredClear* clear = nullptr;

    for (const auto& entry : m_deferredClears) {
      // Entries in the deferred clear array cannot overlap, so
      // if we find an entry covering all source subresources,
      // it's the only one in the list that does.
      if ((entry.imageView->image() == srcImage) && ((srcSubresource.aspectMask & entry.clearAspects) == srcSubresource.aspectMask)
       && (vk::checkSubresourceRangeSuperset(entry.imageView->subresources(), vk::makeSubresourceRange(srcSubresource)))) {
        clear = &entry;
        break;
      }
    }

    if (!clear)
      return false;

    // Create a view for the destination image with the general
    // properties ofthe source image view used for the clear
    DxvkImageViewCreateInfo viewInfo = clear->imageView->info();
    viewInfo.type = dstImage->info().type == VK_IMAGE_TYPE_1D
      ? VK_IMAGE_VIEW_TYPE_1D_ARRAY
      : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.minLevel = dstSubresource.mipLevel;
    viewInfo.numLevels = 1;
    viewInfo.minLayer = dstSubresource.baseArrayLayer;
    viewInfo.numLayers = dstSubresource.layerCount;

    // That is, if the formats are actually compatible
    // so that we can safely use the same clear value
    if (!dstImage->isViewCompatible(viewInfo.format))
      return false;

    // Ignore mismatched size for now, needs more testing since we'd
    // need to prepare the image first and then call clearImageViewFb
    if (dstImage->mipLevelExtent(dstSubresource.mipLevel) != dstExtent)
      return false;

    auto view = m_device->createImageView(dstImage, viewInfo);
    this->deferClear(view, srcSubresource.aspectMask, clear->clearValue);
    return true;
  }


  void DxvkContext::resolveImageHw(
    const Rc<DxvkImage>&            dstImage,
    const Rc<DxvkImage>&            srcImage,
    const VkImageResolve&           region) {
    auto dstSubresourceRange = vk::makeSubresourceRange(region.dstSubresource);
    auto srcSubresourceRange = vk::makeSubresourceRange(region.srcSubresource);
    
    if (m_execBarriers.isImageDirty(dstImage, dstSubresourceRange, DxvkAccess::Write)
     || m_execBarriers.isImageDirty(srcImage, srcSubresourceRange, DxvkAccess::Write))
      m_execBarriers.recordCommands(m_cmd);
    
    // We only support resolving to the entire image
    // area, so we might as well discard its contents
    VkImageLayout dstLayout = dstImage->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageLayout srcLayout = srcImage->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkImageLayout initialLayout = dstImage->info().layout;

    if (dstImage->isFullSubresource(region.dstSubresource, region.extent))
      initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (dstLayout != initialLayout) {
      m_execAcquires.accessImage(
        dstImage, dstSubresourceRange, initialLayout,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
        dstLayout,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT);
    }

    if (srcLayout != srcImage->info().layout) {
      m_execAcquires.accessImage(
        srcImage, srcSubresourceRange,
        srcImage->info().layout,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
        srcLayout,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_TRANSFER_READ_BIT);
    }

    m_execAcquires.recordCommands(m_cmd);
    
    m_cmd->cmdResolveImage(
      srcImage->handle(), srcLayout,
      dstImage->handle(), dstLayout,
      1, &region);
  
    m_execBarriers.accessImage(
      dstImage, dstSubresourceRange, dstLayout,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      dstImage->info().layout,
      dstImage->info().stages,
      dstImage->info().access);

    m_execBarriers.accessImage(
      srcImage, srcSubresourceRange, srcLayout,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_READ_BIT,
      srcImage->info().layout,
      srcImage->info().stages,
      srcImage->info().access);
    
    m_cmd->trackResource<DxvkAccess::Write>(dstImage);
    m_cmd->trackResource<DxvkAccess::Read>(srcImage);
  }


  void DxvkContext::resolveImageDs(
    const Rc<DxvkImage>&            dstImage,
    const Rc<DxvkImage>&            srcImage,
    const VkImageResolve&           region,
          VkResolveModeFlagBitsKHR  depthMode,
          VkResolveModeFlagBitsKHR  stencilMode) {
    auto dstSubresourceRange = vk::makeSubresourceRange(region.dstSubresource);
    auto srcSubresourceRange = vk::makeSubresourceRange(region.srcSubresource);

    if (m_execBarriers.isImageDirty(dstImage, dstSubresourceRange, DxvkAccess::Write)
     || m_execBarriers.isImageDirty(srcImage, srcSubresourceRange, DxvkAccess::Write))
      m_execBarriers.recordCommands(m_cmd);
    
    // Create image views covering the requested subresourcs
    DxvkImageViewCreateInfo dstViewInfo;
    dstViewInfo.type      = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    dstViewInfo.format    = dstImage->info().format;
    dstViewInfo.usage     = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    dstViewInfo.aspect    = region.dstSubresource.aspectMask;
    dstViewInfo.minLevel  = region.dstSubresource.mipLevel;
    dstViewInfo.numLevels = 1;
    dstViewInfo.minLayer  = region.dstSubresource.baseArrayLayer;
    dstViewInfo.numLayers = region.dstSubresource.layerCount;

    DxvkImageViewCreateInfo srcViewInfo;
    srcViewInfo.type      = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    srcViewInfo.format    = srcImage->info().format;
    srcViewInfo.usage     = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    srcViewInfo.aspect    = region.srcSubresource.aspectMask;
    srcViewInfo.minLevel  = region.srcSubresource.mipLevel;
    srcViewInfo.numLevels = 1;
    srcViewInfo.minLayer  = region.srcSubresource.baseArrayLayer;
    srcViewInfo.numLayers = region.srcSubresource.layerCount;

    Rc<DxvkImageView> dstImageView = m_device->createImageView(dstImage, dstViewInfo);
    Rc<DxvkImageView> srcImageView = m_device->createImageView(srcImage, srcViewInfo);

    // Create a framebuffer for the resolve op
    VkExtent3D passExtent = dstImageView->mipLevelExtent(0);

    Rc<DxvkMetaResolveRenderPass> fb = new DxvkMetaResolveRenderPass(
      m_device->vkd(), dstImageView, srcImageView, depthMode, stencilMode);

    VkRenderPassBeginInfo info;
    info.sType              = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    info.pNext              = nullptr;
    info.renderPass         = fb->renderPass();
    info.framebuffer        = fb->framebuffer();
    info.renderArea.offset  = { 0, 0 };
    info.renderArea.extent  = { passExtent.width, passExtent.height };
    info.clearValueCount    = 0;
    info.pClearValues       = nullptr;

    m_cmd->cmdBeginRenderPass(&info, VK_SUBPASS_CONTENTS_INLINE);
    m_cmd->cmdEndRenderPass();

    m_cmd->trackResource<DxvkAccess::Write>(dstImage);
    m_cmd->trackResource<DxvkAccess::Read>(srcImage);
    m_cmd->trackResource<DxvkAccess::None>(fb);
  }


  void DxvkContext::resolveImageFb(
    const Rc<DxvkImage>&            dstImage,
    const Rc<DxvkImage>&            srcImage,
    const VkImageResolve&           region,
          VkFormat                  format,
          VkResolveModeFlagBitsKHR  depthMode,
          VkResolveModeFlagBitsKHR  stencilMode) {
    auto dstSubresourceRange = vk::makeSubresourceRange(region.dstSubresource);
    auto srcSubresourceRange = vk::makeSubresourceRange(region.srcSubresource);
    
    if (m_execBarriers.isImageDirty(dstImage, dstSubresourceRange, DxvkAccess::Write)
     || m_execBarriers.isImageDirty(srcImage, srcSubresourceRange, DxvkAccess::Write))
      m_execBarriers.recordCommands(m_cmd);

    // We might have to transition the source image layout
    VkImageLayout srcLayout = srcImage->pickLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    
    if (srcImage->info().layout != srcLayout) {
      m_execAcquires.accessImage(
        srcImage, srcSubresourceRange,
        srcImage->info().layout,
        srcImage->info().stages, 0,
        srcLayout,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT);
      
      m_execAcquires.recordCommands(m_cmd);
    }

    // Create image views covering the requested subresourcs
    DxvkImageViewCreateInfo dstViewInfo;
    dstViewInfo.type      = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    dstViewInfo.format    = format ? format : dstImage->info().format;
    dstViewInfo.usage     = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    dstViewInfo.aspect    = region.dstSubresource.aspectMask;
    dstViewInfo.minLevel  = region.dstSubresource.mipLevel;
    dstViewInfo.numLevels = 1;
    dstViewInfo.minLayer  = region.dstSubresource.baseArrayLayer;
    dstViewInfo.numLayers = region.dstSubresource.layerCount;

    if (region.dstSubresource.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT)
      dstViewInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

    DxvkImageViewCreateInfo srcViewInfo;
    srcViewInfo.type      = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    srcViewInfo.format    = format ? format : srcImage->info().format;
    srcViewInfo.usage     = VK_IMAGE_USAGE_SAMPLED_BIT;
    srcViewInfo.aspect    = region.srcSubresource.aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_COLOR_BIT);
    srcViewInfo.minLevel  = region.srcSubresource.mipLevel;
    srcViewInfo.numLevels = 1;
    srcViewInfo.minLayer  = region.srcSubresource.baseArrayLayer;
    srcViewInfo.numLayers = region.srcSubresource.layerCount;

    Rc<DxvkImageView> dstImageView = m_device->createImageView(dstImage, dstViewInfo);
    Rc<DxvkImageView> srcImageView = m_device->createImageView(srcImage, srcViewInfo);
    Rc<DxvkImageView> srcStencilView = nullptr;

    if ((region.dstSubresource.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) && stencilMode != VK_RESOLVE_MODE_NONE_KHR) {
      srcViewInfo.aspect  = VK_IMAGE_ASPECT_STENCIL_BIT;
      srcStencilView = m_device->createImageView(srcImage, srcViewInfo);
    }

    // Create a framebuffer and pipeline for the resolve op
    VkExtent3D passExtent = dstImageView->mipLevelExtent(0);

    Rc<DxvkMetaResolveRenderPass> fb = new DxvkMetaResolveRenderPass(
      m_device->vkd(), dstImageView, srcImageView, srcStencilView,
      dstImage->isFullSubresource(region.dstSubresource, region.extent));

    auto pipeInfo = m_common->metaResolve().getPipeline(
      dstViewInfo.format, srcImage->info().sampleCount, depthMode, stencilMode);
    
    VkDescriptorImageInfo descriptorImage;
    descriptorImage.sampler          = VK_NULL_HANDLE;
    descriptorImage.imageView        = srcImageView->handle();
    descriptorImage.imageLayout      = srcLayout;

    VkWriteDescriptorSet descriptorWrite;
    descriptorWrite.sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrite.pNext            = nullptr;
    descriptorWrite.dstBinding       = 0;
    descriptorWrite.dstArrayElement  = 0;
    descriptorWrite.descriptorCount  = 1;
    descriptorWrite.descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrite.pImageInfo       = &descriptorImage;
    descriptorWrite.pBufferInfo      = nullptr;
    descriptorWrite.pTexelBufferView = nullptr;
    
    descriptorWrite.dstSet = allocateDescriptorSet(pipeInfo.dsetLayout);
    m_cmd->updateDescriptorSets(1, &descriptorWrite);

    if (srcStencilView != nullptr) {
      descriptorWrite.dstBinding     = 1;
      descriptorImage.imageView      = srcStencilView->handle();
      m_cmd->updateDescriptorSets(1, &descriptorWrite);
    }

    VkViewport viewport;
    viewport.x        = float(region.dstOffset.x);
    viewport.y        = float(region.dstOffset.y);
    viewport.width    = float(region.extent.width);
    viewport.height   = float(region.extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor;
    scissor.offset    = { region.dstOffset.x,  region.dstOffset.y   };
    scissor.extent    = { region.extent.width, region.extent.height };

    VkRenderPassBeginInfo info;
    info.sType              = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    info.pNext              = nullptr;
    info.renderPass         = fb->renderPass();
    info.framebuffer        = fb->framebuffer();
    info.renderArea.offset  = { 0, 0 };
    info.renderArea.extent  = { passExtent.width, passExtent.height };
    info.clearValueCount    = 0;
    info.pClearValues       = nullptr;
    
    // Perform the actual resolve operation
    VkOffset2D srcOffset = {
      region.srcOffset.x - region.dstOffset.x,
      region.srcOffset.y - region.dstOffset.y };
    
    m_cmd->cmdBeginRenderPass(&info, VK_SUBPASS_CONTENTS_INLINE);
    m_cmd->cmdBindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeInfo.pipeHandle);
    m_cmd->cmdBindDescriptorSet(VK_PIPELINE_BIND_POINT_GRAPHICS,
      pipeInfo.pipeLayout, descriptorWrite.dstSet, 0, nullptr);
    m_cmd->cmdSetViewport(0, 1, &viewport);
    m_cmd->cmdSetScissor (0, 1, &scissor);
    m_cmd->cmdPushConstants(pipeInfo.pipeLayout,
      VK_SHADER_STAGE_FRAGMENT_BIT,
      0, sizeof(srcOffset), &srcOffset);
    m_cmd->cmdDraw(3, region.dstSubresource.layerCount, 0, 0);
    m_cmd->cmdEndRenderPass();

    if (srcImage->info().layout != srcLayout) {
      m_execBarriers.accessImage(
        srcImage, srcSubresourceRange, srcLayout,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
        srcImage->info().layout,
        srcImage->info().stages,
        srcImage->info().access);
    }
    
    m_cmd->trackResource<DxvkAccess::Write>(dstImage);
    m_cmd->trackResource<DxvkAccess::Read>(srcImage);
    m_cmd->trackResource<DxvkAccess::None>(fb);
  }


  void DxvkContext::startRenderPass() {
    if (!m_flags.test(DxvkContextFlag::GpRenderPassBound)) {
      this->applyRenderTargetLoadLayouts();
      this->flushClears(true);

      const auto& depthView = m_state.om.renderTargets.depth.view;
      if (winehuaSampleTraceEnabled() && winehuaTraceDepthView(depthView)) {
        const int32_t depthIndex = m_state.om.framebufferInfo.findAttachment(depthView);
        const VkClearDepthStencilValue clearValue = depthIndex >= 0
          ? m_state.om.clearValues[depthIndex].depthStencil
          : VkClearDepthStencilValue { 0.0f, 0 };
        winehuaSampleTrace(str::format(
          "depth-render-pass-begin cookie=", depthView->cookie(),
          " image=0x", std::hex, depthView->imageHandle(),
          " view=0x", depthView->handle(),
          " layout=", std::dec, m_state.om.renderTargets.depth.layout,
          " loadLayout=", m_state.om.renderPassOps.depthOps.loadLayout,
          " loadOpDepth=", m_state.om.renderPassOps.depthOps.loadOpD,
          " loadOpStencil=", m_state.om.renderPassOps.depthOps.loadOpS,
          " clearDepth=", clearValue.depth,
          " clearStencil=", clearValue.stencil,
          " attachmentIndex=", depthIndex));
      }

      m_flags.set(DxvkContextFlag::GpRenderPassBound);
      m_flags.clr(DxvkContextFlag::GpRenderPassSuspended);

      m_execBarriers.recordCommands(m_cmd);

      this->renderPassBindFramebuffer(
        m_state.om.framebufferInfo,
        m_state.om.renderPassOps,
        m_state.om.framebufferInfo.numAttachments(),
        m_state.om.clearValues.data());

      // Track the final layout of each render target
      this->applyRenderTargetStoreLayouts();

      // Don't discard image contents if we have
      // to spill the current render pass
      this->resetRenderPassOps(
        m_state.om.renderTargets,
        m_state.om.renderPassOps);
      
      // Begin occlusion queries
      m_queryManager.beginQueries(m_cmd, VK_QUERY_TYPE_OCCLUSION);
      m_queryManager.beginQueries(m_cmd, VK_QUERY_TYPE_PIPELINE_STATISTICS);
    }
  }
  
  
  void DxvkContext::spillRenderPass(bool suspend) {
    if (m_flags.test(DxvkContextFlag::GpRenderPassBound)) {
      m_flags.clr(DxvkContextFlag::GpRenderPassBound);

      this->pauseTransformFeedback();
      
      m_queryManager.endQueries(m_cmd, VK_QUERY_TYPE_OCCLUSION);
      m_queryManager.endQueries(m_cmd, VK_QUERY_TYPE_PIPELINE_STATISTICS);
      
      this->renderPassUnbindFramebuffer();

      if (suspend)
        m_flags.set(DxvkContextFlag::GpRenderPassSuspended);
      else
        this->transitionRenderTargetLayouts(m_gfxBarriers, false);

      m_gfxBarriers.recordCommands(m_cmd);

      if (winehuaRenderTargetDumpEnabled()
       && m_winehuaFrameId == winehuaRenderTargetDumpFrame()) {
        this->winehuaCaptureRenderPass(
          m_state.om.framebufferInfo,
          m_winehuaActivePassOps);
      }

      this->unbindGraphicsPipeline();
    } else if (!suspend) {
      // We may end a previously suspended render pass
      if (m_flags.test(DxvkContextFlag::GpRenderPassSuspended)) {
        m_flags.clr(DxvkContextFlag::GpRenderPassSuspended);
        this->transitionRenderTargetLayouts(m_gfxBarriers, false);
        m_gfxBarriers.recordCommands(m_cmd);
      }

      // Execute deferred clears if necessary
      this->flushClears(false);
    }
  }


  void DxvkContext::renderPassBindFramebuffer(
    const DxvkFramebufferInfo&  framebufferInfo,
    const DxvkRenderPassOps&    ops,
          uint32_t              clearValueCount,
    const VkClearValue*         clearValues) {
    const DxvkFramebufferSize fbSize = framebufferInfo.size();

    Rc<DxvkFramebuffer> framebuffer = this->lookupFramebuffer(framebufferInfo);

    VkRect2D renderArea;
    renderArea.offset = VkOffset2D { 0, 0 };
    renderArea.extent = VkExtent2D { fbSize.width, fbSize.height };
    
    VkRenderPassBeginInfo info;
    info.sType                = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    info.pNext                = nullptr;
    info.renderPass           = framebufferInfo.renderPass()->getHandle(ops);
    info.framebuffer          = framebuffer->handle();
    info.renderArea           = renderArea;
    info.clearValueCount      = clearValueCount;
    info.pClearValues         = clearValues;

    const bool dumpThisPass = winehuaRenderTargetDumpEnabled()
      && m_winehuaFrameId == winehuaRenderTargetDumpFrame();
    const bool traceThisPass = winehuaDrawTraceEnabled()
      && m_winehuaFrameId == winehuaRenderTargetDumpFrame();
    if (dumpThisPass || traceThisPass) {
      m_winehuaActivePassId = m_winehuaPassId++;
      m_winehuaPassDrawId = 0;
    }
    if (dumpThisPass) {
      m_winehuaActivePassOps = ops;
    }

    if (winehuaSampleTraceEnabled() || dumpThisPass || traceThisPass) {
      for (uint32_t i = 0; i < framebufferInfo.numAttachments(); i++) {
        const auto& attachment = framebufferInfo.getAttachment(i);
        if (attachment.view == nullptr)
          continue;

        const int32_t colorIndex = framebufferInfo.getColorAttachmentIndex(i);
        const bool isDepth = colorIndex < 0;
        const auto& viewInfo = attachment.view->info();
        const auto& imageInfo = attachment.view->imageInfo();

        const std::string message = str::format(
          "begin frame=", m_winehuaFrameId,
          " pass=", m_winehuaActivePassId,
          " size=", fbSize.width, "x", fbSize.height,
          " attachment=", i,
          " kind=", isDepth ? "depth" : "color",
          " colorIndex=", colorIndex,
          " viewCookie=", attachment.view->cookie(),
          " image=0x", std::hex, attachment.view->imageHandle(),
          " imageFormat=", std::dec, imageInfo.format,
          " viewFormat=", viewInfo.format,
          " aspect=0x", std::hex, viewInfo.aspect,
          " baseMip=", std::dec, viewInfo.minLevel,
          " mipCount=", viewInfo.numLevels,
          " baseLayer=", viewInfo.minLayer,
          " layerCount=", viewInfo.numLayers,
          " layout=", attachment.layout,
          " loadLayout=", isDepth ? ops.depthOps.loadLayout : ops.colorOps[colorIndex].loadLayout,
          " loadOp=", isDepth ? ops.depthOps.loadOpD : ops.colorOps[colorIndex].loadOp,
          " storeLayout=", isDepth ? ops.depthOps.storeLayout : ops.colorOps[colorIndex].storeLayout);

        if (winehuaSampleTraceEnabled())
          winehuaRenderPassTrace(message);
        if (dumpThisPass)
          Logger::info("WineHuaRenderTargetPass: " + message);
        if (traceThisPass)
          Logger::info("WineHuaDrawPass: " + message);
      }
    }
    
    m_cmd->cmdBeginRenderPass(&info,
      VK_SUBPASS_CONTENTS_INLINE);
    
    m_cmd->trackResource<DxvkAccess::None>(framebuffer);

    for (uint32_t i = 0; i < framebufferInfo.numAttachments(); i++) {
      m_cmd->trackResource<DxvkAccess::None> (framebufferInfo.getAttachment(i).view);
      m_cmd->trackResource<DxvkAccess::Write>(framebufferInfo.getAttachment(i).view->image());
    }

    m_cmd->addStatCtr(DxvkStatCounter::CmdRenderPassCount, 1);
  }
  
  
  void DxvkContext::renderPassUnbindFramebuffer() {
    m_cmd->cmdEndRenderPass();
  }


  void DxvkContext::winehuaCaptureRenderPass(
    const DxvkFramebufferInfo&  framebufferInfo,
    const DxvkRenderPassOps&    ops) {
    if (m_winehuaActivePassId < winehuaRenderTargetDumpFirstPass()
     || !winehuaRenderTargetDumpSelectPass(m_winehuaActivePassId))
      return;

    const std::string resourceViews = m_winehuaLastGraphicsResourceViews;

    const std::string vertexShader = m_state.gp.shaders.vs != nullptr
      ? m_state.gp.shaders.vs->debugName() : std::string();
    const std::string fragmentShader = m_state.gp.shaders.fs != nullptr
      ? m_state.gp.shaders.fs->debugName() : std::string();

    for (uint32_t i = 0; i < framebufferInfo.numAttachments(); i++) {
      if (m_winehuaRenderTargetDumps.size()
          >= winehuaRenderTargetDumpMaxAttachments()) {
        Logger::info(str::format(
          "WineHuaRenderTargetDump: attachment limit reached frame=",
          m_winehuaFrameId,
          " pass=", m_winehuaActivePassId));
        break;
      }

      const auto& attachment = framebufferInfo.getAttachment(i);
      if (attachment.view == nullptr)
        continue;

      const auto& viewInfo = attachment.view->info();
      const auto& imageInfo = attachment.view->imageInfo();
      const int32_t colorIndex = framebufferInfo.getColorAttachmentIndex(i);
      const bool isDepth = colorIndex < 0;

      WineHuaRenderTargetDump dump;
      dump.kind = isDepth ? "depth" : "color";
      dump.frameId = m_winehuaFrameId;
      dump.passId = m_winehuaActivePassId;
      dump.attachmentId = i;
      dump.colorIndex = colorIndex;
      dump.viewCookie = attachment.view->cookie();
      dump.imageHandle = attachment.view->imageHandle();
      dump.imageFormat = imageInfo.format;
      dump.viewFormat = viewInfo.format;
      dump.aspect = viewInfo.aspect;
      dump.layout = attachment.layout;
      dump.loadLayout = isDepth
        ? ops.depthOps.loadLayout : ops.colorOps[colorIndex].loadLayout;
      dump.loadOp = isDepth
        ? ops.depthOps.loadOpD : ops.colorOps[colorIndex].loadOp;
      dump.storeLayout = isDepth
        ? ops.depthOps.storeLayout : ops.colorOps[colorIndex].storeLayout;
      dump.extent = attachment.view->mipLevelExtent(0);
      dump.viewport = m_state.vp.viewports[0];
      dump.scissor = m_state.vp.scissorRects[0];
      dump.baseMip = viewInfo.minLevel;
      dump.baseLayer = imageInfo.type == VK_IMAGE_TYPE_3D
        ? 0u : viewInfo.minLayer;
      dump.layerCount = imageInfo.type == VK_IMAGE_TYPE_3D
        ? 1u : std::min(viewInfo.numLayers, 8u);
      dump.vertexShader = vertexShader;
      dump.fragmentShader = fragmentShader;
      dump.resourceViews = resourceViews;

      VkImageAspectFlags copyAspect = viewInfo.aspect;
      if (copyAspect & VK_IMAGE_ASPECT_COLOR_BIT)
        copyAspect = VK_IMAGE_ASPECT_COLOR_BIT;
      else if (copyAspect & VK_IMAGE_ASPECT_DEPTH_BIT)
        copyAspect = VK_IMAGE_ASPECT_DEPTH_BIT;
      else if (copyAspect & VK_IMAGE_ASPECT_STENCIL_BIT)
        copyAspect = VK_IMAGE_ASPECT_STENCIL_BIT;
      dump.aspect = copyAspect;

      const auto formatInfo = attachment.view->formatInfo();
      const auto blockCount = util::computeBlockCount(
        dump.extent, formatInfo->blockSize);
      dump.rowPitch = blockCount.width * formatInfo->elementSize;
      dump.slicePitch = blockCount.height * dump.rowPitch;
      dump.dataSize = blockCount.depth * dump.slicePitch * dump.layerCount;

      const bool captureSupported = imageInfo.sampleCount == VK_SAMPLE_COUNT_1_BIT
        && (imageInfo.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
        && !formatInfo->flags.test(DxvkFormatFlag::MultiPlane)
        && dump.extent.width && dump.extent.height && dump.dataSize
        && m_winehuaDumpBytes + dump.dataSize
            <= winehuaRenderTargetDumpMaxBytes();

      if (captureSupported) {
        DxvkBufferCreateInfo bufferInfo;
        bufferInfo.size = dump.dataSize;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
        bufferInfo.access = VK_ACCESS_TRANSFER_WRITE_BIT;

        dump.buffer = m_device->createBuffer(bufferInfo,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
        | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        VkImageSubresourceLayers subresource;
        subresource.aspectMask = copyAspect;
        subresource.mipLevel = dump.baseMip;
        subresource.baseArrayLayer = dump.baseLayer;
        subresource.layerCount = dump.layerCount;

        this->copyImageToBuffer(
          dump.buffer, 0, 0, 0,
          attachment.view->image(), subresource,
          VkOffset3D { 0, 0, 0 }, dump.extent);
        m_winehuaDumpBytes += dump.dataSize;
      } else {
        Logger::info(str::format(
          "WineHuaRenderTargetDump: metadata-only frame=", dump.frameId,
          " pass=", dump.passId,
          " attachment=", dump.attachmentId,
          " samples=", imageInfo.sampleCount,
          " usage=0x", std::hex, imageInfo.usage,
          " bytes=", std::dec, dump.dataSize));
      }

      m_winehuaRenderTargetDumps.push_back(std::move(dump));
    }

    if (!winehuaRenderTargetDumpSampledEnabled())
      return;

    for (const auto& resource : m_winehuaLastGraphicsImages) {
      bool duplicate = false;
      for (const auto& existing : m_winehuaRenderTargetDumps) {
        duplicate |= existing.passId == m_winehuaActivePassId
                  && existing.viewCookie == resource.view->cookie();
      }
      if (duplicate)
        continue;

      WineHuaRenderTargetDump dump;
      dump.kind = "sampled";
      dump.frameId = m_winehuaFrameId;
      dump.passId = m_winehuaActivePassId;
      dump.attachmentId = resource.binding;
      dump.colorIndex = -2;
      dump.descriptorBinding = resource.binding;
      dump.resourceSlot = resource.resourceSlot;
      dump.layout = resource.view->imageInfo().layout;
      dump.viewport = m_state.vp.viewports[0];
      dump.scissor = m_state.vp.scissorRects[0];
      dump.vertexShader = vertexShader;
      dump.fragmentShader = fragmentShader;
      dump.resourceViews = resourceViews;
      this->winehuaCaptureImageView(std::move(dump), resource.view);
    }
  }


  void DxvkContext::winehuaCaptureImageView(
          WineHuaRenderTargetDump dump,
    const Rc<DxvkImageView>&      view) {
    const auto& viewInfo = view->info();
    const auto& imageInfo = view->imageInfo();
    dump.viewCookie = view->cookie();
    dump.imageHandle = view->imageHandle();
    dump.imageFormat = imageInfo.format;
    dump.viewFormat = viewInfo.format;
    dump.baseLayer = imageInfo.type == VK_IMAGE_TYPE_3D
      ? 0u : viewInfo.minLayer;
    dump.layerCount = imageInfo.type == VK_IMAGE_TYPE_3D
      ? 1u : std::min(viewInfo.numLayers, 8u);

    VkImageAspectFlags copyAspect = viewInfo.aspect;
    if (copyAspect & VK_IMAGE_ASPECT_COLOR_BIT)
      copyAspect = VK_IMAGE_ASPECT_COLOR_BIT;
    else if (copyAspect & VK_IMAGE_ASPECT_DEPTH_BIT)
      copyAspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    else if (copyAspect & VK_IMAGE_ASPECT_STENCIL_BIT)
      copyAspect = VK_IMAGE_ASPECT_STENCIL_BIT;
    dump.aspect = copyAspect;

    const auto formatInfo = view->formatInfo();
    for (uint32_t mip = 0; mip < viewInfo.numLevels; mip++) {
      if (m_winehuaRenderTargetDumps.size()
          >= winehuaRenderTargetDumpMaxAttachments())
        break;

      WineHuaRenderTargetDump mipDump = dump;
      mipDump.extent = view->mipLevelExtent(mip);
      mipDump.baseMip = viewInfo.minLevel + mip;

      const auto blockCount = util::computeBlockCount(
        mipDump.extent, formatInfo->blockSize);
      mipDump.rowPitch = blockCount.width * formatInfo->elementSize;
      mipDump.slicePitch = blockCount.height * mipDump.rowPitch;
      mipDump.dataSize = blockCount.depth * mipDump.slicePitch * mipDump.layerCount;

      const bool captureSupported = imageInfo.sampleCount == VK_SAMPLE_COUNT_1_BIT
        && (imageInfo.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
        && !formatInfo->flags.test(DxvkFormatFlag::MultiPlane)
        && mipDump.extent.width && mipDump.extent.height && mipDump.dataSize
        && m_winehuaDumpBytes + mipDump.dataSize
            <= winehuaRenderTargetDumpMaxBytes();

      if (captureSupported) {
        DxvkBufferCreateInfo bufferInfo;
        bufferInfo.size = mipDump.dataSize;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
        bufferInfo.access = VK_ACCESS_TRANSFER_WRITE_BIT;

        mipDump.buffer = m_device->createBuffer(bufferInfo,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
        | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        VkImageSubresourceLayers subresource;
        subresource.aspectMask = copyAspect;
        subresource.mipLevel = mipDump.baseMip;
        subresource.baseArrayLayer = mipDump.baseLayer;
        subresource.layerCount = mipDump.layerCount;

        this->copyImageToBuffer(
          mipDump.buffer, 0, 0, 0,
          view->image(), subresource,
          VkOffset3D { 0, 0, 0 }, mipDump.extent);
        m_winehuaDumpBytes += mipDump.dataSize;
      }

      m_winehuaRenderTargetDumps.push_back(std::move(mipDump));
    }
  }
  
  
  void DxvkContext::resetRenderPassOps(
    const DxvkRenderTargets&    renderTargets,
          DxvkRenderPassOps&    renderPassOps) {
    VkAccessFlags access = 0;

    if (renderTargets.depth.view != nullptr) {
      renderPassOps.depthOps = DxvkDepthAttachmentOps {
        VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_LOAD_OP_LOAD,
        renderTargets.depth.layout, renderTargets.depth.layout };

      access |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;

      if (renderTargets.depth.layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL)
        access |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    } else {
      renderPassOps.depthOps = DxvkDepthAttachmentOps { };
    }
    
    for (uint32_t i = 0; i < MaxNumRenderTargets; i++) {
      if (renderTargets.color[i].view != nullptr) {
        renderPassOps.colorOps[i] = DxvkColorAttachmentOps {
            VK_ATTACHMENT_LOAD_OP_LOAD,
            renderTargets.color[i].layout,
            renderTargets.color[i].layout };

        access |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
               |  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      } else {
        renderPassOps.colorOps[i] = DxvkColorAttachmentOps { };
      }
    }

    renderPassOps.barrier.srcStages = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
    renderPassOps.barrier.srcAccess = access;
    renderPassOps.barrier.dstStages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    renderPassOps.barrier.dstAccess = access;
  }
  
  
  void DxvkContext::startTransformFeedback() {
    if (!m_flags.test(DxvkContextFlag::GpXfbActive)) {
      m_flags.set(DxvkContextFlag::GpXfbActive);

      VkBuffer     ctrBuffers[MaxNumXfbBuffers];
      VkDeviceSize ctrOffsets[MaxNumXfbBuffers];

      for (uint32_t i = 0; i < MaxNumXfbBuffers; i++) {
        auto physSlice = m_state.xfb.counters[i].getSliceHandle();

        ctrBuffers[i] = physSlice.handle;
        ctrOffsets[i] = physSlice.offset;

        if (physSlice.handle != VK_NULL_HANDLE)
          m_cmd->trackResource<DxvkAccess::Read>(m_state.xfb.counters[i].buffer());
      }
      
      m_cmd->cmdBeginTransformFeedback(
        0, MaxNumXfbBuffers, ctrBuffers, ctrOffsets);
      
      m_queryManager.beginQueries(m_cmd,
        VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT);
    }
  }


  void DxvkContext::pauseTransformFeedback() {
    if (m_flags.test(DxvkContextFlag::GpXfbActive)) {
      m_flags.clr(DxvkContextFlag::GpXfbActive);
      
      VkBuffer     ctrBuffers[MaxNumXfbBuffers];
      VkDeviceSize ctrOffsets[MaxNumXfbBuffers];

      for (uint32_t i = 0; i < MaxNumXfbBuffers; i++) {
        auto physSlice = m_state.xfb.counters[i].getSliceHandle();

        ctrBuffers[i] = physSlice.handle;
        ctrOffsets[i] = physSlice.offset;

        if (physSlice.handle != VK_NULL_HANDLE)
          m_cmd->trackResource<DxvkAccess::Write>(m_state.xfb.counters[i].buffer());
      }

      m_queryManager.endQueries(m_cmd, 
        VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT);
      
      m_cmd->cmdEndTransformFeedback(
        0, MaxNumXfbBuffers, ctrBuffers, ctrOffsets);
    }
  }


  void DxvkContext::unbindComputePipeline() {
    m_flags.set(
      DxvkContextFlag::CpDirtyPipeline,
      DxvkContextFlag::CpDirtyPipelineState,
      DxvkContextFlag::CpDirtyResources);
    
    m_cpActivePipeline = VK_NULL_HANDLE;
  }
  
  
  bool DxvkContext::updateComputePipeline() {
    m_state.cp.pipeline = lookupComputePipeline(m_state.cp.shaders);

    if (unlikely(m_state.cp.pipeline == nullptr))
      return false;
    
    if (m_state.cp.pipeline->layout()->pushConstRange().size)
      m_flags.set(DxvkContextFlag::DirtyPushConstants);

    m_flags.clr(DxvkContextFlag::CpDirtyPipeline);
    return true;
  }
  
  
  bool DxvkContext::updateComputePipelineState() {
    m_cpActivePipeline = m_state.cp.pipeline->getPipelineHandle(m_state.cp.state);
    
    if (unlikely(!m_cpActivePipeline))
      return false;

    m_cmd->cmdBindPipeline(
      VK_PIPELINE_BIND_POINT_COMPUTE,
      m_cpActivePipeline);

    m_flags.clr(DxvkContextFlag::CpDirtyPipelineState);
    return true;
  }
  
  
  void DxvkContext::unbindGraphicsPipeline() {
    m_flags.set(
      DxvkContextFlag::GpDirtyPipeline,
      DxvkContextFlag::GpDirtyPipelineState,
      DxvkContextFlag::GpDirtyResources,
      DxvkContextFlag::GpDirtyVertexBuffers,
      DxvkContextFlag::GpDirtyIndexBuffer,
      DxvkContextFlag::GpDirtyXfbBuffers,
      DxvkContextFlag::GpDirtyBlendConstants,
      DxvkContextFlag::GpDirtyStencilRef,
      DxvkContextFlag::GpDirtyViewport,
      DxvkContextFlag::GpDirtyDepthBias,
      DxvkContextFlag::GpDirtyDepthBounds);
    
    m_gpActivePipeline = VK_NULL_HANDLE;
  }
  
  
  bool DxvkContext::updateGraphicsPipeline() {
    m_state.gp.pipeline = lookupGraphicsPipeline(m_state.gp.shaders);

    if (unlikely(m_state.gp.pipeline == nullptr)) {
      m_state.gp.flags = DxvkGraphicsPipelineFlags();
      return false;
    }

    if (m_state.gp.flags != m_state.gp.pipeline->flags()) {
      m_state.gp.flags = m_state.gp.pipeline->flags();

      // Force-update vertex/index buffers for hazard checks
      m_flags.set(DxvkContextFlag::GpDirtyIndexBuffer,
                  DxvkContextFlag::GpDirtyVertexBuffers,
                  DxvkContextFlag::GpDirtyXfbBuffers,
                  DxvkContextFlag::DirtyDrawBuffer);

      // This is necessary because we'll only do hazard
      // tracking if the active pipeline has side effects
      if (!m_barrierControl.test(DxvkBarrierControl::IgnoreGraphicsBarriers))
        this->spillRenderPass(true);
    }

    if (m_state.gp.pipeline->layout()->pushConstRange().size)
      m_flags.set(DxvkContextFlag::DirtyPushConstants);

    m_flags.clr(DxvkContextFlag::GpDirtyPipeline);
    return true;
  }
  
  
  bool DxvkContext::updateGraphicsPipelineState() {
    // Set up vertex buffer strides for active bindings
    for (uint32_t i = 0; i < m_state.gp.state.il.bindingCount(); i++) {
      const uint32_t binding = m_state.gp.state.ilBindings[i].binding();
      m_state.gp.state.ilBindings[i].setStride(m_state.vi.vertexStrides[binding]);
    }
    
    // Check which dynamic states need to be active. States that
    // are not dynamic will be invalidated in the command buffer.
    m_flags.clr(DxvkContextFlag::GpDynamicBlendConstants,
                DxvkContextFlag::GpDynamicDepthBias,
                DxvkContextFlag::GpDynamicDepthBounds,
                DxvkContextFlag::GpDynamicStencilRef);
    
    m_flags.set(m_state.gp.state.useDynamicBlendConstants()
      ? DxvkContextFlag::GpDynamicBlendConstants
      : DxvkContextFlag::GpDirtyBlendConstants);
    
    m_flags.set(m_state.gp.state.useDynamicDepthBias()
      ? DxvkContextFlag::GpDynamicDepthBias
      : DxvkContextFlag::GpDirtyDepthBias);
    
    m_flags.set(m_state.gp.state.useDynamicDepthBounds()
      ? DxvkContextFlag::GpDynamicDepthBounds
      : DxvkContextFlag::GpDirtyDepthBounds);
    
    m_flags.set(m_state.gp.state.useDynamicStencilRef()
      ? DxvkContextFlag::GpDynamicStencilRef
      : DxvkContextFlag::GpDirtyStencilRef);
    
    // Retrieve and bind actual Vulkan pipeline handle
    m_gpActivePipeline = m_state.gp.pipeline->getPipelineHandle(
      m_state.gp.state, m_state.om.framebufferInfo.renderPass());

    if (unlikely(!m_gpActivePipeline))
      return false;

    m_cmd->cmdBindPipeline(
      VK_PIPELINE_BIND_POINT_GRAPHICS,
      m_gpActivePipeline);

    m_flags.clr(DxvkContextFlag::GpDirtyPipelineState);
    return true;
  }
  
  
  void DxvkContext::updateComputeShaderResources() {
    if ((m_flags.test(DxvkContextFlag::CpDirtyResources))
     || (m_state.cp.pipeline->layout()->hasStaticBufferBindings()))
      this->updateShaderResources<VK_PIPELINE_BIND_POINT_COMPUTE>(m_state.cp.pipeline->layout());

    this->updateShaderDescriptorSetBinding<VK_PIPELINE_BIND_POINT_COMPUTE>(
      m_cpSet, m_state.cp.pipeline->layout());

    m_flags.clr(DxvkContextFlag::CpDirtyResources,
                DxvkContextFlag::CpDirtyDescriptorBinding);
  }
  
  
  void DxvkContext::updateGraphicsShaderResources() {
    if ((m_flags.test(DxvkContextFlag::GpDirtyResources))
     || (m_state.gp.pipeline->layout()->hasStaticBufferBindings()))
      this->updateShaderResources<VK_PIPELINE_BIND_POINT_GRAPHICS>(m_state.gp.pipeline->layout());

    this->updateShaderDescriptorSetBinding<VK_PIPELINE_BIND_POINT_GRAPHICS>(
      m_gpSet, m_state.gp.pipeline->layout());

    m_flags.clr(DxvkContextFlag::GpDirtyResources,
                DxvkContextFlag::GpDirtyDescriptorBinding);
  }
  
  
  template<VkPipelineBindPoint BindPoint>
  void DxvkContext::updateShaderResources(const DxvkPipelineLayout* layout) {
    std::array<DxvkDescriptorInfo, MaxNumActiveBindings> descriptors;

    // Assume that all bindings are active as a fast path
    DxvkBindingMask bindMask;
    bindMask.setFirst(layout->bindingCount());

    for (uint32_t i = 0; i < layout->bindingCount(); i++) {
      const auto& binding = layout->binding(i);
      const auto& res     = m_rc[binding.slot];
      
      switch (binding.type) {
        case VK_DESCRIPTOR_TYPE_SAMPLER:
          if (res.sampler != nullptr) {
            descriptors[i].image.sampler     = res.sampler->handle();
            descriptors[i].image.imageView   = VK_NULL_HANDLE;
            descriptors[i].image.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            
            if (m_rcTracked.set(binding.slot))
              m_cmd->trackResource<DxvkAccess::None>(res.sampler);
          } else {
            descriptors[i].image = m_common->dummyResources().samplerDescriptor();
          } break;
        
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
          if (res.imageView != nullptr && res.imageView->handle(binding.view) != VK_NULL_HANDLE) {
            descriptors[i].image.sampler     = VK_NULL_HANDLE;
            descriptors[i].image.imageView   = res.imageView->handle(binding.view);
            descriptors[i].image.imageLayout = res.imageView->imageInfo().layout;

            if (winehuaSampleTraceEnabled()
             && (winehuaTraceDepthView(res.imageView)
              || (res.imageView->imageInfo().format == VK_FORMAT_R8G8B8A8_UNORM
               && res.imageView->imageInfo().extent.width <= 16
               && res.imageView->imageInfo().extent.height <= 16))) {
              winehuaSampleTrace(str::format(
                "descriptor sampled set=0 binding=", i,
                " resourceSlot=", binding.slot,
                " viewType=", binding.view,
                " viewCookie=", res.imageView->cookie(),
                " imageView=0x", std::hex, descriptors[i].image.imageView,
                " image=0x", res.imageView->imageHandle(),
                " imageFormat=", std::dec, res.imageView->imageInfo().format,
                " viewFormat=", res.imageView->info().format,
                " aspect=0x", std::hex, res.imageView->info().aspect,
                " baseMip=", std::dec, res.imageView->info().minLevel,
                " mipCount=", res.imageView->info().numLevels,
                " baseLayer=", res.imageView->info().minLayer,
                " layerCount=", res.imageView->info().numLayers,
                " sampler=0x", descriptors[i].image.sampler,
                " layout=", descriptors[i].image.imageLayout,
                " descriptorType=", binding.type,
                " shaderStages=0x", binding.stages,
                " imageUsage=0x", res.imageView->imageInfo().usage));
            }
            
            if (m_rcTracked.set(binding.slot)) {
              m_cmd->trackResource<DxvkAccess::None>(res.imageView);
              m_cmd->trackResource<DxvkAccess::Read>(res.imageView->image());
            }
          } else {
            bindMask.clr(i);
            descriptors[i].image = m_common->dummyResources().imageViewDescriptor(binding.view, true);
          } break;
        
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
          if (res.imageView != nullptr && res.imageView->handle(binding.view) != VK_NULL_HANDLE) {
            descriptors[i].image.sampler     = VK_NULL_HANDLE;
            descriptors[i].image.imageView   = res.imageView->handle(binding.view);
            descriptors[i].image.imageLayout = res.imageView->imageInfo().layout;
            
            if (m_rcTracked.set(binding.slot)) {
              m_cmd->trackResource<DxvkAccess::None>(res.imageView);
              m_cmd->trackResource<DxvkAccess::Write>(res.imageView->image());
            }
          } else {
            bindMask.clr(i);
            descriptors[i].image = m_common->dummyResources().imageViewDescriptor(binding.view, false);
          } break;
        
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
          {
            /* WineHua's opt-in DXBC compatibility mode declares the
             * combined descriptor at the SRV binding while the D3D11 API
             * still stores the sampler at its normal s# binding. Pair those
             * slots here. Existing users that bind both objects to one slot
             * continue to take the fast path. */
            const DxvkShaderResourceSlot* samplerRes = &res;
            const uint32_t stageSlot = binding.slot % DxbcStageBindingCount;
            const bool splitD3d11Binding =
              res.sampler == nullptr && stageSlot >= DxbcResourceBindingIndex &&
              stageSlot < DxbcResourceBindingIndex + DxbcResourceBindingCount;
            const uint32_t samplerSlot = splitD3d11Binding
              ? binding.slot - (DxbcResourceBindingIndex - DxbcSamplerBindingIndex)
              : binding.slot;
            if (splitD3d11Binding)
              samplerRes = &m_rc[samplerSlot];

          const bool imageReady = res.imageView != nullptr
            && res.imageView->handle(binding.view) != VK_NULL_HANDLE;
          const bool samplerReady = samplerRes->sampler != nullptr;
          /* Texture2D.Load has no s# operand, but combined descriptors still
           * require a VkSampler handle.  Use the immutable DXVK dummy sampler
           * when the D3D11 SRV is present and the binding is the WineHua
           * split SRV/sampler form.  OpImageFetch ignores the sampler; this
           * keeps the descriptor bound and lets the shader's image-only Load
           * path remain valid without inventing a sampler in DXBC. */
          if (imageReady && (samplerReady || splitD3d11Binding)) {
            const VkDescriptorImageInfo dummy =
              m_common->dummyResources().imageSamplerDescriptor(binding.view);
            descriptors[i].image.sampler     = samplerReady
              ? samplerRes->sampler->handle() : dummy.sampler;
            descriptors[i].image.imageView   = res.imageView->handle(binding.view);
            descriptors[i].image.imageLayout = res.imageView->imageInfo().layout;

            if (winehuaSampleTraceEnabled()
             && (winehuaTraceDepthView(res.imageView)
              || (res.imageView->imageInfo().format == VK_FORMAT_R8G8B8A8_UNORM
               && res.imageView->imageInfo().extent.width <= 16
               && res.imageView->imageInfo().extent.height <= 16))) {
              winehuaSampleTrace(str::format(
                "descriptor combined set=0 binding=", i,
                " resourceSlot=", binding.slot,
                " samplerSlot=", samplerSlot,
                " viewType=", binding.view,
                " viewCookie=", res.imageView->cookie(),
                " imageView=0x", std::hex, descriptors[i].image.imageView,
                " image=0x", res.imageView->imageHandle(),
                " imageFormat=", std::dec, res.imageView->imageInfo().format,
                " viewFormat=", res.imageView->info().format,
                " aspect=0x", std::hex, res.imageView->info().aspect,
                " baseMip=", std::dec, res.imageView->info().minLevel,
                " mipCount=", res.imageView->info().numLevels,
                " baseLayer=", res.imageView->info().minLayer,
                " layerCount=", res.imageView->info().numLayers,
                " sampler=0x", descriptors[i].image.sampler,
                " layout=", descriptors[i].image.imageLayout,
                " descriptorType=", binding.type,
                " shaderStages=0x", binding.stages,
                " imageUsage=0x", res.imageView->imageInfo().usage,
                " split=", splitD3d11Binding ? 1 : 0,
                " dummySampler=", samplerReady ? 0 : 1));
            }
            
            if (m_rcTracked.set(binding.slot)) {
              if (samplerReady)
                m_cmd->trackResource<DxvkAccess::None>(samplerRes->sampler);
              m_cmd->trackResource<DxvkAccess::None>(res.imageView);
              m_cmd->trackResource<DxvkAccess::Read>(res.imageView->image());
            }
            if (splitD3d11Binding && samplerReady && m_rcTracked.set(samplerSlot))
              m_cmd->trackResource<DxvkAccess::None>(samplerRes->sampler);
          } else {
            bindMask.clr(i);
            descriptors[i].image = m_common->dummyResources().imageSamplerDescriptor(binding.view);
          } break;
          }
        
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
          if (res.bufferView != nullptr) {
            res.bufferView->updateView();
            descriptors[i].texelBuffer = res.bufferView->handle();
            
            if (m_rcTracked.set(binding.slot)) {
              m_cmd->trackResource<DxvkAccess::None>(res.bufferView);
              m_cmd->trackResource<DxvkAccess::Read>(res.bufferView->buffer());
            }
          } else {
            bindMask.clr(i);
            descriptors[i].texelBuffer = m_common->dummyResources().bufferViewDescriptor();
          } break;
        
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
          if (res.bufferView != nullptr) {
            res.bufferView->updateView();
            descriptors[i].texelBuffer = res.bufferView->handle();
            
            if (m_rcTracked.set(binding.slot)) {
              m_cmd->trackResource<DxvkAccess::None>(res.bufferView);
              m_cmd->trackResource<DxvkAccess::Write>(res.bufferView->buffer());
            }
          } else {
            bindMask.clr(i);
            descriptors[i].texelBuffer = m_common->dummyResources().bufferViewDescriptor();
          } break;
        
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
          if (res.bufferSlice.defined()) {
            descriptors[i] = res.bufferSlice.getDescriptor();
            
            if (m_rcTracked.set(binding.slot))
              m_cmd->trackResource<DxvkAccess::Read>(res.bufferSlice.buffer());
          } else {
            bindMask.clr(i);
            descriptors[i].buffer = m_common->dummyResources().bufferDescriptor();
          } break;
        
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
          if (res.bufferSlice.defined()) {
            descriptors[i] = res.bufferSlice.getDescriptor();
            
            if (m_rcTracked.set(binding.slot))
              m_cmd->trackResource<DxvkAccess::Write>(res.bufferSlice.buffer());
          } else {
            bindMask.clr(i);
            descriptors[i].buffer = m_common->dummyResources().bufferDescriptor();
          } break;
        
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
          if (res.bufferSlice.defined()) {
            descriptors[i] = res.bufferSlice.getDescriptor();
            winehuaSampleTrace(str::format(
              "dynamic-cb-descriptor slot=", binding.slot,
              " handle=0x", std::hex, descriptors[i].buffer.buffer,
              " physicalOffset=", std::dec, descriptors[i].buffer.offset,
              " range=", descriptors[i].buffer.range));
            descriptors[i].buffer.offset = 0;
            
            if (m_rcTracked.set(binding.slot))
              m_cmd->trackResource<DxvkAccess::Read>(res.bufferSlice.buffer());
          } else {
            bindMask.clr(i);
            descriptors[i].buffer = m_common->dummyResources().bufferDescriptor();
          } break;
        
        default:
          Logger::err(str::format("DxvkContext: Unhandled descriptor type: ", binding.type));
      }
    }

    // Allocate and update descriptor set
    auto& set = BindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS ? m_gpSet : m_cpSet;

    if (layout->bindingCount()) {
      set = allocateDescriptorSet(layout->descriptorSetLayout());

      if (winehuaSampleTraceEnabled()) {
        for (uint32_t i = 0; i < layout->bindingCount(); i++) {
          const auto& binding = layout->binding(i);
          if (binding.type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
           || binding.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
           || binding.type == VK_DESCRIPTOR_TYPE_SAMPLER) {
            winehuaSampleTrace(str::format(
              "descriptor-template-write bindPoint=", BindPoint,
              " set=0 binding=", i,
              " resourceSlot=", binding.slot,
              " descriptorSet=0x", std::hex, set,
              " updateTemplate=0x", layout->descriptorTemplate(),
              " descriptorType=", binding.type,
              " shaderStages=0x", binding.stages,
              " imageView=0x", descriptors[i].image.imageView,
              " sampler=0x", descriptors[i].image.sampler,
              " layout=", descriptors[i].image.imageLayout));
          }
        }
      }

      m_cmd->updateDescriptorSetWithTemplate(set,
        layout->descriptorTemplate(), descriptors.data());
    } else {
      set = VK_NULL_HANDLE;
    }

    if constexpr (BindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS) {
      if (winehuaDrawTraceEnabled() || winehuaCameraTraceEnabled()) {
        m_winehuaDescriptorUpdateSerial++;
        m_winehuaGraphicsBindings.clear();
        m_winehuaGraphicsBindings.reserve(layout->bindingCount());

        for (uint32_t i = 0; i < layout->bindingCount(); i++) {
          const auto& descriptorBinding = layout->binding(i);
          const auto& resource = m_rc[descriptorBinding.slot];

          WineHuaGraphicsBindingTrace trace;
          trace.binding = i;
          trace.resourceSlot = descriptorBinding.slot;
          trace.descriptorType = descriptorBinding.type;
          trace.viewType = descriptorBinding.view;
          trace.stages = descriptorBinding.stages;

          switch (descriptorBinding.type) {
            case VK_DESCRIPTOR_TYPE_SAMPLER:
              trace.image = descriptors[i].image;
              trace.sampler = resource.sampler;
              break;

            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
              trace.image = descriptors[i].image;
              trace.imageView = resource.imageView;
              break;

            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: {
              trace.image = descriptors[i].image;
              trace.imageView = resource.imageView;

              const uint32_t stageSlot = descriptorBinding.slot % DxbcStageBindingCount;
              const bool splitD3d11Binding = resource.sampler == nullptr
                && stageSlot >= DxbcResourceBindingIndex
                && stageSlot < DxbcResourceBindingIndex + DxbcResourceBindingCount;
              const uint32_t samplerSlot = splitD3d11Binding
                ? descriptorBinding.slot
                    - (DxbcResourceBindingIndex - DxbcSamplerBindingIndex)
                : descriptorBinding.slot;
              trace.sampler = m_rc[samplerSlot].sampler;
            } break;

            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
              trace.buffer = descriptors[i].buffer;
              trace.bufferSlice = resource.bufferSlice;
              break;

            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
              trace.texelBuffer = descriptors[i].texelBuffer;
              break;

            default:
              break;
          }

          m_winehuaGraphicsBindings.push_back(std::move(trace));
        }

        if (m_winehuaFrameId == winehuaRenderTargetDumpFrame()) {
          Logger::info(str::format(
            "WineHuaDrawDescriptor: event=update frame=", m_winehuaFrameId,
            " pass=", m_winehuaActivePassId,
            " serial=", m_winehuaDescriptorUpdateSerial,
            " descriptorSet=0x", std::hex, set,
            " bindingCount=", std::dec, layout->bindingCount()));
        }
      }
    }

    if constexpr (BindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS) {
      if (winehuaRenderTargetDumpEnabled()
       && m_winehuaFrameId == winehuaRenderTargetDumpFrame()) {
        std::string resourceViews = "[";
        bool firstResource = true;
        m_winehuaLastGraphicsImages.clear();
        for (uint32_t i = 0; i < layout->bindingCount(); i++) {
          const auto& binding = layout->binding(i);
          const auto& view = m_rc[binding.slot].imageView;
          if (view == nullptr)
            continue;

          m_winehuaLastGraphicsImages.push_back(WineHuaGraphicsResourceDump {
            i, binding.slot, binding.type, view });

          if (!firstResource)
            resourceViews += ',';
          resourceViews += str::format(
            "{\"binding\":", i,
            ",\"resourceSlot\":", binding.slot,
            ",\"descriptorType\":", uint32_t(binding.type),
            ",\"descriptorImageView\":", uint64_t(descriptors[i].image.imageView),
            ",\"descriptorSampler\":", uint64_t(descriptors[i].image.sampler),
            ",\"viewCookie\":", view->cookie(),
            ",\"imageHandle\":", uint64_t(view->imageHandle()),
            ",\"viewFormat\":", uint32_t(view->info().format),
            ",\"aspect\":", uint32_t(view->info().aspect),
            ",\"baseMip\":", view->info().minLevel,
            ",\"baseLayer\":", view->info().minLayer,
            ",\"layerCount\":", view->info().numLayers,
            '}');
          firstResource = false;
        }
        resourceViews += ']';
        m_winehuaLastGraphicsResourceViews = std::move(resourceViews);
      }
    }

    // Select the active binding mask to update
    auto& refMask = BindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS
      ? m_state.gp.state.bsBindingMask
      : m_state.cp.state.bsBindingMask;

    // If some resources are not bound, we may need to
    // update spec constants and rebind the pipeline
    if (refMask != bindMask) {
      refMask = bindMask;

      m_flags.set(BindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS
        ? DxvkContextFlag::GpDirtyPipelineState
        : DxvkContextFlag::CpDirtyPipelineState);
    }
  }
  
  
  template<VkPipelineBindPoint BindPoint>
  void DxvkContext::updateShaderDescriptorSetBinding(
          VkDescriptorSet         set,
    const DxvkPipelineLayout*     layout) {
    if (set) {
      std::array<uint32_t, MaxNumActiveBindings> offsets;

      if (winehuaSampleTraceEnabled()) {
        winehuaSampleTrace(str::format(
          "descriptor-bind bindPoint=", BindPoint,
          " set=0 descriptorSet=0x", std::hex, set,
          " pipelineLayout=0x", layout->pipelineLayout(),
          " dynamicBindingCount=", std::dec, layout->dynamicBindingCount()));
      }

      for (uint32_t i = 0; i < layout->dynamicBindingCount(); i++) {
        const auto& binding = layout->dynamicBinding(i);
        const auto& res     = m_rc[binding.slot];

        offsets[i] = res.bufferSlice.defined()
          ? res.bufferSlice.getDynamicOffset()
          : 0;

        if constexpr (BindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS) {
          if (winehuaDrawTraceEnabled() || winehuaCameraTraceEnabled()) {
            for (auto& trace : m_winehuaGraphicsBindings) {
              if (trace.resourceSlot == binding.slot
               && trace.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) {
                trace.dynamicOffset = offsets[i];
                trace.dynamicOffsetBound = true;
                trace.bufferSlice = res.bufferSlice;
                break;
              }
            }
          }
        }
        winehuaSampleTrace(str::format(
          "dynamic-cb-bind index=", i,
          " slot=", binding.slot,
          " dynamicOffset=", offsets[i]));
      }

      if constexpr (BindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS) {
        if (winehuaDrawTraceEnabled() || winehuaCameraTraceEnabled()) {
          m_winehuaDescriptorBindSerial++;
          if (m_winehuaFrameId == winehuaRenderTargetDumpFrame()) {
            Logger::info(str::format(
              "WineHuaDrawDescriptor: event=bind frame=", m_winehuaFrameId,
              " pass=", m_winehuaActivePassId,
              " serial=", m_winehuaDescriptorBindSerial,
              " descriptorSet=0x", std::hex, set,
              " dynamicBindingCount=", std::dec, layout->dynamicBindingCount()));
          }
        }
      }
      
      m_cmd->cmdBindDescriptorSet(BindPoint,
        layout->pipelineLayout(), set,
        layout->dynamicBindingCount(),
        offsets.data());
    }
  }
  
  
  DxvkFramebufferInfo DxvkContext::makeFramebufferInfo(
    const DxvkRenderTargets&      renderTargets) {
    auto renderPassFormat = DxvkFramebufferInfo::getRenderPassFormat(renderTargets);
    auto renderPassObject = m_common->renderPassPool().getRenderPass(renderPassFormat);

    return DxvkFramebufferInfo(renderTargets, m_device->getDefaultFramebufferSize(), renderPassObject);
  }


  void DxvkContext::updateFramebuffer() {
    if (m_flags.test(DxvkContextFlag::GpDirtyFramebuffer)) {
      m_flags.clr(DxvkContextFlag::GpDirtyFramebuffer);

      this->spillRenderPass(true);

      DxvkFramebufferInfo fbInfo = makeFramebufferInfo(m_state.om.renderTargets);
      this->updateRenderTargetLayouts(fbInfo, m_state.om.framebufferInfo);

      m_state.gp.state.ms.setSampleCount(fbInfo.getSampleCount());
      m_state.om.framebufferInfo = fbInfo;

      for (uint32_t i = 0; i < MaxNumRenderTargets; i++) {
        const Rc<DxvkImageView>& attachment = fbInfo.getColorTarget(i).view;

        VkComponentMapping mapping = attachment != nullptr
          ? util::invertComponentMapping(attachment->info().swizzle)
          : VkComponentMapping();

        m_state.gp.state.omSwizzle[i] = DxvkOmAttachmentSwizzle(mapping);
      }

      m_flags.set(DxvkContextFlag::GpDirtyPipelineState);
    }
  }


  void DxvkContext::applyRenderTargetLoadLayouts() {
    for (uint32_t i = 0; i < MaxNumRenderTargets; i++)
      m_state.om.renderPassOps.colorOps[i].loadLayout = m_rtLayouts.color[i];

    m_state.om.renderPassOps.depthOps.loadLayout = m_rtLayouts.depth;
  }


  void DxvkContext::applyRenderTargetStoreLayouts() {
    for (uint32_t i = 0; i < MaxNumRenderTargets; i++)
      m_rtLayouts.color[i] = m_state.om.renderPassOps.colorOps[i].storeLayout;

    m_rtLayouts.depth = m_state.om.renderPassOps.depthOps.storeLayout;
  }


  void DxvkContext::transitionRenderTargetLayouts(
          DxvkBarrierSet&         barriers,
          bool                    sharedOnly) {
    for (uint32_t i = 0; i < MaxNumRenderTargets; i++) {
      const DxvkAttachment& color = m_state.om.framebufferInfo.getColorTarget(i);

      if (color.view != nullptr && (!sharedOnly || color.view->imageInfo().shared)) {
        this->transitionColorAttachment(barriers, color, m_rtLayouts.color[i]);
        m_rtLayouts.color[i] = color.view->imageInfo().layout;
      }
    }

    const DxvkAttachment& depth = m_state.om.framebufferInfo.getDepthTarget();

    if (depth.view != nullptr && (!sharedOnly || depth.view->imageInfo().shared)) {
      this->transitionDepthAttachment(barriers, depth, m_rtLayouts.depth);
      m_rtLayouts.depth = depth.view->imageInfo().layout;
    }
  }


  void DxvkContext::transitionColorAttachment(
          DxvkBarrierSet&         barriers,
    const DxvkAttachment&         attachment,
          VkImageLayout           oldLayout) {
    if (oldLayout != attachment.view->imageInfo().layout) {
      barriers.accessImage(
        attachment.view->image(),
        attachment.view->imageSubresources(), oldLayout,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        attachment.view->imageInfo().layout,
        attachment.view->imageInfo().stages,
        attachment.view->imageInfo().access);

      m_cmd->trackResource<DxvkAccess::Write>(attachment.view->image());
    }
  }


  void DxvkContext::transitionDepthAttachment(
          DxvkBarrierSet&         barriers,
    const DxvkAttachment&         attachment,
          VkImageLayout           oldLayout) {
    if (oldLayout != attachment.view->imageInfo().layout) {
      barriers.accessImage(
        attachment.view->image(),
        attachment.view->imageSubresources(), oldLayout,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        oldLayout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
          ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : 0,
        attachment.view->imageInfo().layout,
        attachment.view->imageInfo().stages,
        attachment.view->imageInfo().access);

      m_cmd->trackResource<DxvkAccess::Write>(attachment.view->image());
    }
  }


  void DxvkContext::updateRenderTargetLayouts(
    const DxvkFramebufferInfo&    newFb,
    const DxvkFramebufferInfo&    oldFb) {
    DxvkRenderTargetLayouts layouts = { };

    for (uint32_t i = 0; i < MaxNumRenderTargets; i++) {
      if (newFb.getColorTarget(i).view != nullptr)
        layouts.color[i] = newFb.getColorTarget(i).view->imageInfo().layout;
    }

    if (newFb.getDepthTarget().view != nullptr)
      layouts.depth = newFb.getDepthTarget().view->imageInfo().layout;

    // Check whether any of the previous attachments have been moved
    // around or been rebound with a different view. This may help
    // reduce the number of image layout transitions between passes.
    for (uint32_t i = 0; i < MaxNumRenderTargets; i++) {
      const DxvkAttachment& oldAttachment = oldFb.getColorTarget(i);

      if (oldAttachment.view != nullptr) {
        bool found = false;

        for (uint32_t j = 0; j < MaxNumRenderTargets && !found; j++) {
          const DxvkAttachment& newAttachment = newFb.getColorTarget(j);

          found = newAttachment.view == oldAttachment.view || (newAttachment.view != nullptr
            && newAttachment.view->image()        == oldAttachment.view->image()
            && newAttachment.view->subresources() == oldAttachment.view->subresources());

          if (found)
            layouts.color[j] = m_rtLayouts.color[i];
        }

        if (!found && m_flags.test(DxvkContextFlag::GpRenderPassSuspended))
          this->transitionColorAttachment(m_execBarriers, oldAttachment, m_rtLayouts.color[i]);
      }
    }

    const DxvkAttachment& oldAttachment = oldFb.getDepthTarget();

    if (oldAttachment.view != nullptr) {
      const DxvkAttachment& newAttachment = newFb.getDepthTarget();

      bool found = newAttachment.view == oldAttachment.view || (newAttachment.view != nullptr
        && newAttachment.view->image()        == oldAttachment.view->image()
        && newAttachment.view->subresources() == oldAttachment.view->subresources());

      if (found)
        layouts.depth = m_rtLayouts.depth;
      else if (m_flags.test(DxvkContextFlag::GpRenderPassSuspended))
        this->transitionDepthAttachment(m_execBarriers, oldAttachment, m_rtLayouts.depth);
    }

    m_rtLayouts = layouts;
  }
  
  
  void DxvkContext::prepareImage(
          DxvkBarrierSet&         barriers,
    const Rc<DxvkImage>&          image,
    const VkImageSubresourceRange& subresources,
          bool                    flushClears) {
    // Images that can't be used as attachments are always in their
    // default layout, so we don't have to do anything in this case
    if (!(image->info().usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)))
      return;

    // Flush clears if there are any since they may affect the image
    if (!m_deferredClears.empty() && flushClears)
      this->spillRenderPass(false);

    // All images are in their default layout for suspended passes
    if (!m_flags.test(DxvkContextFlag::GpRenderPassSuspended))
      return;

    // 3D images require special care because they only have one
    // layer, but views may address individual 2D slices as layers
    bool is3D = image->info().type == VK_IMAGE_TYPE_3D;

    // Transition any attachment with overlapping subresources
    if (image->info().usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) {
      for (uint32_t i = 0; i < MaxNumRenderTargets; i++) {
        const DxvkAttachment& attachment = m_state.om.framebufferInfo.getColorTarget(i);

        if (attachment.view != nullptr && attachment.view->image() == image
         && (is3D || vk::checkSubresourceRangeOverlap(attachment.view->subresources(), subresources))) {
          this->transitionColorAttachment(barriers, attachment, m_rtLayouts.color[i]);
          m_rtLayouts.color[i] = image->info().layout;
        }
      }
    } else {
      const DxvkAttachment& attachment = m_state.om.framebufferInfo.getDepthTarget();

      if (attachment.view != nullptr && attachment.view->image() == image
       && (is3D || vk::checkSubresourceRangeOverlap(attachment.view->subresources(), subresources))) {
        this->transitionDepthAttachment(barriers, attachment, m_rtLayouts.depth);
        m_rtLayouts.depth = image->info().layout;
      }
    }
  }


  bool DxvkContext::updateIndexBufferBinding() {
    if (unlikely(!m_state.vi.indexBuffer.defined()))
      return false;

    m_flags.clr(DxvkContextFlag::GpDirtyIndexBuffer);
    auto bufferInfo = m_state.vi.indexBuffer.getDescriptor();

    m_cmd->cmdBindIndexBuffer(
      bufferInfo.buffer.buffer,
      bufferInfo.buffer.offset,
      m_state.vi.indexType);

    if (m_vbTracked.set(MaxNumVertexBindings))
      m_cmd->trackResource<DxvkAccess::Read>(m_state.vi.indexBuffer.buffer());

    return true;
  }
  
  
  void DxvkContext::updateVertexBufferBindings() {
    m_flags.clr(DxvkContextFlag::GpDirtyVertexBuffers);

    if (unlikely(!m_state.gp.state.il.bindingCount()))
      return;
    
    std::array<VkBuffer,     MaxNumVertexBindings> buffers;
    std::array<VkDeviceSize, MaxNumVertexBindings> offsets;
    std::array<VkDeviceSize, MaxNumVertexBindings> lengths;
    
    // Set buffer handles and offsets for active bindings
    for (uint32_t i = 0; i < m_state.gp.state.il.bindingCount(); i++) {
      uint32_t binding = m_state.gp.state.ilBindings[i].binding();
      
      if (likely(m_state.vi.vertexBuffers[binding].defined())) {
        auto vbo = m_state.vi.vertexBuffers[binding].getDescriptor();
        
        buffers[i] = vbo.buffer.buffer;
        offsets[i] = vbo.buffer.offset;
        lengths[i] = vbo.buffer.range;
        
        if (m_vbTracked.set(binding))
          m_cmd->trackResource<DxvkAccess::Read>(m_state.vi.vertexBuffers[binding].buffer());
      } else if (m_features.test(DxvkContextFeature::NullDescriptors)) {
        buffers[i] = VK_NULL_HANDLE;
        offsets[i] = 0;
        lengths[i] = 0;
      } else {
        buffers[i] = m_common->dummyResources().bufferHandle();
        offsets[i] = 0;
        lengths[i] = 0;
      }
    }
    
    // Vertex bindigs get remapped when compiling the
    // pipeline, so this actually does the right thing
    if (m_features.test(DxvkContextFeature::ExtendedDynamicState)) {
      m_cmd->cmdBindVertexBuffers2(0, m_state.gp.state.il.bindingCount(),
        buffers.data(), offsets.data(), lengths.data(), nullptr);
    } else {
      m_cmd->cmdBindVertexBuffers(0, m_state.gp.state.il.bindingCount(),
        buffers.data(), offsets.data());
    }
  }
  
  
  void DxvkContext::updateTransformFeedbackBuffers() {
    const auto& gsInfo = m_state.gp.shaders.gs->info();

    VkBuffer     xfbBuffers[MaxNumXfbBuffers];
    VkDeviceSize xfbOffsets[MaxNumXfbBuffers];
    VkDeviceSize xfbLengths[MaxNumXfbBuffers];

    for (size_t i = 0; i < MaxNumXfbBuffers; i++) {
      auto physSlice = m_state.xfb.buffers[i].getSliceHandle();
      
      xfbBuffers[i] = physSlice.handle;
      xfbOffsets[i] = physSlice.offset;
      xfbLengths[i] = physSlice.length;

      if (physSlice.handle == VK_NULL_HANDLE)
        xfbBuffers[i] = m_common->dummyResources().bufferHandle();
      
      if (physSlice.handle != VK_NULL_HANDLE) {
        const Rc<DxvkBuffer>& buffer = m_state.xfb.buffers[i].buffer();
        buffer->setXfbVertexStride(gsInfo.xfbStrides[i]);
        
        m_cmd->trackResource<DxvkAccess::Write>(buffer);
      }
    }

    m_cmd->cmdBindTransformFeedbackBuffers(
      0, MaxNumXfbBuffers,
      xfbBuffers, xfbOffsets, xfbLengths);
  }


  void DxvkContext::updateTransformFeedbackState() {
    if (m_flags.test(DxvkContextFlag::GpDirtyXfbBuffers)) {
      m_flags.clr(DxvkContextFlag::GpDirtyXfbBuffers);

      this->pauseTransformFeedback();
      this->updateTransformFeedbackBuffers();
    }

    this->startTransformFeedback();
  }

  
  void DxvkContext::updateDynamicState() {
    if (!m_gpActivePipeline)
      return;
    
    if (m_flags.test(DxvkContextFlag::GpDirtyViewport)) {
      m_flags.clr(DxvkContextFlag::GpDirtyViewport);

      uint32_t viewportCount = m_state.gp.state.rs.viewportCount();
      m_cmd->cmdSetViewport(0, viewportCount, m_state.vp.viewports.data());
      m_cmd->cmdSetScissor (0, viewportCount, m_state.vp.scissorRects.data());
    }

    if (m_flags.all(DxvkContextFlag::GpDirtyBlendConstants,
                    DxvkContextFlag::GpDynamicBlendConstants)) {
      m_flags.clr(DxvkContextFlag::GpDirtyBlendConstants);
      m_cmd->cmdSetBlendConstants(&m_state.dyn.blendConstants.r);
    }

    if (m_flags.all(DxvkContextFlag::GpDirtyStencilRef,
                    DxvkContextFlag::GpDynamicStencilRef)) {
      m_flags.clr(DxvkContextFlag::GpDirtyStencilRef);

      m_cmd->cmdSetStencilReference(
        VK_STENCIL_FRONT_AND_BACK,
        m_state.dyn.stencilReference);
    }
    
    if (m_flags.all(DxvkContextFlag::GpDirtyDepthBias,
                    DxvkContextFlag::GpDynamicDepthBias)) {
      m_flags.clr(DxvkContextFlag::GpDirtyDepthBias);

      m_cmd->cmdSetDepthBias(
        m_state.dyn.depthBias.depthBiasConstant,
        m_state.dyn.depthBias.depthBiasClamp,
        m_state.dyn.depthBias.depthBiasSlope);
    }
    
    if (m_flags.all(DxvkContextFlag::GpDirtyDepthBounds,
                    DxvkContextFlag::GpDynamicDepthBounds)) {
      m_flags.clr(DxvkContextFlag::GpDirtyDepthBounds);

      m_cmd->cmdSetDepthBounds(
        m_state.dyn.depthBounds.minDepthBounds,
        m_state.dyn.depthBounds.maxDepthBounds);
    }
  }


  template<VkPipelineBindPoint BindPoint>
  void DxvkContext::updatePushConstants() {
    m_flags.clr(DxvkContextFlag::DirtyPushConstants);

    auto layout = BindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS
      ? m_state.gp.pipeline->layout()
      : m_state.cp.pipeline->layout();
    
    if (!layout)
      return;
    
    VkPushConstantRange pushConstRange = layout->pushConstRange();
    if (!pushConstRange.size)
      return;
    
    m_cmd->cmdPushConstants(
      layout->pipelineLayout(),
      pushConstRange.stageFlags,
      pushConstRange.offset,
      pushConstRange.size,
      &m_state.pc.data[pushConstRange.offset]);
  }
  
  
  bool DxvkContext::commitComputeState() {
    this->spillRenderPass(false);

    if (m_flags.test(DxvkContextFlag::CpDirtyPipeline)) {
      if (unlikely(!this->updateComputePipeline()))
        return false;
    }
    
    if (m_flags.any(
          DxvkContextFlag::CpDirtyResources,
          DxvkContextFlag::CpDirtyDescriptorBinding))
      this->updateComputeShaderResources();

    if (m_flags.test(DxvkContextFlag::CpDirtyPipelineState)) {
      if (unlikely(!this->updateComputePipelineState()))
        return false;
    }
    
    if (m_flags.test(DxvkContextFlag::DirtyPushConstants))
      this->updatePushConstants<VK_PIPELINE_BIND_POINT_COMPUTE>();

    return true;
  }
  
  
  template<bool Indexed, bool Indirect>
  bool DxvkContext::commitGraphicsState() {
    if (m_flags.test(DxvkContextFlag::GpDirtyPipeline)) {
      if (unlikely(!this->updateGraphicsPipeline()))
        return false;
    }
    
    if (m_state.gp.flags.any(DxvkGraphicsPipelineFlag::HasStorageDescriptors,
                             DxvkGraphicsPipelineFlag::HasTransformFeedback)) {
      this->commitGraphicsBarriers<Indexed, Indirect, false>();
      this->commitGraphicsBarriers<Indexed, Indirect, true>();
    }

    if (m_flags.test(DxvkContextFlag::GpDirtyFramebuffer))
      this->updateFramebuffer();

    if (!m_flags.test(DxvkContextFlag::GpRenderPassBound))
      this->startRenderPass();
    
    if (m_flags.test(DxvkContextFlag::GpDirtyIndexBuffer) && Indexed) {
      if (unlikely(!this->updateIndexBufferBinding()))
        return false;
    }
    
    if (m_flags.test(DxvkContextFlag::GpDirtyVertexBuffers))
      this->updateVertexBufferBindings();
    
    if (m_flags.any(
          DxvkContextFlag::GpDirtyResources,
          DxvkContextFlag::GpDirtyDescriptorBinding))
      this->updateGraphicsShaderResources();
    
    if (m_flags.test(DxvkContextFlag::GpDirtyPipelineState)) {
      if (unlikely(!this->updateGraphicsPipelineState()))
        return false;
    }
    
    if (m_state.gp.flags.test(DxvkGraphicsPipelineFlag::HasTransformFeedback))
      this->updateTransformFeedbackState();
    
    if (m_flags.any(
          DxvkContextFlag::GpDirtyViewport,
          DxvkContextFlag::GpDirtyBlendConstants,
          DxvkContextFlag::GpDirtyStencilRef,
          DxvkContextFlag::GpDirtyDepthBias,
          DxvkContextFlag::GpDirtyDepthBounds))
      this->updateDynamicState();
    
    if (m_flags.test(DxvkContextFlag::DirtyPushConstants))
      this->updatePushConstants<VK_PIPELINE_BIND_POINT_GRAPHICS>();

    if (m_flags.test(DxvkContextFlag::DirtyDrawBuffer) && Indirect)
      this->trackDrawBuffer();

    return true;
  }
  
  
  void DxvkContext::commitComputeInitBarriers() {
    auto layout = m_state.cp.pipeline->layout();

    bool requiresBarrier = false;

    for (uint32_t i = 0; i < layout->bindingCount() && !requiresBarrier; i++) {
      if (m_state.cp.state.bsBindingMask.test(i)) {
        const DxvkDescriptorSlot binding = layout->binding(i);
        const DxvkShaderResourceSlot& slot = m_rc[binding.slot];

        DxvkAccessFlags dstAccess = DxvkBarrierSet::getAccessTypes(binding.access);
        DxvkAccessFlags srcAccess = 0;
        
        switch (binding.type) {
          case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
          case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
          case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            srcAccess = m_execBarriers.getBufferAccess(
              slot.bufferSlice.getSliceHandle());
            break;
        
          case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
          case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            srcAccess = m_execBarriers.getBufferAccess(
              slot.bufferView->getSliceHandle());
            break;
          
          case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
          case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
          case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            srcAccess = m_execBarriers.getImageAccess(
              slot.imageView->image(),
              slot.imageView->imageSubresources());
            break;

          default:
            /* nothing to do */;
        }

        if (srcAccess == 0)
          continue;

        // Skip write-after-write barriers if explicitly requested
        VkPipelineStageFlags stageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                                       | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;

        if ((m_barrierControl.test(DxvkBarrierControl::IgnoreWriteAfterWrite))
         && (!(m_execBarriers.getSrcStages() & ~stageMask))
         && ((srcAccess | dstAccess) == DxvkAccess::Write))
          continue;

        requiresBarrier = (srcAccess | dstAccess).test(DxvkAccess::Write);
      }
    }

    if (requiresBarrier)
      m_execBarriers.recordCommands(m_cmd);
  }
  

  void DxvkContext::commitComputePostBarriers() {
    auto layout = m_state.cp.pipeline->layout();
    
    for (uint32_t i = 0; i < layout->bindingCount(); i++) {
      if (m_state.cp.state.bsBindingMask.test(i)) {
        const DxvkDescriptorSlot binding = layout->binding(i);
        const DxvkShaderResourceSlot& slot = m_rc[binding.slot];

        VkPipelineStageFlags stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        VkAccessFlags        access = binding.access;
        
        switch (binding.type) {
          case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
          case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
          case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            m_execBarriers.accessBuffer(
              slot.bufferSlice.getSliceHandle(),
              stages, access,
              slot.bufferSlice.bufferInfo().stages,
              slot.bufferSlice.bufferInfo().access);
            break;
        
          case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
          case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            m_execBarriers.accessBuffer(
              slot.bufferView->getSliceHandle(),
              stages, access,
              slot.bufferView->bufferInfo().stages,
              slot.bufferView->bufferInfo().access);
            break;
          
          case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
          case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
          case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            m_execBarriers.accessImage(
              slot.imageView->image(),
              slot.imageView->imageSubresources(),
              slot.imageView->imageInfo().layout,
              stages, access,
              slot.imageView->imageInfo().layout,
              slot.imageView->imageInfo().stages,
              slot.imageView->imageInfo().access);
            break;

          default:
            /* nothing to do */;
        }
      }
    }
  }
  
  
  template<bool Indexed, bool Indirect, bool DoEmit>
  void DxvkContext::commitGraphicsBarriers() {
    if (m_barrierControl.test(DxvkBarrierControl::IgnoreGraphicsBarriers))
      return;

    auto layout = m_state.gp.pipeline->layout();

    constexpr auto storageBufferAccess = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT;
    constexpr auto storageImageAccess  = VK_ACCESS_SHADER_WRITE_BIT;

    bool requiresBarrier = false;

    // Check the draw buffer for indirect draw calls
    if (m_flags.test(DxvkContextFlag::DirtyDrawBuffer) && Indirect) {
      std::array<DxvkBufferSlice*, 2> slices = {{
        &m_state.id.argBuffer,
        &m_state.id.cntBuffer,
      }};

      for (uint32_t i = 0; i < slices.size() && !requiresBarrier; i++) {
        if ((slices[i]->defined())
         && (slices[i]->bufferInfo().access & storageBufferAccess)) {
          requiresBarrier = this->checkGfxBufferBarrier<DoEmit>(*slices[i],
            VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
            VK_ACCESS_INDIRECT_COMMAND_READ_BIT).test(DxvkAccess::Write);
        }
      }
    }

    // Read-only stage, so we only have to check this if
    // the bindngs have actually changed between draws
    if (m_flags.test(DxvkContextFlag::GpDirtyIndexBuffer) && !requiresBarrier && Indexed) {
      const auto& indexBufferSlice = m_state.vi.indexBuffer;

      if ((indexBufferSlice.defined())
       && (indexBufferSlice.bufferInfo().access & storageBufferAccess)) {
        requiresBarrier = this->checkGfxBufferBarrier<DoEmit>(indexBufferSlice,
          VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
          VK_ACCESS_INDEX_READ_BIT).test(DxvkAccess::Write);
      }
    }

    // Same here, also ignore unused vertex bindings
    if (m_flags.test(DxvkContextFlag::GpDirtyVertexBuffers)) {
      uint32_t bindingCount = m_state.gp.state.il.bindingCount();

      for (uint32_t i = 0; i < bindingCount && !requiresBarrier; i++) {
        uint32_t binding = m_state.gp.state.ilBindings[i].binding();
        const auto& vertexBufferSlice = m_state.vi.vertexBuffers[binding];

        if ((vertexBufferSlice.defined())
         && (vertexBufferSlice.bufferInfo().access & storageBufferAccess)) {
          requiresBarrier = this->checkGfxBufferBarrier<DoEmit>(vertexBufferSlice,
            VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
            VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT).test(DxvkAccess::Write);
        }
      }
    }

    // Transform feedback buffer writes won't overlap, so we
    // also only need to check those when they are rebound
    if (m_flags.test(DxvkContextFlag::GpDirtyXfbBuffers)
     && m_state.gp.flags.test(DxvkGraphicsPipelineFlag::HasTransformFeedback)) {
      for (uint32_t i = 0; i < MaxNumXfbBuffers && !requiresBarrier; i++) {
        const auto& xfbBufferSlice = m_state.xfb.buffers[i];
        const auto& xfbCounterSlice = m_state.xfb.counters[i];

        if (xfbBufferSlice.defined()) {
          requiresBarrier = this->checkGfxBufferBarrier<DoEmit>(xfbBufferSlice,
            VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT,
            VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT) != 0;

          if (xfbCounterSlice.defined()) {
            requiresBarrier |= this->checkGfxBufferBarrier<DoEmit>(xfbCounterSlice,
              VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
              VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT,
              VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_READ_BIT_EXT |
              VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT) != 0;
          }
        }
      }
    }

    // Check shader resources on every draw to handle WAW hazards
    for (uint32_t i = 0; i < layout->bindingCount() && !requiresBarrier; i++) {
      const DxvkDescriptorSlot binding = layout->binding(i);
      const DxvkShaderResourceSlot& slot = m_rc[binding.slot];

      DxvkAccessFlags dstAccess = DxvkBarrierSet::getAccessTypes(binding.access);
      DxvkAccessFlags srcAccess = 0;
      
      switch (binding.type) {
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
          if ((slot.bufferSlice.defined())
           && (slot.bufferSlice.bufferInfo().access & storageBufferAccess)) {
            srcAccess = this->checkGfxBufferBarrier<DoEmit>(slot.bufferSlice,
              util::pipelineStages(binding.stages), binding.access);
          }
          break;

        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
          if ((slot.bufferView != nullptr)
           && (slot.bufferView->bufferInfo().access & storageBufferAccess)) {
            srcAccess = this->checkGfxBufferBarrier<DoEmit>(slot.bufferView->slice(),
              util::pipelineStages(binding.stages), binding.access);
          }
          break;

        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
          if ((slot.imageView != nullptr)
           && (slot.imageView->imageInfo().access & storageImageAccess)) {
            srcAccess = this->checkGfxImageBarrier<DoEmit>(slot.imageView,
              util::pipelineStages(binding.stages), binding.access);
          }
          break;

        default:
          /* nothing to do */;
      }

      if (srcAccess == 0)
        continue;

      // Skip write-after-write barriers if explicitly requested
      if ((m_barrierControl.test(DxvkBarrierControl::IgnoreWriteAfterWrite))
        && ((srcAccess | dstAccess) == DxvkAccess::Write))
        continue;

      requiresBarrier = (srcAccess | dstAccess).test(DxvkAccess::Write);
    }

    // External subpass dependencies serve as full memory
    // and execution barriers, so we can use this to allow
    // inter-stage synchronization.
    if (requiresBarrier)
      this->spillRenderPass(true);
  }


  template<bool DoEmit>
  DxvkAccessFlags DxvkContext::checkGfxBufferBarrier(
    const DxvkBufferSlice&          slice,
          VkPipelineStageFlags      stages,
          VkAccessFlags             access) {
    if constexpr (DoEmit) {
      m_gfxBarriers.accessBuffer(
        slice.getSliceHandle(),
        stages, access,
        slice.bufferInfo().stages,
        slice.bufferInfo().access);
      return DxvkAccessFlags();
    } else {
      return m_gfxBarriers.getBufferAccess(slice.getSliceHandle());
    }
  }


  template<bool DoEmit>
  DxvkAccessFlags DxvkContext::checkGfxImageBarrier(
    const Rc<DxvkImageView>&        imageView,
          VkPipelineStageFlags      stages,
          VkAccessFlags             access) {
    if constexpr (DoEmit) {
      m_gfxBarriers.accessImage(
        imageView->image(),
        imageView->imageSubresources(),
        imageView->imageInfo().layout,
        stages, access,
        imageView->imageInfo().layout,
        imageView->imageInfo().stages,
        imageView->imageInfo().access);
      return DxvkAccessFlags();
    } else {
      return m_gfxBarriers.getImageAccess(
        imageView->image(),
        imageView->imageSubresources());
    }
  }


  void DxvkContext::emitMemoryBarrier(
          VkDependencyFlags         flags,
          VkPipelineStageFlags      srcStages,
          VkAccessFlags             srcAccess,
          VkPipelineStageFlags      dstStages,
          VkAccessFlags             dstAccess) {
    VkMemoryBarrier barrier;
    barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.pNext         = nullptr;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;

    m_cmd->cmdPipelineBarrier(
      DxvkCmdBuffer::ExecBuffer, srcStages, dstStages,
      flags, 1, &barrier, 0, nullptr, 0, nullptr);

    m_cmd->addStatCtr(DxvkStatCounter::CmdBarrierCount, 1);
  }


  VkDescriptorSet DxvkContext::allocateDescriptorSet(
          VkDescriptorSetLayout     layout) {
    if (m_descPool == nullptr)
      m_descPool = m_device->createDescriptorPool();
    
    VkDescriptorSet set = m_descPool->alloc(layout);

    if (set == VK_NULL_HANDLE) {
      m_cmd->trackDescriptorPool(std::move(m_descPool));

      m_descPool = m_device->createDescriptorPool();
      set = m_descPool->alloc(layout);
    }

    return set;
  }

  
  void DxvkContext::trackDrawBuffer() {
    if (m_flags.test(DxvkContextFlag::DirtyDrawBuffer)) {
      m_flags.clr(DxvkContextFlag::DirtyDrawBuffer);

      if (m_state.id.argBuffer.defined())
        m_cmd->trackResource<DxvkAccess::Read>(m_state.id.argBuffer.buffer());

      if (m_state.id.cntBuffer.defined())
        m_cmd->trackResource<DxvkAccess::Read>(m_state.id.cntBuffer.buffer());
    }
  }


  bool DxvkContext::tryInvalidateDeviceLocalBuffer(
      const Rc<DxvkBuffer>&           buffer,
            VkDeviceSize              copySize) {
    // We can only discard if the full buffer gets written, and we will only discard
    // small buffers in order to not waste significant amounts of memory.
    if (copySize != buffer->info().size || copySize > 0x40000)
      return false;

    // Don't discard host-visible buffers since that may interfere with the frontend
    if (buffer->memFlags() & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
      return false;

    // Suspend the current render pass if transform feedback is active prior to
    // invalidating the buffer, since otherwise we may invalidate a bound buffer.
    if ((buffer->info().usage & VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT)
     && (m_flags.test(DxvkContextFlag::GpXfbActive)))
      this->spillRenderPass(true);

    this->invalidateBuffer(buffer, buffer->allocSlice());
    return true;
  }
  

  DxvkGraphicsPipeline* DxvkContext::lookupGraphicsPipeline(
    const DxvkGraphicsPipelineShaders&  shaders) {
    auto idx = shaders.hash() % m_gpLookupCache.size();
    
    if (unlikely(!m_gpLookupCache[idx] || !shaders.eq(m_gpLookupCache[idx]->shaders())))
      m_gpLookupCache[idx] = m_common->pipelineManager().createGraphicsPipeline(shaders);

    return m_gpLookupCache[idx];
  }


  DxvkComputePipeline* DxvkContext::lookupComputePipeline(
    const DxvkComputePipelineShaders&   shaders) {
    auto idx = shaders.hash() % m_cpLookupCache.size();
    
    if (unlikely(!m_cpLookupCache[idx] || !shaders.eq(m_cpLookupCache[idx]->shaders())))
      m_cpLookupCache[idx] = m_common->pipelineManager().createComputePipeline(shaders);

    return m_cpLookupCache[idx];
  }


  Rc<DxvkFramebuffer> DxvkContext::lookupFramebuffer(
    const DxvkFramebufferInfo&      framebufferInfo) {
    DxvkFramebufferKey key = framebufferInfo.key();
    size_t idx = key.hash() % m_framebufferCache.size();

    if (m_framebufferCache[idx] == nullptr || !m_framebufferCache[idx]->key().eq(key))
      m_framebufferCache[idx] = m_device->createFramebuffer(framebufferInfo);

    return m_framebufferCache[idx];
  }


  Rc<DxvkBuffer> DxvkContext::createZeroBuffer(
          VkDeviceSize              size) {
    if (m_zeroBuffer != nullptr && m_zeroBuffer->info().size >= size)
      return m_zeroBuffer;

    DxvkBufferCreateInfo bufInfo;
    bufInfo.size    = align<VkDeviceSize>(size, 1 << 20);
    bufInfo.usage   = VK_BUFFER_USAGE_TRANSFER_DST_BIT
                    | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufInfo.stages  = VK_PIPELINE_STAGE_TRANSFER_BIT;
    bufInfo.access  = VK_ACCESS_TRANSFER_WRITE_BIT
                    | VK_ACCESS_TRANSFER_READ_BIT;

    m_zeroBuffer = m_device->createBuffer(bufInfo,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    this->initBuffer(m_zeroBuffer);
    return m_zeroBuffer;
  }
  
}
