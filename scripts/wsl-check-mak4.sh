#!/usr/bin/env bash
cd /root/csgo-src/CSGO-Source-Linux-20260928/src
echo "=== all androidarm64 mak files anywhere ==="
find . -maxdepth 2 -name "*androidarm64*.mak" 2>/dev/null | head -8
echo "=== panorama_s1wrapper mak DEVELOPMENT ==="
P=$(find . -maxdepth 2 -name "panorama_s1wrapper*androidarm64*.mak" | head -1)
echo "mak file: $P"
grep -o -- "-DDEVELOPMENT_ONLY=1" "$P" 2>/dev/null | head -1 || echo NOT_PRESENT
echo "=== wrap_resource.o mtime vs panorama lib ==="
ls -la panorama_s1wrapper/obj_panorama_s1wrapper_androidarm64_client/release/wrap_resource.o game/../game 2>/dev/null | head -2
ls -la /root/csgo-src/CSGO-Source-Linux-20260928/game/bin/androidarm64/release/libpanorama_client.so
echo "=== does new lib still reference code.pbin error path ==="
strings -a /root/csgo-src/CSGO-Source-Linux-20260928/game/bin/androidarm64/release/libpanorama_client.so | grep -m2 "Resource %s failed"
