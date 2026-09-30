#!/usr/bin/env python3
"""修复 launcher.cpp 补丁多加的开括号"""
import io

p = '/root/csgo-src/CSGO-Source-Linux-20260928/src/launcher/launcher.cpp'
s = io.open(p, encoding='utf-8', errors='replace').read()
old = '''	fprintf( stderr, "CSGO_TRACE: SourceAppGroup Main enter (all InitSystems done)\\n" );
{'''
new = '''	fprintf( stderr, "CSGO_TRACE: SourceAppGroup Main enter (all InitSystems done)\\n" );'''
assert old in s, 'stray brace pattern not found'
s = s.replace(old, new, 1)
io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('launcher.cpp stray brace removed')
