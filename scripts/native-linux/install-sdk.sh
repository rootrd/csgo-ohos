#!/usr/bin/env bash
# Obtain the official archive from https://developer.huawei.com/consumer/cn/download/command-line-tools-for-hmos
# Version: Linux(x86)26.0.0.851. No login, agreement acceptance or signing is performed here.
set -euo pipefail
archive=$(realpath "${1:?Path to official commandline-tools-linux-x64-26.0.0.851.zip}")
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
export CSGO_TOOL_ROOT=${CSGO_TOOL_ROOT:-$ROOT/CSGO-Source-Linux-20260928/runtime/ohos/toolchain}
dest=${2:-$CSGO_TOOL_ROOT/sdk}
[[ ! -e "$dest" ]] || { echo "Inspect existing SDK directory instead of overwriting: $dest" >&2; exit 2; }
printf '%s  %s\n' ab604bd92721d5cbcafd154e6461d46b9f1b105e7b89eab04a9d046681198082 "$archive" | sha256sum -c -
python3 - "$archive" <<'PY'
import pathlib,sys,zipfile
with zipfile.ZipFile(sys.argv[1]) as z:
 for i in z.infolist():
  p=pathlib.PurePosixPath(i.filename)
  if p.is_absolute() or '..' in p.parts: raise SystemExit('Unsafe archive path')
PY
mkdir -p "$dest"
unzip -q "$archive" 'command-line-tools/bin/*' 'command-line-tools/hvigor/*' 'command-line-tools/ohpm/*' 'command-line-tools/tool/node/*' 'command-line-tools/sdk/default/sdk-pkg.json' 'command-line-tools/sdk/default/openharmony/native/*' 'command-line-tools/sdk/default/openharmony/ets/*' 'command-line-tools/sdk/default/openharmony/js/*' 'command-line-tools/sdk/default/openharmony/previewer/*' 'command-line-tools/sdk/default/hms/previewer/*' 'command-line-tools/sdk/default/openharmony/toolchains/*' 'command-line-tools/sdk/default/hms/ets/*' 'command-line-tools/sdk/default/hms/js/*' 'command-line-tools/sdk/default/hms/toolchains/*' 'command-line-tools/sdk/default/hms/native/uni-package.json' 'command-line-tools/sdk/default/hms/native/build/*' 'command-line-tools/sdk/default/hms/native/sysroot/*' 'command-line-tools/sdk/default/hms/native/*json' -d "$dest"
echo "SDK extracted with ZIP CRC checks. Source sdk-env.sh to use it."
