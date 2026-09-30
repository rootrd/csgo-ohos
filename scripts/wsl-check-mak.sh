#!/usr/bin/env bash
M=/root/csgo-src/CSGO-Source-Linux-20260928/src/csgo_android_engine.mak
# 找 panorama 工程段的变量定义
grep -n "DEVELOPMENT" "$M" | head -5
echo "=== project1 include/defines sample ==="
sed -n '1,40p' "$M" | grep -nE "VPC_CMD|CONFIG|^include|defines" | head -8
echo "=== panorama sub-mak files ==="
ls /root/csgo-src/CSGO-Source-Linux-20260928/src/ | grep -E "panorama.*mak" | head -5
echo "=== check sub mak DEVELOPMENT ==="
grep -l "DEVELOPMENT_ONLY" /root/csgo-src/CSGO-Source-Linux-20260928/src/panorama_androidarm64_client.mak 2>/dev/null && grep -m2 -o "\-DDEVELOPMENT_ONLY=1" /root/csgo-src/CSGO-Source-Linux-20260928/src/panorama_androidarm64_client.mak | head -2
