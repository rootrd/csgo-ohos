#!/bin/bash
cd /mnt/e/csgo || exit 1
export BUILD_JOBS=5
bash /mnt/e/csgo/scripts/wsl-sync.sh > /mnt/e/csgo/night-logs/loop.log 2>&1
bash scripts/build-ohos-engine.sh engine >> /mnt/e/csgo/night-logs/loop.log 2>&1
echo "engine-exit=$?" >> /mnt/e/csgo/night-logs/loop.log
CP=/mnt/e/csgo/CSGO-Source-Linux-20260928/game/csgo/bin/androidarm64/release/libclient_panorama_client.so
[ -f "$CP" ] && echo "client-lib CircularProgressBar hits: $(strings -a "$CP" | grep -c CircularProgressBar)" >> /mnt/e/csgo/night-logs/loop.log
bash scripts/build-ohos-engine.sh stage >> /mnt/e/csgo/night-logs/loop.log 2>&1
echo "stage-exit=$?" >> /mnt/e/csgo/night-logs/loop.log
tail -12 /mnt/e/csgo/night-logs/loop.log
