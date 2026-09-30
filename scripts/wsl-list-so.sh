#!/usr/bin/env bash
cd /root/csgo-src/CSGO-Source-Linux-20260928/game/bin/androidarm64/release 2>/dev/null || { echo NO_DIR; exit 1; }
ls -la *.so | awk '{printf "%10d  %s\n", $5, $9}'
echo "=== count ==="
ls *.so | wc -l
