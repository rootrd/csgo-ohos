#!/usr/bin/env python3
"""Execute actual Panorama layout callbacks with host-only engine service stubs.

XML tokenization uses ElementTree; StartElement, EndElement and BAddPanel are
compiled verbatim from layoutfile.cpp. This tests layout/factory contracts,
not CSS/assets, real panel constructors, JavaScript or on-device rendering.
"""
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'CSGO-Source-Linux-20260928/src'
source = (SRC / 'panorama/layout/layoutfile.cpp').read_text()

def method(signature):
    start = source.index(signature)
    begin = source.index('{', start)
    depth = 1
    end = begin + 1
    # These callbacks contain no unmatched braces in strings/comments.
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

callbacks = '\n'.join(method(sig) for sig in (
    'int StartElement(', 'int EndElement(', 'bool BAddPanel('))
registered = {'Panel'}
for path in (SRC / 'game/client').rglob('*.cpp'):
    registered.update(re.findall(r'REGISTER_PANEL2D_FACTORY\(\s*\w+\s*,\s*(\w+)\s*\)', path.read_text(errors='replace')))

harness = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <string>
#include <vector>
#include <set>
#include <map>
#define Assert(x) ((void)0)
#define AssertMsg(x,y) ((void)0)
#define VPROF_BUDGET(x,y) ((void)0)
#define V_strcmp strcmp
#define V_stricmp strcasecmp
#define V_isempty(s) (!(s) || !*(s))
constexpr int XML_OK = 1, XML_ABORT = 0;
using XMLCH = char;
struct Attr { const char* qname; const char* value; };
using LPXMLRUNTIMEATT = Attr*;
struct Attrs { int length; std::vector<Attr> data; };
using LPXMLVECTOR = Attrs*;
void* XMLVector_Get(Attrs* a, int i) { return &a->data.at(i); }
struct Str : std::string {
  using std::string::operator=;
  bool IsEmpty() const { return empty(); }
  const char* Get() const { return c_str(); }
  const char* String() const { return c_str(); }
  int Length() const { return size(); }
  void Clear() { clear(); }
};
struct CPanoramaSymbol : Str {
  CPanoramaSymbol() = default;
  CPanoramaSymbol(const char* p) { assign(p); }
  bool IsValid() const { return !empty(); }
};
template<class T> struct Vec : std::vector<T> {
  int Count() const { return this->size(); }
  void AddToTail(T t) { this->push_back(t); }
  void Remove(int i) { this->erase(this->begin()+i); }
  void RemoveAll() { this->clear(); }
};
struct Properties { void Insert(CPanoramaSymbol, const char*) {} };
struct PanelDescription_t {
  CPanoramaSymbol m_symType; Str m_strID; Properties m_mapProperties;
  Vec<PanelDescription_t*> m_vecChildren;
  ~PanelDescription_t() { for (auto* p : m_vecChildren) delete p; }
};
struct Engine {
  std::set<std::string> registered = {__REGISTRY__};
  bool BRegisteredPanelType(CPanoramaSymbol s) { return registered.count(s); }
};
Engine* UIEngine() { static Engine e; return &e; }
struct Layout {
  bool BAddStyle(const char*) { return true; }
  bool BAddJavaScript(const char*) { return true; }
  void AddInlineJavascript(const char*, int, int) {}
  PanelDescription_t* GetSnippet(const char*) { return nullptr; }
  void AddSnippet(const char*, PanelDescription_t*) {}
};
struct Parser {
  enum State { k_EInInvalid, k_EInRoot, k_EInStyles, k_EInScripts,
    k_EInInclude, k_EInPanels, k_EInInlineScript, k_EInSnippets,
    k_EInSnippet, k_EInSnippetPanels };
  State m_eCurrentElement = k_EInInvalid;
  bool m_bPartial=false, m_bFoundStyles=false, m_bFoundScripts=false,
    m_bFoundChildren=false, m_bFoundSnippets=false, m_bFoundSnippetRootPanel=false,
    m_bLastIncludeWasStyle=false;
  int m_nScriptStartLine=0, m_nScriptStartCol=0;
  Str m_strCurrentSnippetName, m_strInlineScript;
  Vec<PanelDescription_t*> m_vecCurrentPanelStack;
  PanelDescription_t* m_pPanelDescription=nullptr;
  Layout layout; Layout* m_pLayoutFile=&layout;
  ~Parser() { delete m_pPanelDescription; }
  int ParseError(const char*, ...) { return XML_ABORT; }
  void GetCurrentParsePosition(int* a, int* b) { *a=*b=1; }
  PanelDescription_t* DetatchPanelDescription() { auto* p=m_pPanelDescription; m_pPanelDescription=nullptr; return p; }
__CALLBACKS__
  bool start(const char* name, std::initializer_list<Attr> attrs) {
    Attrs a{int(attrs.size()), attrs}; return StartElement(nullptr, name, name, &a)==XML_OK;
  }
  bool end(const char* name) { return EndElement(nullptr, name, name)==XML_OK; }
};
int main() {
__CASES__
}
'''.replace('__REGISTRY__', ','.join(json.dumps(s) for s in sorted(registered))).replace('__CALLBACKS__', callbacks)

paths = [ROOT / 'CSGO-Source-Linux-20260928/ohos/overlay/csgo/panorama/layout/hud/hud.xml',
         ROOT / 'hap/entry/src/main/resources/rawfile/csgo/csgo/panorama/layout/hud/hud.xml']
assert paths[0].read_bytes() == paths[1].read_bytes(), 'Packaged HUD drift'
assert paths[0].with_name('base_hud.xml').read_bytes() == paths[1].with_name('base_hud.xml').read_bytes(), 'Packaged base HUD drift'
original = paths[0].read_text()
# Negative controls reproduce each independent regression in the old layout.
cases = [(original, True), (paths[0].with_name('base_hud.xml').read_text(), True),
         (original.replace('<styles>', '<style>').replace('</styles>', '</style>'), False),
         (original.replace('<CSGOHud hittest="false">', '<CSGOHud id="Root" hittest="false">', 1), False),
         (original.replace('</root>', '<Panel /></root>'), False),
         (original.replace('<CSGOHudRadio ', '<CCSGO_HudRadio '), False)]
code = []
for xml, expected in cases:
    tree = ET.fromstring(xml)
    events = []
    def emit(node):
        attrs = ','.join('{' + json.dumps(k) + ',' + json.dumps(v) + '}' for k, v in node.attrib.items())
        events.append('ok = ok && p.start(' + json.dumps(node.tag) + ', {' + attrs + '});')
        for child in node: emit(child)
        events.append('ok = ok && p.end(' + json.dumps(node.tag) + ');')
    emit(tree)
    code.append('{ Parser p; bool ok=true;\n' + '\n'.join(events) + '\nassert(ok == ' + str(expected).lower() + '); }')
harness = harness.replace('__CASES__', '\n'.join(code))
with tempfile.TemporaryDirectory(prefix='panorama-hud-contract-') as d:
    cpp, exe = Path(d)/'test.cpp', Path(d)/'test'
    cpp.write_text(harness)
    subprocess.run([os.environ.get('CXX', 'g++'), '-std=c++17', str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('PASS: actual Panorama layout callbacks accept HUD and reject four broken contracts (host service stubs)')
