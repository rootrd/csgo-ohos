#!/usr/bin/env bash
# Package the audited native runtime in a signing-free project copy.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
: "${TOOL_HOME:?Set TOOL_HOME to official Huawei command-line-tools}"
: "${OHOS_SDK:?Set OHOS_SDK to its openharmony/native SDK directory}"
export DEVECO_SDK_HOME=${DEVECO_SDK_HOME:-$TOOL_HOME/sdk}
export PATH="$TOOL_HOME/bin:$TOOL_HOME/ohpm/bin:$TOOL_HOME/tool/node/bin:$PATH"
export JAVA_HOME=${JAVA_HOME:-/usr/lib/jvm/java-21-openjdk-amd64}
build_dir=${HAP_BUILD_DIR:-$root/CSGO-Source-Linux-20260928/runtime/ohos/hap-project}
python3 "$root/scripts/verify-ohos-libs.py" "$root/hap/entry/libs/arm64-v8a" --sdk "$OHOS_SDK"
# Never edit or load the checkout's signing materials.
if [[ -d "$build_dir" && -n $(ls -A "$build_dir") && ! -f "$build_dir/.csgo-build-copy" ]]; then
    echo "Refusing to replace an unmarked build directory: $build_dir" >&2; exit 1
fi
mkdir -p "$build_dir"
touch "$build_dir/.csgo-build-copy"
rsync -a --delete --filter='P .csgo-build-copy' --exclude=.hvigor --exclude=build --exclude=oh_modules \
    --exclude='/build-profile.json5' --exclude='*.p12' --exclude='*.pfx' --exclude='*.pem' \
    --exclude='*.key' --exclude='*.p7b' --exclude='*.cer' --exclude='*.jks' \
    "$root/hap/" "$build_dir/"
python3 - "$build_dir" "$DEVECO_SDK_HOME/default/sdk-pkg.json" "$root/hap/build-profile.json5" <<'PY'
import json, pathlib, sys
project=pathlib.Path(sys.argv[1]); sdk=json.loads(pathlib.Path(sys.argv[2]).read_text())['data']
profile=json.loads(pathlib.Path(sys.argv[3]).read_text())
profile['app']['signingConfigs']=[]
for product in profile['app']['products']:
 product.pop('signingConfig',None)
 product['targetSdkVersion']=(sdk['platformVersion'] if int(sdk['apiVersion']) >= 26 else f"{sdk['platformVersion']}({sdk['apiVersion']})")
 # Clang comes from OpenHarmony's native SDK; optional BiSheng is unnecessary.
 product.get('buildOption',{}).pop('nativeCompiler',None)
(project/'build-profile.json5').write_text(json.dumps(profile,indent=2)+'\n')
app=project/'AppScope/app.json5'; app_data=json.loads(app.read_text())
app_data['app']['versionCode']+=1
app.write_text(json.dumps(app_data,indent=2)+'\n')
PY
cd "$build_dir"
"$TOOL_HOME/ohpm/bin/ohpm" install --all
"$TOOL_HOME/bin/hvigorw" --mode module -p product=default -p buildMode=release assembleHap --no-daemon
hap="$build_dir/entry/build/default/outputs/default/entry-default-unsigned.hap"
test -s "$hap"
python3 - "$hap" "$root/scripts/verify-ohos-libs.py" "$OHOS_SDK" <<'PY'
import pathlib,subprocess,sys,tempfile,zipfile
with zipfile.ZipFile(sys.argv[1]) as z:
 bad=z.testzip()
 if bad: raise SystemExit('Corrupt HAP entry: '+bad)
 names=z.namelist()
 for name in names:
  if pathlib.PurePosixPath(name).suffix.lower() in {'.p12','.pfx','.pem','.key','.p7b','.cer','.jks'}:
   raise SystemExit('Unexpected signing material in HAP: '+name)
 for suffix in ['libmain.so','d3d9.so','libdxvk_dxgi.so.0','libSDL3.so','libv8.cr.so']:
  if not any(n.endswith('/'+suffix) for n in names): raise SystemExit('Missing HAP native library: '+suffix)
 with tempfile.TemporaryDirectory(prefix='csgo-hap-audit-') as temp:
  destination=pathlib.Path(temp)
  for name in names:
   if '/arm64-v8a/' not in name or '.so' not in pathlib.PurePosixPath(name).name:
    continue
   target=destination/pathlib.PurePosixPath(name).name
   if target.exists(): raise SystemExit('Duplicate native library basename: '+name)
   target.write_bytes(z.read(name))
  subprocess.run([sys.executable,sys.argv[2],str(destination),'--sdk',sys.argv[3]],check=True)
print('HAP ZIP integrity and packaged ELF closure verified')
PY
sha256sum "$hap"
echo "Unsigned HAP (requires authorized signing before normal device installation): $hap"
