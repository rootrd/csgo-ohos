#!/bin/bash
# 后台哨兵：轮询启动 CS:GO（等待用户解锁手机），成功后自动点启动按钮
HDC="/c/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/26.0.0/toolchains/hdc.exe"
LOG=/e/csgo/night-logs/sentinel.log
echo "sentinel start $(date +%T)" >> "$LOG"
for i in $(seq 1 240); do
  OUT=$("$HDC" shell "aa start -a EntryAbility -b com.csgosource.ohos" 2>&1 | tr -d '\r')
  if echo "$OUT" | grep -q "successfully"; then
    echo "$(date +%T) STARTED on attempt $i" >> "$LOG"
    sleep 15
    "$HDC" shell "uitest uiInput click 1424 927" >/dev/null 2>&1
    echo "$(date +%T) clicked start button" >> "$LOG"
    exit 0
  fi
  sleep 20
done
echo "$(date +%T) gave up after 240 attempts" >> "$LOG"
