#!/usr/bin/env bash
P=/root/csgo-src/CSGO-Source-Linux-20260928/runtime/ohos/install
echo "=== prefix/lib ==="
ls "$P/lib" | tr '\n' ' '; echo
echo "=== SDL3 cmake config ==="
ls "$P/lib/cmake/SDL3/" 2>/dev/null || echo NO_SDL3_CONFIG
echo "=== dxvk ==="
ls -la "$P/lib/libdxvk_d3d9.so" 2>/dev/null || echo NO_DXVK
echo "=== sse2neon ==="
ls /root/csgo-src/CSGO-Source-Linux-20260928/runtime/ohos/deps/sse2neon/sse2neon.h 2>/dev/null || echo NO_SSE2NEON
