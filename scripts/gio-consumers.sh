#!/bin/bash
NM=/root/ohos-native/llvm/bin/llvm-nm
L=/mnt/e/csgo/hap/entry/libs/arm64-v8a
echo "=== pango 引用的 gio/glib 符号（可能的 spawn 触发链）==="
$NM -D --undefined-only "$L/libpango-1.0.so.0.5400.0" 2>/dev/null | grep -iE "g_bus|g_dbus|g_app_info|g_file_monitor|g_settings|g_spawn|g_subprocess|g_unix" | head -10
echo "=== pangoft2 ==="
$NM -D --undefined-only "$L/libpangoft2-1.0.so.0.5400.0" 2>/dev/null | grep -iE "g_bus|g_dbus|g_app_info|g_file_monitor|g_settings|g_spawn|g_subprocess|g_unix" | head -10
echo "=== gio 里调用 g_spawn 的引用（谁在 gio 内部用 spawn）==="
$NM -D --undefined-only "$L/libgio-2.0.so.0.8000.4" 2>/dev/null | grep -iE "spawn" | head -6
echo "=== 我们的 text client 引用 ==="
$NM -D --undefined-only "$L/libpanorama_text_pango_client.so" 2>/dev/null | grep -iE "g_bus|g_app_info|g_spawn|g_subprocess|g_unix|popen|exec" | head -10
echo "=== tier0 的 spawn/popen 引用精确内容 ==="
$NM -D --undefined-only /mnt/e/csgo/CSGO-Source-Linux-20260928/game/bin/androidarm64/release/libtier0_client.so 2>/dev/null | grep -iE "popen|exec|spawn|system" | head -6
