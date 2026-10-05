#!/bin/bash
S=/root/csgo-src/CSGO-Source-Linux-20260928
ls -la "$S/src/lib/public/androidarm64/release/panorama_client_client.a" 2>/dev/null | awk '{print "a-file:", $5, $6, $7, $8}' || echo "a-file MISSING"
ls "$S/src/lib/public/androidarm64/release/" | head -10
echo "=== panorama submake manual run, verbose ==="
cd "$S/src/panorama" || exit 1
export OHOS_SDK=/root/ohos-native
export ANDROID_TOOLCHAIN="$OHOS_SDK/llvm"
export ANDROID_NDK_ROOT="$OHOS_SDK"
export ANDROID_PLATFORM=30
make -n -f panorama_androidarm64.mak CFG=release VALVE_NO_AUTO_P4=1 2>&1 | grep -E "libpanorama_client.so|No rule|panorama_client_client.a" | head -6
