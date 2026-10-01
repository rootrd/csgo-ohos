#!/usr/bin/env python3
"""Run the actual initialization guard blocks with a deliberately fake V8.

This is a host regression test for fail-fast ordering, not a V8/runtime test.
Use ohos/tests/panorama_v8_smoke.cpp with real target libraries separately.
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "CSGO-Source-Linux-20260928/src/panorama/uiengine.cpp"
text = SOURCE.read_text()
start = text.index("\t\t\tconst char* v8version = v8::V8::GetVersion();")
end = text.index("\t\t\ts_bGlobalInitDone = true;", start)
global_block = text[start:end + len("\t\t\ts_bGlobalInitDone = true;")]
start = text.index("\tm_pV8Isolate = v8::Isolate::New( createParams );")
end = text.index("\tv8::V8::SetFatalErrorHandler", start)
isolate_block = text[start:end]

harness = r'''
#include <cassert>
#include <stdexcept>
#include <string>
static const char* version;
static bool platform_ok, icu_ok, initialize_ok, isolate_ok, initialized, entered;
static bool s_bGlobalInitDone;
static int nV8ThreadPoolSize = 1;
static void Plat_FatalError(const char* message) { throw std::runtime_error(message); }
static void Msg(const char*, const char*) {}
static void PushContextPanel(void*) {}
namespace v8 {
struct Platform {};
struct Isolate {
    struct CreateParams {};
    static Isolate* New(const CreateParams&) {
        static Isolate result;
        return isolate_ok ? &result : nullptr;
    }
    struct Scope {
        explicit Scope(Isolate* isolate) { assert(isolate); entered = true; }
    };
};
namespace platform {
Platform* CreateDefaultPlatform(int) {
    static Platform result;
    return platform_ok ? &result : nullptr;
}
}
struct V8 {
    static const char* GetVersion() { return version; }
    static void InitializePlatform(Platform* platform) { assert(platform); }
    static bool Initialize() { initialized = true; return initialize_ok; }
    static bool InitializeICU(const char* path) {
#if defined(POSIX)
        assert(std::string(path) == "bin/icudtl.dat");
#else
        assert(std::string(path) == "bin\\icudtl.dat");
#endif
        return icu_ok;
    }
};
}
static void GlobalInit() {
__GLOBAL__
}
static void IsolateInit() {
    v8::Isolate* m_pV8Isolate = nullptr;
    v8::Isolate::CreateParams createParams;
__ISOLATE__
}
static void Reset() {
    version = "5.8.283";
    platform_ok = icu_ok = initialize_ok = isolate_ok = true;
    initialized = entered = s_bGlobalInitDone = false;
}
static void ExpectFatal(void (*call)(), const char* diagnostic) {
    bool failed = false;
    try { call(); }
    catch (const std::runtime_error& error) {
        failed = true;
        assert(std::string(error.what()).find(diagnostic) != std::string::npos);
    }
    assert(failed);
}
int main() {
    Reset(); version = nullptr; ExpectFatal(GlobalInit, "GetVersion");
    assert(!initialized && !s_bGlobalInitDone);
    Reset(); version = ""; ExpectFatal(GlobalInit, "GetVersion");
    assert(!initialized && !s_bGlobalInitDone);
    Reset(); platform_ok = false; ExpectFatal(GlobalInit, "platform creation");
    assert(!initialized && !s_bGlobalInitDone);
    Reset(); icu_ok = false; ExpectFatal(GlobalInit, "ICU initialization");
    assert(!initialized && !s_bGlobalInitDone);
    Reset(); initialize_ok = false; ExpectFatal(GlobalInit, "initialization failed");
    assert(initialized && !s_bGlobalInitDone);
    Reset(); isolate_ok = false; ExpectFatal(IsolateInit, "isolate creation");
    assert(!entered);
    Reset(); GlobalInit(); IsolateInit();
    assert(initialized && entered && s_bGlobalInitDone);
}
'''.replace("__GLOBAL__", global_block).replace("__ISOLATE__", isolate_block)

with tempfile.TemporaryDirectory(prefix="panorama-v8-guards-") as directory:
    source = Path(directory) / "test.cpp"
    binary = Path(directory) / "test"
    source.write_text(harness)
    for platform_flags in ([], ["-DPOSIX=1"]):
        subprocess.run([os.environ.get("CXX", "g++"), "-std=c++11", "-Wall", "-Wextra",
                        *platform_flags, str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
print("PASS: seven Panorama V8 guard cases in POSIX and non-POSIX builds (host mocks)")
