#!/usr/bin/env bash
set -euo pipefail

# CS:GO HarmonyOS 真机联调诊断脚本
# 用法: diagnose-ohos.sh [logs|check|install|run|probe]

hdc="${HDC_PATH:-hdc}"
package="com.csgosource.ohos"
hap_dir="E:/csgo/hap"

# ── 收集日志 ──────────────────────────────────────────────────
collect_logs() {
    echo "[diag] Collecting hilog (CSGOHOS + SDL + DXVK)..."
    "$hdc" shell hilog -T CSGOHOS | head -200
    echo "---"
    "$hdc" shell hilog | grep -iE "CSGOHOS|SDL|DXVK|CSGO|Maleoon|Vulkan|d3d9" | tail -100
}

# ── 检查设备状态 ──────────────────────────────────────────────
check_device() {
    echo "[diag] Device check:"
    "$hdc" list targets
    echo "---"
    "$hdc" shell param get const.product.model 2>/dev/null || echo "(model unknown)"
    "$hdc" shell param get const.product.brand 2>/dev/null || echo "(brand unknown)"
    "$hdc" shell param get const.ohos.apiversion 2>/dev/null || echo "(api unknown)"
    echo "---"
    echo "[diag] Vulkan support:"
    "$hdc" shell "ls /system/lib64/libvulkan.so 2>/dev/null && echo VULKAN_FOUND || echo VULKAN_MISSING"
    echo "---"
    echo "[diag] App installed:"
    "$hdc" shell bm dump -n "$package" 2>/dev/null | head -5 || echo "(not installed)"
}

# ── 安装 HAP ──────────────────────────────────────────────────
install_hap() {
    local hap_file
    hap_file=$(find "$hap_dir/entry/build" -name "*.hap" -type f 2>/dev/null | head -1)
    if [[ -z "$hap_file" ]]; then
        echo "[diag] No HAP found in $hap_dir/entry/build/"
        echo "[diag] Build with DevEco Studio first, then re-run."
        return 1
    fi
    echo "[diag] Installing: $hap_file"
    "$hdc" install "$hap_file"
}

# ── 启动应用 ──────────────────────────────────────────────────
run_app() {
    echo "[diag] Launching $package..."
    "$hdc" shell aa start -a EntryAbility -b "$package"
}

# ── Vulkan 探测（引擎内置）────────────────────────────────────
probe_vulkan() {
    echo "[diag] Vulkan probe (requires engine with --vulkan-probe):"
    "$hdc" shell aa start -a EntryAbility -b "$package" -- --vulkan-probe --probe-seconds 15
    sleep 2
    echo "[diag] Collecting probe logs..."
    "$hdc" shell hilog | grep -iE "VULKAN|PROBE|SURFACE|SDL" | tail -50
}

# ── 崩溃日志收集 ──────────────────────────────────────────────
collect_crash() {
    echo "[diag] Crash logs:"
    "$hdc" shell "ls /data/log/faultlog/temp/ 2>/dev/null" | grep -i csgo || echo "(no crash logs)"
    echo "---"
    "$hdc" shell hilog | grep -iE "FATAL|SIGSEGV|SIGABRT|cppcrash" | tail -30
}

# ── 主入口 ────────────────────────────────────────────────────
action=${1:-check}
case "$action" in
    logs) collect_logs ;;
    check) check_device ;;
    install) install_hap ;;
    run) run_app ;;
    probe) probe_vulkan ;;
    crash) collect_crash ;;
    all) check_device; install_hap; run_app; sleep 5; collect_logs ;;
    *) echo "Usage: diagnose-ohos.sh [logs|check|install|run|probe|crash|all]"; exit 1 ;;
esac