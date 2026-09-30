// Exercise the real binding templates, including registration before main().
#include "vscript/ivscript.h"
#include <cstdio>
#include <cstdlib>

static void require(bool value, const char *message) {
    if (!value) { std::fprintf(stderr, "VSCRIPT_FAIL: %s\n", message); std::exit(1); }
}

struct Base {
    int value = 7;
    virtual int First() const { return value; }
    virtual int Add(int amount) { return value + amount; }
    int Plain() const { return value * 2; }
};
struct Derived : Base {
    int First() const override { return 42; }
    int Add(int amount) override { return value + 2 * amount; }
};
static int FreeFunction(int value) { return value * 3; }

// ARM64 virtual slot zero has a zero pointer word and a nonzero virtual flag.
// This used to trap while dlopen was running the client's static constructors.
static ScriptFunctionBindingStorageType_t first = ScriptConvertFuncPtrToVoid(&Base::First);
static ScriptFunctionBindingStorageType_t add = ScriptConvertFuncPtrToVoid(&Base::Add);
static ScriptFunctionBindingStorageType_t plain = ScriptConvertFuncPtrToVoid(&Base::Plain);
static ScriptFunctionBindingStorageType_t freeFunction = ScriptConvertFreeFuncPtrToVoid(&FreeFunction);

int main() {
    Derived derived;
    Base *base = &derived;
    ScriptVariant_t result, argument(5);
    require(ScriptCreateBinding(base, &Base::First)(first, base, nullptr, 0, &result)
            && int(result) == 42, "virtual dispatch at slot zero");
    require(ScriptCreateBinding(base, &Base::Add)(add, base, &argument, 1, &result)
            && int(result) == 17, "virtual dispatch with arguments at a later slot");
    require(ScriptCreateBinding(base, &Base::Plain)(plain, base, nullptr, 0, &result)
            && int(result) == 14, "nonvirtual const member");
    require(ScriptCreateBinding(&FreeFunction)(freeFunction, nullptr, &argument, 1, &result)
            && int(result) == 15, "free function");
    using Member = int (Base::*)() const;
    Member nullMember = nullptr;
    require(ScriptConvertFuncPtrFromVoid<Member>(ScriptConvertFuncPtrToVoid(nullMember)) == nullptr,
            "null member round trip");
    require(ScriptConvertFuncPtrFromVoid<Member>(first) == &Base::First, "member pointer identity");
    std::puts("VSCRIPT_PASS: static registration, virtual dispatch, const/nonvirtual/free functions, null pointers");
    return 0;
}
