#!/usr/bin/env python3
"""替换旧版崩溃二分为从零基线逐位回填版"""
import io

p = '/mnt/e/csgo/deps/dxvk-ohos-legacy/src/dxvk/dxvk_adapter.cpp'
s = io.open(p, encoding='utf-8', errors='replace').read()

old = '''    if (vr != VK_SUCCESS) {
      Logger::err("DxvkAdapter: bisecting core features one by one");
      const VkBool32* queried = reinterpret_cast<const VkBool32*>(&m_deviceFeatures.core.features);
      VkBool32* feats = reinterpret_cast<VkBool32*>(&enabledFeatures.core.features);
      constexpr size_t coreFeatureCount = sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32);
      for (size_t f = 0; f < coreFeatureCount; f++) {
        feats[f] = queried[f];
        if (!feats[f]) continue;  // 查询即不支持，跳过
        vr = m_vki->vkCreateDevice(m_handle, &info, nullptr, &device);
        if (vr == VK_SUCCESS) {
          Logger::err(str::format("DxvkAdapter: feature[", f, "] accepted - device created"));
          break;
        }
        Logger::err(str::format("DxvkAdapter: feature[", f, "] REJECTED by driver -> disabling"));
        feats[f] = VK_FALSE;
      }
      if (vr != VK_SUCCESS)
        Logger::err("DxvkAdapter: even zero core features failed - problem is elsewhere (queues/instance)");
    }'''
new = '''    // Maleoon: 从零基线逐位回填 core features（step6 证明零 feature 可建）。
    // 被驱动拒绝的位记日志并剔除，得到「真实支持」的最大 feature 集。
    if (vr == VK_SUCCESS && fallbackStep >= 6) {
      Logger::err("DxvkAdapter: bisecting core features from zero baseline");
      const VkBool32* queried = reinterpret_cast<const VkBool32*>(&m_deviceFeatures.core.features);
      VkPhysicalDeviceFeatures acc {};
      VkBool32* accBits = reinterpret_cast<VkBool32*>(&acc);
      constexpr size_t coreFeatureCount = sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32);
      size_t accepted = 0, rejected = 0;
      for (size_t f = 0; f < coreFeatureCount; f++) {
        if (!queried[f]) continue;
        accBits[f] = VK_TRUE;
        if (device) { m_vki->vkDestroyDevice(device, nullptr); device = VK_NULL_HANDLE; }
        VkResult trial = m_vki->vkCreateDevice(m_handle, &info, nullptr, &device);
        if (trial == VK_SUCCESS) {
          accepted++;
        } else {
          accBits[f] = VK_FALSE;
          rejected++;
          Logger::err(str::format("DxvkAdapter: core feature[", f, "] REJECTED by driver - disabled"));
        }
      }
      if (device) { m_vki->vkDestroyDevice(device, nullptr); device = VK_NULL_HANDLE; }
      vr = m_vki->vkCreateDevice(m_handle, &info, nullptr, &device);
      enabledFeatures.core.features = acc;
      Logger::err(str::format("DxvkAdapter: feature bisect done: accepted=", accepted,
        " rejected=", rejected, " final VkResult=", vr));
    }'''
assert old in s, 'old bisect block miss'
s = s.replace(old, new, 1)
io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('replaced with zero-baseline accumulate bisect')
