#!/usr/bin/env python3
"""Compile actual fallback LockImage box validation; no driver needed."""
import os
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
s=(ROOT/'deps/dxvk-ohos-legacy/src/d3d9/d3d9_device.cpp').read_text()
a=s.index('    if (formatMapping.IsBcEmulated() && pBox) {')
b=s.index('    const bool systemmem',a)
code=r'''
#include <cassert>
#include <iostream>
using UINT=unsigned;
struct D3DBOX {UINT Left,Top,Right,Bottom,Front,Back;};
struct Extent {UINT width,height,depth;};
enum class D3D9Format {DXT1,ATI1,ATI2};
struct Desc {D3D9Format Format;};
struct Mapping {bool emulated; bool IsBcEmulated() const {return emulated;}};
constexpr int D3DERR_INVALIDCALL=-1;
int Check(Mapping formatMapping, Desc desc, Extent levelExtent, const D3DBOX* pBox) {
__BODY__
 return 0;
}
int main() {
 D3DBOX full{0,0,8,8,0,1}, bad4{0,4,4,8,0,1}, bad5{0,5,4,8,0,1};
 D3DBOX volumeFull{0,0,8,8,0,3}, volumePartial{0,0,8,8,1,3};
 D3DBOX edge{4,4,7,9,0,1}, unaligned{1,0,4,4,0,1}, outside{0,0,9,8,0,1};
 for(auto f:{D3D9Format::ATI1,D3D9Format::ATI2}) {
   assert(Check({true},{f},{8,8,1},&full)==0);
   assert(Check({true},{f},{8,8,1},nullptr)==0);
   assert(Check({true},{f},{8,8,1},&bad4)==D3DERR_INVALIDCALL);
   assert(Check({true},{f},{8,8,1},&bad5)==D3DERR_INVALIDCALL);
   assert(Check({true},{f},{8,8,3},&volumeFull)==0);
   assert(Check({true},{f},{8,8,3},&volumePartial)==D3DERR_INVALIDCALL);
   assert(Check({false},{f},{8,8,1},&bad4)==0); // native legacy behavior untouched
 }
 assert(Check({true},{D3D9Format::DXT1},{7,9,1},&edge)==0);
 assert(Check({true},{D3D9Format::DXT1},{8,8,1},&unaligned)==D3DERR_INVALIDCALL);
 assert(Check({true},{D3D9Format::DXT1},{8,8,1},&outside)==D3DERR_INVALIDCALL);
 std::cout<<"PASS: actual BC lock bounds reject unsafe partial ATI byte-layout offsets; native path unchanged\n";
}
'''.replace('__BODY__',s[a:b])
with tempfile.TemporaryDirectory(prefix='d3d9-bc-lock-') as d:
 cpp,exe=Path(d)/'test.cpp',Path(d)/'test';cpp.write_text(code)
 subprocess.run([os.environ.get('CXX','g++'),'-std=c++17',str(cpp),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
