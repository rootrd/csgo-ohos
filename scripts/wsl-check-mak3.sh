#!/usr/bin/env bash
cd /root/csgo-src/CSGO-Source-Linux-20260928/src
echo "=== per-project mak files ==="
ls | grep "androidarm64.*mak$" | head -8
echo "=== panorama mak DEVELOPMENT ==="
grep -o -- "-DDEVELOPMENT_ONLY=1" panorama_androidarm64_client.mak 2>/dev/null | head -1 || echo NOT_IN_PANORAMA_MAK
grep -o -- "-DDEVELOPMENT_ONLY=1" launcher_androidarm64_client.mak 2>/dev/null | head -1 || true
echo "=== wrap_resource compiled object rebuild time ==="
ls -la panorama/obj_panorama_androidarm64_client/release/wrap_resource.o 2>/dev/null || find . -name "wrap_resource.o" -newer /root/csgo-src/CSGO-Source-Linux-20260928/src/devtools/makefile_base_posix.mak 2>/dev/null | head -2
find . -name "wrap_resource.o" 2>/dev/null | head -3
