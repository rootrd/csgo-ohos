#!/usr/bin/env bash
cd /root/csgo-src/CSGO-Source-Linux-20260928
echo "=== strings check (CSGO_TRACE in built libs) ==="
for so in game/bin/androidarm64/release/libmaterialsystem_client.so \
          game/bin/androidarm64/release/liblauncher_client.so \
          game/bin/androidarm64/release/libengine_client.so; do
    n=$(strings -a "$so" 2>/dev/null | grep -c "CSGO_TRACE")
    echo "$n  $so"
done
echo "=== mtimes ==="
ls -la game/bin/androidarm64/release/libmaterialsystem_client.so game/bin/androidarm64/release/liblauncher_client.so
echo "=== obj cmaterialsystem mtime ==="
ls -la src/materialsystem/obj_materialsystem_androidarm64_client/release/cmaterialsystem.o 2>/dev/null | head -2
echo "=== HAP copy check ==="
strings -a /mnt/e/csgo/hap/entry/libs/arm64-v8a/libmaterialsystem_client.so 2>/dev/null | grep -c CSGO_TRACE
