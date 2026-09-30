#!/usr/bin/env bash
echo "=== WSL native-build libmain ==="
ls -la /root/csgo-src/CSGO-Source-Linux-20260928/runtime/ohos/native-build/libmain.so
strings -a /root/csgo-src/CSGO-Source-Linux-20260928/runtime/ohos/native-build/libmain.so | grep -m1 "CSGO native Vulkan diagnostic"
echo "=== E tree install/lib libmain ==="
ls -la /mnt/e/csgo/CSGO-Source-Linux-20260928/runtime/ohos/install/lib/libmain.so
strings -a /mnt/e/csgo/CSGO-Source-Linux-20260928/runtime/ohos/install/lib/libmain.so | grep -m1 "CSGO native Vulkan diagnostic"
echo "=== HAP libmain ==="
cd /mnt/e/csgo && rm -rf tmp-hap3 && mkdir tmp-hap3
/c/Windows/System32/tar.exe -xf hap/entry/build/default/outputs/default/entry-default-signed.hap -C tmp-hap3 libs/arm64-v8a/libmain.so 2>/dev/null
strings -a tmp-hap3/libs/arm64-v8a/libmain.so | grep -m1 "CSGO native Vulkan diagnostic"
echo "=== device libmain ==="
sh_path=""
echo skip
