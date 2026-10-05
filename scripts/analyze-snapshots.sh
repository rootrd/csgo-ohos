#!/bin/bash
# 拉取 crash_bt.txt 的 MAIN SNAPSHOT 并符号化主线程栈帧
# 用法: bash scripts/analyze-snapshots.sh [snapshot_count]
HDC="/c/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/26.0.0/toolchains/hdc.exe"
cd /e/csgo
"$HDC" shell "cat /data/app/el2/100/base/com.csgosource.ohos/haps/entry/files/csgo/logs/crash_bt.txt" > night-logs/crash_bt-live.txt 2>&1
# 取最后一个 MAIN SNAPSHOT 段
SNAP=$(awk '/=== MAIN SNAPSHOT/{buf=""; inblk=1} inblk{buf=buf $0 "\n"} END{printf "%s", buf}' night-logs/crash_bt-live.txt)
[ -z "$SNAP" ] && { echo "no MAIN SNAPSHOT found"; exit 1; }
echo "$SNAP"
echo "=== symbolizing (module+offset lines) ==="
# 提取 lib 偏移，生成符号化脚本
echo "$SNAP" | grep -oE 'pc [0-9a-f]+ /data/storage/[^ ]+\.so' | awk '{print $2, $3}' > night-logs/snap-frames.txt
wsl -d Ubuntu-24.04 -- bash /mnt/e/csgo/scripts/sym-frames.sh
