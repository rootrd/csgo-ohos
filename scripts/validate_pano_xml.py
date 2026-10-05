#!/usr/bin/env python3
"""Validate panorama layout XMLs against this engine's parser rules:
1. tags balanced (no stray close tags) 2. no self-closing tags 3. root's direct
children must not carry id= (need WindowRoot wrapper) 4. <styles> not <style>."""
import io, os, re, sys

ROOT = sys.argv[1] if len(sys.argv) > 1 else \
    '/mnt/e/csgo/hap/entry/src/main/resources/rawfile/csgo/csgo/panorama/layout'

bad = 0
for dirpath, _, files in os.walk(ROOT):
    for fn in sorted(files):
        if not fn.endswith('.xml'):
            continue
        p = os.path.join(dirpath, fn)
        txt = io.open(p, encoding='utf-8', errors='replace').read()
        rel = os.path.relpath(p, ROOT)
        issues = []
        # strip comments so they don't confuse the tag scan
        nocomment = re.sub(r'<!--.*?-->', '', txt, flags=re.S)
        # 1. balance
        stack = []
        for m in re.finditer(r'<(/?)([A-Za-z_][A-Za-z0-9_-]*)((?:"[^"]*"|[^>"])*?)(/?)>', nocomment):
            closing, name, attrs, selfc = m.group(1), m.group(2), m.group(3), m.group(4)
            if selfc:
                issues.append('self-closing <%s%s> @char%d' % (name, ' id' if 'id=' in attrs else '', m.start()))
                continue
            if closing:
                if not stack:
                    issues.append('stray </%s> @char%d (stack empty)' % (name, m.start()))
                elif stack[-1][0] != name:
                    issues.append('mismatch </%s> but stack top <%s id=%s>' % (name, stack[-1][0], stack[-1][1]))
                    # pop until match or empty
                    while stack and stack[-1][0] != name:
                        stack.pop()
                    if stack:
                        stack.pop()
                else:
                    stack.pop()
            else:
                mid = re.search(r'id="([^"]*)"', attrs)
                stack.append((name, mid.group(1) if mid else ''))
        for name, idv in stack:
            issues.append('unclosed <%s id=%s>' % (name, idv))
        # 3. root direct children with id
        depth = 0
        root_children_ids = []
        for m in re.finditer(r'<(/?)([A-Za-z_][A-Za-z0-9_-]*)((?:"[^"]*"|[^>"])*?)(/?)>', nocomment):
            closing, name, attrs, selfc = m.group(1), m.group(2), m.group(3), m.group(4)
            if closing:
                depth -= 1
                continue
            if depth == 1:
                mid = re.search(r'id="([^"]*)"', attrs)
                if mid and name.lower() not in ('root',) and name.lower() != 'panel' or (mid and name.lower()=='panel' and depth==1):
                    root_children_ids.append('%s id=%s' % (name, mid.group(1)))
            if not selfc:
                depth += 1
        if root_children_ids:
            issues.append('root direct children with id: %s' % root_children_ids[:4])
        # 4. <style> singular
        if re.search(r'<style[>\s]', nocomment):
            issues.append('singular <style> tag present')
        if issues:
            bad += 1
            print('== %s' % rel)
            for i in issues:
                print('   ! %s' % i)
print('checked layout tree, %d file(s) with issues' % bad)
