#!/usr/bin/env python3
"""Compile and exercise the actual D3D9 requested/required-feature selection.
Host feature stubs, no Vulkan driver or device creation claim.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
DXVK = ROOT / 'deps/dxvk-ohos-legacy'
src = (DXVK/'src/d3d9/d3d9_device.cpp').read_text()
a=src.index('  DxvkDeviceFeatures D3D9DeviceEx::GetDeviceFeatures(')
b=src.index('\n\n  void D3D9DeviceEx::DetermineConstantLayouts',a)
getter=src[a:b].replace('D3D9DeviceEx::GetDeviceFeatures','GetDeviceFeatures')
extensions={}
for struct,field in re.findall(r'(?:enabled|supported)\.(\w+)\.(\w+)',getter):
    if struct!='core': extensions.setdefault(struct,set()).add(field)
structs='\n'.join('struct { '+''.join('VkBool32 '+f+'=0; ' for f in sorted(fields))+'} '+name+';' for name,fields in extensions.items())
src=(DXVK/'src/d3d9/d3d9_interface.cpp').read_text()
a=src.index('      auto requiredCore = features.core.features;')
b=src.index('      auto dxvkDevice =',a)
required=src[a:b]
code=r'''
#include <vulkan/vulkan_core.h>
#include <cassert>
#include <memory>
#include <iostream>
struct DxvkDeviceFeatures {
  struct { VkPhysicalDeviceFeatures features={}; } core;
__STRUCTS__
};
struct DxvkAdapter { DxvkDeviceFeatures value; DxvkDeviceFeatures features() const { return value; } };
template<class T> using Rc=std::shared_ptr<T>;
__GETTER__
static VkPhysicalDeviceFeatures Required(DxvkDeviceFeatures features) {
__REQUIRED__
  return requiredCore;
}
int main() {
  auto adapter=std::make_shared<DxvkAdapter>();
  auto request=GetDeviceFeatures(adapter);
  auto required=Required(request);
  assert(request.core.features.shaderClipDistance && required.shaderClipDistance);
  assert(request.core.features.shaderStorageImageWriteWithoutFormat && required.shaderStorageImageWriteWithoutFormat);
  assert(request.core.features.geometryShader && required.geometryShader);
  assert(request.core.features.sampleRateShading && required.sampleRateShading);
  assert(!request.core.features.textureCompressionBC && !required.textureCompressionBC);
  assert(!request.core.features.samplerAnisotropy && !required.samplerAnisotropy);
  adapter->value.core.features.textureCompressionBC=VK_TRUE;
  adapter->value.core.features.samplerAnisotropy=VK_TRUE;
  request=GetDeviceFeatures(adapter); required=Required(request);
  assert(request.core.features.textureCompressionBC && required.textureCompressionBC);
  assert(request.core.features.samplerAnisotropy && !required.samplerAnisotropy);
  assert(required.shaderClipDistance && required.shaderStorageImageWriteWithoutFormat);
  std::cout << "PASS: actual D3D9 feature getter preserves hard requirements and selectively enables native BC/anisotropy\n";
}
'''.replace('__STRUCTS__',structs).replace('__GETTER__',getter).replace('__REQUIRED__',required)
with tempfile.TemporaryDirectory(prefix='d3d9-feature-contract-') as d:
    cpp,exe=Path(d)/'test.cpp',Path(d)/'test'
    cpp.write_text(code)
    subprocess.run([os.environ.get('CXX','g++'),'-std=c++17','-I'+str(DXVK/'include'),str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
