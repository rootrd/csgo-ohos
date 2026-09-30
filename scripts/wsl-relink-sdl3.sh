#!/usr/bin/env bash
# 删除 NEEDED libSDL3.so.0 的引擎模块（game/bin 产物 + obj 目录中间产物），
# 强制 make 真正重新链接到新 soname libSDL3.so
RE=/root/ohos-native/llvm/bin/llvm-readelf
SRC=/root/csgo-src/CSGO-Source-Linux-20260928/src
removed=0
for f in $(find /root/csgo-src/CSGO-Source-Linux-20260928/game/bin/androidarm64/release \
               /root/csgo-src/CSGO-Source-Linux-20260928/game/csgo/bin/androidarm64/release \
               -maxdepth 1 -name "*.so" 2>/dev/null); do
    if $RE -d "$f" 2>/dev/null | grep -q "libSDL3.so.0"; then
        echo "relink: $(basename "$f")"
        rm -f "$f"
        removed=$((removed+1))
    fi
done
# obj 目录里的链接产物（make 的 copy 步骤源）
for o in $(find "$SRC" -name "obj_*_androidarm64_client" -type d 2>/dev/null); do
    for f in "$o/release/"*.so; do
        [[ -f "$f" ]] || continue
        if $RE -d "$f" 2>/dev/null | grep -q "libSDL3.so.0"; then
            echo "obj relink: $(basename "$f")"
            rm -f "$f"
            removed=$((removed+1))
        fi
    done
done
echo "removed=$removed"
