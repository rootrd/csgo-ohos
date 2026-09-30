#!/usr/bin/env bash
# 对 HAP 里所有 .so 做 NEEDED 闭合审计：每个 NEEDED 必须在 HAP 内或是系统库
HAP=/mnt/e/csgo/hap/entry/libs/arm64-v8a
RE=/root/ohos-native/llvm/bin/llvm-readelf
missing=0
for f in "$HAP"/*.so "$HAP"/*.so.*; do
    [[ -f "$f" ]] || continue
    $RE -d "$f" 2>/dev/null | grep -oE "\[lib[^]]+\]|\[d3d9\.so\]|\[libc\.so\]|\[libz\.so\]" | tr -d '[]' | while read -r need; do
        if [[ ! -f "$HAP/$need" ]]; then
            case "$need" in
                libc.so|libz.so|libdl.so|libm.so|libpthread.so|librt.so|ld-musl-aarch64.so.1) ;; # 系统库
                *) echo "MISSING: $need  (needed by $(basename "$f"))" ;;
            esac
        fi
    done
done | sort -u
echo "AUDIT_DONE"
