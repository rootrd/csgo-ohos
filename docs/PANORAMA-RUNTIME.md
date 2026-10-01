# Panorama runtime acceptance checklist

## Verification on 2026-10-01

- Built real V8 5.8.283 shared libraries from the pinned official source using
  the recovered official OHOS SDK (Clang 15.0.4, native 26.0.0.105 / API 26)
- Audited all three AArch64 ELF files: `.cr.so` SONAMEs, matching transitive
  library names, OHOS `libc.so`/`libc++_shared.so`, and 16 KB PT_LOAD alignment
- Cross-linked the AArch64 runtime probe against those actual libraries
- Executed the probe successfully on the Linux host using a separately compiled
  V8 ARM64 simulator build: `PANORAMA_V8_SMOKE_PASS`
- Passed seven initialization guard cases in both POSIX and non-POSIX host mocks
- **Not verified:** execution inside a target HAP sandbox, complete Panorama
  assets/menu, graphics, and gameplay. The host simulator pass does not establish
  that the mobile OS permits this runtime's executable-memory allocations

## What the current code requires

Panorama is a real V8 embedder. A library exporting V8 symbol names but returning
zero cannot create an isolate, execute UI scripts, or supply a working menu.
Squirrel map scripting is a separate subsystem; a V8 failure does not mean that
Squirrel itself has been replaced by a stub.

The checked-in public API is **V8 5.8.283**. The official 5.8.283 `include/v8.h`
matches it byte-for-byte. Android/OHOS VPC targets link these names:

- `libv8.cr.so`
- `libv8_libbase.cr.so`
- `libv8_libplatform.cr.so`

The repository does not contain their implementation or an OHOS prebuilt.
The old staging logic synthesized return-zero functions from Android exports.
Those exports are not an implementation, and Android/bionic binaries are not
OHOS/musl binaries. A successful loader/dependency-closure check is insufficient.

## Required checks before calling a HAP playable

1. **Source and ABI:** Build the real pinned V8 sources for AArch64 OHOS/musl,
   with the SDK's libc++ ABI and matching headers. Record source revision,
   compiler, flags, patches and hashes. Match the three required library names
   and their ELF SONAME/DT_NEEDED entries. Audit every undefined V8 symbol in the
   actual Panorama/client libraries, not just a small example.
2. **Packaging:** Include all real runtime dependencies, and ICU/snapshot data
   if the chosen V8 build needs external data. Check ELF architecture, load
   alignment, libc dependencies and symbol closure. Reject fabricated stubs.
3. **Runtime:** In the same app sandbox, signing profile and OS configuration as
   the game, create a platform, isolate and context; compile and execute JS;
   exercise C++ callbacks, persistent handles, typed arrays and exceptions.
   A command-line test outside the app sandbox does not establish this.
4. **Panorama:** Load the actual UI assets and exercise menu input, layout,
   callbacks, panel creation/destruction, localization, text and navigation.
   Then enter a map and exercise the in-game UI. Keep device logs and screenshots.
5. **Graphics:** Independently pass the D3D9 shader/readback probe and game
   rendering checks. A working V8 runtime cannot establish graphics correctness.

`CSGO-Source-Linux-20260928/ohos/tests/panorama_v8_smoke.cpp` is a small real-runtime
probe. It checks a native callback, context/templates, a persistent context,
ArrayBuffer/Int32Array, JSON, a thrown/caught native exception, a regexp and a
closure, expecting `42`. Its success marker is
`PANORAMA_V8_SMOKE_PASS`. Compiling or cross-linking it is **not** a runtime pass.
Pass the ICU data path as its first argument if the V8 build uses external ICU
data. The probe deliberately requires the 5.8.283 ABI.

`python3 scripts/test-panorama-v8-guards.py` checks failure ordering using host
mocks and the actual initialization guard blocks. It does not replace that
probe or a full engine build.

## Executable memory is an independent prerequisite

Official V8 5.8.283 `src/base/platform/platform-linux.cc` requests
`PROT_READ | PROT_WRITE | PROT_EXEC` for executable allocations. Its Ignition
interpreter still creates machine-code bytecode handlers through
`CodeAssembler::GenerateCode` in `src/interpreter/interpreter.cc`. There is no
`--jitless` option in that release. Merely selecting Ignition, disabling
optimization, or disabling snapshots does not remove the executable-memory need.

V8 introduced execution without runtime executable-memory allocation in **7.4**.
Changing a flag on 5.8 is therefore not a fix. Replacing its shared libraries
with a newer V8 is not an ABI-compatible upgrade; Panorama must be adapted and
rebuilt against the new API. Switching to HarmonyOS JSVM also requires adapting
the substantial V8 C++ binding surface, not renaming a library.

Current OpenHarmony JSVM documentation says JIT is disabled by default and
requires an approved `ohos.permission.kernel.ALLOW_EXECUTABLE_FORT_MEMORY` ACL
profile to enable it. Declaring that permission without the corresponding
profile can prevent installation, and Secure Shield disables JIT globally.
This JSVM permission documentation is **not evidence** that an independently
built 2017 V8 RWX allocator will work with that permission. Confirm the exact
target system's supported execution path; do not add permissions or weaken
security settings to make a test pass.

## Source recovery / build experiment

`scripts/build-ohos-v8.sh` downloads checksum-pinned sources from the official
V8/Chromium, Google and Python repositories, builds the host code generator and
the three OHOS shared libraries with their required `.cr.so` SONAMEs, and
cross-links the runtime probe. Run it with the official SDK native directory:

```sh
OHOS_SDK=/path/to/sdk/native BUILD_JOBS=3 bash scripts/build-ohos-v8.sh
```

It requires host Clang/Clang++, curl, tar, make and Python 3. Python 2.7.18 is
built locally for legacy generation if `PYTHON2` is not supplied. Build files
default to the ignored `CSGO-Source-Linux-20260928/runtime/ohos/v8-build/` tree.
Use `V8_BUILD_ROOT`, `V8_DOWNLOAD_DIR`, `V8_SOURCE_DIR`, and `V8_INSTALL_DIR` to
select other directories; `HOST_CC` and `HOST_CXX` select host compilers. The
default install directory is the ignored `src/lib/common/androidarm64/` linker
input directory. It also emits upstream license notices; matching notices are
included in `hap/entry/src/main/resources/rawfile/licenses/` for HAP packaging.
It does not install the HAP or add permissions.

The 5.8.283 source can be recovered from the official V8 repository. Its legacy
GYP dependencies are pinned in `DEPS`. Recovered build prerequisites include
Python 2.7.18 (host code-generation only), the pinned GYP source, the trace-event
header, Clang revision helper and Google's `gtest_prod.h` header.

An initial feasibility build can use `v8_use_snapshot=false`,
`v8_use_external_startup_data=0`, `v8_enable_i18n_support=0`, and
`v8_enable_inspector=0`, with `target_arch=arm64` and shared-library components.
These are **experimental build settings**, not a claim of full UI compatibility:
Intl is absent, all UI assets still need validation, and executable memory is
still required. The existing OHOS release code disables the Windows-only remote
V8 inspector. A target SDK, actual cross-link and on-device execution are still
required; host compilation alone proves none of those.

## Primary sources

- [V8 5.8.283 source](https://github.com/v8/v8/tree/5.8.283)
- [V8 5.8.283 Linux allocations](https://github.com/v8/v8/blob/5.8.283/src/base/platform/platform-linux.cc)
- [V8 5.8.283 interpreter handlers](https://github.com/v8/v8/blob/5.8.283/src/interpreter/interpreter.cc)
- [V8 5.8.283 flags](https://github.com/v8/v8/blob/5.8.283/src/flag-definitions.h)
- [JIT-less V8 announcement](https://v8.dev/blog/jitless)
- [OpenHarmony JSVM JIT profile guidance](https://github.com/openharmony/docs/blob/master/en/application-dev/napi/jsvm-apply-jit-profile.md)
