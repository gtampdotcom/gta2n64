#!/usr/bin/env python3
"""Build-time converter for the front-end screens: a 16-bit TGA laid out for 640x480 -> the picture the N64 shows.

usage: img_n64.py <in.tga> <out> [fb_width fb_height]        (default 640 240, the port's framebuffer)

The game lays its menu screens out for 640x480; the console draws a 640x240 framebuffer. Scaling at run time meant a
quick 2x2 average while the menu loaded (and a stretch back to 640 wide when drawing). Here each image is resampled
once, on the PC, to exactly the pixels it covers on the framebuffer:
  * area (box) filter, per axis, for any ratio;
  * averaged in linear light (gamma 2.2), so thin bright lettering on dark keeps its weight instead of going grey;
  * ordered dither when rounding back to 5 bits per channel, so gradients do not band.
Output file (read by gbh_n64.cpp, gta2_n64_image_from_rom): "N64I", then big-endian u16 layout width, layout height,
stored width, stored height, 4 zero bytes; then stored_height rows, top-down, of big-endian RGBA5551 (alpha = 1).
"""
import struct
import sys

REF_W, REF_H = 640, 480
BAYER = (0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5)  # 4x4 ordered dither


def read_tga16(data):
    idlen, cmap, kind = data[0], data[1], data[2]
    w, h = struct.unpack_from('<HH', data, 12)
    bpp, desc = data[16], data[17]
    if cmap != 0 or kind != 2 or bpp != 16:
        raise ValueError('not an uncompressed 16-bit TGA')
    base = 18 + idlen
    px = struct.unpack_from('<%dH' % (w * h), data, base)
    rows = [px[y * w:(y + 1) * w] for y in range(h)]
    if not (desc & 0x20):  # stored bottom-up
        rows.reverse()
    return w, h, rows


def spans(n_src, n_dst):
    """For each destination pixel: [(source index, weight)], box filter over the source interval it covers."""
    out = []
    step = n_src / n_dst
    for i in range(n_dst):
        a, b = i * step, (i + 1) * step
        taps = []
        j = int(a)
        while j < b - 1e-9 and j < n_src:
            wgt = min(b, j + 1) - max(a, j)
            if wgt > 1e-9:
                taps.append((j, wgt / step))
            j += 1
        out.append(taps)
    return out


def convert(data, fb_w, fb_h):
    w, h, rows = read_tga16(data)
    sw = max(1, (w * fb_w + REF_W // 2) // REF_W)
    sh = max(1, (h * fb_h + REF_H // 2) // REF_H)
    lin = [(i / 31.0) ** 2.2 for i in range(32)]
    # 5-bit level for a linear value, with a threshold in [0,1): lower level plus one when the fraction beats the threshold
    inv = [(v / 4095.0) ** (1 / 2.2) * 31.0 for v in range(4096)]
    xs, ys = spans(w, sw), spans(h, sh)
    out = bytearray(b'N64I' + struct.pack('>HHHH', w, h, sw, sh) + b'\0\0\0\0')
    chan = ((10, 11), (5, 6), (0, 1))  # (shift in TGA 555, shift in RGBA5551)
    # horizontal pass per source row (in linear light), then vertical
    hrows = []
    for row in rows:
        if sw == w:
            hrows.append([(lin[(v >> 10) & 31], lin[(v >> 5) & 31], lin[v & 31]) for v in row])
        else:
            line = []
            for taps in xs:
                r = g = b = 0.0
                for j, wgt in taps:
                    v = row[j]
                    r += lin[(v >> 10) & 31] * wgt
                    g += lin[(v >> 5) & 31] * wgt
                    b += lin[v & 31] * wgt
                line.append((r, g, b))
            hrows.append(line)
    for y, taps in enumerate(ys):
        line = bytearray()
        for x in range(sw):
            acc = [0.0, 0.0, 0.0]
            for j, wgt in taps:
                p = hrows[j][x]
                acc[0] += p[0] * wgt
                acc[1] += p[1] * wgt
                acc[2] += p[2] * wgt
            thr = (BAYER[(y & 3) * 4 + (x & 3)] + 0.5) / 16.0
            word = 1
            for c, (_, shift) in zip(acc, chan):
                f = inv[min(4095, max(0, int(c * 4095.0 + 0.5)))]
                q = int(f)
                if q < 31 and f - q > thr:
                    q += 1
                word |= q << shift
            line += struct.pack('>H', word)
        out += line
    return bytes(out), (w, h, sw, sh)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    fb_w = int(sys.argv[3]) if len(sys.argv) > 3 else 640
    fb_h = int(sys.argv[4]) if len(sys.argv) > 4 else 240
    data, _ = convert(open(sys.argv[1], 'rb').read(), fb_w, fb_h)
    open(sys.argv[2], 'wb').write(data)


if __name__ == '__main__':
    main()
