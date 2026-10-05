#!/usr/bin/env python3
"""Remove broken appended factory registration at end of csgo_hudradio.cpp
(type already registered at line 23; the appended macro was syntactically invalid)."""
import io

BAD = ('// OHOS: register the radio HUD panel type so base_hud.xml can create it\n'
       '#include "tier0/platform.h"\n'
       'PANEL_FACTORY( CCSGO_HudRadio, CCSGO_HudRadio, panorama::CPanel2D )')
P = r'E:\csgo\CSGO-Source-Linux-20260928\src\game\client\cstrike15\panorama\hud\csgo_hudradio.cpp'
txt = io.open(P, encoding='utf-8', errors='replace').read()
if BAD in txt:
    txt = txt.replace(BAD, '').rstrip() + '\n'
    with io.open(P, 'w', encoding='utf-8', newline='\n') as f:
        f.write(txt)
    print('fixed')
else:
    print('pattern not found')
