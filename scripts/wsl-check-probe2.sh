#!/usr/bin/env bash
echo "=== WSL vulkan_probe.cpp patch ==="
grep -c "Maleoon multi-clear" /root/csgo-src/CSGO-Source-Linux-20260928/src/materialsystem/shaderapivulkan/probe/vulkan_probe.cpp
echo "=== libmain strings ==="
strings -a /root/csgo-src/CSGO-Source-Linux-20260928/runtime/ohos/native-build/libmain.so | grep -c "Maleoon multi-clear"
echo "=== libmain mtime vs probe.cpp ==="
ls -la /root/csgo-src/CSGO-Source-Linux-20260928/runtime/ohos/native-build/libmain.so /root/csgo-src/CSGO-Source-Linux-20260928/src/materialsystem/shaderapivulkan/probe/vulkan_probe.cpp | awk '{print $6,$7,$8,$9}'
echo "=== probe obj ==="
ls -la /root/csgo-src/CSGO-Source-Linux-20260928/src/materialsystem/shaderapivulkan/CMakeFiles/csgo_vulkan_probe.dir/probe/vulkan_probe.cpp.o 2>/dev/null | awk '{print $6,$7,$8}'
echo "=== E drive vulkan_probe ==="
grep -c "Maleoon multi-clear" /mnt/e/csgo/CSGO-Source-Linux-20260928/src/materialsystem/shaderapivulkan/probe/vulkan_probe.cpp
