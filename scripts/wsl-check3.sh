#!/usr/bin/env bash
P=/root/csgo-src/CSGO-Source-Linux-20260928/runtime/ohos/install
echo "=== include/ top ==="
ls "$P/include/"
echo "=== curl dir ==="
ls "$P/include/curl" 2>/dev/null | head -3 || echo NO_CURL_DIR
echo "=== ft2build ==="
ls "$P/include/freetype2/ft2build.h" 2>/dev/null || echo NO_FT2BUILD
echo "=== steam_audio ==="
ls "$P/include/steam_audio" 2>/dev/null || echo NO_STEAM_AUDIO
