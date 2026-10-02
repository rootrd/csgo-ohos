import io
p = '/mnt/e/csgo/CSGO-Source-Linux-20260928/src/engine/host_state.cpp'
s = io.open(p, encoding='utf-8', errors='replace').read()
import re
i = s.find('void CHostState::State_NewGame()')
j = s.find('SCR_EndLoadingPlaque', i)
assert i >= 0 and j > i
body = s[i:j]
# 在函数体内关键调用前插 trace（只在 State_NewGame 函数体范围内）
ins = [
  ('if ( Host_ValidGame() )', 'fprintf( stderr, "CSGO_TRACE: NG ValidGame=%d\n", (int)Host_ValidGame() );\n\t'),
  ('SV_InitGameDLL();', 'fprintf( stderr, "CSGO_TRACE: NG InitGameDLL calling\n" );\n\t\t'),
  ('if ( modelloader->Map_IsValid( m_levelName ) )', 'fprintf( stderr, "CSGO_TRACE: NG MapIsValid calling\n" );\n\t\t\t'),
  ('if ( Host_NewGame( m_levelName,', 'fprintf( stderr, "CSGO_TRACE: NG Host_NewGame calling\n" );\n\t\t\t\t'),
]
count = 0
for anchor, trace in ins:
    k = body.find(anchor)
    assert k >= 0, anchor
    body = body[:k] + trace + body[k:]
    count += 1
s = s[:i] + body + s[j:]
io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('inserted', count)
