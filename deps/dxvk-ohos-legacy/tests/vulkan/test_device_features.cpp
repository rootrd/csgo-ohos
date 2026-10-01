#include "../../src/dxvk/dxvk_device_features.h"

#include <cassert>
#include <iostream>
#include <sstream>
#include <unordered_set>

// This host test needs no Vulkan loader, window, driver or logging backend.
namespace dxvk { void Logger::info(const std::string&) { } }

using namespace dxvk;

static bool hasStruct(const void* chain, VkStructureType type) {
  std::unordered_set<const void*> visited;
  auto* node = static_cast<const VkBaseInStructure*>(chain);
  while (node) {
    assert(visited.insert(node).second); // Reject cycles and stale self-links.
    if (node->sType == type) return true;
    node = node->pNext;
  }
  return false;
}

static void testMask() {
  static_assert(DxvkCoreFeatureCount == 55);
  assert(std::string(DxvkCoreFeatures[32].name) == "shaderStorageImageWriteWithoutFormat");
  assert(std::string(DxvkCoreFeatures[37].name) == "shaderClipDistance");
  const uint64_t mask = (uint64_t(1) << 37) | (uint64_t(1) << 32) | 1;
  std::istringstream config("# csgoVkFeatureMask = 0\n"
    "other_csgoVkFeatureMask = 0\n csgoVkFeatureMask = 0x2100000001 # test\n");
  assert(readCoreFeatureMask(config) == mask);
  const auto selected = coreFeaturesFromMask(mask);
  assert(selected.robustBufferAccess);
  assert(selected.shaderClipDistance);
  assert(selected.shaderStorageImageWriteWithoutFormat);
  assert(!selected.shaderCullDistance);
  auto requested = selected;
  requested.samplerAnisotropy = VK_TRUE;
  auto enabled = selectCoreFeatures(requested, selected, requested, mask);
  assert(enabled.shaderClipDistance && !enabled.samplerAnisotropy);
  enabled = selectCoreFeatures(requested, selected, requested, std::nullopt);
  assert(enabled.shaderClipDistance && enabled.samplerAnisotropy);
  for (uint64_t bad : { uint64_t(0), uint64_t(0x4C398F), uint64_t(1) << 63 }) {
    bool rejected = false;
    try { (void)selectCoreFeatures(requested, selected, requested, bad); }
    catch (const std::invalid_argument& e) {
      rejected = true;
      if (bad == 0x4C398F)
        assert(std::string(e.what()).find("shaderClipDistance") != std::string::npos);
    }
    assert(rejected);
  }
  auto unsupported = requested;
  unsupported.shaderClipDistance = VK_FALSE;
  bool rejected = false;
  try { (void)selectCoreFeatures(requested, selected, unsupported, std::nullopt); }
  catch (const std::invalid_argument& e) {
    rejected = std::string(e.what()).find("shaderClipDistance") != std::string::npos;
  }
  assert(rejected);
  const auto oldMask = coreFeaturesFromMask(0x4C398F);
  const auto missing = missingCoreFeatures(selected, oldMask);
  assert(missing.find("shaderClipDistance") != std::string::npos);
  assert(missing.find("shaderStorageImageWriteWithoutFormat") != std::string::npos);

  for (const auto* value : { "", "-1", "+1", "0x", "0x1garbage", "18446744073709551616", "0x80000000000000" }) {
    std::istringstream bad(std::string("csgoVkFeatureMask = ") + value);
    bool rejected = false;
    try { (void)readCoreFeatureMask(bad); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
  }
  std::istringstream unset("# csgoVkFeatureMask = 0\n");
  assert(!readCoreFeatureMask(unset));
  std::istringstream zero("csgoVkFeatureMask = 0\n");
  assert(readCoreFeatureMask(zero) == 0);
  assert(!missingCoreFeatures(selected, coreFeaturesFromMask(0)).empty());
}

static void testRequestAndRetry() {
  DxvkDeviceFeatures features = {};
  features.core.features.shaderClipDistance = VK_TRUE;
  features.extRobustness2.nullDescriptor = VK_TRUE;
  features.extRobustness2.robustBufferAccess2 = VK_TRUE;
  features.khrTimelineSemaphore.timelineSemaphore = VK_TRUE;
  features.khrBufferDeviceAddress.bufferDeviceAddress = VK_TRUE;
  features.extHostQueryReset.hostQueryReset = VK_TRUE;
  features.ext4444Formats.formatA4R4G4B4 = VK_TRUE;
  DxvkDeviceExtensions extensions;
  extensions.extRobustness2.enable(1);
  extensions.khrTimelineSemaphore.enable(1);
  extensions.khrBufferDeviceAddress.enable(1);
  extensions.extHostQueryReset.enable(1);
  extensions.ext4444Formats.enable(1);
  extensions.amdMemoryOverallocationBehaviour.enable(1);
  VkDeviceMemoryOverallocationCreateInfoAMD overallocation;

  auto request = deviceFeatureRequest(features, extensions, overallocation, false);
  assert(request.pNext == &overallocation);
  assert(request.pEnabledFeatures == &features.core.features);
  assert(hasStruct(request.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES));
  assert(hasStruct(request.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES));
  assert(hasStruct(request.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT));

  // Remove the chain head and another node, exactly the old stale-pNext case.
  extensions.khrTimelineSemaphore.disable();
  extensions.khrBufferDeviceAddress.disable();
  extensions.extRobustness2.disable();
  request = deviceFeatureRequest(features, extensions, overallocation, false);
  assert(!hasStruct(request.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES));
  assert(!hasStruct(request.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES));
  assert(!hasStruct(request.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT));
  assert(!features.extRobustness2.nullDescriptor && !features.extRobustness2.robustBufferAccess2);
  assert(!features.khrTimelineSemaphore.timelineSemaphore);
  assert(!features.khrBufferDeviceAddress.bufferDeviceAddress);
  assert(features.extRobustness2.pNext == nullptr);
  assert(features.core.features.shaderClipDistance);
  assert(features.extHostQueryReset.hostQueryReset);
  assert(features.ext4444Formats.formatA4R4G4B4);

  request = deviceFeatureRequest(features, extensions, overallocation, true);
  assert(request.pEnabledFeatures == nullptr);
  assert(overallocation.pNext == &features.core);
  assert(hasStruct(request.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2));
  assert(hasStruct(request.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES));

  extensions.amdMemoryOverallocationBehaviour.disable();
  request = deviceFeatureRequest(features, extensions, overallocation, false);
  assert(request.pNext == features.core.pNext);
  assert(!hasStruct(request.pNext, VK_STRUCTURE_TYPE_DEVICE_MEMORY_OVERALLOCATION_CREATE_INFO_AMD));
  clearDeviceFeatureChain(features);
  assert(!features.core.pNext && !features.extHostQueryReset.pNext && !features.ext4444Formats.pNext);
  assert(features.core.features.shaderClipDistance && features.extHostQueryReset.hostQueryReset);
}

static void testExtensionRevisions() {
  DxvkNameSet supported;
  supported.add(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
  DxvkDeviceExtensions extensions;
  DxvkExt* list[] = { &extensions.khrTimelineSemaphore };
  DxvkNameSet enabled;
  assert(supported.enableExtensions(1, list, enabled));
  assert(extensions.khrTimelineSemaphore);
  extensions.khrTimelineSemaphore.setMode(DxvkExtMode::Disabled);
  enabled = DxvkNameSet();
  assert(supported.enableExtensions(1, list, enabled));
  assert(!extensions.khrTimelineSemaphore);
  assert(!enabled.supports(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME));
  extensions.khrTimelineSemaphore.setMode(DxvkExtMode::Required);
  assert(!DxvkNameSet().enableExtensions(1, list, enabled));
  assert(!extensions.khrTimelineSemaphore);
}

int main() {
  assert(!shouldRetryCudaInterop(VK_SUCCESS, true));
  for (VkResult failure : { VK_ERROR_FEATURE_NOT_PRESENT, VK_ERROR_EXTENSION_NOT_PRESENT,
                           VK_ERROR_INITIALIZATION_FAILED, VK_ERROR_UNKNOWN }) {
    assert(shouldRetryCudaInterop(failure, true));
    assert(!shouldRetryCudaInterop(failure, false));
  }
  assert(isFeatureNegotiationFailure(VK_ERROR_FEATURE_NOT_PRESENT));
  assert(isFeatureNegotiationFailure(VK_ERROR_EXTENSION_NOT_PRESENT));
  assert(!isFeatureNegotiationFailure(VK_SUCCESS));
  assert(!isFeatureNegotiationFailure(VK_ERROR_INITIALIZATION_FAILED));
  assert(!isFeatureNegotiationFailure(VK_ERROR_UNKNOWN));
  assert(!isFeatureNegotiationFailure(VK_ERROR_DEVICE_LOST));
  assert(!isFeatureNegotiationFailure(VK_ERROR_OUT_OF_DEVICE_MEMORY));
  testMask();
  testExtensionRevisions();
  // Repeat with distinct stack objects. No retry table may retain their addresses.
  for (unsigned i = 0; i < 64; i++) testRequestAndRetry();
  std::cout << "PASS: masks, required features, retry policies/chains, capability snapshots, extension revisions\n";
}
