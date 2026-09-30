#!/usr/bin/env bash
# 等待设备解锁（轮询 aa start 是否成功），成功后自动启动应用并抓取日志
HDC="/c/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/26.0.0/toolchains/hdc.exe"
for i in $(seq 1 120); do
  "$HDC" shell "power-shell wakeup" > /dev/null 2>&1
  R=$("$HDC" shell "aa start -a EntryAbility -b com.csgosource.ohos" 2>&1 | tr -d '\r\n ')
  if [[ "$R" == *"start ability successfully"* ]]; then
    echo "[$i] LAUNCHED at $(date +%H:%M:%S)"
    break
  fi
  sleep 5
done
# 抓 90 秒日志
sleep 90
"$HDC" shell "grep -a CSGO_TRACE /data/app/el2/100/base/com.csgosource.ohos/haps/entry/files/csgo/logs/stdio.log 2>/dev/null | tail -12" 2>&1
echo "=== hilog ==="
"$HDC" shell "hilog -x | grep 'A0C001' | tail -12" 2>&1
PID=$("$HDC" shell "pidof com.csgosource.ohos" 2>/dev/null | tr -d '\r\n ')
echo "=== pid=[$PID] ==="
