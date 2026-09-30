#!/usr/bin/env bash
P=/root/csgo-src/CSGO-Source-Linux-20260928/runtime/ohos/install/lib
echo "=== prefix libSDL3 files ==="
ls -la "$P" | grep -i sdl3
echo "=== each one's soname ==="
for f in "$P"/libSDL3*; do
    [[ -f "$f" ]] || continue
    echo "--- $f"
    /root/ohos-native/llvm/bin/llvm-readelf -d "$f" 2>/dev/null | grep SONAME
done
echo "=== engine .so mtime (was relink done?) ==="
ls -la /root/csgo-src/CSGO-Source-Linux-20260928/game/bin/androidarm64/release/libengine_client.so
echo "=== makefile link line for engine ==="
grep -m1 "SYSTEMLIBS" /root/csgo-src/CSGO-Source-Linux-20260928/src/engine/engine_androidarm64.mak 2>/dev/null | cut -c1-200
grep -rn "SDL3" /root/csgo-src/CSGO-Source-Linux-20260928/src/engine/engine_androidarm64.mak 2>/dev/null | head -4
