#!/bin/bash
# 谁在引用进程派生/外部命令相关符号（跨全部装机库 + 引擎库）
cd /e/csgo/hap/entry/libs/arm64-v8a 2>/dev/null || exit 1
echo "=== HAP libs: spawn/system/popen/exec 引用 ==="
for f in *.so *.so.*; do
  [ -f "$f" ] || continue
  h=$(grep -acE "g_spawn_async|g_spawn_sync|g_spawn_command|posix_spawn|popen|system@GLIBC|execv" "$f" 2>/dev/null)
  if [ "${h:-0}" != "0" ]; then echo "$f : $h"; fi
done
echo
echo "=== HAP libs: 谁需要 libgio/gdk-pixbuf（DT_NEEDED 粗查）==="
for f in *.so *.so.*; do
  [ -f "$f" ] || continue
  h=$(grep -ac "libgio-2.0\|libgdk_pixbuf" "$f" 2>/dev/null)
  if [ "${h:-0}" != "0" ]; then echo "$f : $h"; fi
done
echo
echo "=== 引擎库 ==="
cd /e/csgo/CSGO-Source-Linux-20260928/game/bin/androidarm64/release 2>/dev/null
for f in *.so; do
  [ -f "$f" ] || continue
  h=$(grep -acE "g_spawn_async|g_spawn_sync|posix_spawn|popen|execv" "$f" 2>/dev/null)
  if [ "${h:-0}" != "0" ]; then echo "$f : $h"; fi
done