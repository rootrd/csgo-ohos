#!/usr/bin/env bash
set -euo pipefail

# DXVK OHOS 重编（WSL 内）：修复 EnumDisplayDevicesA stub 后的 d3d9.so/libdxvk_dxgi.so.0
# 源码同步自 E:\csgo\deps\dxvk-ohos-legacy；工具链 ~/ohos-native（Linux 版 clang）

E=/mnt/e/csgo/deps/dxvk-ohos-legacy
W=$HOME/dxvk-ohos-legacy
SDK=$HOME/ohos-native
jobs=${BUILD_JOBS:-5}

# 1) 同步源码（排除 Windows build 目录）
mkdir -p "$HOME"
rsync -a --delete --exclude 'build.ohos' --exclude '.git' "$E/" "$W/"

# 2) Linux cross 文件（等价原 build-ohos-win.cross，路径换 WSL）
cat > "$W/build-ohos-linux.cross" <<EOF
[binaries]
c = '$SDK/llvm/bin/clang'
cpp = '$SDK/llvm/bin/clang++'
ar = '$SDK/llvm/bin/llvm-ar'
strip = '$SDK/llvm/bin/llvm-strip'
pkg-config = 'pkg-config'
glslangValidator = '/mnt/e/csgo/deps/glslang/bin/glslangValidator.exe'

[built-in options]
c_args = ['--target=aarch64-linux-ohos', '--sysroot=$SDK/sysroot', '-D__OHOS__=1', '-fPIC']
cpp_args = ['--target=aarch64-linux-ohos', '--sysroot=$SDK/sysroot', '-D__OHOS__=1', '-fPIC']
c_link_args = ['--target=aarch64-linux-ohos', '--sysroot=$SDK/sysroot', '-fuse-ld=lld', '-Wl,-z,max-page-size=16384', '-Wl,-z,common-page-size=16384']
cpp_link_args = ['--target=aarch64-linux-ohos', '--sysroot=$SDK/sysroot', '-fuse-ld=lld', '-Wl,-z,max-page-size=16384', '-Wl,-z,common-page-size=16384']

[properties]
needs_exe_wrapper = true

[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
EOF

# 3) meson setup（对齐原配置：d3d9/dxgi/d3d11 on，d3d10 off，native_ohos on）
if [[ ! -f "$W/build-linux/build.ninja" ]]; then
    rm -rf "$W/build-linux"
    meson setup "$W/build-linux" "$W" --cross-file "$W/build-ohos-linux.cross" \
        --buildtype debugoptimized --default-library shared \
        -Denable_tests=false -Denable_dxgi=true -Denable_d3d9=true \
        -Denable_d3d10=false -Denable_d3d11=true -Dbuild_id=false \
        -Dnative_ohos=true
fi

# 4) 编译（全量：d3d9/dxgi/d3d11 共享 dxvk 核心）
ninja -C "$W/build-linux" -j"$jobs"

# 5) 产物拷回：E 盘 deps 记录位 + E/WSL 两份 prefix（stage 从 WSL prefix 取库，
#    漏了 WSL 侧会被 stage 的反向 rsync 用旧库覆盖 E 盘新库）
mkdir -p /mnt/e/csgo/CSGO-Source-Linux-20260928/runtime/ohos/install/lib
WSL_PREFIX=$HOME/csgo-src/CSGO-Source-Linux-20260928/runtime/ohos/install/lib
cp -p "$W/build-linux/src/d3d9/d3d9.so" /mnt/e/csgo/CSGO-Source-Linux-20260928/runtime/ohos/install/lib/libdxvk_d3d9.so
cp -L -p "$W/build-linux/src/dxgi/libdxvk_dxgi.so.0" /mnt/e/csgo/CSGO-Source-Linux-20260928/runtime/ohos/install/lib/libdxvk_dxgi.so.0
cp -p "$W/build-linux/src/d3d9/d3d9.so" "$WSL_PREFIX/libdxvk_d3d9.so"
cp -L -p "$W/build-linux/src/dxgi/libdxvk_dxgi.so.0" "$WSL_PREFIX/libdxvk_dxgi.so.0"
cp -p "$W/build-linux/src/d3d9/d3d9.so" /mnt/e/csgo/deps/dxvk-ohos-legacy/build.ohos/src/d3d9/d3d9.so
cp -L -p "$W/build-linux/src/dxgi/libdxvk_dxgi.so.0" /mnt/e/csgo/deps/dxvk-ohos-legacy/build.ohos/src/dxgi/libdxvk_dxgi.so.0 2>/dev/null || true
echo "DXVK_REBUILD_DONE"
ls -la "$W/build-linux/src/d3d9/d3d9.so" "$WSL_PREFIX/libdxvk_d3d9.so"
