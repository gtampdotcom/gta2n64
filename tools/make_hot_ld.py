#!/usr/bin/env python3
"""Build-time helper: writes a copy of libdragon's n64.ld that places the port's hot functions first, next to each other.

usage: make_hot_ld.py <n64.ld> <hot_order.txt> <out.ld>

Why: the VR4300 has a 16 KB direct-mapped instruction cache, and code that fell out of it costs ~48 cycles per 32-byte
line to fetch again. The functions that run for every tile / sprite / triangle of a frame were scattered over 2 MB of
code, so they kept evicting each other. hot_order.txt lists their input sections (.text.<symbol>, one per line, '#'
comments; produced from a profiling run by tools/prof_order.py); the linker puts those first, in that order, so the
code of one phase of the frame sits in one contiguous run. Sections that no longer exist are simply ignored by the
linker, so a stale list is harmless (just less effective).

If the anchor line is not found (a different libdragon), the script is copied unchanged.
"""
import sys

src, order, dst = sys.argv[1], sys.argv[2], sys.argv[3]
names = []
for line in open(order, encoding='utf-8'):
    line = line.split('#')[0].strip()
    if line and line not in names:
        names.append(line)
text = open(src, encoding='utf-8').read()
anchor = '        *(.text)\n'
if text.count(anchor) == 1 and names:
    hot = ''.join('        *(%s)\n' % n for n in names)
    text = text.replace(anchor, '        /* hot functions first, contiguous (tools/make_hot_ld.py) */\n' + hot + anchor)
    print('    [HOTLD] %s: %d hot sections' % (dst, len(names)))
else:
    print('    [HOTLD] anchor not found or empty list: using the stock linker script')
open(dst, 'w', encoding='utf-8').write(text)
