#!/bin/bash
cd /mnt/e/csgo || exit 1
export BUILD_JOBS=5
bash scripts/iterate.sh none > /tmp/iterate-run.log 2>&1
echo "iterate-exit=$?" >> /tmp/iterate-run.log
tail -60 /tmp/iterate-run.log
