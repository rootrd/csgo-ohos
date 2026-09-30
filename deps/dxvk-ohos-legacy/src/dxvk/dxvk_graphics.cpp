#include "../util/util_time.h"

#include "dxvk_device.h"
#include "dxvk_graphics.h"
#include "dxvk_pipemanager.h"
#include "dxvk_spec_const.h"
#include "dxvk_state_cache.h"
#include "dxvk_winehua_trace.h"
#include "dxvk_winehua_submit_stats.h"
#if defined(DXVK_NATIVE_OHOS)
#include "../util/util_ohos_perf.h"
#endif

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace dxvk {

  DxvkGraphicsPipeline::DxvkGraphicsPipeline(
          DxvkPipelineManager*        pipeMgr,
          DxvkGraphicsPipelineShaders shaders)
  : m_vkd(pipeMgr->m_device->vkd()), m_pipeMgr(pipeMgr),
    m_shaders(std::move(shaders)) {
    if (m_shaders.vs  != nullptr) m_shaders.vs ->defineResourceSlots(m_slotMapping);
    if (m_shaders.tcs != nullptr) m_shaders.tcs->defineResourceSlots(m_slotMapping);
    if (m_shaders.tes != nullptr) m_shaders.tes->defineResourceSlots(m_slotMapping);
    if (m_shaders.gs  != nullptr) m_shaders.gs ->defineResourceSlots(m_slotMapping);
    if (m_shaders.fs  != nullptr) m_shaders.fs ->defineResourceSlots(m_slotMapping);
    
    m_slotMapping.makeDescriptorsDynamic(
      pipeMgr->m_device->options().maxNumDynamicUniformBuffers,
      pipeMgr->m_device->options().maxNumDynamicStorageBuffers);
    
    m_layout = new DxvkPipelineLayout(m_vkd,
      m_slotMapping, VK_PIPELINE_BIND_POINT_GRAPHICS);
    
    m_vsIn  = m_shaders.vs != nullptr ? m_shaders.vs->info().inputMask  : 0;
    m_fsOut = m_shaders.fs != nullptr ? m_shaders.fs->info().outputMask : 0;

    if (m_shaders.gs != nullptr && m_shaders.gs->flags().test(DxvkShaderFlag::HasTransformFeedback))
      m_flags.set(DxvkGraphicsPipelineFlag::HasTransformFeedback);
    
    if (m_layout->getStorageDescriptorStages())
      m_flags.set(DxvkGraphicsPipelineFlag::HasStorageDescriptors);
    
    m_common.msSampleShadingEnable = m_shaders.fs != nullptr && m_shaders.fs->flags().test(DxvkShaderFlag::HasSampleRateShading);
    m_common.msSampleShadingFactor = 1.0f;
  }
  
  
  DxvkGraphicsPipeline::~DxvkGraphicsPipeline() {
    for (const auto& instance : m_pipelines)
      this->destroyPipeline(instance.pipeline());
  }
  
  
  Rc<DxvkShader> DxvkGraphicsPipeline::getShader(
          VkShaderStageFlagBits             stage) const {
    switch (stage) {
      case VK_SHADER_STAGE_VERTEX_BIT:                  return m_shaders.vs;
      case VK_SHADER_STAGE_GEOMETRY_BIT:                return m_shaders.gs;
      case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:    return m_shaders.tcs;
      case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: return m_shaders.tes;
      case VK_SHADER_STAGE_FRAGMENT_BIT:                return m_shaders.fs;
      default:
        return nullptr;
    }
  }


  VkPipeline DxvkGraphicsPipeline::getPipelineHandle(
    const DxvkGraphicsPipelineStateInfo& state,
    const DxvkRenderPass*                renderPass,
          bool                           secondaryOutput) {
    DxvkGraphicsPipelineInstance* instance = this->findInstance(
      state, renderPass, secondaryOutput);

    if (unlikely(!instance)) {
      // Exit early if the state vector is invalid
      if (!this->validatePipelineState(state, true))
        return VK_NULL_HANDLE;

      // Prevent other threads from adding new instances and check again
      std::lock_guard<dxvk::mutex> lock(m_mutex);
      instance = this->findInstance(state, renderPass, secondaryOutput);

      if (!instance) {
        // Keep pipeline object locked, at worst we're going to stall
        // a state cache worker and the current thread needs priority.
        instance = this->createInstance(state, renderPass, secondaryOutput);
        if (!secondaryOutput)
          this->writePipelineStateToCache(state, renderPass->format());
      }
    }

    return instance->pipeline();
  }


  void DxvkGraphicsPipeline::compilePipeline(
    const DxvkGraphicsPipelineStateInfo& state,
    const DxvkRenderPass*                renderPass) {
    // Exit early if the state vector is invalid
    if (!this->validatePipelineState(state, false))
      return;

    // Keep the object locked while compiling a pipeline since compiling
    // similar pipelines concurrently is fragile on some drivers
    std::lock_guard<dxvk::mutex> lock(m_mutex);

    if (!this->findInstance(state, renderPass, false))
      this->createInstance(state, renderPass);
  }


  DxvkGraphicsPipelineInstance* DxvkGraphicsPipeline::createInstance(
    const DxvkGraphicsPipelineStateInfo& state,
    const DxvkRenderPass*                renderPass,
          bool                           secondaryOutput) {
    VkPipeline pipeline = this->createPipeline(
      state, renderPass, secondaryOutput);

    m_pipeMgr->m_numGraphicsPipelines += 1;
    return &(*m_pipelines.emplace(
      state, renderPass, pipeline, secondaryOutput));
  }
  
  
  DxvkGraphicsPipelineInstance* DxvkGraphicsPipeline::findInstance(
    const DxvkGraphicsPipelineStateInfo& state,
    const DxvkRenderPass*                renderPass,
          bool                           secondaryOutput) {
    for (auto& instance : m_pipelines) {
      if (instance.isCompatible(state, renderPass, secondaryOutput))
        return &instance;
    }
    
    return nullptr;
  }
  
  
  VkPipeline DxvkGraphicsPipeline::createPipeline(
    const DxvkGraphicsPipelineStateInfo& state,
    const DxvkRenderPass*                renderPass,
          bool                           secondaryOutput) const {
    if (Logger::logLevel() <= LogLevel::Debug) {
      Logger::debug("Compiling graphics pipeline...");
      this->logPipelineState(LogLevel::Debug, state);
    }

    // Render pass format and image layouts
    DxvkRenderPassFormat passFormat = renderPass->format();

    uint32_t colorAttachmentCount = 0;
    for (uint32_t i = 0; i < MaxNumRenderTargets; i++) {
      if (passFormat.color[i].format != VK_FORMAT_UNDEFINED)
        colorAttachmentCount = i + 1;
    }

    const bool knownD32S8Mrt =
      winehuaSkipKnownD32S8MrtPipeline()
      && colorAttachmentCount >= 4
      && passFormat.depth.format == VK_FORMAT_D32_SFLOAT_S8_UINT
      && m_shaders.vs != nullptr
      && m_shaders.fs != nullptr
      && m_shaders.vs->debugName() ==
        "VS_bdbaa3489b3ec8c417f06cedbbdff0cbbda6cab2"
      && m_shaders.fs->debugName() ==
        "FS_50c4199f44db8227ce517bf10c1e196229651ce5";
    
    // Set up dynamic states as needed
    std::array<VkDynamicState, 6> dynamicStates;
    uint32_t                      dynamicStateCount = 0;
    
    dynamicStates[dynamicStateCount++] = VK_DYNAMIC_STATE_VIEWPORT;
    dynamicStates[dynamicStateCount++] = VK_DYNAMIC_STATE_SCISSOR;

    if (state.useDynamicDepthBias())
      dynamicStates[dynamicStateCount++] = VK_DYNAMIC_STATE_DEPTH_BIAS;
    
    if (state.useDynamicDepthBounds())
      dynamicStates[dynamicStateCount++] = VK_DYNAMIC_STATE_DEPTH_BOUNDS;
    
    if (state.useDynamicBlendConstants())
      dynamicStates[dynamicStateCount++] = VK_DYNAMIC_STATE_BLEND_CONSTANTS;
    
    if (state.useDynamicStencilRef())
      dynamicStates[dynamicStateCount++] = VK_DYNAMIC_STATE_STENCIL_REFERENCE;

    // Figure out the actual sample count to use
    VkSampleCountFlagBits sampleCount = VK_SAMPLE_COUNT_1_BIT;

    if (state.ms.sampleCount())
      sampleCount = VkSampleCountFlagBits(state.ms.sampleCount());
    else if (state.rs.sampleCount())
      sampleCount = VkSampleCountFlagBits(state.rs.sampleCount());
    
    // Set up some specialization constants
    DxvkSpecConstants specData;
    specData.set(uint32_t(DxvkSpecConstantId::RasterizerSampleCount), sampleCount, VK_SAMPLE_COUNT_1_BIT);
    const bool emulateSingleSampleA2C = sampleCount == VK_SAMPLE_COUNT_1_BIT
      && state.ms.enableAlphaToCoverage()
      && dxvkWineHuaEmulateSingleSampleA2C(m_pipeMgr->m_device);
    specData.set(uint32_t(DxvkSpecConstantId::AlphaToCoverageSingleSample),
      emulateSingleSampleA2C, false);

    float epsilon = emulateSingleSampleA2C
      ? dxvkWineHuaSingleSampleA2CEpsilon(m_pipeMgr->m_device) : 0.0f;
    uint32_t epsilonBits = 0;
    std::memcpy(&epsilonBits, &epsilon, sizeof(epsilonBits));
    specData.set(uint32_t(DxvkSpecConstantId::AlphaToCoverageSingleSampleEpsilon),
      epsilonBits, 0u);
    
    for (uint32_t i = 0; i < m_layout->bindingCount(); i++)
      specData.set(i, state.bsBindingMask.test(i), true);
    
    for (uint32_t i = 0; i < MaxNumRenderTargets; i++) {
      if ((m_fsOut & (1 << i)) != 0) {
        specData.set(uint32_t(DxvkSpecConstantId::ColorComponentMappings) + i,
          state.omSwizzle[i].rIndex() << 0 | state.omSwizzle[i].gIndex() << 4 |
          state.omSwizzle[i].bIndex() << 8 | state.omSwizzle[i].aIndex() << 12, 0x3210u);
      }
    }

    for (uint32_t i = 0; i < MaxNumSpecConstants; i++)
      specData.set(getSpecId(i), state.sc.specConstants[i], 0u);
    
    VkSpecializationInfo specInfo = specData.getSpecInfo();

    // Serialize the complete driver compiler lifetime. The state-cache
    // workers and the CS thread may reach different pipeline objects at the
    // same time, which is unstable on affected mobile Vulkan drivers.
    std::lock_guard<dxvk::mutex> pipelineCompileLock(
      m_pipeMgr->m_pipelineCompileMutex);
    const uint64_t pipelineSequence =
      m_pipeMgr->m_pipelineCreateSequence.fetch_add(
        1, std::memory_order_relaxed) + 1;
    const bool tracePipeline = winehuaPipelineTraceEnabled();
    
    winehuaFlowTrace("graphics-pipeline begin");

    auto vsm  = createShaderModule(m_shaders.vs,  state, false);
    auto tcsm = createShaderModule(m_shaders.tcs, state, false);
    auto tesm = createShaderModule(m_shaders.tes, state, false);
    auto gsm  = createShaderModule(m_shaders.gs,  state, false);
    auto fsm  = createShaderModule(m_shaders.fs,  state, secondaryOutput,
      knownD32S8Mrt);

    std::vector<VkPipelineShaderStageCreateInfo> stages;
    if (vsm)  stages.push_back(vsm.stageInfo(&specInfo));
    if (tcsm) stages.push_back(tcsm.stageInfo(&specInfo));
    if (tesm) stages.push_back(tesm.stageInfo(&specInfo));
    if (gsm)  stages.push_back(gsm.stageInfo(&specInfo));
    if (fsm)  stages.push_back(fsm.stageInfo(&specInfo));

    // Fix up color write masks using the component mappings
    std::array<VkPipelineColorBlendAttachmentState, MaxNumRenderTargets> omBlendAttachments;

    const VkColorComponentFlags fullMask
      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
      | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    const uint32_t fsOutMask = knownD32S8Mrt ? 0x1u : m_fsOut;

    for (uint32_t i = 0; i < MaxNumRenderTargets; i++) {
      auto formatInfo = imageFormatInfo(passFormat.color[i].format);
      omBlendAttachments[i] = state.omBlend[i].state();

      if (!(fsOutMask & (1 << i)) || !formatInfo) {
        omBlendAttachments[i].colorWriteMask = 0;
      } else {
        if (omBlendAttachments[i].colorWriteMask != fullMask) {
          omBlendAttachments[i].colorWriteMask = util::remapComponentMask(
            state.omBlend[i].colorWriteMask(), state.omSwizzle[i].mapping());
        }

        omBlendAttachments[i].colorWriteMask &= formatInfo->componentMask;

        if (omBlendAttachments[i].colorWriteMask == formatInfo->componentMask) {
          omBlendAttachments[i].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                                               | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        }
      }
    }

    // Generate per-instance attribute divisors
    std::array<VkVertexInputBindingDivisorDescriptionEXT, MaxNumVertexBindings> viDivisorDesc;
    uint32_t                                                                    viDivisorCount = 0;

    for (uint32_t i = 0; i < state.il.bindingCount(); i++) {
      if (state.ilBindings[i].inputRate() == VK_VERTEX_INPUT_RATE_INSTANCE
       && state.ilBindings[i].divisor()   != 1) {
        const uint32_t id = viDivisorCount++;
        
        viDivisorDesc[id].binding = i; /* see below */
        viDivisorDesc[id].divisor = state.ilBindings[i].divisor();
      }
    }

    int32_t rasterizedStream = m_shaders.gs != nullptr
      ? m_shaders.gs->info().xfbRasterizedStream
      : 0;
    
    // Compact vertex bindings so that we can more easily update vertex buffers
    std::array<VkVertexInputAttributeDescription, MaxNumVertexAttributes> viAttribs;
    std::array<VkVertexInputBindingDescription,   MaxNumVertexBindings>   viBindings;
    std::array<uint32_t,                          MaxNumVertexBindings>   viBindingMap = { };

    for (uint32_t i = 0; i < state.il.bindingCount(); i++) {
      viBindings[i] = state.ilBindings[i].description();
      viBindings[i].binding = i;
      viBindingMap[state.ilBindings[i].binding()] = i;
    }

    for (uint32_t i = 0; i < state.il.attributeCount(); i++) {
      viAttribs[i] = state.ilAttributes[i].description();
      viAttribs[i].binding = viBindingMap[state.ilAttributes[i].binding()];
    }

    VkPipelineVertexInputDivisorStateCreateInfoEXT viDivisorInfo;
    viDivisorInfo.sType                     = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT;
    viDivisorInfo.pNext                     = nullptr;
    viDivisorInfo.vertexBindingDivisorCount = viDivisorCount;
    viDivisorInfo.pVertexBindingDivisors    = viDivisorDesc.data();
    
    VkPipelineVertexInputStateCreateInfo viInfo;
    viInfo.sType                            = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    viInfo.pNext                            = &viDivisorInfo;
    viInfo.flags                            = 0;
    viInfo.vertexBindingDescriptionCount    = state.il.bindingCount();
    viInfo.pVertexBindingDescriptions       = viBindings.data();
    viInfo.vertexAttributeDescriptionCount  = state.il.attributeCount();
    viInfo.pVertexAttributeDescriptions     = viAttribs.data();
    
    if (viDivisorCount == 0)
      viInfo.pNext = viDivisorInfo.pNext;
    
    // TODO remove this once the extension is widely supported
    if (!m_pipeMgr->m_device->features().extVertexAttributeDivisor.vertexAttributeInstanceRateDivisor)
      viInfo.pNext = viDivisorInfo.pNext;
    
    VkPipelineInputAssemblyStateCreateInfo iaInfo;
    iaInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    iaInfo.pNext                  = nullptr;
    iaInfo.flags                  = 0;
    iaInfo.topology               = state.ia.primitiveTopology();
    iaInfo.primitiveRestartEnable = state.ia.primitiveRestart();
    
    VkPipelineTessellationStateCreateInfo tsInfo;
    tsInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;
    tsInfo.pNext                  = nullptr;
    tsInfo.flags                  = 0;
    tsInfo.patchControlPoints     = state.ia.patchVertexCount();
    
    VkPipelineViewportStateCreateInfo vpInfo;
    vpInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vpInfo.pNext                  = nullptr;
    vpInfo.flags                  = 0;
    vpInfo.viewportCount          = state.rs.viewportCount();
    vpInfo.pViewports             = nullptr;
    vpInfo.scissorCount           = state.rs.viewportCount();
    vpInfo.pScissors              = nullptr;
    
    VkPipelineRasterizationConservativeStateCreateInfoEXT conservativeInfo;
    conservativeInfo.sType        = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT;
    conservativeInfo.pNext        = nullptr;
    conservativeInfo.flags        = 0;
    conservativeInfo.conservativeRasterizationMode = state.rs.conservativeMode();
    conservativeInfo.extraPrimitiveOverestimationSize = 0.0f;

    VkPipelineRasterizationStateStreamCreateInfoEXT xfbStreamInfo;
    xfbStreamInfo.sType           = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_STREAM_CREATE_INFO_EXT;
    xfbStreamInfo.pNext           = nullptr;
    xfbStreamInfo.flags           = 0;
    xfbStreamInfo.rasterizationStream = uint32_t(rasterizedStream);

    VkPipelineRasterizationDepthClipStateCreateInfoEXT rsDepthClipInfo;
    rsDepthClipInfo.sType         = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT;
    rsDepthClipInfo.pNext         = nullptr;
    rsDepthClipInfo.flags         = 0;
    rsDepthClipInfo.depthClipEnable = state.rs.depthClipEnable();

    VkPipelineRasterizationStateCreateInfo rsInfo;
    rsInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rsInfo.pNext                  = nullptr;
    rsInfo.flags                  = 0;
    rsInfo.depthClampEnable       = VK_TRUE;
    rsInfo.rasterizerDiscardEnable = rasterizedStream < 0;
    rsInfo.polygonMode            = state.rs.polygonMode();
    rsInfo.cullMode               = state.rs.cullMode();
    rsInfo.frontFace              = state.rs.frontFace();
    rsInfo.depthBiasEnable        = state.rs.depthBiasEnable();
    rsInfo.depthBiasConstantFactor= 0.0f;
    rsInfo.depthBiasClamp         = 0.0f;
    rsInfo.depthBiasSlopeFactor   = 0.0f;
    rsInfo.lineWidth              = 1.0f;
    
    if (rasterizedStream > 0)
      xfbStreamInfo.pNext = std::exchange(rsInfo.pNext, &xfbStreamInfo);

    if (conservativeInfo.conservativeRasterizationMode != VK_CONSERVATIVE_RASTERIZATION_MODE_DISABLED_EXT)
      conservativeInfo.pNext = std::exchange(rsInfo.pNext, &conservativeInfo);

    if (m_pipeMgr->m_device->features().extDepthClipEnable.depthClipEnable)
      rsDepthClipInfo.pNext = std::exchange(rsInfo.pNext, &rsDepthClipInfo);
    else
      rsInfo.depthClampEnable = !state.rs.depthClipEnable();

    uint32_t sampleMask = state.ms.sampleMask();

    VkPipelineMultisampleStateCreateInfo msInfo;
    msInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    msInfo.pNext                  = nullptr;
    msInfo.flags                  = 0;
    msInfo.rasterizationSamples   = sampleCount;
    msInfo.sampleShadingEnable    = m_common.msSampleShadingEnable;
    msInfo.minSampleShading       = m_common.msSampleShadingFactor;
    msInfo.pSampleMask            = &sampleMask;
    msInfo.alphaToCoverageEnable  = state.ms.enableAlphaToCoverage();
    msInfo.alphaToOneEnable       = VK_FALSE;
    
    VkPipelineDepthStencilStateCreateInfo dsInfo;
    dsInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    dsInfo.pNext                  = nullptr;
    dsInfo.flags                  = 0;
    dsInfo.depthTestEnable        = state.ds.enableDepthTest();
    dsInfo.depthWriteEnable       = state.ds.enableDepthWrite() && !util::isDepthReadOnlyLayout(passFormat.depth.layout);
    dsInfo.depthCompareOp         = state.ds.depthCompareOp();
    dsInfo.depthBoundsTestEnable  = state.ds.enableDepthBoundsTest();
    dsInfo.stencilTestEnable      = state.ds.enableStencilTest();
    dsInfo.front                  = state.dsFront.state();
    dsInfo.back                   = state.dsBack.state();
    dsInfo.minDepthBounds         = 0.0f;
    dsInfo.maxDepthBounds         = 1.0f;

    const bool hasDepthAttachment =
      passFormat.depth.format != VK_FORMAT_UNDEFINED;
    if (!hasDepthAttachment) {
      const bool requestedDepthStencil =
        dsInfo.depthTestEnable || dsInfo.depthWriteEnable
        || dsInfo.depthBoundsTestEnable || dsInfo.stencilTestEnable;
      if (requestedDepthStencil) {
        static std::atomic<uint32_t> noDepthAttachmentCount { 0 };
        const uint32_t sequence = noDepthAttachmentCount.fetch_add(
          1, std::memory_order_relaxed) + 1;
        if (sequence <= 32) {
          Logger::warn(str::format(
            "WineHuaNoDepthAttachment: sequence=", sequence,
            " action=disable-depth-stencil",
            " depthTest=", uint32_t(dsInfo.depthTestEnable),
            " depthWrite=", uint32_t(dsInfo.depthWriteEnable),
            " depthBounds=", uint32_t(dsInfo.depthBoundsTestEnable),
            " stencil=", uint32_t(dsInfo.stencilTestEnable),
            " vs=", m_shaders.vs != nullptr ? m_shaders.vs->debugName() : "none",
            " fs=", m_shaders.fs != nullptr ? m_shaders.fs->debugName() : "none"));
        }
      }
      dsInfo.depthTestEnable = VK_FALSE;
      dsInfo.depthWriteEnable = VK_FALSE;
      dsInfo.depthBoundsTestEnable = VK_FALSE;
      dsInfo.stencilTestEnable = VK_FALSE;
    }

    const bool forceHeavenPass2DepthAlways =
      winehuaForceHeavenPass2DepthAlways()
      && dsInfo.depthTestEnable
      && passFormat.color[0].format == VK_FORMAT_R16G16B16A16_SFLOAT
      && passFormat.color[1].format == VK_FORMAT_UNDEFINED
      && passFormat.depth.format == VK_FORMAT_D24_UNORM_S8_UINT
      && m_shaders.vs != nullptr
      && m_shaders.fs != nullptr;

    if (forceHeavenPass2DepthAlways) {
      winehuaPipelineTraceEmit(str::format(
        "WineHuaHeavenDepthAB: forcing compare ALWAYS vs=",
        m_shaders.vs->debugName(), " fs=", m_shaders.fs->debugName(),
        " colorFormat=", uint32_t(passFormat.color[0].format),
        " depthFormat=", uint32_t(passFormat.depth.format),
        " originalCompare=", uint32_t(dsInfo.depthCompareOp),
        " depthWrite=", uint32_t(dsInfo.depthWriteEnable)));
      dsInfo.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    }
    
    /* The stencil workaround exists for one medium-quality foliage shader pair
     * whose 4-MRT + D32S8 pipeline faults inside the Maleoon driver.  Applying
     * it to *every* pipeline with that attachment combination also removed the
     * stencil classification the deferred lighting reads (t11.y & 8), which
     * blacked out all geometry while the sky stayed correct.  The other 27
     * pipelines with the same attachments compiled fine, so the workaround is
     * limited to the pair that actually faults. */
    const bool avoidD32S8MrtStencil =
      winehuaAvoidD32S8MrtStencil()
      && colorAttachmentCount >= 4
      && passFormat.depth.format == VK_FORMAT_D32_SFLOAT_S8_UINT
      && dsInfo.stencilTestEnable
      && m_shaders.vs != nullptr
      && m_shaders.fs != nullptr
      && m_shaders.vs->debugName() ==
        "VS_bdbaa3489b3ec8c417f06cedbbdff0cbbda6cab2"
      && m_shaders.fs->debugName() ==
        "FS_50c4199f44db8227ce517bf10c1e196229651ce5";
    if (avoidD32S8MrtStencil) {
      winehuaPipelineTraceEmit(str::format(
        "WineHuaD32S8MrtStencil: action=disable-stencil",
        " colorAttachmentCount=", colorAttachmentCount,
        " depthFormat=", uint32_t(passFormat.depth.format),
        " vs=", m_shaders.vs != nullptr ? m_shaders.vs->debugName() : "none",
        " fs=", m_shaders.fs != nullptr ? m_shaders.fs->debugName() : "none"));
      dsInfo.stencilTestEnable = VK_FALSE;
    }

    if (knownD32S8Mrt) {
      winehuaPipelineTraceEmit(str::format(
        "WineHuaD32S8MrtKnown: action=mask-aux-outputs",
        " colorAttachmentCount=", colorAttachmentCount,
        " depthFormat=", uint32_t(passFormat.depth.format),
        " vs=", m_shaders.vs->debugName(),
        " fs=", m_shaders.fs->debugName(),
        " originalFsOutMask=0x", std::hex, m_fsOut,
        " effectiveFsOutMask=0x", fsOutMask));
    }

    /* Record the native state of the medium-quality foliage pipeline for every
     * workaround combination, so the stencil / MRT / output-mask matrix can be
     * compared field by field from one build instead of being inferred. */
    if (m_shaders.fs != nullptr
        && m_shaders.fs->debugName()
          == "FS_50c4199f44db8227ce517bf10c1e196229651ce5") {
      const char* dumpPath = std::getenv("DXVK_SHADER_DUMP_PATH");
      if (winehuaShaderDumpEnabled() && dumpPath && dumpPath[0]) {
        std::ofstream record(
          str::tows(str::format(dumpPath, "/g9-tree-shader.log").c_str()).c_str(),
          std::ios_base::app);
        record << "TREE_PIPELINE_STATE vs="
               << (m_shaders.vs != nullptr ? m_shaders.vs->debugName() : "none")
               << " fs=" << m_shaders.fs->debugName()
               << " samples=" << uint32_t(msInfo.rasterizationSamples)
               << " sampleShading=" << uint32_t(msInfo.sampleShadingEnable)
               << " colorAttachmentCount=" << colorAttachmentCount
               << " fsOutMask=0x" << std::hex << fsOutMask << std::dec
               << " depthTest=" << uint32_t(dsInfo.depthTestEnable)
               << " depthWrite=" << uint32_t(dsInfo.depthWriteEnable)
               << " stencilTest=" << uint32_t(dsInfo.stencilTestEnable)
               << " front=(compare=" << uint32_t(dsInfo.front.compareOp)
               << ",fail=" << uint32_t(dsInfo.front.failOp)
               << ",depthFail=" << uint32_t(dsInfo.front.depthFailOp)
               << ",pass=" << uint32_t(dsInfo.front.passOp)
               << ",compareMask=0x" << std::hex << dsInfo.front.compareMask
               << ",writeMask=0x" << dsInfo.front.writeMask
               << ",ref=0x" << dsInfo.front.reference << std::dec << ")"
               << std::endl;
      }
    }

    VkPipelineColorBlendStateCreateInfo cbInfo;
    cbInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbInfo.pNext                  = nullptr;
    cbInfo.flags                  = 0;
    cbInfo.logicOpEnable          = state.om.enableLogicOp();
    cbInfo.logicOp                = state.om.logicOp();
    cbInfo.attachmentCount        = colorAttachmentCount;
    cbInfo.pAttachments           = omBlendAttachments.data();
    
    for (uint32_t i = 0; i < 4; i++)
      cbInfo.blendConstants[i] = 0.0f;
    
    VkPipelineDynamicStateCreateInfo dyInfo;
    dyInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyInfo.pNext                  = nullptr;
    dyInfo.flags                  = 0;
    dyInfo.dynamicStateCount      = dynamicStateCount;
    dyInfo.pDynamicStates         = dynamicStates.data();
    
    VkGraphicsPipelineCreateInfo info;
    info.sType                    = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext                    = nullptr;
    info.flags                    = 0;
    info.stageCount               = stages.size();
    info.pStages                  = stages.data();
    info.pVertexInputState        = &viInfo;
    info.pInputAssemblyState      = &iaInfo;
    info.pTessellationState       = &tsInfo;
    info.pViewportState           = &vpInfo;
    info.pRasterizationState      = &rsInfo;
    info.pMultisampleState        = &msInfo;
    info.pDepthStencilState       = &dsInfo;
    info.pColorBlendState         = &cbInfo;
    info.pDynamicState            = &dyInfo;
    info.layout                   = m_layout->pipelineLayout();
    info.renderPass               = renderPass->getDefaultHandle();
    info.subpass                  = 0;
    info.basePipelineHandle       = VK_NULL_HANDLE;
    info.basePipelineIndex        = -1;
    
    if (tsInfo.patchControlPoints == 0)
      info.pTessellationState = nullptr;
    
    // Time pipeline compilation for debugging purposes
    dxvk::high_resolution_clock::time_point t0, t1;

    if (Logger::logLevel() <= LogLevel::Debug)
      t0 = dxvk::high_resolution_clock::now();
    
    winehuaFlowTrace(str::format(
      "graphics-pipeline vkCreate begin stages=", stages.size()));

    if (tracePipeline) {
      winehuaPipelineTraceEmit(str::format(
        "WineHuaPipelineCreate: phase=begin sequence=", pipelineSequence,
        " type=graphics secondary=", uint32_t(secondaryOutput),
        " profile=", winehuaDiagnosticProfile(),
        " stages=", stages.size(),
        " vs=", m_shaders.vs != nullptr ? m_shaders.vs->debugName() : "none",
        " tcs=", m_shaders.tcs != nullptr ? m_shaders.tcs->debugName() : "none",
        " tes=", m_shaders.tes != nullptr ? m_shaders.tes->debugName() : "none",
        " gs=", m_shaders.gs != nullptr ? m_shaders.gs->debugName() : "none",
        " fs=", m_shaders.fs != nullptr ? m_shaders.fs->debugName() : "none",
        " topology=", uint32_t(iaInfo.topology),
        " patchPoints=", tsInfo.patchControlPoints,
        " viewports=", vpInfo.viewportCount,
        " bindings=", viInfo.vertexBindingDescriptionCount,
        " attributes=", viInfo.vertexAttributeDescriptionCount,
        " samples=", uint32_t(msInfo.rasterizationSamples),
        " renderPassSamples=", uint32_t(passFormat.sampleCount),
        " sampleMask=0x", std::hex, sampleMask,
        " alphaToCoverage=", uint32_t(msInfo.alphaToCoverageEnable),
        " sampleShading=", uint32_t(msInfo.sampleShadingEnable),
        " sampleShadingFactor=", msInfo.minSampleShading,
        " specCount=", specInfo.mapEntryCount,
        " specDataBytes=", specInfo.dataSize,
        " specHash=0x", std::hex, winehuaSpecializationHash(specInfo),
        " depthFormat=", uint32_t(passFormat.depth.format),
        " depthTest=", uint32_t(dsInfo.depthTestEnable),
        " depthWrite=", uint32_t(dsInfo.depthWriteEnable),
        " stencil=", uint32_t(dsInfo.stencilTestEnable),
        " colorAttachmentCount=", colorAttachmentCount,
        " colorFormats=", uint32_t(passFormat.color[0].format), ",",
          uint32_t(passFormat.color[1].format), ",",
          uint32_t(passFormat.color[2].format), ",",
          uint32_t(passFormat.color[3].format), ",",
          uint32_t(passFormat.color[4].format), ",",
          uint32_t(passFormat.color[5].format), ",",
          uint32_t(passFormat.color[6].format), ",",
          uint32_t(passFormat.color[7].format),
        " fsOutMask=0x", std::hex, fsOutMask,
        " vsInMask=0x", m_vsIn,
        " fsFlags=0x", m_shaders.fs != nullptr
          ? uint64_t(m_shaders.fs->flags().raw()) : uint64_t(0),
        " layout=0x", std::hex, m_layout->pipelineLayout(),
        " renderPass=0x", renderPass->getDefaultHandle()));

      auto traceShader = [&](const char* stageName,
          const Rc<DxvkShader>& shader, const DxvkShaderModule& module) {
        if (shader == nullptr)
          return;
        uint32_t resourceTypes = 0;
        for (uint32_t i = 0; i < shader->info().resourceSlotCount && i < 16; i++)
          resourceTypes |= (uint32_t(shader->info().resourceSlots[i].type) & 0x1fu) << (i * 2);
        winehuaPipelineTraceEmit(str::format(
          "WineHuaPipelineShader: sequence=", pipelineSequence,
          " stage=", stageName,
          " name=", shader->debugName(),
          " spirvHash=0x", std::hex, module.winehuaCodeHash(),
          " spirvBytes=", std::dec, module.winehuaCodeSize(),
          " resources=", shader->info().resourceSlotCount,
          " resourceTypes=0x", std::hex, resourceTypes,
          " inputMask=0x", shader->info().inputMask,
          " outputMask=0x", shader->info().outputMask,
          " pushConstants=", std::dec, shader->info().pushConstSize));
      };
      traceShader("vs",  m_shaders.vs,  vsm);
      traceShader("tcs", m_shaders.tcs, tcsm);
      traceShader("tes", m_shaders.tes, tesm);
      traceShader("gs",  m_shaders.gs,  gsm);
      traceShader("fs",  m_shaders.fs,  fsm);

      for (uint32_t i = 0; i < colorAttachmentCount; i++) {
        winehuaPipelineTraceEmit(str::format(
          "WineHuaPipelineBlend: sequence=", pipelineSequence,
          " index=", i,
          " enable=", uint32_t(omBlendAttachments[i].blendEnable),
          " srcColor=", uint32_t(omBlendAttachments[i].srcColorBlendFactor),
          " dstColor=", uint32_t(omBlendAttachments[i].dstColorBlendFactor),
          " colorOp=", uint32_t(omBlendAttachments[i].colorBlendOp),
          " srcAlpha=", uint32_t(omBlendAttachments[i].srcAlphaBlendFactor),
          " dstAlpha=", uint32_t(omBlendAttachments[i].dstAlphaBlendFactor),
          " alphaOp=", uint32_t(omBlendAttachments[i].alphaBlendOp),
          " writeMask=0x", std::hex,
            uint32_t(omBlendAttachments[i].colorWriteMask)));
      }

      for (uint32_t i = 0; i < viInfo.vertexAttributeDescriptionCount; i++) {
        winehuaPipelineTraceEmit(str::format(
          "WineHuaPipelineAttrib: sequence=", pipelineSequence,
          " index=", i,
          " location=", viAttribs[i].location,
          " binding=", viAttribs[i].binding,
          " format=", uint32_t(viAttribs[i].format),
          " offset=", viAttribs[i].offset));
      }
    }

    VkPipeline pipeline = VK_NULL_HANDLE;
    const uint64_t winehuaCompileBeginUs = [&] {
      struct timespec ts = {};
      clock_gettime(CLOCK_MONOTONIC, &ts);
      return uint64_t(ts.tv_sec) * 1000000ull + uint64_t(ts.tv_nsec) / 1000ull;
    }();
    const VkResult pipelineStatus = m_vkd->vkCreateGraphicsPipelines(
      m_vkd->device(), m_pipeMgr->m_cache->handle(),
      1, &info, nullptr, &pipeline);
    {
      struct timespec ts = {};
      clock_gettime(CLOCK_MONOTONIC, &ts);
      const uint64_t endUs = uint64_t(ts.tv_sec) * 1000000ull
        + uint64_t(ts.tv_nsec) / 1000ull;
      winehuaRecordPipelineCompile(endUs - winehuaCompileBeginUs);
      winehuaRecordPipelineOrigin(
        winehuaStateCacheWorkerFlag()
          ? WinehuaPipelineOrigin::StateCacheWorker
          : WinehuaPipelineOrigin::FirstUseSync,
        endUs - winehuaCompileBeginUs);
#if defined(DXVK_NATIVE_OHOS)
      if (ohosperf::enabled())
        ohosperf::record(DXVK_OHOS_PERF_PIPELINE_CREATE, 0,
          uint32_t(std::min<uint64_t>(endUs - winehuaCompileBeginUs, UINT32_MAX)),
          1, uint32_t(pipelineStatus),
          winehuaStateCacheWorkerFlag() ? 1 : 0);
#endif
    }
    if (tracePipeline) {
      winehuaPipelineTraceEmit(str::format(
        "WineHuaPipelineCreate: phase=end sequence=", pipelineSequence,
        " type=graphics result=", int32_t(pipelineStatus),
        " pipeline=0x", std::hex, pipeline));
    }
    if (pipelineStatus != VK_SUCCESS) {
      Logger::err("DxvkGraphicsPipeline: Failed to compile pipeline");
      this->logPipelineState(LogLevel::Error, state);
      return VK_NULL_HANDLE;
    }

    winehuaFlowTrace("graphics-pipeline vkCreate end");

    if (!vsm.winehuaVariantId().empty() || !fsm.winehuaVariantId().empty()) {
      Logger::info(str::format(
        "WineHuaPipelineVariant: pipeline=0x", std::hex, pipeline,
        " vs=", vsm.winehuaVariantId(),
        " tcs=", tcsm.winehuaVariantId(),
        " tes=", tesm.winehuaVariantId(),
        " gs=", gsm.winehuaVariantId(),
        " fs=", fsm.winehuaVariantId()));
    }
    
    if (Logger::logLevel() <= LogLevel::Debug) {
      t1 = dxvk::high_resolution_clock::now();
      auto td = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);
      Logger::debug(str::format("DxvkGraphicsPipeline: Finished in ", td.count(), " ms"));
    }

    return pipeline;
  }
  
  
  void DxvkGraphicsPipeline::destroyPipeline(VkPipeline pipeline) const {
    m_vkd->vkDestroyPipeline(m_vkd->device(), pipeline, nullptr);
  }


  DxvkShaderModule DxvkGraphicsPipeline::createShaderModule(
    const Rc<DxvkShader>&                shader,
    const DxvkGraphicsPipelineStateInfo& state,
          bool                           secondaryOutput,
          bool                           winehuaDropAuxiliaryOutputs) const {
    if (shader == nullptr)
      return DxvkShaderModule();

    const DxvkShaderCreateInfo& shaderInfo = shader->info();
    DxvkShaderModuleCreateInfo info;
    info.freezeBoolSpec = dxvkWineHuaFreezeBoolSpec(m_pipeMgr->m_device);
    info.boolSpecMask = &state.bsBindingMask;
    info.boolSpecCount = m_layout->bindingCount();
    info.winehuaDropAuxiliaryOutputs = winehuaDropAuxiliaryOutputs;
    info.winehuaSingleSampleInterpolation =
      winehuaReplaceSingleSampleInterpolation()
      && shaderInfo.stage == VK_SHADER_STAGE_FRAGMENT_BIT
      && state.ms.sampleCount() == VK_SAMPLE_COUNT_1_BIT;

    // Fix up fragment shader outputs for dual-source blending
    if (shaderInfo.stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
      info.fsSecondaryOutput = secondaryOutput;
      info.fsDualSrcBlend = !secondaryOutput
        && state.omBlend[0].blendEnable() && (
        util::isDualSourceBlendFactor(state.omBlend[0].srcColorBlendFactor()) ||
        util::isDualSourceBlendFactor(state.omBlend[0].dstColorBlendFactor()) ||
        util::isDualSourceBlendFactor(state.omBlend[0].srcAlphaBlendFactor()) ||
        util::isDualSourceBlendFactor(state.omBlend[0].dstAlphaBlendFactor()));
    }

    // Deal with undefined shader inputs
    uint32_t consumedInputs = shaderInfo.inputMask;
    uint32_t providedInputs = 0;

    if (shaderInfo.stage == VK_SHADER_STAGE_VERTEX_BIT) {
      for (uint32_t i = 0; i < state.il.attributeCount(); i++)
        providedInputs |= 1u << state.ilAttributes[i].location();
    } else if (shaderInfo.stage != VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT) {
      auto prevStage = getPrevStageShader(shaderInfo.stage);
      providedInputs = prevStage->info().outputMask;
    } else {
      // Technically not correct, but this
      // would need a lot of extra care
      providedInputs = consumedInputs;
    }

    info.undefinedInputs = (providedInputs & consumedInputs) ^ consumedInputs;
    return shader->createShaderModule(m_vkd, m_slotMapping, info);
  }


  Rc<DxvkShader> DxvkGraphicsPipeline::getPrevStageShader(VkShaderStageFlagBits stage) const {
    if (stage == VK_SHADER_STAGE_VERTEX_BIT)
      return nullptr;

    if (stage == VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT)
      return m_shaders.tcs;

    Rc<DxvkShader> result = m_shaders.vs;

    if (stage == VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT)
      return result;

    if (m_shaders.tes != nullptr)
      result = m_shaders.tes;

    if (stage == VK_SHADER_STAGE_GEOMETRY_BIT)
      return result;

    if (m_shaders.gs != nullptr)
      result = m_shaders.gs;

    return result;
  }


  bool DxvkGraphicsPipeline::validatePipelineState(
    const DxvkGraphicsPipelineStateInfo&  state,
          bool                            trusted) const {
    // Tessellation shaders and patches must be used together
    bool hasPatches = state.ia.primitiveTopology() == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;

    bool hasTcs = m_shaders.tcs != nullptr;
    bool hasTes = m_shaders.tes != nullptr;

    if (hasPatches != hasTcs || hasPatches != hasTes)
      return false;
    
    // Filter out undefined primitive topologies
    if (state.ia.primitiveTopology() == VK_PRIMITIVE_TOPOLOGY_MAX_ENUM)
      return false;
    
    // Prevent unintended out-of-bounds access to the IL arrays
    if (state.il.attributeCount() > DxvkLimits::MaxNumVertexAttributes
     || state.il.bindingCount()   > DxvkLimits::MaxNumVertexBindings)
      return false;

    // Exit here on the fast path, perform more thorough validation if
    // the state vector comes from an untrusted source (i.e. the cache)
    if (trusted)
      return true;

    // Validate shaders
    if (!m_shaders.validate()) {
      Logger::err("Invalid pipeline: Shader types do not match stage");
      return false;
    }

    // Validate vertex input layout
    const DxvkDevice* device = m_pipeMgr->m_device;
    uint32_t ilLocationMask = 0;
    uint32_t ilBindingMask = 0;

    for (uint32_t i = 0; i < state.il.bindingCount(); i++)
      ilBindingMask |= 1u << state.ilBindings[i].binding();

    for (uint32_t i = 0; i < state.il.attributeCount(); i++) {
      const DxvkIlAttribute& attribute = state.ilAttributes[i];

      if (ilLocationMask & (1u << attribute.location())) {
        Logger::err(str::format("Invalid pipeline: Vertex location ", attribute.location(), " defined twice"));
        return false;
      }

      if (!(ilBindingMask & (1u << attribute.binding()))) {
        Logger::err(str::format("Invalid pipeline: Vertex binding ", attribute.binding(), " not defined"));
        return false;
      }

      VkFormatProperties formatInfo = device->adapter()->formatProperties(attribute.format());

      if (!(formatInfo.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) {
        Logger::err(str::format("Invalid pipeline: Format ", attribute.format(), " not supported for vertex buffers"));
        return false;
      }

      ilLocationMask |= 1u << attribute.location();
    }

    // Validate rasterization state
    if (state.rs.conservativeMode() != VK_CONSERVATIVE_RASTERIZATION_MODE_DISABLED_EXT) {
      if (!device->extensions().extConservativeRasterization) {
        Logger::err("Conservative rasterization not supported by device");
        return false;
      }

      if (state.rs.conservativeMode() == VK_CONSERVATIVE_RASTERIZATION_MODE_UNDERESTIMATE_EXT
       && !device->properties().extConservativeRasterization.primitiveUnderestimation) {
        Logger::err("Primitive underestimation not supported by device");
        return false;
      }
    }

    // Validate depth-stencil state
    if (state.ds.enableDepthBoundsTest() && !device->features().core.features.depthBounds) {
      Logger::err("Depth bounds not supported by device");
      return false;
    }

    return true;
  }
  
  
  void DxvkGraphicsPipeline::writePipelineStateToCache(
    const DxvkGraphicsPipelineStateInfo& state,
    const DxvkRenderPassFormat&          format) const {
    if (m_pipeMgr->m_stateCache == nullptr)
      return;
    
    DxvkStateCacheKey key;
    if (m_shaders.vs  != nullptr) key.vs = m_shaders.vs->getShaderKey();
    if (m_shaders.tcs != nullptr) key.tcs = m_shaders.tcs->getShaderKey();
    if (m_shaders.tes != nullptr) key.tes = m_shaders.tes->getShaderKey();
    if (m_shaders.gs  != nullptr) key.gs = m_shaders.gs->getShaderKey();
    if (m_shaders.fs  != nullptr) key.fs = m_shaders.fs->getShaderKey();

    m_pipeMgr->m_stateCache->addGraphicsPipeline(key, state, format);
  }
  
  
  void DxvkGraphicsPipeline::logPipelineState(
          LogLevel                       level,
    const DxvkGraphicsPipelineStateInfo& state) const {
    if (m_shaders.vs  != nullptr) Logger::log(level, str::format("  vs  : ", m_shaders.vs ->debugName()));
    if (m_shaders.tcs != nullptr) Logger::log(level, str::format("  tcs : ", m_shaders.tcs->debugName()));
    if (m_shaders.tes != nullptr) Logger::log(level, str::format("  tes : ", m_shaders.tes->debugName()));
    if (m_shaders.gs  != nullptr) Logger::log(level, str::format("  gs  : ", m_shaders.gs ->debugName()));
    if (m_shaders.fs  != nullptr) Logger::log(level, str::format("  fs  : ", m_shaders.fs ->debugName()));

    for (uint32_t i = 0; i < state.il.attributeCount(); i++) {
      const auto& attr = state.ilAttributes[i];
      Logger::log(level, str::format("  attr ", i, " : location ", attr.location(), ", binding ", attr.binding(), ", format ", attr.format(), ", offset ", attr.offset()));
    }
    for (uint32_t i = 0; i < state.il.bindingCount(); i++) {
      const auto& bind = state.ilBindings[i];
      Logger::log(level, str::format("  binding ", i, " : binding ", bind.binding(), ", stride ", bind.stride(), ", rate ", bind.inputRate(), ", divisor ", bind.divisor()));
    }
    
    // TODO log more pipeline state
  }
  
}
