#!/usr/bin/env bash
NB=/root/csgo-src/CSGO-Source-Linux-20260928/runtime/ohos/native-build
echo "=== native-build libmain* ==="
ls -la $NB/libmain* 2>/dev/null
echo "=== file type ==="
file $NB/libmain.so 2>/dev/null
echo "=== CMakeCache CSGO_OHOS_PROBE ==="
grep -i "CSGO_OHOS_PROBE\|CSGO_DEV_BUILD" $NB/CMakeCache.txt 2>/dev/null
echo "=== main.cpp probe line in WSL ==="
grep -n "CSGO_OHOS_PROBE" /root/csgo-src/CSGO-Source-Linux-20260928/android/native/android_main.cpp | head -3
grep -n "CSGO_OHOS_PROBE" /root/csgo-src/CSGO-Source-Linux-20260928/android/CMakeLists.txt | head -3
