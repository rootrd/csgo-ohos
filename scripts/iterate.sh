#!/usr/bin/env bash
# 一键迭代（WSL 内）：增量编译 → stage → 打包(Windows hvigor 互操作) → 装机 → 启动 → 抓日志
# 用法（WSL）: BUILD_JOBS=5 bash /mnt/e/csgo/scripts/iterate.sh [engine|native|all|none]
#   第 2 参控制重编范围: all=引擎+native(默认) engine=仅引擎 native=仅libmain none=不编译只重装
# 注意：hvigor 打包与 hdc 是 Windows 工具，WSL 互操作直调 Git Bash/hdc.exe
set -euo pipefail
cd /mnt/e/csgo
BUILD_JOBS=${BUILD_JOBS:-5}
WHAT=${1:-all}
HDC="/mnt/c/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/26.0.0/toolchains/hdc.exe"
WIN_BASH=/mnt/c/Program\ Files/Git/bin/bash.exe
LOGS=/data/app/el2/100/base/com.csgosource.ohos/haps/entry/files/csgo/logs
T0=$(date +%s)

winbash() { "$WIN_BASH" -c "$1"; }

case "$WHAT" in
  engine|all)
    # 失败必须中止（tee 留全量日志），否则旧库静默上机浪费一轮真机验证
    bash scripts/build-ohos-engine.sh engine 2>&1 | tee /tmp/engine-build.log | grep -E " error:|Error [0-9]|COPYING" | tail -5
    ;;
esac
case "$WHAT" in
  native|all)
    bash scripts/build-ohos-engine.sh native 2>&1 | tail -2
    ;;
esac
# stage 必须无条件执行（含 DXVK d3d9.so 回传拷贝）——none 模式跳过 stage 会让
# hap libs 里残留旧库（曾导致 DXVK 修复两轮未上机）
bash scripts/build-ohos-engine.sh stage > /dev/null 2>&1

# 打包（Windows hvigorw，含 versionCode bump + 资源装配 + 签名）
winbash "cd /e/csgo/CSGO-Source-Linux-20260928 && bash scripts/build-ohos.sh package" 2>&1 | grep -E "BUILD" | head -1

HAP_LINUX=/mnt/e/csgo/hap/entry/build/default/outputs/default/entry-default-signed.hap
[[ -f "$HAP_LINUX" ]] || { echo "[iterate] HAP 未生成，打包失败"; exit 1; }

"$HDC" install -r "E:\\csgo\\hap\\entry\\build\\default\\outputs\\default\\entry-default-signed.hap" 2>&1 | tail -1

# 装机后抽查库是否真的更新（防 install -r 静默不更新）
CHECK=$("$HDC" shell "grep -ac 'ResourceRoot=' /data/storage/el1/bundle/libs/arm64/libmain.so" 2>/dev/null | tr -d '\r\n ' || true)
echo "[iterate] device libmain 新打点标记: ${CHECK:-?} (1=新库)"

"$HDC" shell hilog -r > /dev/null 2>&1
"$HDC" shell "aa force-stop com.csgosource.ohos" > /dev/null 2>&1 || true
sleep 1
"$HDC" shell "power-shell wakeup" > /dev/null 2>&1 || true
"$HDC" shell "aa start -a EntryAbility -b com.csgosource.ohos" 2>&1 | head -1
# 等 seed 解包完成后自动点击「启动游戏」按钮（2848x1276 横屏，按钮中心 1424,927）
sleep 15
"$HDC" shell "uitest uiInput click 1424 927" > /dev/null 2>&1 || true
echo "[iterate] launched + auto-click, waiting 60s..."
sleep 60

echo "=========== 引擎日志摘要 ==========="
"$HDC" shell "hilog -x | grep 'A0C001' | tail -25" 2>&1
echo "=========== CSGO_TRACE ==========="
"$HDC" shell "grep -a CSGO_TRACE $LOGS/stdio.log 2>/dev/null | tail -20" 2>&1
echo "=========== DXVK tail ==========="
"$HDC" shell "tail -8 $LOGS/appspawn_d3d9.log 2>/dev/null" 2>&1
PID=$("$HDC" shell "pidof com.csgosource.ohos" 2>/dev/null | tr -d '\r\n ')
echo "=========== 进程: [${PID:-已退出}] ==========="
echo "[iterate] 总耗时 $(( $(date +%s) - T0 ))s"
