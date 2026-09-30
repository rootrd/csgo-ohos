#!/usr/bin/env bash
LIBS=/data/storage/el1/bundle/libs/arm64
echo "versionCode: $(bm dump -n com.csgosource.ohos 2>/dev/null | grep versionCode | head -1 | grep -o '[0-9]*')"
echo "launcher Init-system: $(strings -a $LIBS/liblauncher_client.so 2>/dev/null | grep -c 'Init system')"
echo "engine OnStartup:     $(strings -a $LIBS/libengine_client.so 2>/dev/null | grep -c 'OnStartup create')"
echo "materialsystem CreateD3D: $(strings -a $LIBS/libmaterialsystem_client.so 2>/dev/null | grep -c 'CreateD3DDevice enter')"
echo "materialsystem renderctx: $(strings -a $LIBS/libmaterialsystem_client.so 2>/dev/null | grep -c 'renderctx base done')"
echo "libmain probe:        $(strings -a $LIBS/libmain.so 2>/dev/null | grep -c 'CSGO native Vulkan diagnostic')"
echo "SDL3 touch-guard:     $(strings -a $LIBS/libSDL3.so 2>/dev/null | grep -c 'not created yet')"
