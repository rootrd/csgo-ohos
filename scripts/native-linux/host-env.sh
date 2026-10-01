# Source in a native Debian13 x86_64 build shell. Does not replace system tools.
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
export CSGO_TOOL_ROOT=${CSGO_TOOL_ROOT:-$ROOT/CSGO-Source-Linux-20260928/runtime/ohos/toolchain}
CSGO_HOST_TOOLS=$CSGO_TOOL_ROOT/host
export PATH="$CSGO_HOST_TOOLS/root/usr/bin:$CSGO_HOST_TOOLS/root/usr/lib/llvm-19/bin:$PATH"
export LD_LIBRARY_PATH="$CSGO_HOST_TOOLS/root/usr/lib/x86_64-linux-gnu:$CSGO_HOST_TOOLS/root/usr/lib/llvm-19/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PYTHONPATH="$CSGO_HOST_TOOLS/root/usr/lib/python3/dist-packages${PYTHONPATH:+:$PYTHONPATH}"
