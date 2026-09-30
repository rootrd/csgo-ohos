#include "dxvk_shader.h"
#include "dxvk_adapter.h"
#include "dxvk_device.h"
#include "dxvk_winehua_trace.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "../util/util_env.h"

namespace dxvk {

  static uint32_t rewriteWinehuaAuxiliaryFragmentOutputs(
      SpirvCodeBuffer& code,
      bool             auxiliaryLocations,
      bool             sampleMaskBuiltIn);

  static uint32_t rewriteWinehuaSampleInterpolation(SpirvCodeBuffer& code);

  static bool winehuaShaderDumpSelected(const std::string& debugName) {
    const char* filter = std::getenv("DXVK_WINEHUA_DUMP_SHADER_NAMES");
    if (!filter || !filter[0])
      return true;

    /* Comma-separated exact debug names. Keep matching deliberately strict so
     * a diagnostic run cannot unexpectedly dump the full shader catalogue. */
    const std::string names(filter);
    size_t begin = 0;
    while (begin <= names.size()) {
      size_t end = names.find(',', begin);
      if (end == std::string::npos)
        end = names.size();
      if (names.substr(begin, end - begin) == debugName)
        return true;
      if (end == names.size())
        break;
      begin = end + 1;
    }
    return false;
  }

  static bool winehuaShaderDumpClaim(uint64_t hash) {
    static std::mutex mutex;
    static std::unordered_set<uint64_t> hashes;
    std::lock_guard<std::mutex> lock(mutex);
    if (hashes.find(hash) != hashes.end())
      return false;
    if (hashes.size() >= 512)
      return false;
    hashes.insert(hash);
    return true;
  }

  bool dxvkWineHuaFreezeBoolSpec(const DxvkDevice* device) {
    const std::string override = env::getEnvVar("DXVK_WINEHUA_FREEZE_BOOL_SPEC");
    if (override == "1" || override == "true")
      return true;
    if (override == "0" || override == "false")
      return false;

    const std::string quirks = env::getEnvVar("WINEHUA_DXVK_QUIRKS");
    if (quirks.find("venus-bool-spec") != std::string::npos)
      return true;
    if (quirks.find("no-venus-bool-spec") != std::string::npos)
      return false;

    if (!device || device->adapter() == nullptr)
      return false;

    const char* deviceName = device->adapter()->deviceProperties().deviceName;
    /* The current Harmony path exposes the Maleoon device through Mesa
     * Venus.  Keep this deliberately narrow: other Vulkan implementations
     * retain native specialization constants unless explicitly quirked. */
    return std::strstr(deviceName, "Venus") != nullptr ||
           std::strstr(deviceName, "Maleoon") != nullptr;
  }

  bool dxvkWineHuaEmulateSingleSampleA2C(const DxvkDevice* device) {
    const std::string override =
      env::getEnvVar("DXVK_WINEHUA_SINGLE_SAMPLE_A2C");
    bool enabled = false;

    if (override == "1" || override == "true") {
      enabled = true;
    } else if (override == "0" || override == "false") {
      enabled = false;
    } else {
      const std::string quirks = env::getEnvVar("WINEHUA_DXVK_QUIRKS");
      if (quirks.find("maleoon-single-sample-a2c") != std::string::npos) {
        enabled = true;
      } else if (quirks.find("no-maleoon-single-sample-a2c") != std::string::npos) {
        enabled = false;
      } else if (device && device->adapter() != nullptr) {
        const char* deviceName = device->adapter()->deviceProperties().deviceName;
        enabled = std::strstr(deviceName, "Maleoon") != nullptr;
      }
    }

    if (enabled) {
      static std::atomic<bool> logged { false };
      if (!logged.exchange(true, std::memory_order_relaxed)) {
        Logger::info(
          "WineHua: single-sample alpha-to-coverage transparent-texel fallback enabled");
      }
    }

    return enabled;
  }

  float dxvkWineHuaSingleSampleA2CEpsilon(const DxvkDevice* device) {
    if (!dxvkWineHuaEmulateSingleSampleA2C(device))
      return 0.0f;

    static const float epsilon = [] {
      const std::string value =
        env::getEnvVar("DXVK_WINEHUA_SINGLE_SAMPLE_A2C_EPSILON");
      if (value.empty())
        return 0.0f;

      char* end = nullptr;
      const float parsed = std::strtof(value.c_str(), &end);
      if (end == value.c_str() || *end != '\0' || parsed < 0.0f || parsed > 0.25f)
        return 0.0f;
      return parsed;
    }();

    return epsilon;
  }

  static bool freezeBoolSpecConstants(
          SpirvCodeBuffer&       codeBuffer,
          const DxvkBindingMask* bindingMask,
          uint32_t               bindingCount) {
    const uint32_t* code = codeBuffer.data();
    uint32_t wordCount = codeBuffer.dwords();

    if (wordCount < 5 || code[0] != 0x07230203u || !code[3] || code[3] > 65536u)
      return false;

    std::vector<uint8_t> boolSpecIds(code[3]);
    std::vector<uint32_t> boolSpecValues(code[3], uint32_t(-1));
    uint32_t offset = 5;

    while (offset < wordCount) {
      uint32_t instruction = code[offset];
      uint16_t words = uint16_t(instruction >> 16);
      uint16_t opcode = uint16_t(instruction & 0xffffu);

      if (!words || offset + words > wordCount)
        return false;

      if ((opcode == spv::OpSpecConstantTrue || opcode == spv::OpSpecConstantFalse)
       && words >= 3 && code[offset + 2] < boolSpecIds.size()) {
        boolSpecIds[code[offset + 2]] = 1;
        boolSpecValues[code[offset + 2]] =
          opcode == spv::OpSpecConstantTrue ? 1u : 0u;
      }

      offset += words;
    }

    std::vector<uint32_t> frozen(code, code + 5);
    bool changed = false;
    offset = 5;

    while (offset < wordCount) {
      uint32_t instruction = code[offset];
      uint16_t words = uint16_t(instruction >> 16);
      uint16_t opcode = uint16_t(instruction & 0xffffu);

      if (opcode == spv::OpDecorate && words >= 4
       && code[offset + 2] == spv::DecorationSpecId
       && code[offset + 1] < boolSpecIds.size()
       && boolSpecIds[code[offset + 1]]) {
        changed = true;
        offset += words;
        continue;
      }

      size_t start = frozen.size();
      frozen.insert(frozen.end(), code + offset, code + offset + words);

      if (opcode == spv::OpSpecConstantTrue || opcode == spv::OpSpecConstantFalse) {
        uint32_t value = boolSpecValues[code[offset + 2]];
        if (bindingMask && value != uint32_t(-1)) {
          /* Binding bools use SpecId 0..bindingCount-1.  Other bool
           * specialization constants (for example legacy fixed-function
           * state) retain their SPIR-V default value. */
          uint32_t specId = uint32_t(-1);
          uint32_t scan = 5;
          while (scan < wordCount) {
            uint32_t scanInstruction = code[scan];
            uint16_t scanWords = uint16_t(scanInstruction >> 16);
            uint16_t scanOpcode = uint16_t(scanInstruction & 0xffffu);
            if (scanOpcode == spv::OpDecorate && scanWords >= 4 &&
                code[scan + 1] == code[offset + 2] &&
                code[scan + 2] == spv::DecorationSpecId) {
              specId = code[scan + 3];
              break;
            }
            if (!scanWords || scan + scanWords > wordCount) break;
            scan += scanWords;
          }
          if (specId < bindingCount)
            value = bindingMask->test(specId) ? 1u : 0u;
        }
        frozen[start] = (uint32_t(words) << 16) |
          uint32_t(value ? spv::OpConstantTrue : spv::OpConstantFalse);
        changed = true;
      }

      offset += words;
    }

    if (changed)
      codeBuffer = SpirvCodeBuffer(frozen.size(), frozen.data());

    return changed;
  }
  
  DxvkShaderModule::DxvkShaderModule()
  : m_vkd(nullptr), m_stage() {

  }


  DxvkShaderModule::DxvkShaderModule(DxvkShaderModule&& other)
  : m_vkd(std::move(other.m_vkd)),
    m_winehuaVariantId(std::move(other.m_winehuaVariantId)),
    m_winehuaCodeHash(other.m_winehuaCodeHash),
    m_winehuaCodeSize(other.m_winehuaCodeSize) {
    this->m_stage = other.m_stage;
    other.m_stage = VkPipelineShaderStageCreateInfo();
    other.m_winehuaCodeHash = 0;
    other.m_winehuaCodeSize = 0;
  }


  DxvkShaderModule::DxvkShaderModule(
    const Rc<vk::DeviceFn>&     vkd,
    const Rc<DxvkShader>&       shader,
    const SpirvCodeBuffer&      code)
  : m_vkd(vkd), m_stage() {
    m_stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    m_stage.pNext = nullptr;
    m_stage.flags = 0;
    m_stage.stage = shader->info().stage;
    m_stage.module = VK_NULL_HANDLE;
    m_stage.pName = "main";
    m_stage.pSpecializationInfo = nullptr;

    VkShaderModuleCreateInfo info;
    info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.pNext    = nullptr;
    info.flags    = 0;
    info.codeSize = code.size();
    info.pCode    = code.data();

    uint64_t hash = 1469598103934665603ull;
    const auto* bytes = reinterpret_cast<const uint8_t*>(code.data());
    for (size_t i = 0; i < code.size(); i++) {
      hash ^= bytes[i];
      hash *= 1099511628211ull;
    }
    m_winehuaCodeHash = hash;
    m_winehuaCodeSize = code.size();

    const char* remappedDump = std::getenv("DXVK_WINEHUA_DUMP_REMAPPED_SPIRV");
    const char* dumpPath = std::getenv("DXVK_SHADER_DUMP_PATH");
    /* The game tags the draws whose shaders matter for the current
     * investigation (the deferred lighting draws). A tagged shader is dumped
     * regardless of the general name filter and hash budget, because the
     * catalogue cap is consumed long before the lighting draws run. */
    // The game marks the draw it wants dumped with a file in the dump
    // directory; DXVK already writes there, so the path is known-good.
    bool lightDraw = false;
    /* Reading the marker costs a file open per compiled shader, so it follows
     * the port's telemetry switch together with the dumps it selects. */
    if (winehuaShaderDumpEnabled() && dumpPath && dumpPath[0]) {
      std::ifstream lightMark(
        str::tows(str::format(dumpPath, "/g9-light-draw").c_str()).c_str());
      lightDraw = lightMark.good();
    }
    if (dumpPath && dumpPath[0] && (lightDraw
        || (remappedDump && remappedDump[0] == '1'
          && winehuaShaderDumpSelected(shader->debugName())
          && winehuaShaderDumpClaim(hash)))) {

      m_winehuaVariantId = str::format(shader->debugName(), "-", std::hex, hash);
      std::ofstream uniqueDump(
        str::tows(str::format(dumpPath, "/", shader->debugName(),
          ".remapped-", std::hex, hash, ".spv").c_str()).c_str(),
        std::ios_base::binary | std::ios_base::trunc);
      code.store(uniqueDump);

      std::ofstream conventionalDump(
        str::tows(str::format(dumpPath, "/", shader->debugName(),
          ".remapped.spv").c_str()).c_str(),
        std::ios_base::binary | std::ios_base::trunc);
      code.store(conventionalDump);

      if (lightDraw) {
        std::ofstream markerDump(
          str::tows(str::format(dumpPath, "/", shader->debugName(),
            ".lightdraw-", std::hex, hash, ".spv").c_str()).c_str(),
          std::ios_base::binary | std::ios_base::trunc);
        code.store(markerDump);
      }
    }
    
    if (m_vkd->vkCreateShaderModule(m_vkd->device(), &info, nullptr, &m_stage.module) != VK_SUCCESS)
      throw DxvkError("DxvkComputePipeline::DxvkComputePipeline: Failed to create shader module");
  }
  
  
  DxvkShaderModule::~DxvkShaderModule() {
    if (m_vkd != nullptr) {
      m_vkd->vkDestroyShaderModule(
        m_vkd->device(), m_stage.module, nullptr);
    }
  }
  
  
  DxvkShaderModule& DxvkShaderModule::operator = (DxvkShaderModule&& other) {
    this->m_vkd   = std::move(other.m_vkd);
    this->m_stage = other.m_stage;
    this->m_winehuaVariantId = std::move(other.m_winehuaVariantId);
    this->m_winehuaCodeHash = other.m_winehuaCodeHash;
    this->m_winehuaCodeSize = other.m_winehuaCodeSize;
    other.m_stage = VkPipelineShaderStageCreateInfo();
    other.m_winehuaCodeHash = 0;
    other.m_winehuaCodeSize = 0;
    return *this;
  }


  DxvkShader::DxvkShader(
    const DxvkShaderCreateInfo&   info,
          SpirvCodeBuffer&&       spirv)
  : m_info(info), m_code(spirv) {
    m_info.resourceSlots = nullptr;
    m_info.uniformData = nullptr;

    // Copy resource binding slot infos
    if (info.resourceSlotCount) {
      m_slots.resize(info.resourceSlotCount);
      for (uint32_t i = 0; i < info.resourceSlotCount; i++)
        m_slots[i] = info.resourceSlots[i];
      m_info.resourceSlots = m_slots.data();
    }

    // Copy uniform buffer data
    if (info.uniformSize) {
      m_uniformData.resize(info.uniformSize);
      std::memcpy(m_uniformData.data(), info.uniformData, info.uniformSize);
      m_info.uniformData = m_uniformData.data();
    }

    // Run an analysis pass over the SPIR-V code to gather some
    // info that we may need during pipeline compilation.
    SpirvCodeBuffer code = std::move(spirv);
    std::unordered_map<uint32_t, size_t> locationZeroOffsets;
    std::unordered_map<uint32_t, size_t> locationOneOffsets;
    std::unordered_map<uint32_t, size_t> indexOffsets;
    std::unordered_set<uint32_t> outputVars;
    
    for (auto ins : code) {
      if (ins.opCode() == spv::OpDecorate) {
        if (ins.arg(2) == spv::DecorationBinding
         || ins.arg(2) == spv::DecorationSpecId)
          m_idOffsets.push_back(ins.offset() + 3);
        
        if (ins.arg(2) == spv::DecorationLocation) {
          if (ins.arg(3) == 0)
            locationZeroOffsets.insert({ ins.arg(1), ins.offset() + 3 });
          else if (ins.arg(3) == 1)
            locationOneOffsets.insert({ ins.arg(1), ins.offset() + 3 });
        }
        
        if (ins.arg(2) == spv::DecorationIndex)
          indexOffsets.insert({ ins.arg(1), ins.offset() + 3 });
      }

      if (ins.opCode() == spv::OpVariable
       && spv::StorageClass(ins.arg(3)) == spv::StorageClassOutput)
        outputVars.insert(ins.arg(2));

      if (ins.opCode() == spv::OpExecutionMode) {
        if (ins.arg(2) == spv::ExecutionModeStencilRefReplacingEXT)
          m_flags.set(DxvkShaderFlag::ExportsStencilRef);

        if (ins.arg(2) == spv::ExecutionModeXfb)
          m_flags.set(DxvkShaderFlag::HasTransformFeedback);
      }

      if (ins.opCode() == spv::OpCapability) {
        if (ins.arg(1) == spv::CapabilitySampleRateShading)
          m_flags.set(DxvkShaderFlag::HasSampleRateShading);

        if (ins.arg(1) == spv::CapabilityShaderViewportIndexLayerEXT)
          m_flags.set(DxvkShaderFlag::ExportsViewportIndexLayerFromVertexStage);
      }
    }

    for (uint32_t varId : outputVars) {
      const auto locationZero = locationZeroOffsets.find(varId);
      const auto location = locationOneOffsets.find(varId);
      const auto index = indexOffsets.find(varId);

      if (locationZero != locationZeroOffsets.end()
       && index != indexOffsets.end())
        m_o0LocOffset = locationZero->second;

      if (location != locationOneOffsets.end() && index != indexOffsets.end()) {
        m_o1LocOffset = location->second;
        m_o1IdxOffset = index->second;
      }

      if (m_o0LocOffset && m_o1LocOffset)
        break;
    }
  }


  DxvkShader::~DxvkShader() {
    
  }
  
  
  void DxvkShader::defineResourceSlots(
          DxvkDescriptorSlotMapping& mapping) const {
    for (const auto& slot : m_slots)
      mapping.defineSlot(m_info.stage, slot);
    
    if (m_info.pushConstSize) {
      mapping.definePushConstRange(m_info.stage,
        m_info.pushConstOffset,
        m_info.pushConstSize);
    }
  }
  
  
  DxvkShaderModule DxvkShader::createShaderModule(
    const Rc<vk::DeviceFn>&          vkd,
    const DxvkDescriptorSlotMapping& mapping,
    const DxvkShaderModuleCreateInfo& info) {
    SpirvCodeBuffer spirvCode = m_code.decompress();
    uint32_t* code = spirvCode.data();
    
    // Remap resource binding IDs
    for (uint32_t ofs : m_idOffsets) {
      if (code[ofs] < MaxNumResourceSlots)
        code[ofs] = mapping.getBindingId(code[ofs]);
    }

    /* The ordinary DXVK shader dump is emitted before this remap. For the
     * WineHua investigation, optionally capture the exact SPIR-V binary that
     * is handed to vkCreateShaderModule, including final set/binding IDs. */
    // For dual-source blending we need to re-map
    // location 1, index 0 to location 0, index 1
    if (info.fsSecondaryOutput && m_o0LocOffset && m_o1LocOffset)
      std::swap(code[m_o0LocOffset], code[m_o1LocOffset]);
    else if (info.fsDualSrcBlend && m_o1IdxOffset && m_o1LocOffset)
      std::swap(code[m_o1IdxOffset], code[m_o1LocOffset]);
    
    // Replace undefined input variables with zero
    for (uint32_t u : bit::BitMask(info.undefinedInputs))
      eliminateInput(spirvCode, u);

    if (info.freezeBoolSpec)
      freezeBoolSpecConstants(spirvCode, info.boolSpecMask, info.boolSpecCount);

    const bool isFragment = m_info.stage == VK_SHADER_STAGE_FRAGMENT_BIT;
    /* The medium-quality foliage shader reads its interpolant per sample
     * (SSTAA coverage).  On a single-sample pipeline the Maleoon driver faults
     * while compiling the pipeline that pairs that module with the full
     * four-attachment G-buffer and the engine's stencil state, and the engine
     * cannot recompile the shader.  Reading the interpolant directly is
     * value-identical at one sample, so the rewrite is applied to this shader
     * unconditionally instead of being left to the global diagnostic switch.
     * That is what lets the geometry pass keep its stencil classification and
     * all four G-buffer outputs, which the deferred lighting needs. */
    const bool foliageInterpolationFix = isFragment
      && debugName() == "FS_50c4199f44db8227ce517bf10c1e196229651ce5";
    if (foliageInterpolationFix) {
      const uint32_t replaced = rewriteWinehuaSampleInterpolation(spirvCode);
      /* A second pass returning zero proves no InterpolateAtSample is left. */
      const uint32_t remaining = rewriteWinehuaSampleInterpolation(spirvCode);
      const char* dumpPath = std::getenv("DXVK_SHADER_DUMP_PATH");
      if (winehuaShaderDumpEnabled() && dumpPath && dumpPath[0]) {
        std::ofstream record(
          str::tows(str::format(dumpPath, "/g9-tree-shader.log").c_str()).c_str(),
          std::ios_base::app);
        record << "TREE_SHADER_FINAL fs=" << debugName()
               << " stage=fragment foliageFix=1 replaced=" << replaced
               << " remaining=" << remaining
               << " codeSize=" << spirvCode.dwords() * 4
               << std::endl;
      }
      Logger::info(str::format(
        "WineHuaTreeShader: fs=", debugName(), " replaced=", replaced,
        " remaining=", remaining,
        " codeSize=", spirvCode.dwords() * 4));
    }
    const bool dropAuxiliaryOutputs = info.winehuaDropAuxiliaryOutputs && isFragment;
    const bool dropSampleMaskOutput = isFragment && winehuaDropSampleMaskOutput();

    if (dropAuxiliaryOutputs || dropSampleMaskOutput) {
      const uint32_t rewritten = rewriteWinehuaAuxiliaryFragmentOutputs(
        spirvCode, dropAuxiliaryOutputs, dropSampleMaskOutput);
      if (rewritten) {
        Logger::info(str::format(
          "WineHuaD32S8MrtShader: action=drop-aux-outputs shader=",
          isFragment ? "fragment" : "other",
          " variables=", rewritten,
          " auxiliary=", uint32_t(dropAuxiliaryOutputs),
          " sampleMask=", uint32_t(dropSampleMaskOutput)));
      }
    }

    if (isFragment && info.winehuaSingleSampleInterpolation) {
      const uint32_t replaced = rewriteWinehuaSampleInterpolation(spirvCode);
      if (replaced) {
        Logger::info(str::format(
          "WineHuaSampleInterpolation: action=replace-at-sample count=", replaced));
      }
    }

    /* Dump the final module consumed by vkCreate*Pipelines.  The bool
     * binding specialization workaround above can rewrite OpSpecConstantTrue
     * to OpConstantTrue and remove its SpecId decoration.  Dumping before
     * that rewrite made the so-called exact replay exercise a different
     * SPIR-V binary than the runtime pipeline. */
    return DxvkShaderModule(vkd, this, spirvCode);
  }
  
  
  void DxvkShader::dump(std::ostream& outputStream) const {
    m_code.decompress().store(outputStream);
  }


  /*
   * Rewrites selected fragment outputs into private variables.  Affected
   * Maleoon drivers have been observed to fault while compiling one very
   * specific D32S8/4-MRT pipeline even when the auxiliary color write masks
   * are zero.  Merely removing the OpEntryPoint interface IDs is not enough:
   * SPIR-V still contains Output variables and stores to them, so the driver
   * lowers the same interface.  Converting the auxiliary variables (locations
   * 1..3) and any access-chain pointers derived from them to Private removes
   * them from the fragment interface while preserving the shader's internal
   * stores.  The primary location 0 output is untouched.
   *
   * The same transformation optionally covers the fragment SampleMask
   * (SV_Coverage) output and is then used as an isolation switch: the crashing
   * foliage shader writes coverage from inside a loop, which its low-quality
   * counterpart that compiles fine does not do.  The switch only removes the
   * output from this module; it never changes the rest of the pipeline.
   */
  static uint32_t rewriteWinehuaAuxiliaryFragmentOutputs(
      SpirvCodeBuffer& code,
      bool             auxiliaryLocations,
      bool             sampleMaskBuiltIn) {
    const uint32_t* source = code.data();
    const uint32_t wordCount = code.dwords();
    if (!source || wordCount < 5 || source[0] != spv::MagicNumber)
      return 0;

    struct Instruction {
      uint32_t offset;
      uint16_t words;
      spv::Op op;
    };

    std::vector<Instruction> instructions;
    instructions.reserve(wordCount / 4);
    for (uint32_t offset = 5; offset < wordCount; ) {
      const uint32_t token = source[offset];
      const uint16_t words = uint16_t(token >> spv::WordCountShift);
      if (!words || offset + words > wordCount)
        return 0;
      instructions.push_back({ offset, words,
        static_cast<spv::Op>(token & spv::OpCodeMask) });
      offset += words;
    }

    std::unordered_set<uint32_t> interfaceOutputs;
    for (const Instruction& instruction : instructions) {
      if (instruction.op != spv::OpDecorate || instruction.words < 4)
        continue;
      const uint32_t offset = instruction.offset;
      if (auxiliaryLocations &&
          source[offset + 2] == spv::DecorationLocation &&
          source[offset + 3] >= 1 && source[offset + 3] <= 3) {
        interfaceOutputs.insert(source[offset + 1]);
      } else if (sampleMaskBuiltIn &&
          source[offset + 2] == spv::DecorationBuiltIn &&
          source[offset + 3] == spv::BuiltInSampleMask) {
        interfaceOutputs.insert(source[offset + 1]);
      }
    }

    std::unordered_map<uint32_t, uint32_t> targetVariableTypes;
    std::unordered_set<uint32_t> targetVariables;
    for (const Instruction& instruction : instructions) {
      if (instruction.op != spv::OpVariable || instruction.words < 4)
        continue;
      const uint32_t offset = instruction.offset;
      const uint32_t variableId = source[offset + 2];
      if (source[offset + 3] == spv::StorageClassOutput &&
          interfaceOutputs.find(variableId) != interfaceOutputs.end()) {
        targetVariables.insert(variableId);
        targetVariableTypes[variableId] = source[offset + 1];
      }
    }

    if (targetVariables.empty())
      return 0;

    std::unordered_map<uint32_t, uint32_t> pointerPointeeTypes;
    std::unordered_map<uint32_t, uint32_t> pointerStorageClasses;
    for (const Instruction& instruction : instructions) {
      if (instruction.op != spv::OpTypePointer || instruction.words < 4)
        continue;
      const uint32_t offset = instruction.offset;
      pointerStorageClasses[source[offset + 1]] = source[offset + 2];
      pointerPointeeTypes[source[offset + 1]] = source[offset + 3];
    }

    /* Track pointers derived from the selected variables.  Direct stores do
     * not need a type rewrite, but an OpAccessChain result explicitly names a
     * pointer type and must move from Output to Private as well. */
    std::unordered_set<uint32_t> derivedIds = targetVariables;
    std::unordered_set<uint32_t> derivedPointerTypes;
    for (const auto& entry : targetVariableTypes)
      derivedPointerTypes.insert(entry.second);

    bool discovered = true;
    while (discovered) {
      discovered = false;
      for (const Instruction& instruction : instructions) {
        const uint32_t offset = instruction.offset;
        uint32_t baseId = 0;
        uint32_t resultId = 0;
        if (instruction.op == spv::OpAccessChain ||
            instruction.op == spv::OpInBoundsAccessChain ||
            instruction.op == spv::OpPtrAccessChain) {
          if (instruction.words < 4)
            continue;
          resultId = source[offset + 2];
          baseId = source[offset + 3];
        } else if (instruction.op == spv::OpCopyObject ||
                   instruction.op == spv::OpBitcast) {
          if (instruction.words < 4)
            continue;
          resultId = source[offset + 2];
          baseId = source[offset + 3];
        } else {
          continue;
        }

        if (derivedIds.find(baseId) == derivedIds.end())
          continue;
        if (derivedIds.insert(resultId).second)
          discovered = true;
        derivedPointerTypes.insert(source[offset + 1]);
      }
    }

    std::unordered_map<uint32_t, uint32_t> privatePointerTypes;
    uint32_t nextId = source[3];
    for (uint32_t pointerType : derivedPointerTypes) {
      const auto storage = pointerStorageClasses.find(pointerType);
      const auto pointee = pointerPointeeTypes.find(pointerType);
      if (storage == pointerStorageClasses.end() ||
          pointee == pointerPointeeTypes.end() ||
          storage->second != spv::StorageClassOutput)
        continue;
      privatePointerTypes[pointerType] = nextId++;
    }

    if (privatePointerTypes.empty())
      return 0;

    uint32_t firstVariableOffset = wordCount;
    for (const Instruction& instruction : instructions) {
      if (instruction.op == spv::OpVariable) {
        firstVariableOffset = instruction.offset;
        break;
      }
    }
    if (firstVariableOffset == wordCount)
      return 0;

    std::vector<uint32_t> rewritten;
    rewritten.reserve(wordCount + privatePointerTypes.size() * 4);
    rewritten.insert(rewritten.end(), source, source + 5);
    rewritten[3] = nextId;

    auto isInterfaceDecoration = [](uint32_t decoration) {
      switch (decoration) {
        case spv::DecorationLocation:
        case spv::DecorationIndex:
        case spv::DecorationComponent:
        case spv::DecorationBuiltIn:
        case spv::DecorationFlat:
        case spv::DecorationNoPerspective:
        case spv::DecorationCentroid:
        case spv::DecorationSample:
        case spv::DecorationPatch:
        case spv::DecorationInvariant:
        case spv::DecorationStream:
        case spv::DecorationXfbBuffer:
        case spv::DecorationXfbStride:
          return true;
        default:
          return false;
      }
    };

    bool pointerTypesInserted = false;
    uint32_t rewrittenVariables = 0;
    for (const Instruction& instruction : instructions) {
      if (!pointerTypesInserted && instruction.offset == firstVariableOffset) {
        for (const auto& entry : privatePointerTypes) {
          const auto pointee = pointerPointeeTypes.find(entry.first);
          if (pointee == pointerPointeeTypes.end())
            continue;
          rewritten.push_back((uint32_t(4) << spv::WordCountShift) |
            uint32_t(spv::OpTypePointer));
          rewritten.push_back(entry.second);
          rewritten.push_back(spv::StorageClassPrivate);
          rewritten.push_back(pointee->second);
        }
        pointerTypesInserted = true;
      }

      const uint32_t offset = instruction.offset;
      if (instruction.op == spv::OpDecorate && instruction.words >= 3 &&
          targetVariables.find(source[offset + 1]) != targetVariables.end() &&
          isInterfaceDecoration(source[offset + 2])) {
        continue;
      }

      if (instruction.op == spv::OpEntryPoint && instruction.words >= 4) {
        std::vector<uint32_t> entryPoint(source + offset,
          source + offset + instruction.words);
        uint32_t nameEnd = offset + 3;
        while (nameEnd < offset + instruction.words) {
          const uint32_t word = source[nameEnd++];
          if ((word & 0xffU) == 0 || (word & 0xff00U) == 0 ||
              (word & 0xff0000U) == 0 || (word & 0xff000000U) == 0)
            break;
        }
        const uint32_t interfaceStart = nameEnd;
        if (interfaceStart <= offset + instruction.words) {
          entryPoint.resize(3 + (interfaceStart - (offset + 3)));
          for (uint32_t cursor = interfaceStart;
               cursor < offset + instruction.words; ++cursor) {
            if (targetVariables.find(source[cursor]) == targetVariables.end())
              entryPoint.push_back(source[cursor]);
          }
          entryPoint[0] = (uint32_t(entryPoint.size()) << spv::WordCountShift) |
            uint32_t(spv::OpEntryPoint);
          rewritten.insert(rewritten.end(), entryPoint.begin(), entryPoint.end());
          continue;
        }
      }

      std::vector<uint32_t> current(source + offset,
        source + offset + instruction.words);
      if (instruction.op == spv::OpVariable && instruction.words >= 4) {
        const uint32_t variableId = source[offset + 2];
        const auto target = targetVariableTypes.find(variableId);
        if (target != targetVariableTypes.end()) {
          const auto privateType = privatePointerTypes.find(target->second);
          if (privateType != privatePointerTypes.end()) {
            current[1] = privateType->second;
            current[3] = spv::StorageClassPrivate;
            ++rewrittenVariables;
          }
        }
      }

      if ((instruction.op == spv::OpAccessChain ||
           instruction.op == spv::OpInBoundsAccessChain ||
           instruction.op == spv::OpPtrAccessChain ||
           instruction.op == spv::OpCopyObject ||
           instruction.op == spv::OpBitcast) && instruction.words >= 3) {
        const uint32_t resultId = source[offset + 2];
        if (derivedIds.find(resultId) != derivedIds.end()) {
          const auto privateType = privatePointerTypes.find(source[offset + 1]);
          if (privateType != privatePointerTypes.end())
            current[1] = privateType->second;
        }
      }

      rewritten.insert(rewritten.end(), current.begin(), current.end());
    }

    if (!pointerTypesInserted || rewrittenVariables == 0)
      return 0;

    code = SpirvCodeBuffer(rewritten.size(), rewritten.data());
    return rewrittenVariables;
  }


  /* Replaces per-sample interpolant reads with ordinary input reads.  See the
   * rationale on DxvkShaderModuleCreateInfo::winehuaSingleSampleInterpolation:
   * with one rasterization sample both forms evaluate the same interpolant, so
   * the replacement is semantics-preserving, while the affected Maleoon driver
   * faults on the original instruction inside the foliage shader's loop. */
  static uint32_t rewriteWinehuaSampleInterpolation(SpirvCodeBuffer& code) {
    constexpr uint32_t kGlslInterpolateAtSample = 77;

    const uint32_t* source = code.data();
    const uint32_t wordCount = code.dwords();
    if (!source || wordCount < 5 || source[0] != spv::MagicNumber)
      return 0;

    struct Instruction {
      uint32_t offset;
      uint16_t words;
      spv::Op op;
    };

    std::vector<Instruction> instructions;
    instructions.reserve(wordCount / 4);
    for (uint32_t offset = 5; offset < wordCount; ) {
      const uint32_t token = source[offset];
      const uint16_t words = uint16_t(token >> spv::WordCountShift);
      if (!words || offset + words > wordCount)
        return 0;
      instructions.push_back({ offset, words,
        static_cast<spv::Op>(token & spv::OpCodeMask) });
      offset += words;
    }

    /* Resolve variable ids to the pointee type of their pointer type so every
     * replacement can be proven type-correct before it is emitted. */
    std::unordered_map<uint32_t, uint32_t> pointerPointees;
    for (const Instruction& instruction : instructions) {
      if (instruction.op == spv::OpTypePointer && instruction.words >= 4)
        pointerPointees[source[instruction.offset + 1]] = source[instruction.offset + 3];
    }

    std::unordered_map<uint32_t, uint32_t> variablePointees;
    for (const Instruction& instruction : instructions) {
      if (instruction.op != spv::OpVariable || instruction.words < 4)
        continue;
      const auto entry = pointerPointees.find(source[instruction.offset + 1]);
      if (entry != pointerPointees.end())
        variablePointees[source[instruction.offset + 2]] = entry->second;
    }

    std::vector<uint32_t> rewritten;
    rewritten.reserve(wordCount);
    rewritten.insert(rewritten.end(), source, source + 5);

    uint32_t replaced = 0;
    for (const Instruction& instruction : instructions) {
      const uint32_t offset = instruction.offset;
      if (instruction.op == spv::OpExtInst && instruction.words == 7
          && source[offset + 4] == kGlslInterpolateAtSample) {
        const uint32_t resultType = source[offset + 1];
        const auto pointee = variablePointees.find(source[offset + 5]);
        if (pointee != variablePointees.end() && pointee->second == resultType) {
          rewritten.push_back((uint32_t(4) << spv::WordCountShift) |
            uint32_t(spv::OpLoad));
          rewritten.push_back(resultType);
          rewritten.push_back(source[offset + 2]);
          rewritten.push_back(source[offset + 5]);
          ++replaced;
          continue;
        }
      }
      rewritten.insert(rewritten.end(), source + offset,
        source + offset + instruction.words);
    }

    if (!replaced)
      return 0;

    code = SpirvCodeBuffer(rewritten.size(), rewritten.data());
    return replaced;
  }


  void DxvkShader::eliminateInput(SpirvCodeBuffer& code, uint32_t location) {
    struct SpirvTypeInfo {
      spv::Op           op            = spv::OpNop;
      uint32_t          baseTypeId    = 0;
      uint32_t          compositeSize = 0;
      spv::StorageClass storageClass  = spv::StorageClassMax;
    };

    std::unordered_map<uint32_t, SpirvTypeInfo> types;
    std::unordered_map<uint32_t, uint32_t>      constants;
    std::unordered_set<uint32_t>                candidates;

    // Find the input variable in question
    size_t   inputVarOffset = 0;
    uint32_t inputVarTypeId = 0;
    uint32_t inputVarId     = 0;

    for (auto ins : code) {
      if (ins.opCode() == spv::OpDecorate) {
        if (ins.arg(2) == spv::DecorationLocation
         && ins.arg(3) == location)
          candidates.insert(ins.arg(1));
      }

      if (ins.opCode() == spv::OpConstant)
        constants.insert({ ins.arg(2), ins.arg(3) });

      if (ins.opCode() == spv::OpTypeFloat || ins.opCode() == spv::OpTypeInt)
        types.insert({ ins.arg(1), { ins.opCode(), 0, ins.arg(2), spv::StorageClassMax }});

      if (ins.opCode() == spv::OpTypeVector)
        types.insert({ ins.arg(1), { ins.opCode(), ins.arg(2), ins.arg(3), spv::StorageClassMax }});

      if (ins.opCode() == spv::OpTypeArray) {
        auto constant = constants.find(ins.arg(3));
        if (constant == constants.end())
          continue;
        types.insert({ ins.arg(1), { ins.opCode(), ins.arg(2), constant->second, spv::StorageClassMax }});
      }

      if (ins.opCode() == spv::OpTypePointer)
        types.insert({ ins.arg(1), { ins.opCode(), ins.arg(3), 0, spv::StorageClass(ins.arg(2)) }});

      if (ins.opCode() == spv::OpVariable && spv::StorageClass(ins.arg(3)) == spv::StorageClassInput) {
        if (candidates.find(ins.arg(2)) != candidates.end()) {
          inputVarOffset = ins.offset();
          inputVarTypeId = ins.arg(1);
          inputVarId     = ins.arg(2);
          break;
        }
      }
    }

    if (!inputVarId)
      return;

    // Declare private pointer types
    auto pointerType = types.find(inputVarTypeId);
    if (pointerType == types.end())
      return;

    code.beginInsertion(inputVarOffset);
    std::vector<std::pair<uint32_t, SpirvTypeInfo>> privateTypes;

    for (auto p  = types.find(pointerType->second.baseTypeId);
              p != types.end();
              p  = types.find(p->second.baseTypeId)) {
      std::pair<uint32_t, SpirvTypeInfo> info = *p;
      info.first = 0;
      info.second.baseTypeId = p->first;
      info.second.storageClass = spv::StorageClassPrivate;

      for (auto t : types) {
        if (t.second.op           == info.second.op
         && t.second.baseTypeId   == info.second.baseTypeId
         && t.second.storageClass == info.second.storageClass)
          info.first = t.first;
      }

      if (!info.first) {
        info.first = code.allocId();

        code.putIns(spv::OpTypePointer, 4);
        code.putWord(info.first);
        code.putWord(info.second.storageClass);
        code.putWord(info.second.baseTypeId);
      }

      privateTypes.push_back(info);
    }

    // Define zero constants
    uint32_t constantId = 0;

    for (auto i = privateTypes.rbegin(); i != privateTypes.rend(); i++) {
      if (constantId) {
        uint32_t compositeSize = i->second.compositeSize;
        uint32_t compositeId   = code.allocId();

        code.putIns(spv::OpConstantComposite, 3 + compositeSize);
        code.putWord(i->second.baseTypeId);
        code.putWord(compositeId);

        for (uint32_t i = 0; i < compositeSize; i++)
          code.putWord(constantId);

        constantId = compositeId;
      } else {
        constantId = code.allocId();

        code.putIns(spv::OpConstant, 4);
        code.putWord(i->second.baseTypeId);
        code.putWord(constantId);
        code.putWord(0);
      }
    }

    // Erase and re-declare variable
    code.erase(4);

    code.putIns(spv::OpVariable, 5);
    code.putWord(privateTypes[0].first);
    code.putWord(inputVarId);
    code.putWord(spv::StorageClassPrivate);
    code.putWord(constantId);

    code.endInsertion();

    // Remove variable from interface list
    for (auto ins : code) {
      if (ins.opCode() == spv::OpEntryPoint) {
        uint32_t argIdx = 2 + code.strLen(ins.chr(2));

        while (argIdx < ins.length()) {
          if (ins.arg(argIdx) == inputVarId) {
            ins.setArg(0, spv::OpEntryPoint | ((ins.length() - 1) << spv::WordCountShift));

            code.beginInsertion(ins.offset() + argIdx);
            code.erase(1);
            code.endInsertion();
            break;
          }

          argIdx += 1;
        }
      }
    }

    // Remove location and other declarations
    for (auto iter = code.begin(); iter != code.end(); ) {
      auto ins = *(iter++);

      if (ins.opCode() == spv::OpDecorate && ins.arg(1) == inputVarId) {
        uint32_t numWords;

        switch (ins.arg(2)) {
          case spv::DecorationLocation:
          case spv::DecorationFlat:
          case spv::DecorationNoPerspective:
          case spv::DecorationCentroid:
          case spv::DecorationPatch:
          case spv::DecorationSample:
            numWords = ins.length();
            break;

          default:
            numWords = 0;
        }

        if (numWords) {
          code.beginInsertion(ins.offset());
          code.erase(numWords);

          iter = SpirvInstructionIterator(code.data(), code.endInsertion(), code.dwords());
        }
      }

      if (ins.opCode() == spv::OpFunction)
        break;
    }

    // Fix up pointer types used in access chain instructions
    std::unordered_map<uint32_t, uint32_t> accessChainIds;

    for (auto ins : code) {
      if (ins.opCode() == spv::OpAccessChain
       || ins.opCode() == spv::OpInBoundsAccessChain) {
        uint32_t depth = ins.length() - 4;

        if (ins.arg(3) == inputVarId) {
          // Access chains accessing the variable directly
          ins.setArg(1, privateTypes.at(depth).first);
          accessChainIds.insert({ ins.arg(2), depth });
        } else {
          // Access chains derived from the variable
          auto entry = accessChainIds.find(ins.arg(2));
          if (entry != accessChainIds.end()) {
            depth += entry->second;
            ins.setArg(1, privateTypes.at(depth).first);
            accessChainIds.insert({ ins.arg(2), depth });
          }
        }
      }
    }
  }
  
}
