#!/usr/bin/env bash
set -e
E=/mnt/e/csgo/CSGO-Source-Linux-20260928
W=/root/csgo-src/CSGO-Source-Linux-20260928
# E 盘新改的文件先同步
for f in \
  src/materialsystem/shaderapidx9/shaderdevicedx8.cpp \
  src/launcher/launcher.cpp \
  src/engine/sys_dll2.cpp; do
  cp -p "$E/$f" "$W/$f"
done
# 所有 CSGO_TRACE 打点：Warning( -> fprintf(stderr,   （参数形式相同，直接换函数名）
cd "$W/src"
for f in \
  appframework/sdlmgr_android.cpp \
  appframework/appsystemgroup.cpp \
  materialsystem/cmaterialsystem.cpp \
  materialsystem/shaderapidx9/shaderdevicedx8.cpp \
  launcher/launcher.cpp \
  engine/sys_dll2.cpp; do
  sed -i 's/Warning( "CSGO_TRACE: /fprintf( stderr, "CSGO_TRACE: /g' "$f"
  echo "$f: $(grep -c 'fprintf( stderr, "CSGO_TRACE' $f) traces"
done
echo TRACE_STDERR_DONE
