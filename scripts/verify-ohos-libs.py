#!/usr/bin/env python3
"""Fail-fast package audit. This validates ELF closure, not device behavior."""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('directory', type=pathlib.Path)
parser.add_argument('--sdk', type=pathlib.Path, required=True)
args = parser.parse_args()
readelf = args.sdk / 'llvm/bin/llvm-readelf'
libs = {p.name: p for p in args.directory.iterdir() if p.is_file() and '.so' in p.name}
system = {p.name for p in (args.sdk / 'sysroot').rglob('*.so*')}
errors = []
v8_exports = set()
v8_required = {
    '_ZN2v82V810GetVersionEv', '_ZN2v82V810InitializeEv',
    '_ZN2v87Isolate3NewERKNS0_12CreateParamsE',
    '_ZN2v88platform21CreateDefaultPlatformEi',
    '_ZN2v86Script3RunENS_5LocalINS_7ContextEEE',
}
objdump = args.sdk / 'llvm/bin/llvm-objdump'
nm = args.sdk / 'llvm/bin/llvm-nm'
required = ['libmain.so', 'd3d9.so', 'libdxvk_dxgi.so.0', 'libSDL3.so',
            'libv8.cr.so', 'libv8_libbase.cr.so', 'libv8_libplatform.cr.so']
engine_modules = (
    'launcher engine filesystem_stdio inputsystem vphysics materialsystem '
    'shaderapidx9 datacache studiorender soundemittersystem vaudio_minimp3 '
    'scenefilecache vscript vguimatsurface vgui2 localize stdshader_dbg stdshader_dx9 '
    'panorama panoramauiclient panorama_text_pango matchmaking client_panorama server '
    'tier0 vstdlib'
).split()
required += [f'lib{module}_client.so' for module in engine_modules]
frontend_exports = {
    'libmain.so': {'SDL_main'},
    'libSDL3.so': {'SDL_Init'},
    'd3d9.so': {'Direct3DCreate9', 'Direct3DCreate9Ex'},
    'libdxvk_dxgi.so.0': {'CreateDXGIFactory'},
}
for name in required:
    if name not in libs:
        errors.append(f'missing required runtime: {name}')
for name, path in sorted(libs.items()):
    try:
        elf = subprocess.check_output([str(readelf), '-h', '-S', '-d', str(path)], text=True)
    except subprocess.CalledProcessError:
        errors.append(f'not a valid ELF: {name}')
        continue
    if not re.search(r'Machine:\s+AArch64', elf):
        errors.append(f'not ARM64 ELF: {name}')
    if '.note.android.ident' in elf:
        errors.append(f'Android/Bionic build must not be packaged as OHOS: {name}')
    if name == 'libphonon.so':
        errors.append('obsolete Steam Audio runtime: OHOS build disables phonon')
    if name.startswith('libv8') and path.stat().st_size < 32768:
        errors.append(f'V8 runtime is implausibly small; likely a stub: {name}')
    dynamic_symbols = subprocess.check_output([str(nm), '-D', str(path)], text=True)
    exports = set()
    for line in dynamic_symbols.splitlines():
        parts = line.split()
        if not parts:
            continue
        symbol = parts[-1].split('@')[0]
        kind = parts[-2] if len(parts) >= 2 else ''
        if len(parts) >= 3 and kind.upper() in {'T', 'D', 'R', 'B', 'W', 'V'}:
            exports.add(symbol)
        if not symbol.startswith(('_ZN2v8', '_ZNK2v8')):
            continue
        if kind == 'U':
            v8_required.add(symbol)
        elif len(parts) >= 3 and name.startswith('libv8') and kind.upper() in {'T', 'D', 'R', 'B', 'W', 'V'}:
            v8_exports.add(symbol)
    if not exports:
        errors.append(f'ELF exports no runtime symbols: {name}')
    for symbol in sorted(frontend_exports.get(name, set()) - exports):
        errors.append(f'{name} is missing required export {symbol}')
    if name == 'libv8.cr.so':
        # Old staging generated exactly "mov x0, xzr; ret" for every export.
        # File size alone is insufficient because symbol tables can pad stubs.
        disassembly = subprocess.check_output([
            str(objdump), '-d', '--disassemble-symbols=_ZN2v82V810GetVersionEv', str(path)
        ], text=True)
        if re.search(r'\bmov\s+x0,\s*(?:xzr|#0(?:x0)?)\b', disassembly):
            errors.append('V8 GetVersion is a zero-return stub')
    for needed in re.findall(r'\(NEEDED\).*?\[(.*?)\]', elf):
        if needed not in libs and needed not in system:
            errors.append(f'{name} requires unstaged library {needed}')
for symbol in sorted(v8_required - v8_exports):
    errors.append(f'real V8 runtime does not export required symbol: {symbol}')
if errors:
    print('\n'.join('ERROR: ' + error for error in errors), file=sys.stderr)
    sys.exit(1)
print(f'ELF architecture and dependency audit passed for {len(libs)} files')
print('Device startup, JavaScript/JIT permission and rendering still require on-device validation')
