// Standalone compatibility probe for the V8 ABI used by Panorama.
// Compile against src/thirdparty/v8/include and the real OHOS V8 libraries.
// A successful cross-link is NOT a runtime pass. Run in the app's sandbox.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "v8.h"
#include "libplatform/libplatform.h"

namespace {
class Allocator : public v8::ArrayBuffer::Allocator {
public:
    void* Allocate(size_t length) override { return std::calloc(1, length); }
    void* AllocateUninitialized(size_t length) override { return std::malloc(length); }
    void Free(void* data, size_t) override { std::free(data); }
};

void NativeAdd(const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* isolate = args.GetIsolate();
    if (args.Length() != 2) {
        isolate->ThrowException(v8::Exception::TypeError(
            v8::String::NewFromUtf8(isolate, "NativeAdd requires two values")));
        return;
    }
    args.GetReturnValue().Set(v8::Number::New(isolate,
        args[0]->NumberValue() + args[1]->NumberValue()));
}

bool ExerciseContext(v8::Isolate* isolate) {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handles(isolate);
    v8::TryCatch exceptions(isolate);
    v8::Local<v8::ObjectTemplate> global = v8::ObjectTemplate::New(isolate);
    if (global.IsEmpty()) return false;
    global->Set(v8::String::NewFromUtf8(isolate, "nativeAdd"),
        v8::FunctionTemplate::New(isolate, NativeAdd));
    v8::Local<v8::Context> context = v8::Context::New(isolate, nullptr, global);
    if (context.IsEmpty()) return false;
    v8::Persistent<v8::Context> retained(isolate, context);
    v8::Context::Scope context_scope(context);
    const char* source_text =
        "(function () {"
        "var values = new Int32Array(new ArrayBuffer(8));"
        "values[0] = 19; values[1] = 23;"
        "var result = nativeAdd(values[0], values[1]);"
        "var saved = JSON.parse(JSON.stringify({answer: result}));"
        "var caught = false;"
        "try { nativeAdd(1); } catch (e) { caught = e instanceof TypeError; }"
        "if (!caught || !/^panorama-([0-9]+)$/.test('panorama-42'))"
        "throw new Error('exception or regexp test failed');"
        "var closure = function () { return saved.answer; };"
        "return closure();"
        "})()";
    v8::Local<v8::Script> script;
    v8::Local<v8::Value> result;
    bool ok = v8::Script::Compile(context,
        v8::String::NewFromUtf8(isolate, source_text)).ToLocal(&script)
        && script->Run(context).ToLocal(&result)
        && result->Int32Value(context).FromMaybe(-1) == 42;
    if (!ok && exceptions.HasCaught()) {
        v8::String::Utf8Value message(exceptions.Exception());
        std::fprintf(stderr, "PANORAMA_V8_SMOKE_FAIL exception: %s\n",
            *message ? *message : "unknown");
    }
    retained.Reset();
    return ok;
}
}  // namespace

int main(int argc, char** argv) {
    const char* version = v8::V8::GetVersion();
    if (!version || !version[0]) {
        std::fprintf(stderr, "PANORAMA_V8_SMOKE_FAIL missing runtime version\n");
        return 1;
    }
    std::printf("Panorama V8 runtime: %s\n", version);
    if (std::strncmp(version, "5.8.283", 7) != 0
        || (version[7] && version[7] != '.' && version[7] != '-')) {
        std::fprintf(stderr, "PANORAMA_V8_SMOKE_FAIL unexpected runtime ABI\n");
        return 1;
    }
    v8::Platform* platform = v8::platform::CreateDefaultPlatform(1);
    if (!platform) {
        std::fprintf(stderr, "PANORAMA_V8_SMOKE_FAIL no platform\n");
        return 1;
    }
    // An ICU file is required only when V8 was built with external ICU data.
    if (!v8::V8::InitializeICU(argc > 1 ? argv[1] : nullptr)) {
        std::fprintf(stderr, "PANORAMA_V8_SMOKE_FAIL ICU initialization\n");
        delete platform;
        return 1;
    }
    v8::V8::InitializePlatform(platform);
    if (!v8::V8::Initialize()) {
        std::fprintf(stderr, "PANORAMA_V8_SMOKE_FAIL initialization\n");
        v8::V8::ShutdownPlatform();
        delete platform;
        return 1;
    }
    Allocator allocator;
    v8::Isolate::CreateParams params;
    params.array_buffer_allocator = &allocator;
    v8::Isolate* isolate = v8::Isolate::New(params);
    bool ok = isolate && ExerciseContext(isolate);
    if (isolate) isolate->Dispose();
    v8::V8::Dispose();
    v8::V8::ShutdownPlatform();
    delete platform;
    std::puts(ok ? "PANORAMA_V8_SMOKE_PASS" : "PANORAMA_V8_SMOKE_FAIL");
    return ok ? 0 : 1;
}
