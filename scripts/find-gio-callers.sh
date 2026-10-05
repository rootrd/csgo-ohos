#!/bin/bash
NM=/root/ohos-native/llvm/bin/llvm-nm
L=/mnt/e/csgo/hap/entry/libs/arm64-v8a
E=/mnt/e/csgo/CSGO-Source-Linux-20260928/game/bin/androidarm64/release
E2=/mnt/e/csgo/CSGO-Source-Linux-20260928/game/csgo/bin/androidarm64/release

# gio 导出符号全集
$NM -D --defined-only "$L/libgio-2.0.so.0.8000.4" 2>/dev/null | awk '{print $3}' | sort -u > /tmp/gio-exports.txt
wc -l /tmp/gio-exports.txt

echo "=== 各库对 gio 导出符号的未定义引用（真正的 gio 调用方）==="
for f in "$L"/*.so "$L"/*.so.* "$E"/*.so "$E2"/*.so; do
  [ -f "$f" ] || continue
  b=$(basename "$f")
  case "$b" in libgio-2.0.so*|libglib-2.0.so*) continue ;; esac
  $NM -D --undefined-only "$f" 2>/dev/null | awk '{print $2}' | sort -u > /tmp/u.txt
  n=$(comm -12 /tmp/u.txt /tmp/gio-exports.txt | wc -l)
  if [ "${n:-0}" -gt 0 ]; then
    echo "--- $b : $n 个 gio 符号"
    comm -12 /tmp/u.txt /tmp/gio-exports.txt | head -8
  fi
done

echo
echo "=== 各库对 g_spawn* / glib-exec 的引用 ==="
for f in "$L"/*.so "$L"/*.so.* "$E"/*.so "$E2"/*.so; do
  [ -f "$f" ] || continue
  b=$(basename "$f")
  hits=$($NM -D --undefined-only "$f" 2>/dev/null | awk '{print $2}' | grep -E "^g_spawn|^g_fork|^g_child_watch_add" | head -5)
  [ -n "$hits" ] && echo "--- $b" && echo "$hits"
done
