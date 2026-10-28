/* Renderer N64 (etapa 3): desenha direto no RDP, via rdpq, a lista de comandos que o motor gta2_re gera.
 *
 * O motor ja projeta tudo para o espaco de tela (x,y em 640x480), entao nao ha 3D aqui: so triangulos 2D
 * com cor Gouraud + textura. As texturas do GTA 2 sao 8 bits com paleta (paginas de 256 px); aqui cada textura
 * e convertida SOB DEMANDA para RGBA16, reduzida com MEDIA POR AREA (nao por amostragem pontual) para ate 2048
 * texels (= os 4 KB inteiros da TMEM), e guardada num cache simples. A cor 0 e' transparente; as bordas
 * transparentes herdam a cor dos vizinhos para o filtro bilinear nao escurecer contornos.
 *
 * "Laboratorio" de render (botoes do controle, edge-triggered):
 *   L        liga/desliga filtro bilinear
 *   R        perspectiva: 0 desligada, 1 ligada em tudo (w do motor), 2 ligada so nos triangulos com w bem diferente
            entre os vertices (razao >= PERSP_MIN_RATIO: postes, paredes), chao fica sem perspectiva.
 *   C-cima   depuracao: 0 normal, 1 esconde translucidos, 2 so translucidos, 3 cor chapada por textura,
            4 cor pela razao de w do triangulo (verde <1.1, amarelo <1.25, laranja <1.5, vermelho >=1.5)
 *
 * Indicadores: barra de RAM no topo, quadradinho piscando (loop vivo), botoes do joypad (embaixo a esquerda),
 * estado das opcoes (embaixo a direita: filtro, perspectiva, depuracao).
 */
#include <libdragon.h>
#include <malloc.h>
#include <unistd.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "render/n64tex.h"
#include <rdpq_tex.h>
#include <rdpq_text.h>
#include <rdpq_font.h>
extern const char *gta2_frame_text_pool(void); /* frame_n64.cpp: frases do quadro (comando kind 2) */

typedef struct { float x, y, w, u, v; uint32_t rgba; } FVert;                              /* = FrameVertex (render/frame.h) */
typedef struct { uint32_t kind, texture, blend, first, count, colour; int32_t rect[4]; } FCmd; /* = FrameCommand */

#define CACHE_N     N64_CACHE_N   /* captura no desenho: copias do mesmo quadro precisam coexistir (192 x 4 KB = 768 KB; pico medido ~135 em uso) */
#define SLOT_U16    2048          /* 4096 bytes por entrada = TMEM inteira em RGBA16 */
#define MAX_TEXELS  2048

typedef struct { const N64Tex *owner; uint16_t gen, tw, th, pw; uint32_t used; float req; uint8_t empty, add; } CEnt; /* empty = nenhum texel visivel */ /* used = g_frames+1 do ultimo uso (0 = nunca); req = escala pedida na conversao */
#define SCALE_MIN   0.25f  /* menor reducao aceita por escala de tela */
#define SCALE_UP    1.25f  /* reconverte (maior) se a textura passar a aparecer 25% maior na tela */
#define CACHE_GUARD 3  /* quadros que um slot fica protegido apos o uso: o RDP le a textura depois, de forma assincrona */
static CEnt g_c[CACHE_N];
static uint16_t *g_cbuf;
static int g_next;
static int g_active;
static uint32_t g_frames;

/* Blocos de upload por slot (ver N64_UPLOAD_BLOCKS mais abaixo). Liberacao ADIADA: um bloco ja pode estar na fila do RSP/RDP, entao
 * ele so e liberado 4 quadros depois de largado (ou apos rspq_wait se a lista encher). Chamado ao reconverter o slot e quando a textura
 * dona e' esquecida (troca de fase), para nao deixar centenas de blocos pequenos espalhados pelo heap (fragmentacao). */
#define UBLK_PEND_N 64
static rspq_block_t *g_ublk_pend[UBLK_PEND_N];
static uint32_t g_ublk_pend_f[UBLK_PEND_N];
static int g_ublk_npend;
static rspq_block_t *g_ublk[CACHE_N];
static uint8_t g_ublk_n[CACHE_N];
static int g_ublk_live;
static void ublk_release(int slot)
{
    rspq_block_t *b = g_ublk[slot];
    g_ublk_n[slot] = 0;
    if (!b) return;
    g_ublk[slot] = NULL;
    --g_ublk_live;
    if (g_ublk_npend >= UBLK_PEND_N) {
        rspq_wait();
        for (int i = 0; i < g_ublk_npend; ++i) rspq_block_free(g_ublk_pend[i]);
        g_ublk_npend = 0;
    }
    g_ublk_pend[g_ublk_npend] = b;
    g_ublk_pend_f[g_ublk_npend] = g_frames;
    ++g_ublk_npend;
}
/* 1 se ha memoria folgada para gravar um bloco (o rdpq aborta o jogo com 'Out of memory' se faltar no meio da gravacao) */
/* RESERVA para o motor: os blocos de upload/texto sao so cache de velocidade e nao podem comer a folga que o jogo precisa para alocar
 * em tempo de execucao (texturas sob demanda, scripts, sons): sem isso o heap enchia aos poucos e o motor abortava com 'Out of memory'. */
#ifndef N64_HEAP_RESERVE_KB
#define N64_HEAP_RESERVE_KB 320
#endif
#define UBLK_MAX_LIVE 128 /* blocos de upload vivos ao mesmo tempo (~2,4 KB cada) */
extern char *__heap_top;
static int heap_has_room(void)
{
    if ((long)(__heap_top - (char *)sbrk(0)) < (long)N64_HEAP_RESERVE_KB * 1024) return 0; /* a folga ainda nao pedida ao sistema (apos o fim do heap atual) */
    void *p = malloc(24 * 1024);
    if (!p) return 0;
    free(p);
    return 1;
}
static void ublk_reap(void)
{
    for (int i = 0; i < g_ublk_npend; ) {
        if ((uint32_t)(g_frames - g_ublk_pend_f[i]) >= 4u) {
            rspq_block_free(g_ublk_pend[i]);
            --g_ublk_npend;
            g_ublk_pend[i] = g_ublk_pend[g_ublk_npend];
            g_ublk_pend_f[i] = g_ublk_pend_f[g_ublk_npend];
        } else ++i;
    }
}

/* opcoes do laboratorio */
static int g_opt_filter = 1;
static int g_opt_persp = 1;                    /* perspectiva SEMPRE ligada em tudo (modo 1); nao ha mais como alterar */
static int g_dbg = 0;

/* audio (audio_n64.c) */
extern void gta2_n64_audio_init(void);
extern void gta2_n64_audio_poll(void);

int gta2_n64_gfx_active(void) { return g_active; }

void gta2_n64_render_toggle(int which)
{
    if (which == 0) g_opt_filter = !g_opt_filter;
    else if (which == 1) { /* perspectiva fixa ligada */ }
    else if (which == 2) g_dbg = (g_dbg + 1) % 5;
}

void gta2_n64_tex_forget(const N64Tex *t)
{
    if (t && t->slot >= 0 && t->slot < CACHE_N && g_c[t->slot].owner == t) { g_c[t->slot].owner = 0; ublk_release(t->slot); }
}

/* devolve a entrada do cache para a textura (convertendo se preciso); -1 se nao ha dados */
/* scale = pixels de tela por texel (<= 1): a textura e' reduzida com media por area para o tamanho em que aparece */
/* Leitura da ROM (pixels e paletas mapeados na cartucha): a CPU so le a ROM direito em palavras de 32 bits
 * alinhadas, e nunca durante um DMA do PI. Byte solto volta da posicao errada da palavra (sprites quebrados).
 * io_read() da libdragon espera o PI ficar livre e le 32 bits; daqui os bytes sao extraidos na posicao certa. */
static inline int in_rom(const void *p) { const uintptr_t a = (uintptr_t)p; return a >= 0xB0000000u && a < 0xC0000000u; }
static uint8_t g_srcbuf[256 * 256 + 16] __attribute__((aligned(16))); /* copia em RAM das linhas de origem (stride 256, como as paginas do .sty) */
/* copia len bytes de src (ROM ou RAM) para dst; na ROM le so palavras de 32 bits alinhadas, via io_read */
void gta2_n64_rom_copy(void *dst, const void *src, unsigned len)
{
    if (!in_rom(src)) { memcpy(dst, src, len); return; }
    uint8_t *o = (uint8_t *)dst;
    const uint32_t pi = (uint32_t)((uintptr_t)src & 0x1FFFFFFFu);
    uint32_t p = pi & ~3u;
    unsigned k = 0;
    int j = (int)(pi & 3u);
    while (k < len) {
        const uint32_t v = io_read(p);
        for (; j < 4 && k < len; ++j) o[k++] = (uint8_t)(v >> (24 - 8 * j));
        j = 0;
        p += 4;
    }
}

/* le 32 bits big-endian de QUALQUER endereco da ROM (fisico). io_read exige alinhamento de 4 bytes, e os arquivos do DragonFS
 * so ficam alinhados a 2 bytes: a posicao do .sty na ROM muda quando outros arquivos entram/saem (ex.: frontend removido). */
static inline uint32_t rom_rd32_any(uint32_t p)
{
    const uint32_t k = p & 3u;
    if (k == 0) return io_read(p);
    const uint32_t a = p - k;
    const uint32_t lo = io_read(a), hi = io_read(a + 4);
    return (lo << (8 * k)) | (hi >> (32 - 8 * k));
}

static const uint8_t *src_pixels(const N64Tex *t, int w, int h)
{
    if (!in_rom(t->pix)) return t->pix;
    if (w > 256) w = 256;
    if (h > 256) h = 256;
    /* DMA do PI em vez de io_read palavra por palavra. O destino fica com o mesmo alinhamento (mod 8) da ROM,
     * exigencia do dma_read (paridade) e caminho mais rapido. Linhas com stride 256 na ROM e na copia. */
    const uint32_t pi = (uint32_t)((uintptr_t)t->pix & 0x1FFFFFFFu);
    uint8_t *base = g_srcbuf + (pi & 7u);
    const unsigned span = (unsigned)(h - 1) * 256u + (unsigned)w; /* do 1o byte da 1a linha ao ultimo da ultima */
    data_cache_hit_writeback_invalidate(base, span); /* a CPU nao pode ter linhas antigas desse trecho em cache */
    if (w >= 192) {
        dma_read(base, pi, span); /* linhas quase inteiras: um DMA so */
    } else {
        for (int y = 0; y < h; ++y) dma_read(base + y * 256, pi + (uint32_t)y * 256u, (unsigned long)w); /* so os w bytes de cada linha */
    }
    return base;
}

/* media sem divisao: inverso em ponto fixo 8.24. Testado para todas as somas possiveis (0..255*n):
 * resultado identico a s/n para n <= 266; acima disso usa a divisao normal. */
#define INV_N 267
static uint32_t g_inv[INV_N];
static inline unsigned avg_div(unsigned s, unsigned n)
{
    if (n < INV_N) {
        if (!g_inv[1]) for (unsigned i = 1; i < INV_N; ++i) g_inv[i] = ((1u << 24) + i - 1) / i;
        return (unsigned)(((uint64_t)s * g_inv[n]) >> 24);
    }
    return s / n;
}
/* ultima paleta lida da ROM (na ROM ela nunca muda): evita 256 leituras do PI quando a proxima textura usa a mesma */
#define PAL_CACHE 16 /* paletas da ROM ja lidas (cada leitura = 256 acessos ao barramento da cartucha) */
static struct { const uint32_t *src; uint32_t rgb[256]; } g_palc[PAL_CACHE];
static int g_palc_next;

/* additive: textura desenhada com mistura aditiva no PC. No N64 ela vira transparencia comum (o RDP estoura a soma),
 * entao os pixels quase pretos -- que na soma nao apareciam -- viram transparentes na conversao. */
#define ADD_CUT 48u /* r+g+b abaixo disto = "preto" de textura aditiva */
/* UPLOAD EM BLOCO: o upload de cada slot (rdpq_tex_upload, ~40 us de CPU cada, ~130 por quadro) e' gravado uma vez num bloco do RSP
 * e depois so' executado (1 comando). Grava na 2a vez que o slot e' usado com o mesmo conteudo; o bloco e' liberado quando o slot e'
 * reconvertido (o slot so' e' reaproveitado apos CACHE_GUARD quadros sem uso, entao o RDP ja nao o executa). 0 = desliga (volta ao antigo). */
#ifndef N64_UPLOAD_BLOCKS
#define N64_UPLOAD_BLOCKS 1
#endif
static int s_rec;                                /* 1 enquanto grava um bloco maior (texto estatico): nao se aninha gravacao */
static uint32_t g_slot_ver[CACHE_N];            /* sobe a cada conversao: identifica o conteudo atual do slot (para pular o upload) */
extern const uint32_t *gta2_n64_palette_ptr(int id);
/* textura sem dados (sem descritor / pixels / paleta, ou tamanho 0): desenha sem textura */
static int texfail(const N64Tex *t, int why)
{
    (void)t; (void)why;
    return -1;
}
/* N64: the texture cache (768 KB) lives for the whole game; allocated at boot (main_n64.c) so it sits at the bottom of the
 * heap instead of in the middle of the first level's memory, where it split the heap for every later level load */
void gta2_n64_render_prealloc(void)
{
    if (!g_cbuf) {
        g_cbuf = (uint16_t *)memalign(64, (size_t)CACHE_N * SLOT_U16 * 2);
        if (g_cbuf) memset(g_c, 0, sizeof(g_c));
    }
}

static int tex_slot(N64Tex *t, float scale, int additive)
{
    if (!t) return texfail(t, 0);
    if (t->palid >= 0) { /* a paleta pode ter sido registrada (ou trocada) depois da textura */
        const uint32_t *cur = gta2_n64_palette_ptr(t->palid);
        if (cur && cur != t->pal) {
            t->pal = cur;
            if (t->slot >= 0 && t->slot < CACHE_N && g_c[t->slot].owner == t) g_c[t->slot].owner = 0;
        }
    }
    if (!t->pix) return texfail(t, 1);
    if (!t->pal) return texfail(t, 2);
    if (t->w == 0 || t->h == 0) return texfail(t, 3);
    if (t->slot >= 0 && g_c[t->slot].owner == t && g_c[t->slot].gen == t->gen && g_c[t->slot].add != (uint8_t)additive)
        g_c[t->slot].owner = 0; /* mesma textura pedida no outro modo (aditivo x normal): reconverte */
    if (t->slot >= 0 && g_c[t->slot].owner == t && g_c[t->slot].gen == t->gen) {
        if (scale <= g_c[t->slot].req * SCALE_UP) { g_c[t->slot].used = g_frames + 1; return t->slot; }
        g_c[t->slot].owner = 0; /* aparece maior que na conversao: reconverte com mais resolucao (so cresce) */
    }
    if (!g_cbuf) {
        g_cbuf = (uint16_t *)memalign(64, (size_t)CACHE_N * SLOT_U16 * 2);
        if (!g_cbuf) return -1;
        memset(g_c, 0, sizeof(g_c));
    }
    const int w = t->w, h = t->h;
    int tw = w, th = h;
    if (scale < 0.95f) { /* reduz para o tamanho na tela (sprites: ~metade; blocos: ~44 de 64) */
        tw = (int)((float)w * scale + 0.5f);
        th = (int)((float)h * scale + 0.5f);
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;
    }
    if (tw * th > MAX_TEXELS) { /* limite da TMEM (4 KB em RGBA16) */
        const float f = sqrtf((float)MAX_TEXELS / (float)(tw * th));
        tw = (int)((float)tw * f) & ~3;
        if (tw < 4) tw = 4;
        th = (int)((float)th * f + 0.5f);
        if (th < 1) th = 1;
    }
    if (tw > w) tw = w;
    if (th > h) th = h;
    if (tw > 256) tw = 256; /* limite do vetor xs[] (paginas do .sty tem 256 px) */
    int pw = (tw + 3) & ~3;
    while (pw * th > MAX_TEXELS && th > 1) --th;

    /* escolhe um slot que o RDP nao vai mais ler: nunca usado ou sem uso nos ultimos CACHE_GUARD quadros */
    int slot = -1;
    for (int i = 0; i < CACHE_N; ++i) {
        const int s = (g_next + i) % CACHE_N;
        if (g_c[s].used == 0 || (g_frames + 1) - g_c[s].used >= CACHE_GUARD) { slot = s; break; }
    }
    if (slot < 0) { /* todos em uso recente: usa um que nao foi capturado NESTE quadro, depois de esperar o RDP */
        for (int i = 0; i < CACHE_N; ++i) {
            const int s = (g_next + i) % CACHE_N;
            if (g_c[s].used != g_frames + 1) { slot = s; break; }
        }
        if (slot < 0) { return -1; } /* cache cheio so com texturas deste quadro: desenha sem textura */

        rspq_wait();
    }
    g_next = (slot + 1) % CACHE_N;
    uint16_t *dst = g_cbuf + (size_t)slot * SLOT_U16;

    uint32_t prgb_ram[256];
    const uint32_t *prgb;
    const uint32_t *pal = t->pal;
    if (in_rom(pal)) {
        int pk = -1;
        for (int i = 0; i < PAL_CACHE; ++i) if (g_palc[i].src == pal) { pk = i; break; }
        if (pk < 0) {
            pk = g_palc_next;
            g_palc_next = (pk + 1) % PAL_CACHE;
            for (int i = 0; i < 256; ++i) g_palc[pk].rgb[i] = rom_rd32_any((uint32_t)((uintptr_t)&pal[i * 64] & 0x1FFFFFFFu));
            g_palc[pk].src = pal;

        }
        prgb = g_palc[pk].rgb;
    } else {
        for (int i = 0; i < 256; ++i) prgb_ram[i] = pal[i * 64]; /* na RAM o jogo pode alterar a paleta: le sempre */
        prgb = prgb_ram;
    }
    const uint8_t *src = src_pixels(t, w, h);
    uint16_t xs[260];
    for (int x = 0; x <= tw; ++x) xs[x] = (uint16_t)(x * w / tw);

    int any_clear = 0, any_opaque = 0;
    if (tw == w && th == h) { /* sem reducao: cada texel e' so uma consulta numa tabela de 256 entradas (resultado identico ao caminho geral) */
        uint16_t p16[256];
        p16[0] = 0;
        for (int i = 1; i < 256; ++i) {
            const uint32_t c = prgb[i];
            const unsigned r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
            p16[i] = ((additive && r + g + b < ADD_CUT) || (i == 1 && t->key1)) ? 0 : (uint16_t)(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1);
        }
        unsigned orv = 0, andv = 1;
        for (int y = 0; y < th; ++y) {
            const uint8_t *row = src + (size_t)y * 256;
            uint16_t *o = dst + y * pw;
            for (int x = 0; x < tw; ++x) {
                const uint16_t v = p16[row[x]];
                o[x] = v;
                orv |= v;
                andv &= v;
            }
            for (int x = tw; x < pw; ++x) o[x] = 0;
        }
        any_opaque = (orv & 1) != 0;
        any_clear = !(andv & 1);

        goto conv_done_loop;
    }
    for (int y = 0; y < th; ++y) {
        const int y0 = y * h / th;
        int y1 = (y + 1) * h / th;
        if (y1 <= y0) y1 = y0 + 1;
        uint16_t *o = dst + y * pw;
        for (int x = 0; x < tw; ++x) {
            const int x0 = xs[x];
            int x1 = xs[x + 1];
            if (x1 <= x0) x1 = x0 + 1;
            unsigned sr = 0, sg = 0, sb = 0, n = 0;
            for (int yy = y0; yy < y1; ++yy) {
                const uint8_t *row = src + (size_t)yy * 256;
                for (int xx = x0; xx < x1; ++xx) {
                    const uint8_t idx = row[xx];
                    if (idx && !(idx == 1 && t->key1)) {
                        const uint32_t c = prgb[idx];
                        sr += (c >> 16) & 255; sg += (c >> 8) & 255; sb += c & 255; ++n;
                    }
                }
            }
            const unsigned area = (unsigned)((x1 - x0) * (y1 - y0));
            unsigned r = 0, g = 0, b = 0;
            int vis = (n && n * 3 >= area);
            if (vis) {
                r = avg_div(sr, n); g = avg_div(sg, n); b = avg_div(sb, n);
                if (additive && r + g + b < ADD_CUT) vis = 0;
            }
            if (vis) {
                o[x] = (uint16_t)(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1);
                any_opaque = 1;
            } else {
                o[x] = 0;
                any_clear = 1;
            }
        }
        for (int x = tw; x < pw; ++x) o[x] = 0;
    }
conv_done_loop:
    /* texels transparentes herdam a cor de um vizinho opaco (alfa continua 0): o bilinear nao escurece as bordas */
    if (any_clear) {
        for (int y = 0; y < th; ++y) {
            uint16_t *o = dst + y * pw;
            for (int x = 0; x < tw; ++x) {
                if (o[x] & 1) continue;
                uint16_t nb = 0;
                if (x > 0 && (o[x - 1] & 1)) nb = o[x - 1];
                else if (x + 1 < tw && (o[x + 1] & 1)) nb = o[x + 1];
                else if (y > 0 && (dst[(y - 1) * pw + x] & 1)) nb = dst[(y - 1) * pw + x];
                else if (y + 1 < th && (dst[(y + 1) * pw + x] & 1)) nb = dst[(y + 1) * pw + x];
                if (nb) o[x] = (uint16_t)(nb & ~1);
            }
        }
    }
    data_cache_hit_writeback(dst, (unsigned long)pw * th * 2);
    ++g_slot_ver[slot];
    ublk_release(slot);
    g_c[slot].owner = t; g_c[slot].gen = t->gen; g_c[slot].tw = (uint16_t)tw; g_c[slot].th = (uint16_t)th; g_c[slot].pw = (uint16_t)pw;
    g_c[slot].used = g_frames + 1;
    g_c[slot].req = scale;
    g_c[slot].empty = (uint8_t)!any_opaque;
    g_c[slot].add = (uint8_t)additive;
    t->slot = (int16_t)slot;
    return slot;
}

static float SCR_W = 320.0f, SCR_H = 240.0f; /* ajustado em present() pelo framebuffer real (libdragon: 640x240) */

static const rdpq_trifmt_t kFmtTex = { .pos_offset = 0, .shade_offset = 2, .tex_offset = 6, .z_offset = -1 };
/* so textura, sem cor por vertice: para comandos com os vertices todos brancos (o RDP nao calcula gradiente de cor) */
static const rdpq_trifmt_t kFmtTexOnly = { .pos_offset = 0, .shade_offset = -1, .tex_offset = 6, .z_offset = -1 };

/* escala do comando: maior razao (pixels de tela / texels) entre os triangulos, limitada a [SCALE_MIN, 1] */
static float cmd_scale(const FCmd *cm, const FVert *verts, const N64Tex *t, float sx, float sy)
{
    if (!t || t->w == 0 || t->h == 0) return 1.0f;
    const float texels = (float)t->w * (float)t->h;
    float best = 0.0f;
    for (uint32_t i = 0; i + 2 < cm->count; i += 3) {
        const FVert *p = &verts[cm->first + i];
        const float ax = (p[1].x - p[0].x) * sx, ay = (p[1].y - p[0].y) * sy;
        const float bx = (p[2].x - p[0].x) * sx, by = (p[2].y - p[0].y) * sy;
        const float a_scr = fabsf(ax * by - bx * ay);
        const float a_tex = fabsf((p[1].u - p[0].u) * (p[2].v - p[0].v) - (p[2].u - p[0].u) * (p[1].v - p[0].v)) * texels;
        if (a_scr < 1.0f || a_tex < 1.0f || !(a_scr < 4.0e6f)) continue;
        const float r = sqrtf(a_scr / a_tex);
        if (r > best) best = r;
    }
    if (best <= 0.0f) return 1.0f;
    if (best > 1.0f) best = 1.0f;
    if (best < SCALE_MIN) best = SCALE_MIN;
    return best;
}

/* Captura no momento do desenho (chamada por gbh_n64.cpp/Draw): converte a textura AGORA, antes que o motor
 * reaproveite pixels/paleta/tamanho para outro sprite no mesmo quadro, e devolve um token do slot (0 = sem textura).
 * a_scr: area do triangulo na tela de referencia (640x480); a_tex: area em texels. */
#define TEX_TOKEN_BASE 0x10000u
static float g_last_sx = 1.0f, g_last_sy = 0.5f; /* escala tela-referencia -> framebuffer do ultimo present */
/* 1 se o token aponta para uma textura sem nenhum texel visivel (o desenho nao mostraria nada em nenhum modo de mistura) */
int gta2_n64_tex_token_empty(uint32_t token)
{
    if (token < TEX_TOKEN_BASE || token >= TEX_TOKEN_BASE + CACHE_N) return 0;
    return g_c[token - TEX_TOKEN_BASE].empty;
}
uint32_t gta2_n64_tex_capture(N64Tex *t, float a_scr, float a_tex, int additive)
{
    if (!t) return 0;
    float scale = 1.0f;
    if (a_scr > 0.0f && a_tex >= 1.0f) {
        scale = sqrtf(a_scr * g_last_sx * g_last_sy / a_tex);
        if (scale > 1.0f) scale = 1.0f;
        if (scale < SCALE_MIN) scale = SCALE_MIN;
    }
    const int slot = tex_slot(t, scale, additive);
    return slot < 0 ? 0u : TEX_TOKEN_BASE + (uint32_t)slot;
}

/* ---- texto com a fonte embutida da libdragon (visual de jogo de N64; uma passada por frase, nao por letra) ----
 * Fonte: FONT_BUILTIN_DEBUG_MONO (ASCII 8x8 com contorno, CC0). Estilo 0 = amarelo, estilo 1 = azul (palavras marcadas
 * com # no texto do GTA 2). O framebuffer e' 640x240 (pixel 1:2), entao esta fonte quadrada aparece alta na tela. */
#define NFONT_ID 1
static rdpq_font_t *g_nfont;
static float g_nf_asc, g_nf_lh; /* ascendente e altura de linha da fonte, em pixels do framebuffer (medidos no 1o uso) */

#ifndef N64_NFONT
#define N64_NFONT 0 /* fonte embutida do N64 desligada: usa as letras do GTA 2 */
#endif
__attribute__((unused)) static void nfont_init(void)
{
    g_nfont = __rdpq_font_load_builtin_1();
    if (!g_nfont) return;
    const rdpq_fontstyle_t s0 = { .color = RGBA32(255, 214, 64, 255), .outline_color = RGBA32(0, 0, 0, 255) };
    const rdpq_fontstyle_t s1 = { .color = RGBA32(110, 200, 255, 255), .outline_color = RGBA32(0, 0, 0, 255) };
    rdpq_font_style(g_nfont, 0, &s0);
    rdpq_font_style(g_nfont, 1, &s1);
    rdpq_text_register_font(NFONT_ID, g_nfont);
}

int gta2_n64_nfont_ready(void) { return g_nfont != NULL; }

/* kind 2: frase em ASCII (com ^00/^01), posicao do topo do bloco em 640x480, altura de linha do GTA 2 em 640x480 */
static void nfont_draw(const FCmd *cm, float sx, float sy)
{
    if (!g_nfont || cm->count == 0) return;
    if (g_nf_lh <= 0.0f) {
        rdpq_font_gmetrics_t ga, gg;
        const int oka = rdpq_font_get_glyph_metrics(g_nfont, 'A', &ga);
        const int okg = rdpq_font_get_glyph_metrics(g_nfont, 'g', &gg);
        g_nf_asc = oka ? -(float)ga.y0 : 8.0f;
        const float desc = okg ? (float)gg.y1 : 2.0f;
        if (g_nf_asc < 4.0f || g_nf_asc > 16.0f) g_nf_asc = 8.0f;
        g_nf_lh = g_nf_asc + desc + 1.0f;
        if (g_nf_lh < 8.0f || g_nf_lh > 24.0f) g_nf_lh = 11.0f;
    }
    const char *s = gta2_frame_text_pool() + cm->first;
    const int n = (int)cm->colour > 0 ? (int)cm->colour : 1; /* linhas */
    const float x = (float)cm->rect[0] * (1.0f / 256.0f) * sx;
    const float ytop = (float)cm->rect[1] * (1.0f / 256.0f) * sy;
    const float lineref = (float)cm->rect[2] * (1.0f / 256.0f);
    const float bottom = ytop + (float)n * lineref * sy;
    float base1;
    if (bottom > SCR_H * 0.85f) { /* texto de rodape (briefing): ancora a ultima linha onde o GTA 2 a poria */
        const float last = bottom - 2.0f;
        base1 = last - (float)(n - 1) * g_nf_lh;
    } else {
        base1 = ytop + g_nf_asc;
    }
    if (base1 + (float)(n - 1) * g_nf_lh + 3.0f > SCR_H) base1 = SCR_H - 3.0f - (float)(n - 1) * g_nf_lh; /* cabe na tela */
    if (base1 < g_nf_asc) base1 = g_nf_asc;
    rdpq_text_printn(NULL, NFONT_ID, x, base1, s, (int)cm->count);
}

uint32_t gta2_n64_frames(void) { return g_frames; }

static inline double ticks_ms(uint64_t t) { return (double)t * 1000.0 / (double)TICKS_PER_SECOND; }

static int s_persp = -1; /* perspectiva do RDP no momento (-1 = desconhecida): ligada so nos triangulos que precisam; retangulo de textura exige desligada */

/* 6 vertices consecutivos (2 triangulos) que formam um retangulo alinhado aos eixos, sem rotacao nem perspectiva e com a
 * textura sem espelhar: desenha com UM comando rdpq_texture_rectangle em vez de 2 rdpq_triangle (texto, HUD, rostos, placas).
 * Retorna 1 se tratou os 6 vertices (desenhou ou descartou por estar fora da tela), 0 se nao e' um retangulo (use triangulos). */
static int try_texrect(const FVert *p, float sx, float sy, float ts, float tt)
{
    float X[6], Y[6], minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f, wmin = 1e30f, wmax = 0.0f;
    for (int i = 0; i < 6; ++i) {
        X[i] = p[i].x * sx; Y[i] = p[i].y * sy;
        if (!(X[i] > -1500.0f && X[i] < 1500.0f && Y[i] > -1500.0f && Y[i] < 1500.0f)) return 0;
        if (X[i] < minx) minx = X[i];
        if (X[i] > maxx) maxx = X[i];
        if (Y[i] < miny) miny = Y[i];
        if (Y[i] > maxy) maxy = Y[i];
        if (p[i].w > wmax) wmax = p[i].w;
        if (p[i].w < wmin) wmin = p[i].w;
    }
    if (maxx - minx < 0.25f || maxy - miny < 0.25f) return 0;
    if (wmin > 0.0f && wmax < 1e20f && wmax > wmin * 1.001f) return 0; /* perspectiva de verdade: so triangulos */
    const float ex = 0.02f;
    float cu[4] = {0}, cv[4] = {0};
    int seen = 0;
    for (int i = 0; i < 6; ++i) {
        int col, row;
        if (fabsf(X[i] - minx) < ex) col = 0; else if (fabsf(X[i] - maxx) < ex) col = 1; else return 0;
        if (fabsf(Y[i] - miny) < ex) row = 0; else if (fabsf(Y[i] - maxy) < ex) row = 1; else return 0;
        const int c = col + 2 * row; /* 0 = topo-esq, 1 = topo-dir, 2 = base-esq, 3 = base-dir */
        if (seen & (1 << c)) {
            if (fabsf(cu[c] - p[i].u) > 0.002f || fabsf(cv[c] - p[i].v) > 0.002f) return 0;
        } else {
            cu[c] = p[i].u; cv[c] = p[i].v; seen |= 1 << c;
        }
    }
    if (seen != 15) return 0;
    const float ue = 0.002f;
    if (fabsf(cu[0] - cu[2]) > ue || fabsf(cu[1] - cu[3]) > ue || fabsf(cv[0] - cv[1]) > ue || fabsf(cv[2] - cv[3]) > ue) return 0; /* girado/torto */
    if (!(cu[1] > cu[0] + 0.0005f) || !(cv[2] > cv[0] + 0.0005f)) return 0; /* espelhado: deixa para os triangulos */
    if (maxx < 0.0f || minx > SCR_W || maxy < 0.0f || miny > SCR_H) { return 1; }
    float s0 = cu[0] * ts, s1 = cu[1] * ts, t0 = cv[0] * tt, t1 = cv[2] * tt;
    const float dsdx = (s1 - s0) / (maxx - minx), dtdy = (t1 - t0) / (maxy - miny);
    float x0 = minx, x1 = maxx, y0 = miny, y1 = maxy;
    if (x0 < 0.0f) { s0 += (0.0f - x0) * dsdx; x0 = 0.0f; }
    if (y0 < 0.0f) { t0 += (0.0f - y0) * dtdy; y0 = 0.0f; }
    if (x1 > SCR_W) { s1 -= (x1 - SCR_W) * dsdx; x1 = SCR_W; }
    if (y1 > SCR_H) { t1 -= (y1 - SCR_H) * dtdy; y1 = SCR_H; }
    if (x1 - x0 < 0.25f || y1 - y0 < 0.25f) { return 1; }
    if (s_persp != 0) { rdpq_mode_persp(0); s_persp = 0; } /* retangulo + perspectiva ligada saia chapado */
    rdpq_texture_rectangle_scaled(TILE0, x0, y0, x1, y1, s0, t0, s1, t1);

    return 1;
}

/* ---- desenho de um comando (usado direto e dentro de blocos gravados do RSP) ---- */
static int s_valid, s_tex = -1, s_blend = -1;      /* modo ja configurado no RDP */
static int s_up_slot = -1; static uint32_t s_up_ver; /* textura que ja esta na TMEM (slot + versao): evita reenviar */
#define PERSP_MIN_RATIO 1.25f
#define DBG_FLAT (g_dbg == 3 || g_dbg == 4)

/* kind 3: imagem do menu (gbh_BlitImage). A copia reduzida na metade (RGBA5551, RAM) vem de gbh_n64.cpp; rdpq_tex_blit divide em
 * pedacos que cabem na TMEM. Origem em pixels da imagem inteira (cm->rect), destino (cm->first, cm->count) no espaco 640x480. */
extern const uint16_t *gta2_n64_image_get(int idx, int *w, int *h);
static void blit_img(const FCmd *cm, float sx, float sy)
{
    int hw = 0, hh = 0;
    const uint16_t *px = gta2_n64_image_get((int)cm->texture, &hw, &hh);
    if (!px || hw <= 0 || hh <= 0) return;
    const int l = cm->rect[0], t = cm->rect[1], r = cm->rect[2], b = cm->rect[3];
    if (r <= l || b <= t) return;
    const int s0 = l / 2, t0 = t / 2;
    int sw = (r - l + 1) / 2, sh = (b - t + 1) / 2;
    if (s0 + sw > hw) sw = hw - s0;
    if (t0 + sh > hh) sh = hh - t0;
    if (sw <= 0 || sh <= 0) return;
    surface_t surf = surface_make((void *)px, FMT_RGBA16, (uint16_t)hw, (uint16_t)hh, (uint16_t)(hw * 2));
    rdpq_set_mode_standard();
    rdpq_mode_filter(FILTER_BILINEAR);
    rdpq_mode_combiner(RDPQ_COMBINER_TEX);
    rdpq_blitparms_t bp;
    memset(&bp, 0, sizeof(bp));
    bp.s0 = s0; bp.t0 = t0; bp.width = sw; bp.height = sh;
    bp.scale_x = (float)(r - l) * sx / (float)sw;
    bp.scale_y = (float)(b - t) * sy / (float)sh;
    bp.filtering = true;
    rdpq_tex_blit(&surf, (float)(int32_t)cm->first * sx, (float)(int32_t)cm->count * sy, &bp);
}

static void draw_cmd(const FCmd *cm, const FVert *verts, float sx, float sy, int IW, int IH)
{
    if (cm->kind == 1) { /* limpar retangulo */
        int x0 = (int)(cm->rect[0] * sx), y0 = (int)(cm->rect[1] * sy);
        int x1 = (int)(cm->rect[2] * sx), y1 = (int)(cm->rect[3] * sy);
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > IW) x1 = IW;
        if (y1 > IH) y1 = IH;
        const uint32_t a = cm->colour >> 24; /* 0 = opaque clear; 1..254 = translucent panel (pause menu) */
        if (x1 > x0 && y1 > y0 && a != 0 && a != 255) {
            rdpq_set_mode_standard();
            rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
            rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
            rdpq_set_prim_color(RGBA32(cm->colour & 0xFF, (cm->colour >> 8) & 0xFF, (cm->colour >> 16) & 0xFF, a));
            rdpq_fill_rectangle(x0, y0, x1, y1);
            s_valid = 0; s_up_slot = -1; s_persp = -1;
        } else if (x1 > x0 && y1 > y0) {
            rdpq_set_mode_fill(RGBA32(cm->colour & 0xFF, (cm->colour >> 8) & 0xFF, (cm->colour >> 16) & 0xFF, 255));
            rdpq_fill_rectangle(x0, y0, x1, y1);
            s_valid = 0; s_up_slot = -1; s_persp = -1; /* o modo de preenchimento substituiu o modo de desenho */
        }
        return;
    }
    if (cm->kind == 3) { blit_img(cm, sx, sy); s_valid = 0; s_up_slot = -1; s_persp = -1; return; } /* imagem do menu */
    if (cm->kind == 2) { nfont_draw(cm, sx, sy); s_valid = 0; s_up_slot = -1; s_persp = -1; return; } /* frase com a fonte do N64: o rdpq_text troca o modo do RDP */
    if (cm->kind != 0 || cm->count < 3) return;
    if (g_dbg == 1 && (cm->blend & 0xFFu) != 0) return;
    if (g_dbg == 2 && (cm->blend & 0xFFu) == 0) return;

    int slot;
    if (cm->texture >= TEX_TOKEN_BASE && cm->texture < TEX_TOKEN_BASE + CACHE_N) { /* capturada no desenho */
        slot = DBG_FLAT ? -1 : (int)(cm->texture - TEX_TOKEN_BASE);
        if (slot >= 0) g_c[slot].used = g_frames + 1;
    } else {
        N64Tex *tx = (N64Tex *)(uintptr_t)cm->texture;
        slot = DBG_FLAT ? -1 : tex_slot(tx, cmd_scale(cm, verts, tx, sx, sy), (cm->blend & 0xFFu) == 2);
    }
    if (slot < 0 && (cm->blend & 0xFFu) != 0 && !DBG_FLAT) return; /* sem dados e translucido: pula */

    /* tipo: 0 = sem textura, 1 = textura x cor do vertice, 2 = so textura (todos os vertices brancos e opacos) */
    int is_tex = slot >= 0 ? 1 : 0;
    if (is_tex) {
        int white = 1;
        for (uint32_t k = 0; k < cm->count && white; ++k) {
            const uint32_t d = verts[cm->first + k].rgba;
            if ((d & 0xFFFFFFu) != 0xFFFFFFu || ((cm->blend & 0xFFu) == 1 && (d >> 24) != 0xFFu) || (cm->blend & 0xFFu) == 2) white = 0;
        }
        if (white) is_tex = 2;
    }
    if (!s_valid || is_tex != s_tex || (int)cm->blend != s_blend) {
        rdpq_set_mode_standard();
        rdpq_mode_persp(0); s_persp = 0; /* ligada triangulo a triangulo, so onde o w varia */
        rdpq_mode_filter(g_opt_filter && !(cm->blend & 0x100u) ? FILTER_BILINEAR : FILTER_POINT); /* 0x100 = HUD, pixel-snapped */
        if (is_tex) {
            rdpq_mode_combiner(is_tex == 2 ? RDPQ_COMBINER_TEX : RDPQ_COMBINER_TEX_SHADE);
            rdpq_mode_alphacompare((cm->blend & 0xFFu) == 0 ? 128 : 0);
        } else {
            rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
            rdpq_mode_alphacompare(0);
        }
        if ((cm->blend & 0xFFu) == 1)      rdpq_mode_blender(RDPQ_BLENDER((IN_RGB, IN_ALPHA, MEMORY_RGB, INV_MUX_ALPHA)));
        else if ((cm->blend & 0xFFu) == 2) rdpq_mode_blender(RDPQ_BLENDER((IN_RGB, IN_ALPHA, MEMORY_RGB, INV_MUX_ALPHA))); /* aditivo do PC: o RDP estoura a soma (arco-iris), entao vira transparencia de 75% */
        else                     rdpq_mode_blender(0);
        s_valid = 1; s_tex = is_tex; s_blend = (int)cm->blend;

    }
    float flat_r = 1.0f, flat_g = 1.0f, flat_b = 1.0f;
    if (is_tex) {
        if (s_up_slot == slot && s_up_ver == g_slot_ver[slot]) {
            /* a mesma textura ja esta na TMEM */
        } else {
            const CEnt *e = &g_c[slot];
            surface_t surf = surface_make(g_cbuf + (size_t)slot * SLOT_U16, FMT_RGBA16, e->pw, e->th, e->pw * 2);
#if N64_UPLOAD_BLOCKS
            if (g_ublk[slot] && !s_rec) {
                rspq_block_run(g_ublk[slot]);

            } else if (!s_rec && g_ublk_n[slot] < 255 && ++g_ublk_n[slot] >= 2 && g_ublk_live < UBLK_MAX_LIVE && heap_has_room()) {
                rspq_block_begin();
                rdpq_tex_upload(TILE0, &surf, NULL);
                rspq_block_t *ub = rspq_block_end();
                if (ub) { g_ublk[slot] = ub; ++g_ublk_live; rspq_block_run(ub); }
                else rdpq_tex_upload(TILE0, &surf, NULL);
            } else
#endif
            rdpq_tex_upload(TILE0, &surf, NULL);
            s_up_slot = slot; s_up_ver = g_slot_ver[slot];

        }
    } else if (g_dbg == 3) { /* cor chapada pelo ponteiro da textura */
        const uint32_t hsh = (cm->texture >> 3) * 2654435761u;
        flat_r = 0.25f + (float)((hsh >> 8) & 0xFF) / 340.0f;
        flat_g = 0.25f + (float)((hsh >> 16) & 0xFF) / 340.0f;
        flat_b = 0.25f + (float)((hsh >> 24) & 0xFF) / 340.0f;
    }

    const float ts = slot >= 0 ? (float)g_c[slot].tw : 1.0f, tt = slot >= 0 ? (float)g_c[slot].th : 1.0f;
    for (uint32_t t = 0; t + 2 < cm->count; t += 3) {
        const FVert *p = &verts[cm->first + t];
        if (is_tex == 2 && t + 5 < cm->count) {
            if (try_texrect(p, sx, sy, ts, tt)) { t += 3; continue; } /* +3 do for = 6 vertices */
        }
        float v[3][9];
        int bad = 0;
        float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
        float wmax = 0.0f, wmin = 1e30f;
        for (int k = 0; k < 3; ++k) {
            float x = p[k].x * sx, y = p[k].y * sy;
            if (!(x > -1500.0f && x < 1500.0f && y > -1500.0f && y < 1500.0f)) { bad = 1; break; }
            if (x < minx) minx = x;
            if (x > maxx) maxx = x;
            if (y < miny) miny = y;
            if (y > maxy) maxy = y;
            if (p[k].w > wmax) wmax = p[k].w;
            if (p[k].w < wmin) wmin = p[k].w;
        }
        if (bad || maxx < 0 || minx > SCR_W || maxy < 0 || miny > SCR_H) { continue; }
        const float wr = (wmin > 0.0f && wmax < 1e20f) ? wmax / wmin : 1.0f;
        const float inv_wmax = wmax > 0.0f ? 1.0f / wmax : 0.0f; /* one divide per triangle instead of three */
        const int persp_ok = g_opt_persp && wmin > 0.0f && wmax < 1e20f && (g_opt_persp == 1 || wr >= PERSP_MIN_RATIO);
        float tr = 0.0f, tg = 0.0f, tb = 0.0f; /* depuracao 4: cor pela razao de w */
        if (g_dbg == 4) {
            if (wr < 1.1f)       { tg = 0.9f; }
            else if (wr < 1.25f) { tr = 0.9f; tg = 0.9f; }
            else if (wr < 1.5f)  { tr = 1.0f; tg = 0.5f; }
            else                 { tr = 1.0f; }
        }
        for (int k = 0; k < 3; ++k) {
            const uint32_t d = p[k].rgba;
            v[k][0] = p[k].x * sx; v[k][1] = p[k].y * sy;
            /* multiplies by 1/255 (the compiler keeps a divide otherwise: ~29 cycles each on the VR4300) */
            const float k255 = 1.0f / 255.0f;
            v[k][2] = g_dbg == 4 ? tr : (float)(d & 0xFF) * (k255 * flat_r);
            v[k][3] = g_dbg == 4 ? tg : (float)((d >> 8) & 0xFF) * (k255 * flat_g);
            v[k][4] = g_dbg == 4 ? tb : (float)((d >> 16) & 0xFF) * (k255 * flat_b);
            v[k][5] = ((cm->blend & 0xFFu) == 0) ? 1.0f : ((cm->blend & 0xFFu) == 2) ? 0.75f : (float)(d >> 24) * k255; /* aditivo: o PC ignorava o alfa */
            const float iw = persp_ok ? p[k].w * inv_wmax : 1.0f;  /* normalizado: o que importa e a razao entre vertices */
            const float pm = 1.0f;
            v[k][6] = p[k].u * ts * pm; v[k][7] = p[k].v * tt * pm; v[k][8] = iw;
        }
        if (is_tex) { /* perspectiva so onde o w realmente varia; com w igual o resultado e' o mesmo sem ela */
            const int want = (persp_ok && wr > 1.001f) ? 1 : 0;
            if (want != s_persp) { rdpq_mode_persp(want != 0); s_persp = want; }
        }

        
        if (is_tex == 2)  { rdpq_triangle(&kFmtTexOnly, v[0], v[1], v[2]); }
        else if (is_tex)  rdpq_triangle(&kFmtTex, v[0], v[1], v[2]);
        else              rdpq_triangle(&TRIFMT_SHADE, v[0], v[1], v[2]);
    }
}

/* ---- blocos do RSP para texto estatico (briefing etc.) ----
 * frame_n64.cpp marca os comandos de uma frase recolocada do cache com rect[3] = indice+1 e rect[2] = stamp. Na 1a vez
 * o desenho e' gravado num rspq_block (e executado); nos quadros seguintes so' se executa o bloco: zero trabalho de CPU
 * por letra/upload. O bloco e' refeito se a frase, as opcoes, a escala da tela ou o conteudo de algum slot de textura mudar. */
#define BLK_N 8
#define BLK_DEP 48
typedef struct {
    rspq_block_t *blk;
    uint32_t stamp, nruns, optf, sxb, syb;
    int ndep;
    const N64Tex *down[BLK_DEP];
    uint16_t dslot[BLK_DEP], dgen[BLK_DEP];
} Blk;
static Blk g_blk[BLK_N];

static void run_cmds(const FCmd *cm, int n, const FVert *verts, float sx, float sy, int IW, int IH)
{
    for (int i = 0; i < n; ++i) draw_cmd(&cm[i], verts, sx, sy, IW, IH);
}

static void blk_run(const FCmd *cm, int n, const FVert *verts, float sx, float sy, int IW, int IH)
{
    const uint32_t id = (uint32_t)cm->rect[3] - 1u;
    if (id >= BLK_N || n > BLK_DEP || g_dbg != 0) { run_cmds(cm, n, verts, sx, sy, IW, IH); return; }
    Blk *b = &g_blk[id];
    const uint32_t stamp = (uint32_t)cm->rect[2];
    const uint32_t optf = (g_opt_filter ? 1u : 0u) | ((uint32_t)g_opt_persp << 1);
    uint32_t sxb, syb;
    memcpy(&sxb, &sx, 4);
    memcpy(&syb, &sy, 4);
    int ok = b->blk && b->stamp == stamp && b->nruns == (uint32_t)n && b->optf == optf && b->sxb == sxb && b->syb == syb;
    if (ok) {
        for (int d = 0; d < b->ndep; ++d) {
            const CEnt *e = &g_c[b->dslot[d]];
            if (e->owner != b->down[d] || e->gen != b->dgen[d]) { ok = 0; break; }
        }
    }
    if (ok) {
        for (int d = 0; d < b->ndep; ++d) g_c[b->dslot[d]].used = g_frames + 1; /* mantem os slots protegidos */
        rspq_block_run(b->blk);
        s_valid = 0; s_up_slot = -1; s_persp = -1;

        return;
    }
    if (b->blk) { rspq_block_free(b->blk); b->blk = NULL; }
    /* so grava se todos os comandos usam tokens de textura validos (sem conversao/espera dentro do bloco) */
    int ndep = 0;
    int dslot[BLK_DEP];
    for (int i = 0; i < n; ++i) {
        const FCmd *c = &cm[i];
        if (c->kind != 0 || c->texture < TEX_TOKEN_BASE || c->texture >= TEX_TOKEN_BASE + CACHE_N) { run_cmds(cm, n, verts, sx, sy, IW, IH); return; }
        const int sl = (int)(c->texture - TEX_TOKEN_BASE);
        if (!g_c[sl].owner) { run_cmds(cm, n, verts, sx, sy, IW, IH); return; }
        int j = 0;
        while (j < ndep && dslot[j] != sl) ++j;
        if (j == ndep) dslot[ndep++] = sl;
    }
    if (!heap_has_room()) { run_cmds(cm, n, verts, sx, sy, IW, IH); return; } /* sem folga de memoria: desenha direto */
    s_valid = 0; s_up_slot = -1; s_persp = -1;
    s_rec = 1;
    rspq_block_begin();
    run_cmds(cm, n, verts, sx, sy, IW, IH);
    rspq_block_t *nb = rspq_block_end();
    s_rec = 0;
    s_valid = 0; s_up_slot = -1; s_persp = -1;
    if (!nb) { return; } /* nao deveria acontecer; sem bloco este quadro fica sem o texto */
    b->blk = nb;
    b->stamp = stamp; b->nruns = (uint32_t)n; b->optf = optf; b->sxb = sxb; b->syb = syb; b->ndep = ndep;
    for (int d = 0; d < ndep; ++d) {
        b->dslot[d] = (uint16_t)dslot[d];
        b->down[d] = g_c[dslot[d]].owner;
        b->dgen[d] = g_c[dslot[d]].gen;
    }

    rspq_block_run(nb);
}

/* ---- ritmo de quadros ----
 * O motor atualiza a logica em passos fixos de 33 ms (catch-up por relogio) e so' desenha quando nao ha passo pendente.
 * Se as apresentacoes saem em intervalos irregulares (50/66 ms), cada quadro avanca 1 ou 2 passos alternadamente e o
 * movimento "pula". Aqui cada apresentacao e' alinhada a um multiplo de 33 ms: k = 2 passos (66 ms, ~15 fps) quando o
 * quadro cabe, k = 1 se o jogo ficar leve, k = 3 se ficar muito pesado. */
#define PACE_TICK_MS 33.0
#ifndef PACE_ENABLE
#define PACE_ENABLE 0 /* 0 = ritmo solto (fps livre); 1 = trava em multiplos de 33 ms */
#endif
static uint32_t g_pace_deadline, g_pace_release;
static int g_pace_k = 2, g_pace_low;
static double g_pace_ema = 50.0;

void gta2_n64_present(const void *cmds_, int cmdCount, const void *verts_, int vertCount, int refWidth)
{
    static int inited;
    if (!inited) {
        rdpq_init();
#if N64_NFONT
        nfont_init();
#endif
        gta2_n64_audio_init(); /* depois do rdpq_init: o mixer usa a fila do RSP */
        inited = 1;
        g_active = 1;
    }
    ublk_reap();

    const FCmd *cmds = (const FCmd *)cmds_;
    const FVert *verts = (const FVert *)verts_;
    if (refWidth <= 0) refWidth = 640;

    gta2_n64_audio_poll();
    surface_t *disp = display_get();
    SCR_W = (float)disp->width;
    SCR_H = (float)disp->height;
    const int IW = (int)disp->width, IH = (int)disp->height;
    const float sx = SCR_W / (float)refWidth, sy = SCR_H / 480.0f;
    g_last_sx = sx; g_last_sy = sy;
    rdpq_attach(disp, NULL);
    rdpq_set_mode_fill(RGBA32(0, 0, 0, 255));
    rdpq_fill_rectangle(0, 0, IW, IH);

    s_valid = 0; s_tex = -1; s_blend = -1; s_up_slot = -1; s_persp = -1;
    for (int c = 0; c < cmdCount; ) {
        const FCmd *cm = &cmds[c];
        if (cm->kind == 0 && cm->rect[3] != 0) { /* texto estatico marcado: tenta o bloco do RSP */
            int n = 1;
            while (c + n < cmdCount && cmds[c + n].kind == 0 && cmds[c + n].rect[3] == cm->rect[3] && cmds[c + n].rect[2] == cm->rect[2]) ++n;
            blk_run(cm, n, verts, sx, sy, IW, IH);
            c += n;
            continue;
        }
        draw_cmd(cm, verts, sx, sy, IW, IH);
        ++c;
    }

    rdpq_detach_show();
    ++g_frames;
#ifdef N64_MEMLOG
    { static unsigned peak; unsigned n = 0, cur = 0, wasted = 0; for (int i = 0; i < CACHE_N; ++i) { if (g_c[i].used && g_frames - g_c[i].used < CACHE_GUARD) { ++n; wasted += 4096 - (unsigned)g_c[i].pw * g_c[i].th * 2; } if (g_c[i].used) ++cur; }
      if (n > peak) { peak = n; }
      if ((g_frames % 120) == 0) { unsigned nb = 0; for (int i = 0; i < CACHE_N; ++i) if (g_ublk[i]) ++nb; debugf("[tcache] inuse=%u peak=%u everused=%u wasted=%uKB ublk=%u live=%d headroom=%ldKB\n", n, peak, cur, wasted / 1024, nb, g_ublk_live, (long)(__heap_top - (char *)sbrk(0)) / 1024); } }
#endif
    gta2_n64_audio_poll();
#if PACE_ENABLE
    { /* limitador de ritmo: ver comentario de PACE_TICK_MS. O tempo de espera conta como "espera", nao como desenho */
        const uint32_t tp0 = timer_ticks();
        if (g_pace_release) {
            const double work = ticks_ms((uint32_t)(tp0 - g_pace_release));
            g_pace_ema = g_pace_ema * 0.9 + work * 0.1;
            if (g_pace_ema > (double)g_pace_k * PACE_TICK_MS + 10.0 && g_pace_k < 3) { ++g_pace_k; g_pace_low = 0; }
            else if (g_pace_k > 1 && g_pace_ema < (double)(g_pace_k - 1) * PACE_TICK_MS - 4.0) { if (++g_pace_low >= 90) { --g_pace_k; g_pace_low = 0; } }
            else g_pace_low = 0;
        }
        const uint32_t interval = (uint32_t)((double)g_pace_k * PACE_TICK_MS * (double)TICKS_PER_SECOND / 1000.0);
        if (!g_pace_deadline) {
            g_pace_deadline = tp0 + interval;
        } else {
            g_pace_deadline += interval;
            if ((int32_t)(tp0 - g_pace_deadline) > 0) { g_pace_deadline = tp0; } /* atrasou: nao tenta recuperar */
            else while ((int32_t)(timer_ticks() - g_pace_deadline) < 0) gta2_n64_audio_poll();
        }
        g_pace_release = timer_ticks();
    }
#endif
}
