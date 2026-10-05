#!/bin/bash
A2L=/root/ohos-native/llvm/bin/llvm-addr2line
S=/root/csgo-src/CSGO-Source-Linux-20260928
CP=$(find $S/src/game/client -name "libclient_panorama_client.so" -not -path "*androidarm64_client/release*" 2>/dev/null | head -1)
[ -z "$CP" ] && CP=$(find $S -name "libclient_panorama_client.so" 2>/dev/null | grep -v "obj_panorama" | head -1)
echo "lib=$CP"
ls -la "$CP"
echo "== client_panorama frames =="
$A2L -Cfipe "$CP" 0x103a064 0x1038c40 0xf6ae78
