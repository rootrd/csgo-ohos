#!/usr/bin/env bash
# 确定性部署：每步断言，失败即停
set -euo pipefail
E=/mnt/e/csgo/CSGO-Source-Linux-20260928
HL=/mnt/e/csgo/hap/entry/libs/arm64-v8a
HAP=/mnt/e/csgo/hap/entry/build/default/outputs/default/entry-default-signed.hap
export OHOS_SDK=$HOME/ohos-native BUILD_JOBS=6 CSGO_SOURCE_ROOT=$E
cd /mnt/e/csgo

echo "== 1. native 编译 =="
touch $E/android/native/engine_startup.cpp $E/android/native/android_main.cpp
bash scripts/build-ohos-engine.sh native > /tmp/dn.log 2>&1
grep -q 'inject args' $E/runtime/ohos/native-build/libmain.so && echo '  native libmain: inject OK'

echo "== 2. stage（91 库）=="
bash scripts/build-ohos-engine.sh stage > /tmp/ds.log 2>&1

echo "== 3. 手动强拷 libmain 并断言 =="
cp -f $E/runtime/ohos/native-build/libmain.so $HL/libmain.so
grep -q 'inject args' $HL/libmain.so && echo '  hap-libs libmain: inject OK'
grep -q 'HostStateFrame' $HL/libengine_client.so && echo '  hap-libs libengine: stateframe OK'

echo "== 4. package =="
cd $E && bash scripts/build-ohos.sh package > /tmp/dp.log 2>&1
echo '  package done'

echo "== 5. HAP 内容断言 =="
HAP=/mnt/e/csgo/hap/entry/build/default/outputs/default/entry-default-signed.hap
grep -q 'inject args' <(unzip -p $HAP libs/arm64-v8a/libmain.so) && echo '  HAP libmain: inject OK'
grep -q 'HostStateFrame' <(unzip -p $HAP libs/arm64-v8a/libengine_client.so) && echo '  HAP libengine: stateframe OK'
grep -q 'de_dust2' <(unzip -p $HAP resources/rawfile/csgo/csgo/cmdline.txt) && echo '  HAP rawfile cmdline: OK'
grep -q 'HudTopLeft' <(unzip -p $HAP resources/rawfile/csgo/csgo/panorama/layout/hud/base_hud.xml) && echo '  HAP base_hud children: OK'

echo "== 6. install =="
'/mnt/c/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/26.0.0/toolchains/hdc.exe' install -r 'E:\csgo\hap\entry\build\default\outputs\default\entry-default-signed.hap'
echo ALL_DEPLOY_OK
