ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
export CSGO_TOOL_ROOT=${CSGO_TOOL_ROOT:-$ROOT/CSGO-Source-Linux-20260928/runtime/ohos/toolchain}
source "$ROOT/scripts/native-linux/host-env.sh"
export BUILD_RECOVERY_ROOT=$CSGO_TOOL_ROOT
export TOOL_HOME="$BUILD_RECOVERY_ROOT/sdk/command-line-tools"
export OHOS_SDK="$TOOL_HOME/sdk/default/openharmony/native"
export DEVECO_SDK_HOME="$TOOL_HOME/sdk"
export PATH="$TOOL_HOME/bin:$TOOL_HOME/ohpm/bin:$TOOL_HOME/tool/node/bin:$OHOS_SDK/build-tools/cmake/bin:$PATH"
export HOME="$BUILD_RECOVERY_ROOT/home" XDG_CACHE_HOME="$BUILD_RECOVERY_ROOT/cache"
export JAVA_HOME=${JAVA_HOME:-/usr/lib/jvm/java-21-openjdk-amd64}
mkdir -p "$HOME" "$XDG_CACHE_HOME"
