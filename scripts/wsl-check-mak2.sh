#!/usr/bin/env bash
M=/root/csgo-src/CSGO-Source-Linux-20260928/src/csgo_android_engine.mak
echo "=== COMPILE_O_SETTINGS defines sample (panorama section) ==="
awk '/^panorama:/{found=1} found && /DEFINES/{print substr($0,1,260); c++} c>3{exit}' "$M"
echo "=== all -D flags containing DEVELOPMENT anywhere ==="
grep -o -- "-D[A-Z_]*" "$M" | sort -u | grep -i develop | head -3
echo "=== how DEFINES var appears at all ==="
grep -n "^DEFINES\|DEFINES +=" "$M" | head -4
