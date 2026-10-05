#!/bin/bash
OBJ=/root/ohos-native/llvm/bin/llvm-objdump
CP=/root/csgo-src/CSGO-Source-Linux-20260928/game/csgo/bin/androidarm64/release/libclient_panorama_client.so
$OBJ -d --start-address=0x1038ba0 --stop-address=0x1038c80 "$CP" 2>/dev/null | tail -40
echo "=== resolve the call target just before 0x1038c40 ==="
$OBJ -d --start-address=0x1038b00 --stop-address=0x1038c45 "$CP" 2>/dev/null | grep -E "bl\s" | tail -6
