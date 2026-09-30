#!/usr/bin/env bash
RE=/root/ohos-native/llvm/bin/llvm-readelf
D9=/mnt/e/csgo/deps/dxvk-ohos-legacy/build.ohos/src/d3d9/d3d9.so
DXGI=$(ls /mnt/e/csgo/deps/dxvk-ohos-legacy/build.ohos/src/dxgi/libdxvk_dxgi.so.0 2>/dev/null)
echo "=== d3d9.so SONAME+NEEDED ==="
$RE -d "$D9" | grep -E "SONAME|NEEDED"
echo "=== dxgi NEEDED ==="
$RE -d "$DXGI" | grep -E "SONAME|NEEDED"
