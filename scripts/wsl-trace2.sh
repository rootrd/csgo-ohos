#!/usr/bin/env bash
set -e
cd /root/csgo-src/CSGO-Source-Linux-20260928/src
cp /mnt/e/csgo/scripts/trace2.py /tmp/trace2.py
python3 /tmp/trace2.py
echo PATCH_DONE
