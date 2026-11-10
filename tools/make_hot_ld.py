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

import os

src, order, dst = sys.argv[1], sys.argv[2], sys.argv[3]
names = []
if order and os.path.exists(order):
    for line in open(order, encoding='utf-8'):
        line = line.split('#')[0].strip()
        if line and line not in names:
            names.append(line)
# Code that only the intro movie uses (port/n64/intro_n64.c): libdragon's H.264 decoder and movie player, ~80 KB. It is
# linked as one block between two symbols; once the movie has played, main_n64.c gives that memory to the game's buffers
# (gta2_n64_reclaim_alloc). Only objects that nothing else in the game calls may be listed here.
INTRO_ONLY = ('*libdragon.a:h264_decoder.o', '*libdragon.a:h264.o', '*libdragon.a:yuv.o', '*libdragon.a:fmv.o',
              '*libdragon.a:subtitles.o', '*libdragon.a:video.o', '*libdragon.a:video_sync.o', '*intro_n64.o')
text = open(src, encoding='utf-8').read()
anchor = '        *(.text)\n'
if text.count(anchor) == 1:
    intro = ('        /* intro-only code, reclaimed as data after the movie (tools/make_hot_ld.py) */\n'
             '        . = ALIGN(64);\n        __n64_intro_text_start = .;\n'
             + ''.join('        %s(.text .text.*)\n' % o for o in INTRO_ONLY)
             + '        . = ALIGN(64);\n        __n64_intro_text_end = .;\n')
    hot = ''.join('        *(%s)\n' % n for n in names)
    text = text.replace(anchor, intro + '        /* hot functions first, contiguous (tools/make_hot_ld.py) */\n' + hot + anchor)
    print('    [HOTLD] %s: %d hot sections' % (dst, len(names)))
else:
    print('    [HOTLD] anchor not found: using the stock linker script')
open(dst, 'w', encoding='utf-8').write(text)
