#include "../d3d11/d3d11_options.h"
#include "../util/util_env.h"

#include "dxbc_options.h"

#include <cstring>
#include <fstream>
#include <mutex>
#include <string>

#include "../util/util_string.h"

namespace dxvk {

  /* The DXVK log lives in the launcher log directory, which the host cannot read
   * back over hdc.  The compatibility paths decided in this file are the ones
   * that differ per device, and they are what a device-specific rendering
   * difference has to be checked against, so they are mirrored next to the other
   * port evidence. */
  static void winehuaOptionLog(const std::string& text) {
    Logger::info(text);
    const char* dumpPath = std::getenv("DXVK_SHADER_DUMP_PATH");
    if (!dumpPath || !dumpPath[0])
      return;
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    std::ofstream record(
      str::tows(str::format(dumpPath, "/g9-dxvk-options.log").c_str()).c_str(),
      std::ios_base::app);
    record << text << std::endl;
  }
  
  DxbcOptions::DxbcOptions() {

  }


  DxbcOptions::DxbcOptions(const Rc<DxvkDevice>& device, const D3D11Options& options) {
    const Rc<DxvkAdapter> adapter = device->adapter();

    {
      const VkPhysicalDeviceProperties& properties = adapter->deviceProperties();
      winehuaOptionLog(str::format("WineHua: device=", properties.deviceName,
        " api=", VK_VERSION_MAJOR(properties.apiVersion), ".",
        VK_VERSION_MINOR(properties.apiVersion), ".",
        VK_VERSION_PATCH(properties.apiVersion),
        " driver=", VK_VERSION_MAJOR(properties.driverVersion), ".",
        VK_VERSION_MINOR(properties.driverVersion), ".",
        VK_VERSION_PATCH(properties.driverVersion)));
    }

    const DxvkDeviceFeatures& devFeatures = device->features();
    const DxvkDeviceInfo& devInfo = adapter->devicePropertiesExt();

    useDepthClipWorkaround
      = !devFeatures.extDepthClipEnable.depthClipEnable;
    useStorageImageReadWithoutFormat
      = devFeatures.core.features.shaderStorageImageReadWithoutFormat;
    useSubgroupOpsForAtomicCounters
      = (devInfo.coreSubgroup.supportedStages     & VK_SHADER_STAGE_COMPUTE_BIT)
     && (devInfo.coreSubgroup.supportedOperations & VK_SUBGROUP_FEATURE_BALLOT_BIT);
    /* D3D-style discard can be expressed either as a real kill or as a demote to
     * helper invocation.  The second form is only equivalent when the driver
     * really stops the invocation from writing, and the device that advertises
     * the extension is also the device that renders light and particle geometry
     * that should be discarded, which is what the very strong lighting report
     * looks like.  The switch forces the kill path the device without the
     * extension already uses, so the two can be compared on one machine:
     * WINEHUA_DXVK_DEMOTE_HELPER=0 (launcher: --ps g9demote 0). */
    useDemoteToHelperInvocation
      = (devFeatures.extShaderDemoteToHelperInvocation.shaderDemoteToHelperInvocation);
    {
      const std::string demoteOverride = env::getEnvVar("WINEHUA_DXVK_DEMOTE_HELPER");
      if (demoteOverride == "0")
        useDemoteToHelperInvocation = false;
      else if (demoteOverride == "1")
        useDemoteToHelperInvocation = true;
    }
    /* Stage-interface component counts.
     *
     * Huawei's Android reference keeps the full four registers on a stage
     * interface unless the device advertises VK_KHR_maintenance4, which is what
     * relaxes the matching rules.  That gate is the wrong key for this driver
     * family: the device that advertises it (Vulkan 1.3) is also the device that
     * renders a particle sprite as a full-screen glow, while the device without
     * it renders the same effect correctly.  The full-register shape is legal on
     * both, so it is what the port emits, and the switch puts the relaxed shape
     * back for an A/B run. */
    supportsMaintenance4 =
      env::getEnvVar("WINEHUA_DXVK_STAGE_INTERFACE") == "relaxed";
    useSubgroupOpsForEarlyDiscard
      = (devInfo.coreSubgroup.subgroupSize >= 4)
     && (devInfo.coreSubgroup.supportedStages     & VK_SHADER_STAGE_FRAGMENT_BIT)
     && (devInfo.coreSubgroup.supportedOperations & VK_SUBGROUP_FEATURE_BALLOT_BIT);
    useSdivForBufferIndex
      = adapter->matchesDriver(DxvkGpuVendor::Nvidia, VK_DRIVER_ID_NVIDIA_PROPRIETARY_KHR, 0, 0);
    
    switch (device->config().useRawSsbo) {
      case Tristate::Auto:  minSsboAlignment = devInfo.core.properties.limits.minStorageBufferOffsetAlignment; break;
      case Tristate::True:  minSsboAlignment =  4u; break;
      case Tristate::False: minSsboAlignment = ~0u; break;
    }
    
    invariantPosition        = options.invariantPosition;
    enableRtOutputNanFixup   = options.enableRtOutputNanFixup;
    zeroInitWorkgroupMemory  = options.zeroInitWorkgroupMemory;
    forceTgsmBarriers        = options.forceTgsmBarriers;
    disableMsaa              = options.disableMsaa;
    useCombinedImageSampler  = env::getEnvVar("WINEHUA_DXVK_COMBINED_SAMPLER") == "1";
    emulateCustomBorderColor = !devFeatures.extCustomBorderColor
      .customBorderColorWithoutFormat
      && env::getEnvVar("WINEHUA_DXVK_DISABLE_CUSTOM_BORDER_EMULATION") != "1";
    {
      const std::string override = env::getEnvVar("WINEHUA_DXVK_PAD_CUBE_DREF_COORD");
      const std::string quirks = env::getEnvVar("WINEHUA_DXVK_QUIRKS");
      const char* deviceName = adapter->deviceProperties().deviceName;

      if (override == "1" || override == "true")
        padCubeDrefCoordinates = true;
      else if (override == "0" || override == "false")
        padCubeDrefCoordinates = false;
      else if (quirks.find("maleoon-cube-dref") != std::string::npos)
        padCubeDrefCoordinates = true;
      else if (quirks.find("no-maleoon-cube-dref") != std::string::npos)
        padCubeDrefCoordinates = false;
      else
        padCubeDrefCoordinates = std::strstr(deviceName, "Maleoon") != nullptr;
    }
    {
      const std::string override =
        env::getEnvVar("WINEHUA_DXVK_EMULATE_CUBE_ARRAY_DREF");
      const std::string quirks = env::getEnvVar("WINEHUA_DXVK_QUIRKS");
      const char* deviceName = adapter->deviceProperties().deviceName;

      if (override == "1" || override == "true")
        emulateCubeArrayDref = true;
      else if (override == "0" || override == "false")
        emulateCubeArrayDref = false;
      else if (quirks.find("maleoon-cube-array-dref") != std::string::npos)
        emulateCubeArrayDref = true;
      else if (quirks.find("no-maleoon-cube-array-dref") != std::string::npos)
        emulateCubeArrayDref = false;
      else
        emulateCubeArrayDref = std::strstr(deviceName, "Maleoon") != nullptr;
    }
    {
      const std::string override =
        env::getEnvVar("WINEHUA_DXVK_REPLACE_CUBE_DREF");
      if (override == "1" || override == "true" || override == "one")
        replaceCubeDref = 1;
      else if (override == "0" || override == "false" || override == "zero")
        replaceCubeDref = 0;
      else
        replaceCubeDref = -1;
    }
    if (useCombinedImageSampler)
      winehuaOptionLog("WineHua: combined image sampler compatibility mode enabled");
    winehuaOptionLog(str::format(
      "WineHua: DXBC stage interface path=",
      supportsMaintenance4 ? "maintenance4-relaxed" : "full-register-compat"));
    winehuaOptionLog(str::format(
      "WineHua: Cube Dref coordinate path=",
      padCubeDrefCoordinates ? "padded-vec4" : "native-minimal"));
    winehuaOptionLog(str::format(
      "WineHua: CubeArray Dref path=",
      emulateCubeArrayDref ? "2d-array-emulation" : "native"));
    winehuaOptionLog(str::format(
      "WineHua: Cube Dref replace path=",
      replaceCubeDref < 0 ? "native"
        : (replaceCubeDref > 0 ? "constant-1" : "constant-0")));
    winehuaOptionLog(str::format(
      "WineHua: custom border capability path=",
      emulateCustomBorderColor ? "shader-emulation" : "native",
      " customBorderColors=",
      devFeatures.extCustomBorderColor.customBorderColors ? 1 : 0,
      " customBorderColorWithoutFormat=",
      devFeatures.extCustomBorderColor.customBorderColorWithoutFormat ? 1 : 0));
    winehuaOptionLog(str::format(
      "WineHua: RT output NaN fixup=",
      enableRtOutputNanFixup ? "on" : "off"));
    winehuaOptionLog(str::format("WineHua: discard path=",
      useDemoteToHelperInvocation ? "demote-to-helper" : "kill",
      " deviceDemoteSupported=",
      devFeatures.extShaderDemoteToHelperInvocation.shaderDemoteToHelperInvocation ? 1 : 0));
    dynamicIndexedConstantBufferAsSsbo = options.constantBufferRangeCheck;

    // Disable subgroup early discard on Nvidia because it may hurt performance
    if (adapter->matchesDriver(DxvkGpuVendor::Nvidia, VK_DRIVER_ID_NVIDIA_PROPRIETARY_KHR, 0, 0))
      useSubgroupOpsForEarlyDiscard = false;
    
    // Figure out float control flags to match D3D11 rules
    if (options.floatControls) {
      if (devInfo.khrShaderFloatControls.shaderSignedZeroInfNanPreserveFloat32)
        floatControl.set(DxbcFloatControlFlag::PreserveNan32);
      if (devInfo.khrShaderFloatControls.shaderSignedZeroInfNanPreserveFloat64)
        floatControl.set(DxbcFloatControlFlag::PreserveNan64);

      if (devInfo.khrShaderFloatControls.denormBehaviorIndependence != VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE) {
        if (devInfo.khrShaderFloatControls.shaderDenormFlushToZeroFloat32)
          floatControl.set(DxbcFloatControlFlag::DenormFlushToZero32);
        if (devInfo.khrShaderFloatControls.shaderDenormPreserveFloat64)
          floatControl.set(DxbcFloatControlFlag::DenormPreserve64);
      }
    }

    if (!devInfo.khrShaderFloatControls.shaderSignedZeroInfNanPreserveFloat32
     || adapter->matchesDriver(DxvkGpuVendor::Amd, VK_DRIVER_ID_MESA_RADV_KHR, 0, VK_MAKE_VERSION(20, 3, 0)))
      enableRtOutputNanFixup = true;

    winehuaOptionLog(str::format("WineHua: float controls=",
      options.floatControls ? "on" : "off",
      " preserveNan32=", floatControl.test(DxbcFloatControlFlag::PreserveNan32) ? 1 : 0,
      " preserveNan64=", floatControl.test(DxbcFloatControlFlag::PreserveNan64) ? 1 : 0,
      " denormFlush32=", floatControl.test(DxbcFloatControlFlag::DenormFlushToZero32) ? 1 : 0,
      " denormPreserve64=", floatControl.test(DxbcFloatControlFlag::DenormPreserve64) ? 1 : 0,
      " devicePreserveNan32=", devInfo.khrShaderFloatControls.shaderSignedZeroInfNanPreserveFloat32 ? 1 : 0,
      " deviceDenormFlush32=", devInfo.khrShaderFloatControls.shaderDenormFlushToZeroFloat32 ? 1 : 0,
      " deviceDenormIndependence=", uint32_t(devInfo.khrShaderFloatControls.denormBehaviorIndependence),
      " deviceRoundingIndependence=", uint32_t(devInfo.khrShaderFloatControls.roundingModeIndependence),
      " rtNanFixup=", enableRtOutputNanFixup ? 1 : 0));
  }
  
}
