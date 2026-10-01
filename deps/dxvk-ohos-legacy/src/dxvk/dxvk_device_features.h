#pragma once

#include <cstddef>
#include <cstdint>
#include <istream>
#include <optional>
#include <stdexcept>
#include <string>

#include "dxvk_device_info.h"
#include "dxvk_extensions.h"

namespace dxvk {

  // Use named members rather than aliasing the struct as an array of VkBool32.
  // The order matches VkPhysicalDeviceFeatures and the diagnostic mask ABI.
  struct DxvkCoreFeature {
    const char* name;
    VkBool32 VkPhysicalDeviceFeatures::* member;
  };

  inline constexpr DxvkCoreFeature DxvkCoreFeatures[] = {
    { "robustBufferAccess", &VkPhysicalDeviceFeatures::robustBufferAccess },
    { "fullDrawIndexUint32", &VkPhysicalDeviceFeatures::fullDrawIndexUint32 },
    { "imageCubeArray", &VkPhysicalDeviceFeatures::imageCubeArray },
    { "independentBlend", &VkPhysicalDeviceFeatures::independentBlend },
    { "geometryShader", &VkPhysicalDeviceFeatures::geometryShader },
    { "tessellationShader", &VkPhysicalDeviceFeatures::tessellationShader },
    { "sampleRateShading", &VkPhysicalDeviceFeatures::sampleRateShading },
    { "dualSrcBlend", &VkPhysicalDeviceFeatures::dualSrcBlend },
    { "logicOp", &VkPhysicalDeviceFeatures::logicOp },
    { "multiDrawIndirect", &VkPhysicalDeviceFeatures::multiDrawIndirect },
    { "drawIndirectFirstInstance", &VkPhysicalDeviceFeatures::drawIndirectFirstInstance },
    { "depthClamp", &VkPhysicalDeviceFeatures::depthClamp },
    { "depthBiasClamp", &VkPhysicalDeviceFeatures::depthBiasClamp },
    { "fillModeNonSolid", &VkPhysicalDeviceFeatures::fillModeNonSolid },
    { "depthBounds", &VkPhysicalDeviceFeatures::depthBounds },
    { "wideLines", &VkPhysicalDeviceFeatures::wideLines },
    { "largePoints", &VkPhysicalDeviceFeatures::largePoints },
    { "alphaToOne", &VkPhysicalDeviceFeatures::alphaToOne },
    { "multiViewport", &VkPhysicalDeviceFeatures::multiViewport },
    { "samplerAnisotropy", &VkPhysicalDeviceFeatures::samplerAnisotropy },
    { "textureCompressionETC2", &VkPhysicalDeviceFeatures::textureCompressionETC2 },
    { "textureCompressionASTC_LDR", &VkPhysicalDeviceFeatures::textureCompressionASTC_LDR },
    { "textureCompressionBC", &VkPhysicalDeviceFeatures::textureCompressionBC },
    { "occlusionQueryPrecise", &VkPhysicalDeviceFeatures::occlusionQueryPrecise },
    { "pipelineStatisticsQuery", &VkPhysicalDeviceFeatures::pipelineStatisticsQuery },
    { "vertexPipelineStoresAndAtomics", &VkPhysicalDeviceFeatures::vertexPipelineStoresAndAtomics },
    { "fragmentStoresAndAtomics", &VkPhysicalDeviceFeatures::fragmentStoresAndAtomics },
    { "shaderTessellationAndGeometryPointSize", &VkPhysicalDeviceFeatures::shaderTessellationAndGeometryPointSize },
    { "shaderImageGatherExtended", &VkPhysicalDeviceFeatures::shaderImageGatherExtended },
    { "shaderStorageImageExtendedFormats", &VkPhysicalDeviceFeatures::shaderStorageImageExtendedFormats },
    { "shaderStorageImageMultisample", &VkPhysicalDeviceFeatures::shaderStorageImageMultisample },
    { "shaderStorageImageReadWithoutFormat", &VkPhysicalDeviceFeatures::shaderStorageImageReadWithoutFormat },
    { "shaderStorageImageWriteWithoutFormat", &VkPhysicalDeviceFeatures::shaderStorageImageWriteWithoutFormat },
    { "shaderUniformBufferArrayDynamicIndexing", &VkPhysicalDeviceFeatures::shaderUniformBufferArrayDynamicIndexing },
    { "shaderSampledImageArrayDynamicIndexing", &VkPhysicalDeviceFeatures::shaderSampledImageArrayDynamicIndexing },
    { "shaderStorageBufferArrayDynamicIndexing", &VkPhysicalDeviceFeatures::shaderStorageBufferArrayDynamicIndexing },
    { "shaderStorageImageArrayDynamicIndexing", &VkPhysicalDeviceFeatures::shaderStorageImageArrayDynamicIndexing },
    { "shaderClipDistance", &VkPhysicalDeviceFeatures::shaderClipDistance },
    { "shaderCullDistance", &VkPhysicalDeviceFeatures::shaderCullDistance },
    { "shaderFloat64", &VkPhysicalDeviceFeatures::shaderFloat64 },
    { "shaderInt64", &VkPhysicalDeviceFeatures::shaderInt64 },
    { "shaderInt16", &VkPhysicalDeviceFeatures::shaderInt16 },
    { "shaderResourceResidency", &VkPhysicalDeviceFeatures::shaderResourceResidency },
    { "shaderResourceMinLod", &VkPhysicalDeviceFeatures::shaderResourceMinLod },
    { "sparseBinding", &VkPhysicalDeviceFeatures::sparseBinding },
    { "sparseResidencyBuffer", &VkPhysicalDeviceFeatures::sparseResidencyBuffer },
    { "sparseResidencyImage2D", &VkPhysicalDeviceFeatures::sparseResidencyImage2D },
    { "sparseResidencyImage3D", &VkPhysicalDeviceFeatures::sparseResidencyImage3D },
    { "sparseResidency2Samples", &VkPhysicalDeviceFeatures::sparseResidency2Samples },
    { "sparseResidency4Samples", &VkPhysicalDeviceFeatures::sparseResidency4Samples },
    { "sparseResidency8Samples", &VkPhysicalDeviceFeatures::sparseResidency8Samples },
    { "sparseResidency16Samples", &VkPhysicalDeviceFeatures::sparseResidency16Samples },
    { "sparseResidencyAliased", &VkPhysicalDeviceFeatures::sparseResidencyAliased },
    { "variableMultisampleRate", &VkPhysicalDeviceFeatures::variableMultisampleRate },
    { "inheritedQueries", &VkPhysicalDeviceFeatures::inheritedQueries },
  };

  inline constexpr size_t DxvkCoreFeatureCount =
    sizeof(DxvkCoreFeatures) / sizeof(DxvkCoreFeatures[0]);
  static_assert(DxvkCoreFeatureCount < 64, "Core feature mask needs more bits");

  inline std::string missingCoreFeatures(
    const VkPhysicalDeviceFeatures& requested,
    const VkPhysicalDeviceFeatures& available) {
    std::string result;
    for (const auto& feature : DxvkCoreFeatures) {
      if (requested.*feature.member && !(available.*feature.member)) {
        if (!result.empty()) result += ", ";
        result += feature.name;
      }
    }
    return result;
  }

  inline std::string trimFeatureOption(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
  }

  inline std::optional<uint64_t> readCoreFeatureMask(std::istream& config) {
    std::optional<uint64_t> result;
    std::string line;
    while (std::getline(config, line)) {
      line = line.substr(0, line.find_first_of("#;"));
      const auto equals = line.find('=');
      if (equals == std::string::npos
       || trimFeatureOption(line.substr(0, equals)) != "csgoVkFeatureMask")
        continue;
      const auto value = trimFeatureOption(line.substr(equals + 1));
      size_t end = 0;
      try {
        if (value.empty() || value.front() == '-' || value.front() == '+')
          throw std::invalid_argument("unsigned mask expected");
        result = std::stoull(value, &end, 0);
        if (end != value.size() || (*result >> DxvkCoreFeatureCount))
          throw std::invalid_argument("invalid mask bits");
      } catch (const std::exception&) {
        throw std::invalid_argument("Invalid csgoVkFeatureMask: use a 64-bit unsigned integer with only VkPhysicalDeviceFeatures bits");
      }
    }
    return result;
  }

  inline VkPhysicalDeviceFeatures coreFeaturesFromMask(uint64_t mask) {
    VkPhysicalDeviceFeatures result = {};
    for (size_t i = 0; i < DxvkCoreFeatureCount; i++)
      result.*DxvkCoreFeatures[i].member = (mask >> i) & 1u;
    return result;
  }

  inline VkPhysicalDeviceFeatures selectCoreFeatures(
    const VkPhysicalDeviceFeatures& requested,
    const VkPhysicalDeviceFeatures& required,
    const VkPhysicalDeviceFeatures& supported,
          std::optional<uint64_t> mask) {
    auto selected = requested;
    if (mask) {
      if (*mask >> DxvkCoreFeatureCount)
        throw std::invalid_argument("csgoVkFeatureMask contains unknown feature bits");
      selected = coreFeaturesFromMask(*mask);
    }
    const auto missing = missingCoreFeatures(required, selected);
    if (!missing.empty())
      throw std::invalid_argument("Required rendering features disabled: " + missing
        + ". Remove csgoVkFeatureMask from dxvk.conf; a zero-feature device cannot render D3D9.");
    const auto extra = missingCoreFeatures(selected, requested);
    if (!extra.empty())
      throw std::invalid_argument("csgoVkFeatureMask selects unrequested features: " + extra
        + ". Remove the override from dxvk.conf.");
    const auto unsupported = missingCoreFeatures(selected, supported);
    if (!unsupported.empty())
      throw std::invalid_argument("Vulkan device does not support requested rendering features: " + unsupported
        + ". Update the Vulkan driver or implement a verified renderer fallback.");
    return selected;
  }

  // Rebuild all links for every attempt, and clear every feature whose extension
  // is absent. The same state is used for VkDeviceCreateInfo and DxvkDevice.
  inline void rebuildDeviceFeatureChain(
          DxvkDeviceFeatures& features,
    const DxvkDeviceExtensions& extensions) {
    features.core.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.core.pNext = nullptr;
    auto link = [&](auto& feature, VkStructureType type, bool enabled) {
      if (enabled) {
        feature.sType = type;
        feature.pNext = features.core.pNext;
        features.core.pNext = &feature;
      } else {
        feature = {};
      }
    };
    link(features.shaderDrawParameters,
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES,
      features.shaderDrawParameters.shaderDrawParameters);
    link(features.ext4444Formats,
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_4444_FORMATS_FEATURES_EXT, extensions.ext4444Formats);
    link(features.extCustomBorderColor,
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, extensions.extCustomBorderColor);
    link(features.extDepthClipEnable,
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT, extensions.extDepthClipEnable);
    link(features.extExtendedDynamicState,
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT, extensions.extExtendedDynamicState);
    link(features.extHostQueryReset,
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES_EXT, extensions.extHostQueryReset);
    link(features.extMemoryPriority,
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT, extensions.extMemoryPriority);
    link(features.extNonSeamlessCubeMap,
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_NON_SEAMLESS_CUBE_MAP_FEATURES_EXT, extensions.extNonSeamlessCubeMap);
    link(features.extShaderDemoteToHelperInvocation,
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES_EXT, extensions.extShaderDemoteToHelperInvocation);
    link(features.extRobustness2,
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, extensions.extRobustness2);
    link(features.extTransformFeedback,
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT, extensions.extTransformFeedback);
    link(features.extVertexAttributeDivisor,
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT, extensions.extVertexAttributeDivisor.revision() >= 3);
    link(features.khrBufferDeviceAddress,
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_KHR, extensions.khrBufferDeviceAddress);
    link(features.khrMaintenance4,
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES_KHR, extensions.khrMaintenance4);
    link(features.khrTimelineSemaphore,
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR, extensions.khrTimelineSemaphore);
  }

  // A capability snapshot must not retain links into createDevice's stack.
  inline void clearDeviceFeatureChain(DxvkDeviceFeatures& features) {
    features.core.pNext = nullptr;
    features.shaderDrawParameters.pNext = nullptr;
    features.ext4444Formats.pNext = nullptr;
    features.extCustomBorderColor.pNext = nullptr;
    features.extDepthClipEnable.pNext = nullptr;
    features.extExtendedDynamicState.pNext = nullptr;
    features.extHostQueryReset.pNext = nullptr;
    features.extMemoryPriority.pNext = nullptr;
    features.extNonSeamlessCubeMap.pNext = nullptr;
    features.extShaderDemoteToHelperInvocation.pNext = nullptr;
    features.extRobustness2.pNext = nullptr;
    features.extTransformFeedback.pNext = nullptr;
    features.extVertexAttributeDivisor.pNext = nullptr;
    features.khrBufferDeviceAddress.pNext = nullptr;
    features.khrMaintenance4.pNext = nullptr;
    features.khrTimelineSemaphore.pNext = nullptr;
  }

  inline bool isFeatureNegotiationFailure(VkResult result) {
    return result == VK_ERROR_FEATURE_NOT_PRESENT || result == VK_ERROR_EXTENSION_NOT_PRESENT;
  }

  inline bool shouldRetryCudaInterop(VkResult result, bool enabled) {
    // Upstream's optional NVIDIA interop retry also handles initialization and
    // unknown failures (e.g. an advertised extension with no kernel support).
    return result != VK_SUCCESS && enabled;
  }

  inline VkDeviceCreateInfo deviceFeatureRequest(
          DxvkDeviceFeatures& features,
    const DxvkDeviceExtensions& extensions,
          VkDeviceMemoryOverallocationCreateInfoAMD& overallocation,
          bool useFeatures2) {
    rebuildDeviceFeatureChain(features, extensions);
    VkDeviceCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.pNext = useFeatures2 ? &features.core : features.core.pNext;
    info.pEnabledFeatures = useFeatures2 ? nullptr : &features.core.features;
    overallocation = {};
    overallocation.sType = VK_STRUCTURE_TYPE_DEVICE_MEMORY_OVERALLOCATION_CREATE_INFO_AMD;
    overallocation.overallocationBehavior = VK_MEMORY_OVERALLOCATION_BEHAVIOR_ALLOWED_AMD;
    if (extensions.amdMemoryOverallocationBehaviour) {
      overallocation.pNext = info.pNext;
      info.pNext = &overallocation;
    }
    return info;
  }

}
