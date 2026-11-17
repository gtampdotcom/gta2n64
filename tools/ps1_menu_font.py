#!/usr/bin/env python3
"""ps1_menu_font.py - troca as fontes finas do MENU (fstyle.sty, fontes 4 = branca e 5 = vermelha) pelas da versao PlayStation.

uso: ps1_menu_font.py <fstyle.sty entrada (PC, little-endian)> <saida.sty> [tools/ps1_menu_font.json]

Por que: as letras do menu do PC (640x480, tracos de 2 linhas) viram traco de meia linha no framebuffer 640x240 do N64 e
ficam borradas. As do PS1 (512x240) tem traco de exatamente 1 linha: cada linha delas ocupa 1 linha do framebuffer.
Aqui: cada linha do glifo do PS1 vira 2 linhas do layout 640x480 (= 1 linha do N64) e a largura passa de 512 para 640
(x 5/4) duplicando colunas que sao iguais a vizinha (assim os tracos verticais nao engordam nem afinam).
Letras que o PS1 do jogo nao mostrou nas telas capturadas (Q Z, digitos, pontuacao) ficam com o glifo do PC, so com as
linhas "encaixadas" nas linhas do N64 (media de 2 em 2 e limiar). Os glifos novos vao para paginas novas no fim do SPRG
(mapeado direto da ROM: so custa cartucho). As 18 cores continuam valendo: os indices usados sao os mesmos da rampa de cinza.
"""
import json
import struct
import sys

FONTS = (4, 5)                                  # branca fina, vermelha fina
GREY = {3: 24, 4: 41, 7: 74, 8: 82, 9: 99, 11: 115, 12: 132, 14: 173, 15: 189, 16: 198, 17: 222, 18: 239, 19: 255, 255: 255}
RAMP = {'3': 16, '2': 12, '1': 8}               # niveis intermediarios -> indice da rampa (o nivel cheio e' o indice do 'A' do PC)
PAGE = 65536
INK_TOP = 6                                     # primeira linha da tinta (celula de 22 linhas, 12 de tinta; o PC usa 5). PAR: o N64 funde as linhas do layout 2 a 2 (0-1, 2-3...), entao cada linha do PS1 so fica nitida se comecar numa linha par


def chunks(d):
    out = {}
    o = 6
    while o + 8 <= len(d):
        tag = bytes(d[o:o + 4])
        sz = struct.unpack_from('<I', d, o + 4)[0]
        out[tag] = [o + 8, sz]
        o += 8 + sz
    return out


def stretch(rows):
    """5/4 mais larga: duplica colunas de miolo (iguais a vizinha: dentro das barras horizontais), espalhadas e simetricas.
    Nunca as 2 primeiras nem as 3 ultimas colunas: ali ficam as hastes verticais, que nao podem engordar so de um lado."""
    w = len(rows[0])
    w2 = int(w * 1.25 + 0.5)
    k = w2 - w
    cols = [''.join(r[c] for r in rows) for c in range(w)]
    lo, hi = (2, w - 4) if w >= 9 else (0, w - 1)
    cand = [c for c in range(lo, hi + 1) if cols[c] == cols[min(c + 1, w - 1)]]
    if len(cand) < k:
        cand = list(range(lo, hi + 1)) if hi - lo + 1 >= k else list(range(w))
    chosen = []
    for i in range(k):
        ideal = lo + (i + 0.5) * (hi - lo + 1) / k
        free = [c for c in cand if c not in chosen]
        chosen.append(min(free, key=lambda c: abs(c - ideal)))
    out = []
    for r in rows:
        s = ''
        for c in range(w):
            s += r[c] * (2 if c in chosen else 1)
        out.append(s)
    return out


def main():
    src, dst = sys.argv[1], sys.argv[2]
    jpath = sys.argv[3] if len(sys.argv) > 3 else __file__.replace('ps1_menu_font.py', 'ps1_menu_font.json')
    ps1 = json.load(open(jpath))
    d = bytearray(open(src, 'rb').read())
    ch = chunks(d)
    u16 = lambda tag: list(struct.unpack_from('<%dH' % (ch[tag][1] // 2), d, ch[tag][0]))
    sprb, fonb = u16(b'SPRB'), u16(b'FONB')
    sprx_a = ch[b'SPRX'][0]
    sprg_a, sprg_n = ch[b'SPRG'][0], ch[b'SPRG'][1]
    assert sprg_n % PAGE == 0, 'SPRG nao e multiplo de 64 KB'
    page0 = sprg_n // PAGE
    new = bytearray()
    st = {'x': 0, 'y': 0, 'h': 0}

    def alloc(w, h):
        if new and st['x'] + w > 256:
            st['x'], st['y'], st['h'] = 0, st['y'] + st['h'], 0
        if not new or st['y'] + h > 256:
            new.extend(bytearray(PAGE))
            st['x'] = st['y'] = st['h'] = 0
        pos = (page0 + len(new) // PAGE - 1) * PAGE + st['y'] * 256 + st['x']
        st['x'] += w
        st['h'] = max(st['h'], h)
        return pos

    nps1 = nfall = 0
    for font in FONTS:
        base = sum(sprb[:5]) + sum(fonb[1:1 + font])
        count = fonb[1 + font]
        # indice "cheio" desta fonte: o mais comum no 'A'
        ptrA, wA, hA, _ = struct.unpack_from('<IBBH', d, sprx_a + 8 * (base + ord('A') - 33))
        freq = {}
        for r in range(hA):
            for c in range(wA):
                v = d[sprg_a + ptrA + r * 256 + c]
                if v:
                    freq[v] = freq.get(v, 0) + 1
        full = max(freq, key=freq.get)
        idx = dict(RAMP)
        idx['4'] = full
        for k in range(count):
            i = base + k
            ptr, w, h, pad = struct.unpack_from('<IBBH', d, sprx_a + 8 * i)
            code = chr(k + 33) if k < 94 else None
            if code is not None and code in ps1:
                rows = stretch(ps1[code])
                gw = len(rows[0])
                nw = gw + 3
                canvas = [[0] * nw for _ in range(h)]
                top = INK_TOP if len(rows) > 1 else INK_TOP + 6      # o '-' (1 linha) fica no meio da altura das letras
                for r, line in enumerate(rows):
                    for c, t in enumerate(line):
                        if t == '.':
                            continue
                        for dy in range(2):
                            y = top + r * 2 + dy
                            if 0 <= y < h:
                                canvas[y][1 + c] = idx[t]
                nps1 += 1
            else:
                # sem glifo do PS1: o do PC, com as linhas encaixadas nas linhas do N64 (2 em 2)
                nw = w
                canvas = [[0] * nw for _ in range(h)]
                ink = [r for r in range(h) if any(d[sprg_a + ptr + r * 256 + c] for c in range(w))]
                if ink:
                    r0 = ink[0] - (ink[0] % 2)
                    for r in range(r0, ink[-1] + 1, 2):
                        for c in range(w):
                            vs = [d[sprg_a + ptr + rr * 256 + c] for rr in (r, r + 1) if 0 <= rr < h]
                            avg = sum(GREY.get(v, 128) if v else 0 for v in vs) / 2.0
                            if avg < 100:
                                continue
                            t = '4' if avg >= 215 else '3' if avg >= 165 else '2'
                            for dy in range(2):
                                if 0 <= r + dy < h:
                                    canvas[r + dy][c] = idx[t]
                nfall += 1
            p = alloc(nw, h)
            off = p - page0 * PAGE
            for r in range(h):
                new[off + r * 256:off + r * 256 + nw] = bytes(canvas[r])
            struct.pack_into('<IBBH', d, sprx_a + 8 * i, p, nw, h, pad)
    end = sprg_a + sprg_n
    struct.pack_into('<I', d, sprg_a - 4, sprg_n + len(new))
    open(dst, 'wb').write(bytes(d[:end]) + bytes(new) + bytes(d[end:]))
    print('ps1_menu_font: %d glifos do PS1, %d do PC (encaixados), +%d KB no SPRG' % (nps1, nfall, len(new) // 1024))


if __name__ == '__main__':
    main()
