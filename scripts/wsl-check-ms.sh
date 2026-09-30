#!/usr/bin/env bash
cd /root/csgo-src/CSGO-Source-Linux-20260928
echo "=== game/bin materialsystem ==="
ls -la game/bin/androidarm64/release/libmaterialsystem_client.so
strings -a game/bin/androidarm64/release/libmaterialsystem_client.so | grep -c "CreateD3DDevice enter"
echo "=== obj materialsystem ==="
ls -la src/materialsystem/obj_materialsystem_androidarm64_client/release/cmaterialsystem.o 2>/dev/null
ls -la src/materialsystem/obj_materialsystem_androidarm64_client/release/libmaterialsystem_client.so 2>/dev/null
echo "=== source mtime ==="
ls -la src/materialsystem/cmaterialsystem.cpp
echo "=== find all libmaterialsystem copies ==="
find . -name "libmaterialsystem_client.so" -exec ls -la {} \; 2>/dev/null | head -6
