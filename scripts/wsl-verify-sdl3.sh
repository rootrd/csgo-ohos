#!/usr/bin/env bash
RE=/root/ohos-native/llvm/bin/llvm-readelf
echo "=== new libSDL3 soname ==="
$RE -d /root/csgo-src/CSGO-Source-Linux-20260928/runtime/ohos/install/lib/libSDL3.so | grep SONAME
echo "=== engine modules NEEDED libSDL3 ==="
for f in /root/csgo-src/CSGO-Source-Linux-20260928/game/bin/androidarm64/release/*.so \
         /root/csgo-src/CSGO-Source-Linux-20260928/game/csgo/bin/androidarm64/release/*.so; do
    n=$($RE -d "$f" 2>/dev/null | grep -o "libSDL3[^]]*" | head -1)
    echo "$(basename "$f") -> ${n:-none}"
done | grep -v "none" | head -12
