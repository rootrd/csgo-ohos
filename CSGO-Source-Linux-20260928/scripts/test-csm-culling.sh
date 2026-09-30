#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ ${CONTAINER_ID:-} != dev ]]; then
    exec distrobox enter -T -n dev -- bash "$repo_dir/scripts/test-csm-culling.sh" "$@"
fi
[[ $# == 0 || ( $# == 1 && $1 == --android ) ]] || { echo 'Usage: test-csm-culling.sh [--android]' >&2; exit 2; }

out="$repo_dir/runtime/android/csm-culling-tests"
mkdir -p "$out"
flags=(
    -std=c++17 -O1 -g -fno-strict-aliasing -fsigned-char -fno-fast-math -ffp-contract=off
    -Wno-c++11-narrowing -Wno-register -Wno-return-type-c-linkage
    -DNDEBUG -DPOSIX -D_POSIX -DLINUX -D_LINUX -DGNUC -DCOMPILER_GCC
    -DRAD_TELEMETRY_DISABLED -DCSTRIKE15 -D_DLL_EXT=_client.so -D_FILE_OFFSET_BITS=64
    -I"$repo_dir/src/public" -I"$repo_dir/src/public/tier0" -I"$repo_dir/src/common"
)
source="$repo_dir/src/mathlib/tests/volumeculler_test.cpp"
libraries="$repo_dir/src/lib/public/linux64"
clang++ "${flags[@]}" -march=nocona -D_GLIBCXX_USE_CXX11_ABI=0 \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    "$source" "$repo_dir/src/mathlib/volumeculler.cpp" "$libraries/mathlib_client.a" \
    -L"$libraries" -ltier0_client -lvstdlib_client -ldl -pthread -o "$out/culling-host"
LD_LIBRARY_PATH="$repo_dir/game/bin/linux64" "$out/culling-host"

if [[ ${1:-} == --android ]]; then
    ndk=${ANDROID_NDK_ROOT:-/home/deck/Code/Toolchains/android-ndk}
    sdk=${ANDROID_SDK_ROOT:-/home/deck/Code/Toolchains/android-sdk}
    libraries="$repo_dir/src/lib/public/androidarm64/release"
    "$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang++" \
        "${flags[@]}" -march=armv8-a -moutline-atomics -DANDROID \
        -I"$repo_dir/runtime/android/deps/sse2neon" \
        "$source" "$repo_dir/src/mathlib/volumeculler.cpp" "$libraries/mathlib_client.a" \
        -L"$libraries" -ltier0_client -lvstdlib_client -ldl -llog \
        -Wl,-z,max-page-size=16384 -o "$out/culling-android"
    adb=("$sdk/platform-tools/adb")
    if [[ -n ${ANDROID_SERIAL:-} ]]; then adb+=(-s "$ANDROID_SERIAL"); fi
    device_out=/data/local/tmp/csgo-culling-test
    "${adb[@]}" shell mkdir -p "$device_out"
    "${adb[@]}" push "$out/culling-android" "$libraries/libtier0_client.so" \
        "$libraries/libvstdlib_client.so" \
        "$ndk/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" \
        "$device_out/"
    "${adb[@]}" shell "chmod 755 $device_out/culling-android && cd $device_out && LD_LIBRARY_PATH=. ./culling-android"
fi
