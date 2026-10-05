#!/bin/bash
NM=/root/ohos-native/llvm/bin/llvm-nm
L=/mnt/e/csgo/hap/entry/libs/arm64-v8a
echo "=== pango 全部未定义符号里与 gio/spawn/file 相关的 ==="
$NM -D --undefined-only "$L/libpango-1.0.so.0.5400.0" 2>/dev/null | awk '{print $2}' | sort -u > /tmp/pango-imports.txt
wc -l /tmp/pango-imports.txt
grep -E "^(g_)?(bus|dbus|app_info|subprocess|settings|monitor|file_|io_|content|socket|resolver|proxy|unix|spawn|exec|credentials|mount|volume|vfs)" /tmp/pango-imports.txt | head -40
echo "=== pangoft2 同上 ==="
$NM -D --undefined-only "$L/libpangoft2-1.0.so.0.5400.0" 2>/dev/null | awk '{print $2}' | sort -u > /tmp/pangoft2-imports.txt
wc -l /tmp/pangoft2-imports.txt
grep -E "^(g_)?(bus|dbus|app_info|subprocess|settings|monitor|file_|io_|content|spawn|exec|unix)" /tmp/pangoft2-imports.txt | head -20
echo "=== fontconfig 是否有 spawn/popen/system 相关 ==="
$NM -D --undefined-only "$L/libfontconfig.so.1" 2>/dev/null | awk '{print $2}' | grep -E "spawn|exec|popen|system|fork" | head -10
echo "(空=无)"
echo "=== glib 内部是否导出 spawn 给 gio：gio 未定义 g_spawn 精确列表 ==="
$NM -D --undefined-only "$L/libgio-2.0.so.0.8000.4" 2>/dev/null | awk '{print $2}' | grep spawn | head -8
