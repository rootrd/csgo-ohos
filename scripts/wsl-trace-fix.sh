#!/usr/bin/env bash
# 以 WSL 副本为准回传打点文件（WSL 上有 trace4/5 的完整打点，E 盘是旧的）
set -e
W=/root/csgo-src/CSGO-Source-Linux-20260928
E=/mnt/e/csgo/CSGO-Source-Linux-20260928
for f in \
  src/materialsystem/cmaterialsystem.cpp \
  src/materialsystem/shaderapidx9/shaderdevicedx8.cpp \
  src/materialsystem/shaderapidx9/winutils.cpp \
  src/appframework/sdlmgr_android.cpp \
  src/appframework/appsystemgroup.cpp \
  src/launcher/launcher.cpp \
  src/engine/sys_dll2.cpp; do
  wc=$(grep -c CSGO_TRACE "$W/$f" 2>/dev/null || echo 0)
  ec=$(grep -c CSGO_TRACE "$E/$f" 2>/dev/null || echo 0)
  if [ "$wc" -gt "$ec" ]; then
    cp -p "$W/$f" "$E/$f"
    echo "E<-W: $f ($wc > $ec)"
  else
    cp -p "$E/$f" "$W/$f"
    echo "W<-E: $f ($ec >= $wc)"
  fi
done
echo RECONCILE_DONE
