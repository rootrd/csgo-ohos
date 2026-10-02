#!/usr/bin/env python3
"""Execute actual three Panorama resource selectors with host filesystem stubs.
Tests OHOS/non-OHOS and development/production builds with/without code.pbin.
Missing loose config/bindings must fail rather than fall back to packed data.
"""
import os
import re
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
SRC=ROOT/'CSGO-Source-Linux-20260928/src'
def method(text,signature):
 a=text.index(signature); b=text.index('{',a); depth=1; end=b+1
 while depth:
  depth+=(text[end]=='{')-(text[end]=='}');end+=1
 return text[a:end]
header=(SRC/'public/panorama/iuifilesystem.h').read_text()
policy=method(header,'inline bool ShouldUseLoosePanoramaResources(')
wrap=(SRC/'panorama_s1wrapper/wrap_resource.cpp').read_text()
init=method(wrap,'void CResourceSystem::Init()').replace('CResourceSystem::Init','Init')
assert '#if DEVELOPMENT_ONLY\n\tif ( !bFromZip )' in wrap, 'Production resource guard changed'
def selector(path,signature,negative=False):
 text=(SRC/path).read_text(errors='replace');a=text.index(signature)
 a=text.index('bool bFailed = false;',a);b=text.index('if ( !bFailed )',a) if negative else a+re.search(r'if\s*\(\s*bFailed\s*\)',text[a:]).start()
 return text[a:b]
config=selector('panoramauiclient/panoramauiclient.cpp','bool CPanoramaUIClient::SetupNamedPaths()')
bindings=selector('panorama/input/uiinput.cpp','void CUIInputEngine::ParseKeyConfig(',True)
for code in (init,config,bindings):
 assert code.count('panorama::ShouldUseLoosePanoramaResources(')==1
code=r'''
#include <cassert>
#include <cstring>
#include <cstdio>
#include <iostream>
#define PANORAMA_ZIPFILE_NAME "panorama/code.pbin"
#define MAX_PATH 260
#define V_snprintf snprintf
bool exists, looseOk, packedOk, preloadPacked;
unsigned looseCalls,packedCalls,warningCalls;
void Warning(const char*,...) {warningCalls++;}
struct FS {bool FileExists(const char*,void*) {return exists;}} fs;
FS* g_pFullFileSystem=&fs;
struct Material {void AddEndFramePriorToNextContextFunc(void(*)()) {}} material;
Material* g_pMaterialSystem=&material;
void EndFrameCleanup() {}
struct S1Wrapper_Texture_t {static void StaticInit() {}};
void PreloadResources(bool fromZip) {
#if DEVELOPMENT_ONLY
 preloadPacked=fromZip;
#else
 preloadPacked=true;
#endif
}
struct Buffer {void PutString(const char*) {}};
struct UIFileSystem {
 bool LoadFileIntoBuffer(const char*,Buffer&,bool) {looseCalls++;return looseOk;}
 char* LoadFromPanZip(const char*) {packedCalls++;static char text[]="data";return packedOk?text:nullptr;}
};
struct UIEngineType {struct UIFileSystem* UIFileSystem() {static struct UIFileSystem fs;return &fs;}};
UIEngineType* UIEngine() {static UIEngineType e;return &e;}
namespace panorama {
__POLICY__
UIEngineType* UIEngine() {return ::UIEngine();}
}
struct Config {
 bool LoadFromFile(FS*,const char*,const char*) {looseCalls++;return looseOk;}
 bool LoadFromBuffer(const char*,const char*) {return packedOk;}
};
struct Reference {const char* Get() {return "panorama/keybindings.cfg";}};
struct Resource {Reference GetReferencePath() {return {};}};
__INIT__
bool SetupNamedPaths() {
 Config config;Config* pConfigKV=&config;const char* pConfigFilename="panorama/panorama.cfg";
__CONFIG__
 return !bFailed;
}
bool ParseKeyConfig() {
 const char* pchFileName="file://{resources}/keybindings.cfg";
 Resource fileResource;Buffer buffer;
__BINDINGS__
 return !bFailed;
}
int main() {
 for(bool present:{false,true}) for(bool success:{false,true}) {
  exists=present;looseOk=packedOk=success;
#if DEVELOPMENT_ONLY
#if defined(__OHOS__)
  bool loose=true;
#else
  bool loose=!exists;
#endif
#else
  bool loose=false;
#endif
  Init();assert(preloadPacked==!loose);
  for(auto fn:{SetupNamedPaths,ParseKeyConfig}) {
   looseCalls=packedCalls=warningCalls=0;assert(fn()==success);
#if defined(__OHOS__) && DEVELOPMENT_ONLY
   if(fn==ParseKeyConfig) assert(warningCalls==(success?0u:1u));
#endif
   assert(looseCalls==(loose?1u:0u));assert(packedCalls==(loose?0u:1u));
  }
  if(loose) {
   looseOk=false;packedOk=true; // Never silently use incompatible packed content.
   looseCalls=packedCalls=0;assert(!SetupNamedPaths());assert(packedCalls==0);
   looseCalls=packedCalls=0;assert(!ParseKeyConfig());assert(packedCalls==0);
  }
 }
}
'''.replace('__POLICY__',policy).replace('__INIT__',init).replace('__CONFIG__',config).replace('__BINDINGS__',bindings)
with tempfile.TemporaryDirectory(prefix='panorama-resource-policy-') as d:
 cpp,exe=Path(d)/'test.cpp',Path(d)/'test';cpp.write_text(code)
 for flags in (['-DDEVELOPMENT_ONLY=0'],['-DDEVELOPMENT_ONLY=1'],['-DDEVELOPMENT_ONLY=0','-D__OHOS__=1'],['-DDEVELOPMENT_ONLY=1','-D__OHOS__=1']):
  subprocess.run([os.environ.get('CXX','g++'),'-std=c++11',*flags,str(cpp),'-o',str(exe)],check=True)
  subprocess.run([str(exe)],check=True)
print('PASS: actual preload/config/keybinding selectors across four build modes, packed presence/absence and missing loose resource failures')
