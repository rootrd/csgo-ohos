#!/usr/bin/env bash
echo "=== codeload test ==="
curl -sS -o /tmp/nettest.tgz -w "%{http_code} %{size_download}B %{time_total}s\n" --max-time 30 -L https://codeload.github.com/Mbed-TLS/mbedtls/tar.gz/068ff080b369adfac81509f9b57b2afabaf82dc5 || echo CURL_FAIL
ls -la /tmp/nettest.tgz 2>/dev/null
echo "=== git test ==="
timeout 30 git clone --depth 1 https://github.com/DLTcollab/sse2neon.git /tmp/nettest-git 2>&1 | tail -1 && echo GIT_OK || echo GIT_FAIL
