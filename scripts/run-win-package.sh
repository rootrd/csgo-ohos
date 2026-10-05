#!/bin/bash
# Windows-side: package + install + launch + auto-click + wait
set -e
cd /e/csgo/CSGO-Source-Linux-20260928
bash scripts/build-ohos.sh package 2>&1 | grep -E "BUILD|versionCode|HAP:" | head -5
[ -f "/e/csgo/hap/entry/build/default/outputs/default/entry-default-signed.hap" ] || { echo "HAP missing"; exit 1; }
HDC="/c/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/26.0.0/toolchains/hdc.exe"
"$HDC" install -r "E:\\csgo\\hap\\entry\\build\\default\\outputs\\default\\entry-default-signed.hap" 2>&1 | tail -1
"$HDC" shell "aa force-stop com.csgosource.ohos" >/dev/null 2>&1 || true
sleep 1
"$HDC" shell "power-shell wakeup" >/dev/null 2>&1 || true
"$HDC" shell "aa start -a EntryAbility -b com.csgosource.ohos" 2>&1 | head -1
sleep 15
"$HDC" shell "uitest uiInput click 1424 927" >/dev/null 2>&1 || true
echo "launched + clicked, waiting 75s..."
sleep 75
echo "pid: [$("$HDC" shell "pidof com.csgosource.ohos" 2>/dev/null | tr -d '\r\n ')]"
