#!/usr/bin/env python3
"""ps1_font.py - troca a fonte "normal" (texto de missao, pager, HUD) de um .sty (PC, little-endian) pela fonte da versao PlayStation.

uso: ps1_font.py <entrada.sty> <saida.sty> [tools/ps1_font.json]

Por que: a fonte do PC e' desenhada para 640x480 com 15 linhas de altura nas maiusculas; no N64 (framebuffer 640x240) ela
vira 7,5 linhas e borra. A fonte do PS1 e' de 320x240: cada ponto dela cobre exatamente 2x2 pontos do layout 640x480, ou seja
1 linha x 2 colunas do framebuffer do N64. Escalada 2x (vizinho mais proximo) fica nitida, sem filtragem.

Como: os glifos novos ficam em paginas NOVAS no fim do chunk SPRG (o motor mapeia o SPRG direto da ROM: custa so cartucho,
nenhum byte de heap) e as entradas SPRX dos 144 glifos da fonte 1 passam a apontar para eles. Cores: os 4 tons do PS1
(contorno, escuro, medio, claro) usam os mesmos indices de paleta do glifo do PC, entao as 18 cores de texto continuam valendo.
Simbolos que a fonte do PS1 nao tem (# % & * + = @ [ \\ ] ^ _ |) mantem o glifo do PC, so descido para a mesma linha de base.
"""
import json
import struct
import sys

FONT = 1                 # indice da fonte "normal" no FONB
TONE = {'o': 196, 'd': 189, 'm': 190, 'l': 193}   # contorno, escuro, medio, claro -> indice na paleta do glifo do PC
SCALE = 2
CELL_H = 32              # 16 linhas do PS1 x 2
A_SLOT = 32              # 'A': a altura dele e' o espacamento entre linhas do texto (GetLineSpacing)
A_H = 28                 # 14 linhas do PS1 x 2 (ate' a linha de base do 'A')
PC_BASE = 19             # linha de base (ultima linha do 'A') no glifo do PC (25 de altura)
PS1_BASE = 13 * SCALE + 1
PAGE = 65536


def chunks(d):
    out = []
    o = 6
    while o + 8 <= len(d):
        tag = bytes(d[o:o + 4])
        sz = struct.unpack_from('<I', d, o + 4)[0]
        out.append([tag, o + 8, sz])
        o += 8 + sz
    return out


def main():
    src, dst = sys.argv[1], sys.argv[2]
    jpath = sys.argv[3] if len(sys.argv) > 3 else __file__.replace('ps1_font.py', 'ps1_font.json')
    ps1 = {int(k): v for k, v in json.load(open(jpath))['glyphs'].items()}
    d = bytearray(open(src, 'rb').read())
    ch = {c[0]: c for c in chunks(d)}
    u16 = lambda tag: list(struct.unpack_from('<%dH' % (ch[tag][2] // 2), d, ch[tag][1]))
    sprb, fonb, palb = u16(b'SPRB'), u16(b'FONB'), u16(b'PALB')
    base = sum(sprb[:5]) + sum(fonb[1:1 + FONT])
    count = fonb[1 + FONT]
    sprx_a = ch[b'SPRX'][1]
    sprg_a, sprg_n = ch[b'SPRG'][1], ch[b'SPRG'][2]
    assert sprg_n % PAGE == 0, 'SPRG nao e multiplo de 64 KB'

    rec = lambda i: struct.unpack_from('<IBBH', d, sprx_a + 8 * i)
    new = bytearray()
    page0 = sprg_n // PAGE
    shelf_x = shelf_y = 0
    shelf_h = 0

    def alloc(w, h):
        nonlocal shelf_x, shelf_y, shelf_h
        if new and shelf_x + w > 256:      # proxima prateleira
            shelf_x, shelf_y, shelf_h = 0, shelf_y + shelf_h, 0
        if not new or shelf_y + h > 256:   # pagina nova
            new.extend(bytearray(PAGE))
            shelf_x = shelf_y = shelf_h = 0
        pos = (page0 + len(new) // PAGE - 1) * PAGE + shelf_y * 256 + shelf_x
        shelf_x += w
        shelf_h = max(shelf_h, h)
        return pos

    nps1 = nfall = 0
    for k in range(count):
        i = base + k
        ptr, w, h, pad = rec(i)
        if k in ps1:
            rows = ps1[k]
            pw = len(rows[0])
            nw = SCALE * (pw + 1)
            nh = A_H if k == A_SLOT else CELL_H
            canvas = [[0] * nw for _ in range(nh)]
            for r, line in enumerate(rows):
                for c, t in enumerate(line):
                    if t == '.':
                        continue
                    for dy in range(SCALE):
                        for dx in range(SCALE):
                            y, x = r * SCALE + dy, c * SCALE + dx
                            if y < nh:
                                canvas[y][x] = TONE[t]
            nps1 += 1
        else:
            # simbolo sem glifo no PS1: o do PC, descido para a linha de base do PS1
            nw, nh = w, CELL_H
            canvas = [[0] * nw for _ in range(nh)]
            sh = PS1_BASE - PC_BASE
            for r in range(h):
                for c in range(w):
                    y = r + sh
                    if 0 <= y < nh:
                        canvas[y][c] = d[sprg_a + ptr + r * 256 + c]
            if k == A_SLOT:
                canvas = canvas[:A_H]
                nh = A_H
            nfall += 1
        p = alloc(nw, nh)
        off = p - page0 * PAGE
        for r in range(nh):
            row = canvas[r]
            new[off + r * 256:off + r * 256 + nw] = bytes(row)
        struct.pack_into('<IBBH', d, sprx_a + 8 * i, p, nw, nh, pad)

    # SPRG cresce: o tamanho do chunk muda e os chunks seguintes andam (nao ha' offsets absolutos no arquivo)
    end = sprg_a + sprg_n
    struct.pack_into('<I', d, sprg_a - 4, sprg_n + len(new))
    out = bytes(d[:end]) + bytes(new) + bytes(d[end:])
    open(dst, 'wb').write(out)
    print('ps1_font: %d glifos do PS1, %d do PC (sem equivalente), +%d KB no SPRG' % (nps1, nfall, len(new) // 1024))


if __name__ == '__main__':
    main()
