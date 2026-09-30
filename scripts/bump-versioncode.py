#!/usr/bin/env python3
# versionCode 自增（OHOS install -r 在 versionCode 不变时会静默跳过 native libs 更新）
import io, re, sys

p = sys.argv[1]
s = io.open(p, encoding='utf-8').read()
m = re.search(r'"versionCode":\s*(\d+)', s)
code = int(m.group(1)) + 1
s = s[:m.start(1)] + str(code) + s[m.end(1):]
io.open(p, 'w', encoding='utf-8', newline='').write(s)
print(f'versionCode -> {code}')
