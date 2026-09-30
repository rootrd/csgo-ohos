#include "d3d11_device.h"
#include "d3d11_shader.h"
#include "../dxvk/dxvk_winehua_trace.h"
#if defined(DXVK_NATIVE_OHOS)
#include "../util/util_ohos_perf.h"
#endif

namespace dxvk {
  
  D3D11CommonShader:: D3D11CommonShader() { }
  D3D11CommonShader::~D3D11CommonShader() { }
  
  
  D3D11CommonShader::D3D11CommonShader(
          D3D11Device*    pDevice,
    const DxvkShaderKey*  pShaderKey,
    const DxbcModuleInfo* pDxbcModuleInfo,
    const void*           pShaderBytecode,
          size_t          BytecodeLength) {
    const std::string name = pShaderKey->toString();
    Logger::debug(str::format("Compiling shader ", name));
    
    DxbcReader reader(
      reinterpret_cast<const char*>(pShaderBytecode),
      BytecodeLength);
    
    DxbcModule module(reader);
    
    // If requested by the user, dump both the raw DXBC
    // shader and the compiled SPIR-V module to a file.  The port keeps this
    // behind its shader dump switch: it was writing two files per compiled
    // shader (a few hundred megabytes on a first run) on every shipped launch.
    const std::string dumpPath = env::getEnvVar("DXVK_SHADER_DUMP_PATH");
    const bool dumpShaders = dumpPath.size() != 0 && winehuaShaderDumpEnabled();
    
    if (dumpShaders) {
      reader.store(std::ofstream(str::tows(str::format(dumpPath, "/", name, ".dxbc").c_str()).c_str(),
        std::ios_base::binary | std::ios_base::trunc));
    }
    
    // Decide whether we need to create a pass-through
    // geometry shader for vertex shader stream output
    bool passthroughShader = pDxbcModuleInfo->xfb != nullptr
      && (module.programInfo().type() == DxbcProgramType::VertexShader
       || module.programInfo().type() == DxbcProgramType::DomainShader);

    if (module.programInfo().shaderStage() != pShaderKey->type() && !passthroughShader)
      throw DxvkError("Mismatching shader type.");

#if defined(DXVK_NATIVE_OHOS)
    const uint64_t perfCompileStart = ohosperf::enabled() ? ohosperf::nowNs() : 0;
#endif
    m_shader = passthroughShader
      ? module.compilePassthroughShader(*pDxbcModuleInfo, name)
      : module.compile                 (*pDxbcModuleInfo, name);
#if defined(DXVK_NATIVE_OHOS)
    if (perfCompileStart)
      ohosperf::record(DXVK_OHOS_PERF_SHADER_COMPILE, 0,
        ohosperf::elapsedUs(perfCompileStart), BytecodeLength,
        uint32_t(pShaderKey->type()));
#endif
    m_shader->setShaderKey(*pShaderKey);
    
    if (dumpShaders) {
      std::ofstream dumpStream(
        str::tows(str::format(dumpPath, "/", name, ".spv").c_str()).c_str(),
        std::ios_base::binary | std::ios_base::trunc);
      
      m_shader->dump(dumpStream);
    }
    
    // Create shader constant buffer if necessary
    const DxvkShaderCreateInfo& shaderInfo = m_shader->info();

    if (shaderInfo.uniformSize) {
      DxvkBufferCreateInfo info;
      info.size   = shaderInfo.uniformSize;
      info.usage  = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
      info.stages = util::pipelineStages(shaderInfo.stage);
      info.access = VK_ACCESS_UNIFORM_READ_BIT;
      
      VkMemoryPropertyFlags memFlags
        = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
        | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
        | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
      
      m_buffer = pDevice->GetDXVKDevice()->createBuffer(info, memFlags);
      std::memcpy(m_buffer->mapPtr(0), shaderInfo.uniformData, shaderInfo.uniformSize);
      if (winehuaFlushDynamicMapped())
        m_buffer->flushMappedSlice(m_buffer->getSliceHandle());
    }

    pDevice->GetDXVKDevice()->registerShader(m_shader);
  }

  
  D3D11ShaderModuleSet:: D3D11ShaderModuleSet() { }
  D3D11ShaderModuleSet::~D3D11ShaderModuleSet() { }
  
  
  HRESULT D3D11ShaderModuleSet::GetShaderModule(
          D3D11Device*        pDevice,
    const DxvkShaderKey*      pShaderKey,
    const DxbcModuleInfo*     pDxbcModuleInfo,
    const void*               pShaderBytecode,
          size_t              BytecodeLength,
          D3D11CommonShader*  pShader) {
    // Use the shader's unique key for the lookup
    { std::unique_lock<dxvk::mutex> lock(m_mutex);
      
      auto entry = m_modules.find(*pShaderKey);
      if (entry != m_modules.end()) {
        *pShader = entry->second;
        return S_OK;
      }
    }
    
    // This shader has not been compiled yet, so we have to create a
    // new module. This takes a while, so we won't lock the structure.
    D3D11CommonShader module;
    
    try {
      module = D3D11CommonShader(pDevice, pShaderKey,
        pDxbcModuleInfo, pShaderBytecode, BytecodeLength);
    } catch (const DxvkError& e) {
      Logger::err(e.message());
      return E_INVALIDARG;
    }
    
    // Insert the new module into the lookup table. If another thread
    // has compiled the same shader in the meantime, we should return
    // that object instead and discard the newly created module.
    { std::unique_lock<dxvk::mutex> lock(m_mutex);
      
      auto status = m_modules.insert({ *pShaderKey, module });
      if (!status.second) {
        *pShader = status.first->second;
        return S_OK;
      }
    }
    
    *pShader = std::move(module);
    return S_OK;
  }
  
}
