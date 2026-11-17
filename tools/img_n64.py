#!/usr/bin/env python3
"""Build-time converter for the front-end screens: a 16-bit TGA laid out for 640x480 -> the picture the N64 shows.

usage: img_n64.py <in.tga> <out> [fb_width fb_height]        (default 640 240, the port's framebuffer)

The game lays its menu screens out for 640x480; the console draws a 640x240 framebuffer. Scaling at run time meant a
quick 2x2 average while the menu loaded (and a stretch back to 640 wide when drawing). Here each image is resampled
once, on the PC, to exactly the pixels it covers on the framebuffer:
  * area (box) filter, per axis, for any ratio;
  * averaged in linear light (gamma 2.2), so thin bright lettering on dark keeps its weight instead of going grey;
  * ordered dither when rounding back to 5 bits per channel, so gradients do not band.
The GTA2 title logo (on the menu screens) is not resampled: it is redrawn from tools/gta2_logo.svg (Wikimedia Commons, public domain
text logo) at the framebuffer resolution with exact edge coverage (tools/logo_svg.py), over the picture around it.
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



LOGO_SX, LOGO_SY, LOGO_OX, LOGO_OY = 1.1473437, 1.1542187, 150.4578, 52.0719   # SVG pixel -> 640x480 layout (fitted to the game's logo)
RING = 3.45                                                                      # ring thickness in layout pixels
WHITE = (1.0, 1.0, 1.0)                                                          # linear light
RED = tuple((v / 31.0) ** 2.2 for v in (23, 1, 3))                               # the game's red, (23, 1, 3) in 5 bits
PLATE_GROW = (1.0346, 1.0624)                                                    # the game's ring is the SVG plate a little bigger (same centre)
_logo_cache = {}


def logo_layers(sw, sh, off_x, fb_h):
    """coverage grids (plate, inner plate, letters, '2') of the logo on a sw x sh picture whose left edge is at layout x = off_x"""
    key = (sw, sh, off_x)
    if key in _logo_cache:
        return _logo_cache[key]
    import os
    here = os.path.dirname(os.path.abspath(__file__))
    sys.path.insert(0, here)
    import logo_svg
    shapes = logo_svg.load_svg(os.path.join(here, 'gta2_logo.svg'))
    kx = 1.0 * sw / (sw if sw else 1)
    sy = LOGO_SY * fb_h / float(REF_H)
    ox = LOGO_OX - off_x * (sw / float(sw))
    oy = LOGO_OY * fb_h / float(REF_H)
    plate0 = shapes['whiteplate']
    pts = [p for poly in plate0 for p in poly]
    cx = (max(p[0] for p in pts) + min(p[0] for p in pts)) / 2
    cy = (max(p[1] for p in pts) + min(p[1] for p in pts)) / 2
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    gx, gy = PLATE_GROW
    plate = [[(cx + (x - cx) * gx, cy + (y - cy) * gy) for x, y in poly] for poly in plate0]
    wp, hp = (max(xs) - min(xs)) * LOGO_SX * gx, (max(ys) - min(ys)) * LOGO_SY * gy
    fx, fy = (wp - 2 * RING) / wp, (hp - 2 * RING) / hp
    inner = [[(cx + (x - cx) * gx * fx, cy + (y - cy) * gy * fy) for x, y in poly] for poly in plate0]
    # only the band of rows the logo covers
    cov = lambda polys, eo=True: logo_svg.coverage(polys, sw, sh, LOGO_SX, sy, ox, oy, sub=8, evenodd=eo)
    layers = (cov(plate), cov(inner), cov(shapes['GTA']), cov(shapes['_2']))
    _logo_cache[key] = layers
    return layers


def apply_logo(acc, sw, sh, off_x, fb_h):
    """acc[y][x] = (r, g, b) in linear light: redraw the logo over it"""
    pl, pin, gt, two = logo_layers(sw, sh, off_x, fb_h)
    # rows/cols where the plate is
    rows = [y for y in range(sh) if any(v > 0 for v in pl[y])]
    if not rows:
        return
    y0, y1 = max(0, rows[0] - 4), min(sh - 1, rows[-1] + 4)
    near = [[False] * sw for _ in range(sh)]      # within 2 px of the plate (or inside it)
    for y in range(y0, y1 + 1):
        for x in range(sw):
            if pl[y][x] > 0:
                for yy in range(max(0, y - 2), min(sh, y + 3)):
                    row = near[yy]
                    for xx in range(max(0, x - 2), min(sw, x + 3)):
                        row[xx] = True
    snap = {}
    for y in range(y0, y1 + 1):
        for x in range(sw):
            if near[y][x] and pl[y][x] < 1.0:
                # what the picture shows around the logo: average of the nearest pixels that are not near it
                got = None
                for r in range(1, 9):
                    sr = sg = sb = 0.0
                    n = 0
                    for yy in range(max(0, y - r), min(sh, y + r + 1)):
                        for xx in range(max(0, x - r), min(sw, x + r + 1)):
                            if not near[yy][xx]:
                                p = acc[yy][xx]
                                sr += p[0]; sg += p[1]; sb += p[2]
                                n += 1
                    if n:
                        got = (sr / n, sg / n, sb / n)
                        break
                snap[(y, x)] = got if got else (0.0, 0.0, 0.0)
    for y in range(y0, y1 + 1):
        for x in range(sw):
            cp = pl[y][x]
            if not near[y][x]:
                continue
            base = snap.get((y, x), (0.0, 0.0, 0.0))
            c = list(base) if cp < 1.0 else [0.0, 0.0, 0.0]
            for k in range(3):
                c[k] = c[k] * (1 - cp) + WHITE[k] * cp
            ci = pin[y][x]
            for k in range(3):
                c[k] = c[k] * (1 - ci)
            cg = gt[y][x]
            for k in range(3):
                c[k] = c[k] * (1 - cg) + WHITE[k] * cg
            c2 = two[y][x]
            for k in range(3):
                c[k] = c[k] * (1 - c2) + RED[k] * c2
            acc[y][x] = (c[0], c[1], c[2])


def convert(data, fb_w, fb_h, logo_x=None):
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
    accrows = []
    for y, taps in enumerate(ys):
        rowacc = []
        for x in range(sw):
            a0 = a1 = a2 = 0.0
            for j, wgt in taps:
                p = hrows[j][x]
                a0 += p[0] * wgt
                a1 += p[1] * wgt
                a2 += p[2] * wgt
            rowacc.append((a0, a1, a2))
        accrows.append(rowacc)
    if logo_x is not None:
        apply_logo(accrows, sw, sh, logo_x, fb_h)
    for y in range(sh):
        line = bytearray()
        for x in range(sw):
            acc = accrows[y][x]
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
    raw = open(sys.argv[1], 'rb').read()
    tw = struct.unpack_from('<H', raw, 12)[0]
    logo_x = {278: 0, 362: 278}.get(tw) if fb_w == 640 else None       # menu halves: the picture is 278 wide on the left, 362 on the right
    data, _ = convert(raw, fb_w, fb_h, logo_x)
    open(sys.argv[2], 'wb').write(data)


if __name__ == '__main__':
    main()
