#!/bin/bash
# 符号化 snap-frames.txt（offset 库路径）里的每一帧
A2L=/root/ohos-native/llvm/bin/llvm-addr2line
S=/mnt/e/csgo/CSGO-Source-Linux-20260928
declare -A LIBMAP=(
  [libclient_panorama_client.so]="$S/game/csgo/bin/androidarm64/release/libclient_panorama_client.so"
  [libpanorama_client.so]="$S/game/bin/androidarm64/release/libpanorama_client.so"
  [libengine_client.so]="$S/game/bin/androidarm64/release/libengine_client.so"
  [libdatacache_client.so]="$S/game/bin/androidarm64/release/libdatacache_client.so"
  [liblauncher_client.so]="$S/game/bin/androidarm64/release/liblauncher_client.so"
  [libmaterialsystem_client.so]="$S/game/bin/androidarm64/release/libmaterialsystem_client.so"
  [libshaderapidx9_client.so]="$S/game/bin/androidarm64/release/libshaderapidx9_client.so"
  [libvguimatsurface_client.so]="$S/game/bin/androidarm64/release/libvguimatsurface_client.so"
  [libvstdlib_client.so]="$S/game/bin/androidarm64/release/libvstdlib_client.so"
  [libtier0_client.so]="$S/game/bin/androidarm64/release/libtier0_client.so"
  [libmain.so]="$S/runtime/ohos/native-build/libmain.so"
)
while read -r off lib; do
  name=$(basename "$lib")
  path=${LIBMAP[$name]}
  if [ -n "$path" ] && [ -f "$path" ]; then
    sym=$($A2L -Cfipe "$path" "0x$off" 2>/dev/null | head -2)
    echo "  $name +$off : $sym"
  else
    echo "  $name +$off : (no local lib)"
  fi
done < /mnt/e/csgo/night-logs/snap-frames.txt
