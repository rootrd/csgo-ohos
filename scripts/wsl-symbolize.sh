#!/usr/bin/env bash
cd /root/csgo-src/CSGO-Source-Linux-20260928/game/bin/androidarm64/release
A2L=/root/ohos-native/llvm/bin/llvm-addr2line
echo "=== shaderapidx9 #01 0x8a9d4 ==="
$A2L -Cfipe libshaderapidx9_client.so 0x8a9d4
echo "=== materialsystem #02 0xb7108 ==="
$A2L -Cfipe libmaterialsystem_client.so 0xb7108
echo "=== materialsystem #03 0xaeef0 ==="
$A2L -Cfipe libmaterialsystem_client.so 0xaeef0
echo "=== materialsystem #04 0xaee74 ==="
$A2L -Cfipe libmaterialsystem_client.so 0xaee74
echo "=== engine #05 0x7938e0 ==="
$A2L -Cfipe libengine_client.so 0x7938e0
echo "=== engine #06 0x793798 ==="
$A2L -Cfipe libengine_client.so 0x793798
