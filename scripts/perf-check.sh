#!/usr/bin/env bash
# 单轮"干净前台"性能采样：force-stop → 唤醒 → start → 点击启动 → 等待 → 采样
# 用法: bash scripts/perf-check.sh [等待秒数，默认 90]
# 前提：手机空闲、亮屏、不切应用（否则得到的是后台节流伪影，非本脚本之罪）
set -u
HDC="/c/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/26.0.0/toolchains/hdc.exe"
L=/data/app/el2/100/base/com.csgosource.ohos/haps/entry/files/csgo/logs
WAIT=${1:-90}
TAG=$(date +%H%M%S)

"$HDC" shell "aa force-stop com.csgosource.ohos" >/dev/null 2>&1
sleep 2
"$HDC" shell "power-shell wakeup" >/dev/null 2>&1 || true
"$HDC" shell "aa start -a EntryAbility -b com.csgosource.ohos" 2>&1 | head -1
sleep 16
"$HDC" shell "uitest uiInput click 1424 927" >/dev/null 2>&1 || true
echo "[perf-check] launched+clicked, waiting ${WAIT}s ..."
sleep "$WAIT"

PID=$("$HDC" shell "pidof com.csgosource.ohos" 2>/dev/null | tr -d '\r\n ')
echo "[perf-check] pid=${PID:-已退出}"
echo "[perf-check] inject: $("$HDC" shell "grep -a 'inject args' $L/stdio.log | head -1" 2>/dev/null | tr -d '\r')"
echo "[perf-check] state:  $("$HDC" shell "grep -a 'ChangeGameUIState' $L/stdio.log | tail -1" 2>/dev/null | tr -d '\r')"

"$HDC" shell "grep -a HostStateFrame $L/stdio.log | tail -400" 2>/dev/null | tr -d '\r' | sed 's/.*t=//' > "/tmp/pc-frames-$TAG.txt"
awk 'NR>1{d=$1-p; if(d>0){s+=d;n++; if(d>mx)mx=d; if(mn==""||d<mn)mn=d}} {p=$1} END{if(n>0) printf "[perf-check] 帧率 n=%d 平均 %.2fms → %.1f fps | min %.1f max %.1f ms\n", n, s/n*1000, n/s, mn*1000, mx*1000}' "/tmp/pc-frames-$TAG.txt"

if [ -n "${PID:-}" ]; then
  "$HDC" shell "hidumper --mem $PID 2>/dev/null | grep -a GL | head -2" 2>&1 | tr -d '\r'
  "$HDC" shell "top -b -n 1 2>/dev/null | grep -a $PID" 2>&1 | tr -d '\r'
fi

"$HDC" shell "snapshot_display -f /data/local/tmp/pc-$TAG.jpeg" >/dev/null 2>&1
MSYS_NO_PATHCONV=1 "$HDC" file recv "/data/local/tmp/pc-$TAG.jpeg" "E:\\csgo\\night-logs\\pc-$TAG.jpeg" >/dev/null 2>&1 && echo "[perf-check] 截图 night-logs/pc-$TAG.jpeg"
echo "[perf-check] 完成 tag=$TAG"
