#!/usr/bin/env python3
"""DXVK vkCreateDevice 分级回退：Maleoon 枚举扩展/feature 但 vkCreateDevice 报
FEATURE_NOT_PRESENT —— 按嫌疑度分组禁用可选扩展重试"""
import io

p = '/mnt/e/csgo/deps/dxvk-ohos-legacy/src/dxvk/dxvk_adapter.cpp'
# E: 是补丁真相源：构建脚本的 rsync E:→WSL 在编译前执行，会携带本补丁
s = io.open(p, encoding='utf-8', errors='replace').read()

old = '''    if (vr != VK_SUCCESS) {
      // OHOS 联调：记录 VkResult 与请求的设备扩展，定位驱动拒绝原因
      Logger::err(str::format("DxvkAdapter: Failed to create device: VkResult=", vr,
        ", extensionCount=", extensionNameList.count()));
      for (uint32_t i = 0; i < extensionNameList.count(); i++)
        Logger::err(str::format("DxvkAdapter:   req[", i, "] = ", extensionNameList.names()[i]));
      throw DxvkError("DxvkAdapter: Failed to create device");
    }'''
new = '''    // OHOS/Maleoon workaround: the driver enumerates extensions and reports
    // feature bits via properties2, but vkCreateDevice rejects the chain with
    // VK_ERROR_FEATURE_NOT_PRESENT. Retry while disabling optional
    // feature-coupled extension groups, most suspicious first.
    struct FallbackStep {
      const char* name;
      std::initializer_list<DxvkExt*> exts;
      std::initializer_list<VkStructureType> structs;
    };
    static const FallbackStep fallbackSteps[] = {
      { "timeline_semaphore+maintenance4",
        { &devExtensions.khrTimelineSemaphore, &devExtensions.khrMaintenance4 },
        { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR,
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES_KHR } },
      { "demote+robustness2+extended_dynamic_state",
        { &devExtensions.extShaderDemoteToHelperInvocation,
          &devExtensions.extRobustness2,
          &devExtensions.extExtendedDynamicState },
        { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES_EXT,
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT } },
      { "xform_feedback+host_query_reset+renderpass2+ds_resolve+draw_indirect+va_divisor",
        { &devExtensions.extTransformFeedback,
          &devExtensions.extHostQueryReset,
          &devExtensions.khrCreateRenderPass2,
          &devExtensions.khrDepthStencilResolve,
          &devExtensions.khrDrawIndirectCount,
          &devExtensions.extVertexAttributeDivisor },
        { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT,
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES_EXT,
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT } },
    };

    int fallbackStep = 0;
    while (vr != VK_SUCCESS && fallbackStep < 4) {
      const FallbackStep& fb = fallbackSteps[fallbackStep++];
      Logger::err(str::format("DxvkAdapter: retrying device creation without ", fb.name));
      for (DxvkExt* ext : fb.exts)
        ext->setMode(DxvkExtMode::Disabled);
      if (fallbackStep == 4) {
        // 终极步：彻底剥离整条 pNext 扩展 feature 链
        Logger::err("DxvkAdapter: stripping entire pNext feature chain");
        enabledFeatures.core.pNext = nullptr;
      }
      for (VkStructureType sType : fb.structs)
        vk::removeStructFromPNextChain(&enabledFeatures.core.pNext, sType);
      extensionsEnabled = DxvkNameSet();
      if (!m_deviceExtensions.enableExtensions(
            devExtensionList.size(), devExtensionList.data(), extensionsEnabled))
        break;  // a required extension vanished - bail out to the error below
      extensionsEnabled.merge(m_extraExtensions);
      extensionNameList = extensionsEnabled.toNameList();
      std::vector<const char*> filteredNames;
      for (uint32_t i = 0; i < extensionNameList.count(); i++) {
        const char* name = extensionNameList.names()[i];
        bool skipped = false;
        for (DxvkExt* ext : fb.exts) {
          if (!std::strcmp(name, ext->name())) { skipped = true; break; }
        }
        if (!skipped) filteredNames.push_back(name);
      }
      info.enabledExtensionCount   = filteredNames.size();
      info.ppEnabledExtensionNames = filteredNames.data();
      vr = m_vki->vkCreateDevice(m_handle, &info, nullptr, &device);
      Logger::err(str::format("DxvkAdapter: retry step ", fallbackStep, " -> VkResult=", vr));
    }

    if (vr != VK_SUCCESS) {
      Logger::err(str::format("DxvkAdapter: Failed to create device: VkResult=", vr,
        ", extensionCount=", extensionNameList.count()));
      for (uint32_t i = 0; i < extensionNameList.count(); i++)
        Logger::err(str::format("DxvkAdapter:   req[", i, "] = ", extensionNameList.names()[i]));
      throw DxvkError("DxvkAdapter: Failed to create device");
    }'''
assert old in s, 'anchor miss'
s = s.replace(old, new, 1)
if '#include <vector>' not in s:
    s = s.replace('#include <cstring>', '#include <cstring>\n#include <vector>', 1)
io.open(p, 'w', encoding='utf-8', newline='').write(s)
print(f'retry ladder patched into {p}')
