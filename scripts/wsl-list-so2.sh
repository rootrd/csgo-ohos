#!/usr/bin/env bash
cd /root/csgo-src/CSGO-Source-Linux-20260928/game/csgo/bin/androidarm64/release 2>/dev/null || { echo NO_DIR; exit 1; }
ls -la *.so | awk '{printf "%10d  %s\n", $5, $9}'
echo "=== vscript NEEDED check (bionic v8?) ==="
/root/ohos-native/llvm/bin/llvm-readelf -d libvscript_client.so 2>/dev/null | grep NEEDED
echo "=== client NEEDED sample ==="
/root/ohos-native/llvm/bin/llvm-readelf -d libclient_panorama_client.so 2>/dev/null | grep NEEDED | head -15
