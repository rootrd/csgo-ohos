#!/usr/bin/env bash
# Sync patched files + runtime prefix from E: canonical tree into the WSL build copy.
set -ex
E=/mnt/e/csgo/CSGO-Source-Linux-20260928
W=$HOME/csgo-src/CSGO-Source-Linux-20260928

# patched sources
cp -p "$E/src/public/tier0/platform.h" "$W/src/public/tier0/platform.h"
cp -p "$E/src/materialsystem/shaderapivulkan/CMakeLists.txt" "$W/src/materialsystem/shaderapivulkan/CMakeLists.txt"
cp -p "$E/src/devtools/makefile_base_posix.mak" "$W/src/devtools/makefile_base_posix.mak"
cp -p "$E/src/tier0/threadtools.cpp" "$W/src/tier0/threadtools.cpp"
cp -p "$E/src/panorama/controls/debug/debuglayout.cpp" "$W/src/panorama/controls/debug/debuglayout.cpp"
cp -p "$E/src/vgui2/vgui_surfacelib/linuxfont.cpp" "$W/src/vgui2/vgui_surfacelib/linuxfont.cpp"
cp -p "$E/src/vguimatsurface/vguimatsurface.vpc" "$W/src/vguimatsurface/vguimatsurface.vpc"
cp -p "$E/src/external/crypto++-5.61/misc.h" "$W/src/external/crypto++-5.61/misc.h"
cp -p "$E/src/vscript/languages/squirrel/sqdbg/sqrdbg.cpp" "$W/src/vscript/languages/squirrel/sqdbg/sqrdbg.cpp"
cp -p "$E/src/vscript/languages/squirrel/sqdbg/sqdbgserver.cpp" "$W/src/vscript/languages/squirrel/sqdbg/sqdbgserver.cpp"
cp -p "$E/src/engine/cl_demo.cpp" "$W/src/engine/cl_demo.cpp"
cp -p "$E/android/native/ohos_compat.h" "$W/android/native/ohos_compat.h"
cp -p "$E/android/native/android_main.cpp" "$W/android/native/android_main.cpp"
cp -p "$E/android/native/engine_startup.cpp" "$W/android/native/engine_startup.cpp"
cp -p "$E/android/CMakeLists.txt" "$W/android/CMakeLists.txt"
cp -p "$E/src/materialsystem/shaderapidx9/winutils.cpp" "$W/src/materialsystem/shaderapidx9/winutils.cpp"
cp -p "$E/src/appframework/sdlmgr_android.cpp" "$W/src/appframework/sdlmgr_android.cpp"
cp -p "$E/android/native/engine_startup.cpp" "$W/android/native/engine_startup.cpp"
cp -p "$E/src/materialsystem/cmaterialsystem.cpp" "$W/src/materialsystem/cmaterialsystem.cpp"
cp -p "$E/src/appframework/appsystemgroup.cpp" "$W/src/appframework/appsystemgroup.cpp"

# patched dxvk headers (canonical + install copy)
cp -p /mnt/e/csgo/deps/dxvk-ohos-legacy/include/native/windows/windows_base.h "$E/runtime/ohos/install/include/dxvk/windows_base.h"
cp -p /mnt/e/csgo/deps/dxvk-ohos-legacy/include/native/windows/windows_base.h "$W/runtime/ohos/install/include/dxvk/windows_base.h"
# directx headers + .inl files (full dir contents)
rsync -a "/mnt/e/csgo/deps/dxvk-ohos-legacy/include/native/directx/" "$E/runtime/ohos/install/include/dxvk/" --exclude=wsi --exclude=ohos 2>/dev/null || true
rsync -a "/mnt/e/csgo/deps/dxvk-ohos-legacy/include/native/windows/" "$E/runtime/ohos/install/include/dxvk/" 2>/dev/null || true
rsync -a --delete "$E/runtime/ohos/install/include/dxvk/" "$W/runtime/ohos/install/include/dxvk/"

# runtime prefix：只在首次引导时同步 E:→WSL（此前 --delete 曾把 WSL 侧已安装的
# deps/text-stack 产物抹掉，教训：构建产物只允许 WSL→E: 方向回传，见 build-ohos-engine.sh stage）
if [[ ! -d "$W/runtime/ohos/install/lib" ]]; then
    rsync -a "$E/runtime/ohos/" "$W/runtime/ohos/"
    rm -rf "$W/runtime/android"
    ln -sfn ohos "$W/runtime/android"
    echo "runtime prefix bootstrapped"
fi
ls -la "$W/runtime/"
ls "$W/runtime/android/install/lib/"
echo SYNC_DONE
