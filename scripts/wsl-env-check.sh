#!/usr/bin/env bash
echo "=== CPU/mem ==="
nproc; free -g | head -2
echo "=== disk (WSL fs) ==="
df -h / ~ | tail -2
echo "=== git ==="
git --version
echo "=== network test ==="
timeout 15 git ls-remote https://github.com/DLTcollab/sse2neon.git HEAD 2>&1 | head -2
echo "=== host protoc ==="
ls -la /mnt/e/csgo/CSGO-Source-Linux-20260928/src/devtools/bin/linux/ 2>/dev/null | head
echo "=== sse2neon on disk? ==="
ls -d /mnt/e/csgo/deps/sse2neon /mnt/e/csgo/CSGO-Source-Linux-20260928/runtime/*/deps/sse2neon 2>/dev/null
echo "=== thirdparty ==="
ls /mnt/e/csgo/CSGO-Source-Linux-20260928/src/thirdparty/ 2>/dev/null | head -40
