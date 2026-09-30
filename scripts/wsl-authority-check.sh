#!/usr/bin/env bash
# 一次性权威验证：设备包版本 + 关键库的新旧打点计数
HDC="/c/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/26.0.0/toolchains/hdc.exe"
"$HDC" shell "bm dump -n com.csgosource.ohos 2>/dev/null | grep versionCode | head -1"
LIBS=/data/storage/el1/bundle/libs/arm64
echo "launcher_client  Init-system 打点: $("$HDC" shell "strings -a $LIBS/liblauncher_client.so 2>/dev/null | grep -c 'Init system'" | tr -d '\r\n ')"
echo "engine_client    OnStartup 打点:   $("$HDC" shell "strings -a $LIBS/libengine_client.so 2>/dev/null | grep -c 'OnStartup create'" | tr -d '\r\n ')"
echo "materialsystem   CreateD3D 打点:   $("$HDC" shell "strings -a $LIBS/libmaterialsystem_client.so 2>/dev/null | grep -c 'CreateD3DDevice enter'" | tr -d '\r\n ')"
echo "materialsystem   renderctx 打点:   $("$HDC" shell "strings -a $LIBS/libmaterialsystem_client.so 2>/dev/null | grep -c 'renderctx base done'" | tr -d '\r\n ')"
echo "libmain          probe 字符串:     $("$HDC" shell "strings -a $LIBS/libmain.so 2>/dev/null | grep -c 'CSGO native Vulkan diagnostic'" | tr -d '\r\n ')"
