#!/usr/bin/env bash
# RenderDoc frame capture for the Debug APK, replayed on the phone's own GPU.
#
#   layer                        Extract the Vulkan layer matching the phone's RenderDoc app.
#   start [engine args...]       Enable the layer and launch the Debug APK built with it:
#                                  CSGO_ANDROID_VK_LAYER=$(bash scripts/renderdoc-android.sh layer) \
#                                  BUILD_CONFIG=debug bash scripts/build-android.sh package
#   capture [out.rdc]            Capture one frame of the running game and copy it to the host.
#   replay file.rdc script.py    Replay on the phone and run a qrenderdoc Python script with
#                                `controller` and `log` in scope.
#   off                          Disable the GPU debug layer settings.
set -euo pipefail

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ ${CONTAINER_ID:-} != dev ]]; then
    exec distrobox enter -T -n dev -- env ANDROID_SERIAL="${ANDROID_SERIAL:-}" \
        ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-/home/deck/Code/Toolchains/android-sdk}" \
        bash "$repo_dir/scripts/renderdoc-android.sh" "$@"
fi

sdk=${ANDROID_SDK_ROOT:-/home/deck/Code/Toolchains/android-sdk}
adb=("$sdk/platform-tools/adb")
[[ -z ${ANDROID_SERIAL:-} ]] || adb+=(-s "$ANDROID_SERIAL")
package=${CSGO_PACKAGE:-com.csgosource.android.debug}
rd_package=org.renderdoc.renderdoccmd.arm64
out="$repo_dir/runtime/android/renderdoc"
layer="$out/libVkLayer_GLES_RenderDoc.so"
driver="$repo_dir/scripts/renderdoc-android.py"

die() { echo "[renderdoc] $*" >&2; exit 1; }
shell() { "${adb[@]}" shell "$@" | tr -d '\r'; }

# Output is captured before matching: under pipefail an early-exiting reader such
# as grep -q would turn the SIGPIPE it causes upstream into a failure.
package_path() {
    local listing
    listing=$(shell pm path "$1")
    [[ $listing =~ package:([^[:space:]]+) ]] || die "$1 is not installed on the phone."
    echo "${BASH_REMATCH[1]}"
}

# Target control and remote replay only interoperate within one RenderDoc build.
check_app() {
    local host device
    host=$(renderdoccmd version)
    [[ $host =~ built\ from\ ([0-9a-f]+) ]] || die "Cannot read the host RenderDoc version."
    host=${BASH_REMATCH[1]}
    device=$(shell dumpsys package "$rd_package")
    [[ $device =~ versionName=([0-9a-f]+) ]] || die "Install RenderDoc $host on the phone ($rd_package) first."
    [[ ${BASH_REMATCH[1]} == "$host" ]] || die "Phone RenderDoc ${BASH_REMATCH[1]} differs from host RenderDoc $host."
}

qrenderdoc_run() {
    local log=$1 status=0
    shift
    env QT_QPA_PLATFORM=offscreen CSGO_RD_LOG="$log" "$@" qrenderdoc --python "$driver" || status=$?
    [[ ! -f $log ]] || cat "$log"
    return "$status"
}

action=${1:-}
(($# == 0)) || shift
case "$action" in
    layer)
        check_app
        mkdir -p "$out"
        "${adb[@]}" pull "$(package_path "$rd_package")" "$out/renderdoccmd.apk" >/dev/null
        unzip -o -q -j "$out/renderdoccmd.apk" "lib/arm64-v8a/$(basename "$layer")" -d "$out"
        echo "$layer"
        ;;
    start)
        check_app
        listing=$(shell unzip -l "$(package_path "$package")")
        [[ $listing == *"$(basename "$layer")"* ]] \
            || die "The installed APK has no RenderDoc layer; package the Debug APK with CSGO_ANDROID_VK_LAYER and install it."
        shell settings put global enable_gpu_debug_layers 1
        shell settings put global gpu_debug_app "$package"
        shell settings put global gpu_debug_layers VK_LAYER_RENDERDOC_Capture
        shell settings put global gpu_debug_layer_app "$package"
        BUILD_CONFIG=debug bash "$repo_dir/scripts/build-android.sh" run "$@"
        ;;
    capture)
        output=$(realpath -m "${1:-$repo_dir/runtime/android/debug/captures/csgo-$(date +%Y%m%d-%H%M%S).rdc}")
        [[ ! -e $output ]] || die "Refusing to overwrite $output"
        mkdir -p "$(dirname "$output")"
        pid=$(shell pidof "$package:game") || true
        [[ -n $pid ]] || die "$package:game is not running."
        # Every process of the package loads the layer (HWUI in the launcher process uses
        # Vulkan too) and each takes the next renderdoc_NNNNN abstract socket, so forward
        # them all and let the driver pick the game's PID.
        ports=()
        for name in $(shell cat /proc/net/unix | grep -o '@renderdoc_[0-9]*' | sort -u); do
            ports+=("$("${adb[@]}" forward tcp:0 "localabstract:${name#@}")")
        done
        ((${#ports[@]})) || die "No RenderDoc layer is listening; start the game with the layer enabled."
        trap 'for p in "${ports[@]}"; do "${adb[@]}" forward --remove "tcp:$p" >/dev/null 2>&1 || true; done' EXIT
        qrenderdoc_run "$output.log" CSGO_RD_MODE=capture CSGO_RD_PORTS="${ports[*]}" CSGO_RD_PID="$pid" \
            CSGO_RD_CAPTURE="$output"
        [[ -s $output ]] || die "No capture was copied."
        echo "$output"
        ;;
    replay)
        capture=$(realpath "${1:?Usage: replay file.rdc script.py}")
        script=$(realpath "${2:?Usage: replay file.rdc script.py}")
        check_app
        # adb carries the upload; RenderDoc's own transfer times out on large captures.
        sum=$(sha256sum "$capture" | cut -d' ' -f1)
        remote="/sdcard/Android/media/$rd_package/files/RenderDoc/csgo-$sum.rdc"
        if [[ $(shell sha256sum "$remote" 2>/dev/null | cut -d' ' -f1) != "$sum" ]]; then
            shell mkdir -p "$(dirname "$remote")"
            "${adb[@]}" push "$capture" "$remote" >/dev/null
            [[ $(shell sha256sum "$remote" | cut -d' ' -f1) == "$sum" ]] || die "Capture checksum differs after upload."
        fi
        access=$(shell appops get "$rd_package" MANAGE_EXTERNAL_STORAGE)
        if [[ $access != *allow* ]]; then
            shell appops set "$rd_package" MANAGE_EXTERNAL_STORAGE allow \
                || die "Grant all-files access to the RenderDoc app on the phone, then retry."
        fi
        shell am start -n "$rd_package/.Loader" -e renderdoccmd remoteserver >/dev/null
        qrenderdoc_run "$capture.replay.log" CSGO_RD_MODE=replay CSGO_RD_REMOTE="adb://$("${adb[@]}" get-serialno)" \
            CSGO_RD_REMOTE_FILE="$remote" CSGO_RD_SCRIPT="$script"
        ;;
    off)
        for key in enable_gpu_debug_layers gpu_debug_app gpu_debug_layers gpu_debug_layer_app; do
            shell settings delete global "$key" >/dev/null
        done
        echo "[renderdoc] GPU debug layers disabled; restart the game for normal rendering."
        ;;
    *) die "Usage: renderdoc-android.sh layer|start [engine args...]|capture [out.rdc]|replay file.rdc script.py|off" ;;
esac
