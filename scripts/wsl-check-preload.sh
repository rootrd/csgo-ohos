#!/usr/bin/env bash
echo "=== WSL engine_startup.cpp preload ==="
grep -c "preloaded\|preload %s failed" /root/csgo-src/CSGO-Source-Linux-20260928/android/native/engine_startup.cpp
echo "=== native-build libmain preload strings ==="
strings -a /root/csgo-src/CSGO-Source-Linux-20260928/runtime/ohos/native-build/libmain.so | grep -c "preloaded"
echo "=== native-build libmain mtime ==="
ls -la /root/csgo-src/CSGO-Source-Linux-20260928/runtime/ohos/native-build/libmain.so | awk '{print $5,$6,$7,$8}'
echo "=== WSL stage install/lib ==="
ls -la /root/csgo-src/CSGO-Source-Linux-20260928/runtime/ohos/install/lib/libmain.so | awk '{print $5,$6,$7,$8}'
strings -a /root/csgo-src/CSGO-Source-Linux-20260928/runtime/ohos/install/lib/libmain.so | grep -c "preloaded"
