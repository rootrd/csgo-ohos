# Native Linux recovery and unsigned HAP build

This workflow uses native Linux processes, not Docker. It does not require the original excluded `src/lib` bundle: the missing dependencies, VPC, protobuf compiler and matching V8 can be built from source. Device behavior is a separate validation gate.

## Toolchain provenance

- Official SDK page: https://developer.huawei.com/consumer/cn/download/command-line-tools-for-hmos
- Download selected: Command Line Tools for Linux(x86), version **26.0.0.851**, released 2026-09-23
- Downloaded bytes: **2,348,063,309**
- Computed archive SHA-256: `ab604bd92721d5cbcafd154e6461d46b9f1b105e7b89eab04a9d046681198082`
- Native manifest: **26.0.0.105**, API **26**, Clang **15.0.4**
- Hvigor: **6.26.8**
- The archive was obtained using the official page's download control. ZIP path safety and extraction CRC checks passed. The site's copy-checksum control returned no readable clipboard value, so this recorded hash identifies the downloaded artifact; an independent vendor-hash comparison is not claimed.
- Host build tools come from official Debian13 x86_64 packages, extracted into the project workspace. `scripts/native-linux/packages.tsv` records exact versions and `packages.sha256` records the downloaded package hashes. No system package installation is performed. If a pinned version disappears from the official registry, the bootstrap fails rather than silently switching versions.

The SDK installer extracts the compiler, ArkTS, packaging tools and required HMS declarations. It includes previewer components required by Hvigor SDK validation, but does not extract the emulator or optional BiSheng compiler; the unsigned build copy uses Clang.

## Restore tools

The base system must already provide apt-get and the Debian archive keyring, dpkg-deb, Python3, make, git, curl, unzip, patch, standard shell/coreutils, and Java21. The bootstrap is a pinned tool overlay, not a full operating-system installer.

From the repository root on Debian13 x86_64, download the official ZIP above, then run:

```sh
bash scripts/native-linux/bootstrap-host.sh
bash scripts/native-linux/install-sdk.sh /path/to/commandline-tools-linux-x64-26.0.0.851.zip
source scripts/native-linux/sdk-env.sh
```

Tool files default to `CSGO-Source-Linux-20260928/runtime/ohos/toolchain`. Set `CSGO_TOOL_ROOT` before all three commands to use another writable directory. `OHOS_SDK` means the SDK's **native** directory containing `llvm/` and `sysroot/`. Java21 must already be available; override `JAVA_HOME` if its installation is elsewhere.

## Rebuild from source

```sh
export BUILD_JOBS=3
export CSGO_DEPS_WORK="$PWD/CSGO-Source-Linux-20260928/runtime/ohos/dependency-build"
export PROTOC="$CSGO_DEPS_WORK/host/protobuf/src/protoc"
bash scripts/build-ohos-engine.sh vpc
bash scripts/rebuild-ohos-dependencies.sh all
bash scripts/build-dxvk-ohos-linux.sh

cmake -S deps/SDL -B CSGO-Source-Linux-20260928/runtime/ohos/sdl-build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$OHOS_SDK/build/cmake/ohos.toolchain.cmake" \
  -DOHOS_ARCH=arm64-v8a -DCMAKE_BUILD_TYPE=Release -DSDL_STATIC=OFF -DSDL_TESTS=OFF \
  -DCMAKE_SHARED_LINKER_FLAGS="-Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384" \
  -DCMAKE_INSTALL_PREFIX="$PWD/CSGO-Source-Linux-20260928/runtime/ohos/install"
cmake --build CSGO-Source-Linux-20260928/runtime/ohos/sdl-build --parallel "$BUILD_JOBS"
cmake --install CSGO-Source-Linux-20260928/runtime/ohos/sdl-build

# Existing VPC include rules name runtime/android; it points to the OHOS prefix.
# Refuse to overwrite an existing different runtime directory.
runtime="$PWD/CSGO-Source-Linux-20260928/runtime"
if test -e "$runtime/android" || test -L "$runtime/android"; then
  test "$(readlink -f "$runtime/android")" = "$(readlink -f "$runtime/ohos")" || exit 1
else
  ln -s ohos "$runtime/android"
fi
bash scripts/build-ohos-v8.sh
bash scripts/build-ohos-engine.sh foundation
bash scripts/build-ohos-engine.sh engine
bash scripts/build-ohos-engine.sh native
bash scripts/build-ohos-engine.sh stage
bash scripts/package-ohos-linux.sh
```

The dependency builder verifies pinned upstream source archives. Steam Audio/HRTF is explicitly disabled on OHOS; normal stereo mixing remains. It supplies no callable fake Steam Audio library. The V8 helper builds actual V8 5.8.283 implementations matching the repository headers, not zero-return exports. V8 JIT executable-memory permission on a retail device remains a runtime question; see `PANORAMA-RUNTIME.md`.

## Packaging and validation gates

- `stage` requires all 26 named engine modules, `libmain`, DXVK, SDL and the real V8 trio. The runtime directory is checked for AArch64 ELF, required exported APIs, absence of old zero-return V8 exports, Android ELF notes and complete `DT_NEEDED` closure against the official SDK.
- `package-ohos-linux.sh` builds in a marked isolated HAP project copy, removes signing references, excludes signing material and leaves the canonical signing configuration unused. It uses the installed SDK's target version and preserves the project's declared compatible version. A compatible-version declaration does not prove that every device provides all imported native APIs.
- The final HAP is CRC-checked, its packaged native binaries are extracted and audited again, and its SHA-256 is printed.
- Unsigned HAP output is not a normally installable signed application. Device-specific authorized signing/profile information and a connected device are still needed for normal installation and real launch/rendering tests.
- The official SDK libc++_shared.so currently has 4 KiB ELF load alignment. Project modules are linked for 16 KiB alignment where supported, but whole-package 16 KiB device compatibility is not established.
- Host V8 functional tests and ARM64 compile/link checks cannot establish menu rendering, JIT permissions, map loading or gameplay on a phone.

## Current validation status

On 2026-10-01, the full release build completed with exit0. All26 engine modules linked from source, and both the staged runtime and binaries extracted from the final HAP passed the ELF/API/dependency audit. The HAP contains92 ARM64 native-library files and passed ZIP CRC validation.

- File: `csgo-ohos-0.1.0-1000062-arm64-unsigned.hap`
- Size:142,751,955 bytes
- SHA-256:`5d9f21722f9f837ba620d9b94c9742166e26fbe9254c45ddd2a76383b379b0a2`
- App:`com.csgosource.ohos`, version0.1.0 /1000062, release mode, `debug=false`
- Signing:none. Physical-device execution:not performed.

The machine-readable [build evidence and binary hash manifest](build-evidence/native-linux-2026-10-01.json) records completed gates and remaining runtime limits. Do not treat successful HAP creation as proof of playable CS:GO.
