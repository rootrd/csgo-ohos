#!/usr/bin/env bash
set -x
SDK=/mnt/e/ohos-cli/command-line-tools/sdk/default/openharmony/native
echo "=== clang version ==="
"$SDK/llvm/bin/clang" --version | head -3
echo "=== builtins available ==="
ls "$SDK"/llvm/lib/clang/*/lib/aarch64-linux-ohos/ 2>/dev/null | head -5
echo "=== CRT files in sysroot ==="
ls "$SDK/sysroot/usr/lib/aarch64-linux-ohos/" | grep -E "Scrt1|crti|crtn|^libc\.so" | head
echo "=== hello world C ==="
cat > /tmp/hello.c <<'EOF'
#include <stdio.h>
int main() { printf("hello ohos\n"); return 0; }
EOF
"$SDK/llvm/bin/clang" --target=aarch64-linux-ohos --sysroot="$SDK/sysroot" -fuse-ld=lld -o /tmp/hello /tmp/hello.c && file /tmp/hello && echo LINK_OK
echo "=== __OHOS__ builtin check ==="
echo | "$SDK/llvm/bin/clang" --target=aarch64-linux-ohos --sysroot="$SDK/sysroot" -dM -E - | grep -i "ohos\|android"
echo "=== C++ test ==="
cat > /tmp/hello.cpp <<'EOF'
#include <string>
#include <cstdio>
int main() { std::string s = "hello ohos cpp"; printf("%s\n", s.c_str()); return 0; }
EOF
"$SDK/llvm/bin/clang++" --target=aarch64-linux-ohos --sysroot="$SDK/sysroot" -fuse-ld=lld -std=gnu++11 -stdlib=libc++ -o /tmp/hellocpp /tmp/hello.cpp && file /tmp/hellocpp && echo CPP_LINK_OK
