#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
repo="$(cd "$root/../.." && pwd)"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
flags=(-std=c++17 -fms-extensions -DDXVK_NATIVE_OHOS -D__OHOS__=1
  -I "$root/include" -I "$root/include/native/windows"
  -I "$root/include/native/directx" -I "$repo/deps/SDL/src/video/khronos")
"${CXX:-g++}" "${flags[@]}" -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$root/tests/vulkan/test_device_features.cpp" "$root/src/dxvk/dxvk_extensions.cpp" \
  -o "$out/test-device-features"
# LeakSanitizer cannot inspect processes under some sandbox/ptrace runners.
ASAN_OPTIONS=detect_stack_use_after_return=1:detect_leaks=0 "$out/test-device-features"
"${CXX:-g++}" "${flags[@]}" -fsyntax-only \
  "$root/src/dxvk/dxvk_adapter.cpp" "$root/src/dxvk/dxvk_extensions.cpp" \
  "$root/src/d3d9/d3d9_interface.cpp" "$root/src/d3d9/d3d9_device.cpp"
echo "PASS: host syntax checks for the changed adapter and D3D9 translation units"
