#!/usr/bin/env python3
"""Materialize the real panorama content (from deployed code.pbin) as loose files.

The OHOS engine always prefers loose panorama resources (ShouldUseLoosePanoramaResources
returns true with DEVELOPMENT_ONLY), so the generated HUD skeletons in layout/hud were
shadowing the real layouts inside code.pbin. Constructors expect the real panel
structures -> wrong-typed panels -> wild vtable call (SIGSEGV in
CCSGO_HudTeamCounter::UpdateMiniScoreboard).

This script expands the pbin into the loose tree, then re-applies the protected
overrides (touch HUD + CSNO boot menu files).
"""
import zipfile, io, os, shutil, sys

PBIN = 'code_pbin_fixed.bin'
TARGETS = [
    'CSGO-Source-Linux-20260928/ohos/overlay/csgo/panorama',
    'hap/entry/src/main/resources/rawfile/csgo/csgo/panorama',
]
PROTECT = {
    # our working touch-HUD + CSNO boot menu files survive the extraction
    'panorama/layout/hud/hud.xml',
    'panorama/layout/base_mainmenu.xml',
    'panorama/layout/base_globalpopups.xml',
    'panorama/layout/base_intromovie.xml',
    'panorama/layout/base_jsregistration.xml',
}

data = open(PBIN, 'rb').read()
idx = data.find(b'PK\x03\x04')
zf = zipfile.ZipFile(io.BytesIO(data[idx:]))

protected_content = {}
for t in TARGETS:
    for rel in PROTECT:
        p = os.path.join(t, rel[len('panorama/'):])
        if os.path.exists(p):
            protected_content[(t, rel)] = io.open(p, 'rb').read()

n_write = 0
for name in zf.namelist():
    norm = name.lower().replace('\\', '/')
    if not norm.startswith('panorama/'):
        continue
    if not (norm.endswith('.xml') or norm.endswith('.css') or norm.endswith('.js')):
        continue
    rel = norm  # e.g. panorama/layout/hud/hudradio.xml
    content = zf.read(name)
    for t in TARGETS:
        dst = os.path.join(t, rel[len('panorama/'):])
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, 'wb') as f:
            f.write(content)
        n_write += 1

n_restore = 0
for (t, rel), content in protected_content.items():
    dst = os.path.join(t, rel[len('panorama/'):])
    with open(dst, 'wb') as f:
        f.write(content)
    n_restore += 1

print('wrote %d real files x%d targets, restored %d protected overrides'
      % (n_write // len(TARGETS), len(TARGETS), n_restore))
