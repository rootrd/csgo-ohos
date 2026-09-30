#!/usr/bin/env bash
echo "=== user/tools ==="
whoami; which pkg-config pkg-config meson ninja python3 2>/dev/null; python3 --version
echo "=== libc++ linkage of hellocpp ==="
~/ohos-native/llvm/bin/llvm-readelf -d /tmp/hellocpp 2>/dev/null | grep -E "NEEDED|INTERP"
echo "=== phonon in tree ==="
grep -rln "phonon" /mnt/e/csgo/CSGO-Source-Linux-20260928/src/public/ 2>/dev/null | head -5
find /mnt/e/csgo/CSGO-Source-Linux-20260928/src -name "phonon*.h" 2>/dev/null | head -3
echo "=== which vpc modules link phonon ==="
grep -rn "phonon" /mnt/e/csgo/CSGO-Source-Linux-20260928/src --include="*.vpc" | head -5
echo "=== default sysroot check (no explicit --sysroot) ==="
echo 'int main(){return 0;}' > /tmp/t.c
~/ohos-native/llvm/bin/clang --target=aarch64-linux-ohos -fuse-ld=lld -o /tmp/t /tmp/t.c && echo DEFAULT_SYSROOT_OK || echo DEFAULT_SYSROOT_FAIL
echo "=== expat anywhere? ==="
grep -rn "expat" /mnt/e/csgo/CSGO-Source-Linux-20260928/src/game/shared/*.vpc 2>/dev/null | head -3
find /mnt/e/csgo/CSGO-Source-Linux-20260928/src/thirdparty -maxdepth 1 -iname "*expat*" | head -2
