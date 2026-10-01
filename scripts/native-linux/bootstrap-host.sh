#!/usr/bin/env bash
set -euo pipefail
for tool in apt-get dpkg-deb python3 make git curl unzip patch sha256sum; do
 command -v "$tool" >/dev/null || { echo "Missing base host prerequisite: $tool" >&2; exit 2; }
done
source /etc/os-release
[[ ${ID:-} == debian && ${VERSION_ID:-} == 13 && $(uname -m) == x86_64 ]] || { echo 'Requires Debian13 x86_64' >&2; exit 2; }
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
export CSGO_TOOL_ROOT=${CSGO_TOOL_ROOT:-$ROOT/CSGO-Source-Linux-20260928/runtime/ohos/toolchain}
BASE=$CSGO_TOOL_ROOT/host
if [[ -d "$BASE/root" && -n $(ls -A "$BASE/root") ]]; then
 echo "Refusing to overlay an existing host tool root: $BASE/root. Choose a fresh CSGO_TOOL_ROOT." >&2
 exit 2
fi
mkdir -p "$BASE"/{apt/lists/partial,apt/cache/archives/partial,root}
printf '%s\n' 'deb [signed-by=/usr/share/keyrings/debian-archive-keyring.gpg] https://deb.debian.org/debian trixie main' > "$BASE/apt/sources.list"
opts=(-o Debug::NoLocking=1 -o Dir::Etc::sourcelist="$BASE/apt/sources.list" -o Dir::Etc::sourceparts=- -o Dir::State::lists="$BASE/apt/lists" -o Dir::Cache="$BASE/apt/cache")
apt-get "${opts[@]}" update
mapfile -t packages < <(awk '{print $1 "=" $2}' "$(dirname "$0")/packages.tsv")
(cd "$BASE/apt/cache/archives"; apt-get "${opts[@]}" download "${packages[@]}")
manifest=$(realpath "$(dirname "$0")/packages.sha256")
(cd "$BASE/apt/cache/archives"; sha256sum -c "$manifest")
: > "$BASE/packages.tsv"
while read -r checksum filename; do
 filename=${filename#\*}
 deb="$BASE/apt/cache/archives/$filename"
 dpkg-deb --show --showformat='${Package}\t${Version}\t${Architecture}\n' "$deb" >> "$BASE/packages.tsv"
 dpkg-deb -x "$deb" "$BASE/root"
done < "$manifest"
cp "$manifest" "$BASE/packages.sha256"
