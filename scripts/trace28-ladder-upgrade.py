#!/usr/bin/env python3
"""升级现有回退梯子：step3 补 va_divisor 的 feature struct + 新增 step4 剥整条 pNext"""
import io

p = '/mnt/e/csgo/deps/dxvk-ohos-legacy/src/dxvk/dxvk_adapter.cpp'
s = io.open(p, encoding='utf-8', errors='replace').read()

old1 = '''      { "xform_feedback+host_query_reset+renderpass2+ds_resolve+draw_indirect",
        { &devExtensions.extTransformFeedback,
          &devExtensions.extHostQueryReset,
          &devExtensions.khrCreateRenderPass2,
          &devExtensions.khrDepthStencilResolve,
          &devExtensions.khrDrawIndirectCount },
        { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT,
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES_EXT } },
    };

    int fallbackStep = 0;
    while (vr != VK_SUCCESS && fallbackStep < 3) {
      const FallbackStep& fb = fallbackSteps[fallbackStep++];
      Logger::err(str::format("DxvkAdapter: retrying device creation without ", fb.name));
      for (DxvkExt* ext : fb.exts)
        ext->setMode(DxvkExtMode::Disabled);'''
new1 = '''      { "xform_feedback+host_query_reset+renderpass2+ds_resolve+draw_indirect+va_divisor",
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
        // 终极步：彻底剥离整条 pNext 扩展 feature 链（定位驱动拒绝的来源）
        Logger::err("DxvkAdapter: stripping entire pNext feature chain");
        enabledFeatures.core.pNext = nullptr;
      }'''
assert old1 in s, 'anchor1 miss'
s = s.replace(old1, new1, 1)
io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('ladder upgraded to 4 steps')
