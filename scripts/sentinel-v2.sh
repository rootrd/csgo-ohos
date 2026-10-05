#!/bin/bash
# 哨兵 v2：无限轮询启动 → 点按钮 → 等卡点 → 持续收主线程栈快照
# 退出条件：游戏画面出现内容（截图非纯黑）或超过 24h
HDC="/c/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/26.0.0/toolchains/hdc.exe"
LOG=/e/csgo/night-logs/sentinel.log
START=$(date +%s)
echo "=== sentinel v2 start $(date +%T) ===" >> "$LOG"
while true; do
  NOW=$(date +%s)
  if [ $((NOW - START)) -gt 86400 ]; then echo "$(date +%T) 24h timeout" >> "$LOG"; exit 1; fi
  PID=$("$HDC" shell "pidof com.csgosource.ohos" 2>/dev/null | tr -d '\r\n ')
  if [ -z "$PID" ]; then
    "$HDC" shell "power-shell wakeup" >/dev/null 2>&1
    OUT=$("$HDC" shell "aa start -a EntryAbility -b com.csgosource.ohos" 2>&1 | tr -d '\r')
    if echo "$OUT" | grep -q "successfully"; then
      echo "$(date +%T) started pid soon" >> "$LOG"
      sleep 15
      "$HDC" shell "uitest uiInput click 1424 927" >/dev/null 2>&1
      echo "$(date +%T) clicked" >> "$LOG"
    fi
  else
    # 应用在跑：等 4 分钟到卡点，然后收快照
    sleep 240
    "$HDC" shell "cat /data/app/el2/100/base/com.csgosource.ohos/haps/entry/files/csgo/logs/crash_bt.txt" \
      > /e/csgo/night-logs/crash_bt-live.txt 2>&1
    N=$(grep -c "MAIN SNAPSHOT" /e/csgo/night-logs/crash_bt-live.txt 2>/dev/null)
    echo "$(date +%T) pid=$PID snapshots=$N" >> "$LOG"
    if [ "${N:-0}" -ge 4 ]; then
      # 快照够了，再等 2 分钟取稳定段后停止（保留现场，不杀进程）
      echo "$(date +%T) snapshots collected enough, sentinel done" >> "$LOG"
      exit 0
    fi
  fi
  sleep 20
done
