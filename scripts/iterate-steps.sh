#!/usr/bin/env bash
set -x
cd /mnt/e/csgo
BUILD_JOBS=5 bash scripts/build-ohos-engine.sh native 2>&1 | tail -3
BUILD_JOBS=5 bash scripts/build-ohos-engine.sh stage > /dev/null 2>&1
echo "stage=$?"
cd CSGO-Source-Linux-20260928
bash scripts/build-ohos.sh package 2>&1 | grep -E "BUILD|versionCode" | head -3
