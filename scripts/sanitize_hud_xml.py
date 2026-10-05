#!/usr/bin/env python3
"""Replace XML element types that have no registered panel factory with <Panel>,
so retail layouts (hud.xml etc.) parse against this source snapshot.

Panorama's parser hard-fails a layout on the first unknown panel type, which then
breaks the whole HUD chain (CSGOHud falls back and HudTopLeft goes missing).
Unknown types are replaced (not removed) to keep child ids resolvable.
"""
import io, os, re, subprocess, zipfile

REPO = 'E:/csgo'
SRC = REPO + '/CSGO-Source-Linux-20260928/src'
TARGETS = [
    'CSGO-Source-Linux-20260928/ohos/overlay/csgo/panorama/layout',
    'hap/entry/src/main/resources/rawfile/csgo/csgo/panorama/layout',
]
BUILTINS = set('''Panel Label Image Button ToggleButton TextButton TextEntry NumberEntry
ProgressBar Slider SlottedSlider Spinner RadioButton Movie MoviePanel VolumeSliderPopup
VideoQualityPopup TooltipPanel MouseScrollRegion VerticalScrollList VerticalSplitter
HorizontalSplitter TextInputDaisyGroup TextInputPickPanel VerticalScrollForwarding
Carousel ImageStrip AnimatedImageStrip Frame SalesBanner ItemImage InventoryItemList
VUMeter CS GOImage'''.split())

out = subprocess.run(
    ['grep', '-rhn', '-E', r'REGISTER_PANEL(2D)?_FACTORY', SRC,
     '--include=*.cpp'], capture_output=True, text=True, errors='replace')
registered = set()
for line in out.stdout.splitlines():
    m = re.search(r'FACTORY\s*\(\s*(\w+)\s*,\s*(\w+:*)\s*\)', line)
    if m:
        registered.add(m.group(2).rstrip(':'))
print('registered factory types:', len(registered))

tag_re = re.compile(r'<(/?)([A-Za-z_][A-Za-z0-9_.:-]*)((?:"[^"]*"|[^>""])*?)(/?)>')
changed = {}
for t in TARGETS:
    root = os.path.join(REPO, t)
    for dirpath, _, files in os.walk(root):
        for fn in files:
            if not fn.endswith('.xml'):
                continue
            p = os.path.join(dirpath, fn)
            txt = io.open(p, encoding='utf-8', errors='replace').read()
            unknown = set()
            def repl(m):
                closing, name, attrs, selfc = m.groups()
                if closing:
                    if name in unknown:
                        return '</Panel>'
                    return m.group(0)
                if name in registered or name in BUILTINS or name == 'root' or name in ('styles', 'scripts', 'style', 'script', 'snippets', 'snippet', 'include'):
                    return m.group(0)
                unknown.add(name)
                return '<Panel' + attrs + selfc + '>'
            new = tag_re.sub(repl, txt)
            if unknown:
                changed[os.path.relpath(p, root)] = sorted(unknown)
                io.open(p, 'w', encoding='utf-8', newline='\n').write(new)

for k in sorted(changed):
    print('sanitized', k, '->', changed[k])
