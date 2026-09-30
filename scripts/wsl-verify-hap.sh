#!/usr/bin/env bash
HAP=/mnt/e/csgo/hap/entry/libs/arm64-v8a
RE=/root/ohos-native/llvm/bin/llvm-readelf
echo "=== libmain.so ==="
$RE -h "$HAP/libmain.so" | grep -E "Machine|Type:" | head -2
$RE --dyn-syms "$HAP/libmain.so" | grep -E "SDL_main" | head -2
echo "=== 16KB alignment (libmain, client_panorama, engine) ==="
for lib in libmain.so libclient_panorama_client.so libengine_client.so; do
    segs=$($RE -lW "$HAP/$lib" | grep LOAD | awk '{print $NF}' | sort -u | tr '\n' ' ')
    echo "$lib: $segs"
done
echo "=== client_panorama NEEDED closure ==="
$RE -d "$HAP/libclient_panorama_client.so" | grep NEEDED | sed 's/.*\[\(.*\)\]/\1/' | sort > /tmp/needed.txt
cat /tmp/needed.txt | tr '\n' ' '; echo
missing=0
while read -r lib; do
    if [[ -f "$HAP/$lib" ]]; then continue; fi
    case "$lib" in
        libz.so|libc.so|libdl.so|libm.so|libpthread.so|librt.so|ld-musl-aarch64.so.1) ;;
        *) echo "MISSING: $lib"; missing=1 ;;
    esac
done < /tmp/needed.txt
[[ $missing == 0 ]] && echo "NEEDED_CLOSURE_OK (system libs excluded)"
echo "=== v8 stub check ==="
$RE -h "$HAP/libv8.cr.so" | grep Machine
$RE --dyn-syms "$HAP/libv8.cr.so" | grep -c GLOBAL
echo "=== all libs arch sweep (non-aarch64?) ==="
bad=0
for f in "$HAP"/*.so*; do
    m=$($RE -h "$f" 2>/dev/null | grep -m1 Machine | grep -c AArch64 || true)
    [[ $m == 0 ]] && { echo "NOT AARCH64: $(basename "$f")"; bad=1; }
done
[[ $bad == 0 ]] && echo "ALL_AARCH64_OK"
