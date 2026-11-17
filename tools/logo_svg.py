#!/usr/bin/env python3
"""logo_svg.py - minimal SVG path rasteriser (M, L, C, z; matrices) with exact horizontal coverage and N vertical sub-scanlines.

Used by img_n64.py to redraw the GTA2 title logo (tools/gta2_logo.svg, Wikimedia Commons, public domain text logo) at the
resolution the console runs at instead of resampling the 640x480 picture.
"""
import re


def parse_matrix(s):
    m = re.search(r'matrix\(([^)]*)\)', s)
    if not m:
        return (1, 0, 0, 1, 0, 0)
    return tuple(float(v) for v in re.split(r'[ ,]+', m.group(1).strip()))


def compose(a, b):
    """a after b (apply b first)"""
    return (a[0] * b[0] + a[2] * b[1], a[1] * b[0] + a[3] * b[1], a[0] * b[2] + a[2] * b[3], a[1] * b[2] + a[3] * b[3],
            a[0] * b[4] + a[2] * b[5] + a[4], a[1] * b[4] + a[3] * b[5] + a[5])


def apply(m, x, y):
    return (m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5])


def load_svg(path):
    """-> dict id -> list of polygons (each a list of (x, y) in the SVG's own pixel space), flattened beziers"""
    txt = open(path, encoding='utf-8').read()
    gm = parse_matrix(re.search(r'<g\s[^>]*transform="([^"]*)"', txt).group(1))
    out = {}
    for pm in re.finditer(r'<path\s([^>]*?)/>', txt, re.S):
        attrs = pm.group(1)
        d = re.search(r'\sd="([^"]*)"', ' ' + attrs).group(1)
        pid = re.search(r'id="([^"]*)"', attrs).group(1)
        t = re.search(r'transform="([^"]*)"', attrs)
        m = compose(gm, parse_matrix(t.group(1)) if t else (1, 0, 0, 1, 0, 0))
        toks = re.findall(r'[MLCz]|-?\d+\.?\d*(?:e-?\d+)?', d)
        polys, cur, i, pos = [], [], 0, None
        while i < len(toks):
            c = toks[i]
            i += 1
            if c == 'M':
                if cur:
                    polys.append(cur)
                x, y = float(toks[i]), float(toks[i + 1])
                i += 2
                pos = (x, y)
                cur = [apply(m, x, y)]
            elif c == 'L':
                x, y = float(toks[i]), float(toks[i + 1])
                i += 2
                pos = (x, y)
                cur.append(apply(m, x, y))
            elif c == 'C':
                x1, y1, x2, y2, x3, y3 = [float(v) for v in toks[i:i + 6]]
                i += 6
                x0, y0 = pos
                for k in range(1, 17):
                    u = k / 16.0
                    v = 1 - u
                    bx = v ** 3 * x0 + 3 * v * v * u * x1 + 3 * v * u * u * x2 + u ** 3 * x3
                    by = v ** 3 * y0 + 3 * v * v * u * y1 + 3 * v * u * u * y2 + u ** 3 * y3
                    cur.append(apply(m, bx, by))
                pos = (x3, y3)
            elif c == 'z':
                if cur:
                    polys.append(cur)
                    cur = []
        if cur:
            polys.append(cur)
        out[pid] = polys
    return out


def coverage(polys, W, H, sx, sy, ox, oy, sub=8, evenodd=True):
    """polygon coverage on a W x H grid. Maps svg (x, y) -> (x*sx + ox, y*sy + oy). Returns list of H lists of W floats."""
    cov = [[0.0] * W for _ in range(H)]
    edges = []
    for poly in polys:
        n = len(poly)
        for k in range(n):
            x0, y0 = poly[k]
            x1, y1 = poly[(k + 1) % n]
            x0, y0, x1, y1 = x0 * sx + ox, y0 * sy + oy, x1 * sx + ox, y1 * sy + oy
            if y0 == y1:
                continue
            if y0 < y1:
                edges.append((x0, y0, x1, y1, 1))
            else:
                edges.append((x1, y1, x0, y0, -1))
    if not edges:
        return cov
    ymin = max(0, int(min(e[1] for e in edges)))
    ymax = min(H - 1, int(max(e[3] for e in edges)) + 1)
    inv = 1.0 / sub
    for row in range(ymin, ymax + 1):
        acc = cov[row]
        for s in range(sub):
            yy = row + (s + 0.5) * inv
            xs = []
            for (x0, y0, x1, y1, dirn) in edges:
                if y0 <= yy < y1:
                    xs.append((x0 + (yy - y0) * (x1 - x0) / (y1 - y0), dirn))
            if not xs:
                continue
            xs.sort()
            spans = []
            if evenodd:
                for j in range(0, len(xs) - 1, 2):
                    spans.append((xs[j][0], xs[j + 1][0]))
            else:
                wind = 0
                start = 0.0
                for xv, dirn in xs:
                    if wind == 0:
                        start = xv
                    wind += dirn
                    if wind == 0:
                        spans.append((start, xv))
            for a, b in spans:
                if b <= 0 or a >= W:
                    continue
                a = max(a, 0.0)
                b = min(b, float(W))
                ia, ib = int(a), int(b)
                if ia == ib:
                    acc[ia] += (b - a) * inv
                else:
                    acc[ia] += (ia + 1 - a) * inv
                    for xx in range(ia + 1, ib):
                        acc[xx] += inv
                    if ib < W:
                        acc[ib] += (b - ib) * inv
    for row in range(ymin, ymax + 1):
        r = cov[row]
        for x in range(W):
            if r[x] > 1.0:
                r[x] = 1.0
    return cov
