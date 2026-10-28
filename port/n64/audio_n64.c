/* Som do GTA 2 no N64: os efeitos que o motor dispara pela camada do Miles (miles_n64.cpp) tocam no mixer da
 * libdragon (mistura no RSP). Os samples ficam na ROM (banco .raw mapeado, ja em big-endian) e cada canal le
 * so o trecho que esta tocando, por DMA. Nada do banco ocupa RAM.
 *
 * Suporta PCM 16 bits mono (formato dos bancos .raw do GTA 2). Formatos de 8 bits (sem sinal no Miles) e
 * estereo sao ignorados. Sons pedidos antes do audio iniciar (gta2_n64_audio_init) sao ignorados.
 */
#include <libdragon.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <stdio.h>

#define NVOICE      8     /* canais do mixer para efeitos (0..7); cheio = rouba o som mais antigo (antes 14: menos canais = menos memoria e menos trabalho) */
#define NSTREAM     2     /* streams do Miles: 0 = radio, 1 = falas; canais NVOICE.. */
#define NWAVE       384   /* formas de onda distintas (banco tem 320 entradas) */
#define OUT_FREQ    12000 /* saida do mixer em 12 kHz (radios ja sao 12 kHz; efeitos de 22050 Hz sao reamostrados): menos trabalho no RSP/CPU */
/* TETOS DE TUMULTO */
#define TETO_SFX_VOZES   5   /* efeitos tocando ao mesmo tempo (de NVOICE=8; sobram 2 para loops tipo motor/sirene); cheio: efeito novo e descartado */
#define TETO_SFX_VOL_MIN 20  /* efeito novo (nao em loop infinito) com volume 0..127 abaixo disto e descartado: e longe demais para ouvir */
#define TETO_SFX_INICIOS 3   /* no maximo tantos efeitos novos por janela de 33 ms (rajadas de tiro/explosao); o resto e descartado */
#define TETO_SFX_UPD_MS  80  /* volume/pan/tom de um efeito em andamento so sao atualizados a cada tantos ms (motores) */
#define TETO_SFX_ROM_MS  4   /* tempo maximo de leitura da ROM por efeitos em cada janela de 40 ms; acima disso o trecho vira silencio */
#define TETO_JANELA_MS   40
#define TETO_MS(ms)      ((uint32_t)((ms) * (TICKS_PER_SECOND / 1000)))
static uint32_t g_rom_win_t0, g_rom_win_ticks, g_ini_t0, g_ini_n;
/* FOLGA (HEADROOM) contra estouro: o mixer soma ate 10 canais em 16 bits e satura (clipping = chiado). Cada canal usa so uma fracao
 * do volume e, quanto mais efeitos tocando, mais o volume geral abaixa (ducking). Ajuste olhando o ouvido. */
#define GANHO_SFX     0.45f  /* escala de cada efeito (1.0 = volume cheio do jogo) */
#define GANHO_MUSICA  0.50f  /* escala do radio/falas */
#define DUCK_INICIO   2      /* a partir de tantos efeitos simultaneos o volume geral comeca a abaixar */
#define DUCK_PASSO    0.16f  /* queda por efeito acima do inicio: 5 efeitos -> 1/(1+0.16*3) = 0.68 */
#define MAX_FREQ    (3.0f * OUT_FREQ) /* teto por canal: 3x a saida (mesmo tamanho de buffer de antes); o jogo pede ate ~50 kHz, entao tons muito agudos ficam limitados */

typedef struct {
    const uint8_t *data;  /* inicio do sample (ROM KSEG1 ou RAM) */
    uint32_t len;         /* bytes */
    int32_t loop_start, loop_end; /* bytes, relativos ao inicio; loop_end -1 = fim */
    int loops;            /* 0 = infinito */
    waveform_t wf;
    uint8_t *ram;         /* copia na RAM (cache de efeitos) ou NULL */
    void *ram_alloc;      /* bloco alocado (ram pode estar 1 byte adiante, pela paridade do DMA) */
    uint32_t ram_size;    /* bytes ocupados na arena (g_fxarena) */
    uint32_t last_use;    /* para tirar do cache o usado ha mais tempo */
    uint8_t plays;        /* quantas vezes tocou (sons sem loop entram no cache a partir da 2a) */
} Wave;

/* cache de efeitos na RAM: o mixer pede ~140 bytes por vez, e cada DMA da ROM custa ~45 us so de preparo */
#define FXCACHE_MAX      (192u * 1024u) /* total na RAM. 384 KB comeu a folga do heap (so ~100 KB livres) e causou 'Out of memory'; 256 KB ainda era justo */
#define FXCACHE_ITEM_MAX (64u * 1024u)  /* maior som que entra no cache */
static uint32_t g_fxcache_bytes, g_fxcache_tick;

typedef struct {
    int id;               /* handle do Miles (0 = livre) */
    uint32_t order;       /* para roubar o canal mais antigo */
    uint32_t upd_t;       /* ticks da ultima atualizacao de volume/pan/tom */
} Voice;

static Wave g_wave[NWAVE];
static int g_nwave;
static Voice g_voice[NVOICE];
static uint32_t g_order;
static int g_inited;

extern void gta2_n64_rom_copy(void *dst, const void *src, unsigned len); /* render_n64.c */

static inline int in_rom(const void *p) { const uintptr_t a = (uintptr_t)p; return a >= 0xB0000000u && a < 0xC0000000u; }


static void wave_read(void *ctx, samplebuffer_t *sbuf, int wpos, int wlen, bool seeking)
{
    (void)seeking;
    const uint32_t t_w0 = timer_ticks();
    Wave *w = (Wave *)ctx;
    uint8_t *dst = (uint8_t *)samplebuffer_append(sbuf, wlen);
    const uint32_t off = (uint32_t)wpos * 2u;
    uint32_t n = (uint32_t)wlen * 2u, have = 0;
    if (off < w->len) have = w->len - off;
    if (have > n) have = n;
    if (have && w->ram) {
        memcpy(dst, w->ram + off, have); /* som no cache da RAM */
    } else if (have) {
        const uint8_t *src = w->data + off;
        if ((uint32_t)(t_w0 - g_rom_win_t0) > TETO_MS(TETO_JANELA_MS)) { g_rom_win_t0 = t_w0; g_rom_win_ticks = 0; }
        if (g_rom_win_ticks > TETO_MS(TETO_SFX_ROM_MS)) { /* teto: sem tempo de ROM nesta janela, toca silencio */
            have = 0;
        } else
        if (in_rom(src) && (((uintptr_t)src ^ (uintptr_t)dst) & 1u) == 0u)
            dma_read(dst, (uint32_t)((uintptr_t)src & 0x1FFFFFFFu), have); /* espera o PI e le em DMA */
        else
        {
            gta2_n64_rom_copy(dst, src, have);                              /* RAM, ou paridade diferente */
        }
    }
    if (have < n) memset(dst + have, 0, n - have);
    if (!w->ram) g_rom_win_ticks += (uint32_t)(timer_ticks() - t_w0);
}

static Wave *wave_get(const void *data, uint32_t len, int loops, int32_t ls, int32_t le)
{
    for (int i = 0; i < g_nwave; ++i) {
        Wave *w = &g_wave[i];
        if (w->data == data && w->len == len && w->loops == loops && w->loop_start == ls && w->loop_end == le) return w;
    }
    if (g_nwave >= NWAVE) return NULL;
    Wave *w = &g_wave[g_nwave++];
    memset(w, 0, sizeof(*w));
    w->data = (const uint8_t *)data; w->len = len; w->loops = loops; w->loop_start = ls; w->loop_end = le;
    w->wf.name = "gta2";
    w->wf.bits = 16;
    w->wf.channels = 1;
    w->wf.frequency = 22050.0f;
    w->wf.len = (int)(len / 2u);
    if (loops == 0) { /* Miles: 0 = repete ate o jogo parar o som */
        int32_t s = ls > 0 ? ls : 0, e = (le < 0 || (uint32_t)le > len) ? (int32_t)len : le;
        if (e - s >= 4) {
            w->wf.loop_len = (e - s) / 2;
            w->wf.loop_end = (e == (int32_t)len) ? 0 : e / 2;
        }
    }
    w->wf.read = wave_read;
    w->wf.ctx = w;
    return w;
}


/* The cache lives in one fixed arena allocated at audio start-up. Allocated and freed one sound at a time on the heap,
 * its blocks of every size were the main thing chopping the heap into pieces during play, and after a few level loads
 * the map could no longer find room for its 360 KB column buffer (out of memory with 1 MB free). Inside the arena a
 * sound goes in the first gap between the cached sounds that is large enough. */
static uint8_t *g_fxarena;
static void *fxarena_alloc(uint32_t sz)
{
    if (!g_fxarena) return NULL;
    uint8_t *at = g_fxarena;
    for (;;) {
        /* the cached block that starts lowest at or after 'at' */
        const Wave *next = NULL;
        for (int i = 0; i < g_nwave; ++i) {
            const Wave *o = &g_wave[i];
            if (!o->ram_alloc) continue;
            uint8_t *b = (uint8_t *)o->ram_alloc;
            if (b + o->ram_size <= at) continue;
            if (b < at) { at = b + o->ram_size; i = -1; next = NULL; continue; } /* overlaps 'at': skip past it and rescan */
            if (!next || b < (uint8_t *)next->ram_alloc) next = o;
        }
        uint8_t *end = next ? (uint8_t *)next->ram_alloc : g_fxarena + FXCACHE_MAX;
        if ((uint32_t)(end - at) >= sz) return at;
        if (!next) return NULL;
        at = (uint8_t *)next->ram_alloc + next->ram_size;
    }
}

static void fxcache_free(Wave *w)
{
    if (!w->ram) return;
    w->ram = NULL; /* antes de liberar: uma leitura seguinte volta para a ROM */
    w->ram_alloc = NULL;
    w->ram_size = 0;
    g_fxcache_bytes -= w->len;
}

/* poe o som no cache da RAM: sons em loop ja na 1a vez, os demais a partir da 2a */
static void fxcache_touch(Wave *w)
{
    w->last_use = ++g_fxcache_tick;
    if (w->plays < 255) ++w->plays;
    if (w->ram || !in_rom(w->data) || w->len == 0 || w->len > FXCACHE_ITEM_MAX) return;
    if (w->loops != 0 && w->plays < 2) return;
    while (g_fxcache_bytes + w->len > FXCACHE_MAX) { /* libera os usados ha mais tempo */
        Wave *old = NULL;
        for (int i = 0; i < g_nwave; ++i)
            if (g_wave[i].ram && &g_wave[i] != w && (!old || g_wave[i].last_use < old->last_use)) old = &g_wave[i];
        if (!old) return;
        fxcache_free(old);
    }
    const uintptr_t odd = (uintptr_t)w->data & 1u;          /* dma_read exige a mesma paridade na RAM e na ROM */
    const uint32_t sz = (w->len + (uint32_t)odd + 15u) & ~15u; /* linhas de cache inteiras so deste bloco */
    void *blk = fxarena_alloc(sz);
    for (int tries = 0; !blk && tries < 8; ++tries) { /* fragmented arena: drop the oldest and try again */
        Wave *old = NULL;
        for (int i = 0; i < g_nwave; ++i)
            if (g_wave[i].ram && &g_wave[i] != w && (!old || g_wave[i].last_use < old->last_use)) old = &g_wave[i];
        if (!old) return;
        fxcache_free(old);
        blk = fxarena_alloc(sz);
    }
    if (!blk) return;
    uint8_t *ram = (uint8_t *)blk + odd;
    data_cache_hit_writeback_invalidate(blk, sz);
    dma_read(ram, (uint32_t)((uintptr_t)w->data & 0x1FFFFFFFu), w->len);
    w->ram_alloc = blk;
    w->ram_size = sz;
    w->ram = ram;
    g_fxcache_bytes += w->len;
}

static int voice_find(int id)
{
    for (int c = 0; c < NVOICE; ++c) if (g_voice[c].id == id) return c;
    return -1;
}

/* volumes do menu de Opcoes (0..10): musica = radios/falas (streams), efeitos = sons do jogo */
static int g_vol_music = 10, g_vol_sfx = 10;
static void streams_refresh(void);
int gta2_n64_get_volume(int which) { return which == 0 ? g_vol_music : g_vol_sfx; }
void gta2_n64_set_volume(int which, int v)
{
    if (v < 0) v = 0;
    if (v > 10) v = 10;
    if (which == 0) { g_vol_music = v; streams_refresh(); }
    else g_vol_sfx = v;
}

static void apply_mix(int ch, int volume, int pan, int rate)
{
    float v = (float)volume / 127.0f * ((float)g_vol_sfx * 0.1f) * GANHO_SFX, p = (float)pan / 127.0f;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    if (p < 0.0f) p = 0.0f;
    if (p > 1.0f) p = 1.0f;
    mixer_ch_set_vol_pan(ch, v, p);
    if (rate > 0) {
        float f = (float)rate;
        if (f > MAX_FREQ) f = MAX_FREQ; /* acima do limite o mixer para o jogo (assert) */
        mixer_ch_set_freq(ch, f);
    }
}

void gta2_n64_audio_init(void)
{
    if (g_inited) return;
    audio_init(OUT_FREQ, AUDIO_INIT_LATENCY_MS(240)); /* mais folga no buffer: quadro lento (explosoes) nao esvazia o audio = sem estalos */
    mixer_init(NVOICE + NSTREAM);
    for (int c = 0; c < NVOICE + NSTREAM; ++c) mixer_ch_set_limits(c, 16, MAX_FREQ, 0);
    wav64_init_compression(2); /* radios em ULC (audioconv64 --wav-compress ulc) */
    g_fxarena = (uint8_t *)memalign(16, FXCACHE_MAX); /* sound-effect cache arena (fxarena_alloc) */
    g_inited = 1;
}

static float g_master = 1.0f;
static int g_duck_n;
static void duck_apply(void)
{
    float d = 1.0f;
    if (g_duck_n > DUCK_INICIO) d = 1.0f / (1.0f + DUCK_PASSO * (float)(g_duck_n - DUCK_INICIO));
    mixer_set_vol(g_master * d);
}

/* alimenta o AI: chamado algumas vezes por quadro (render_n64.c) */
void gta2_n64_audio_poll(void)
{
    if (!g_inited) return;
    while (audio_can_write()) {
        short *buf = audio_write_begin();
        const int n = audio_get_buffer_length();
        int ativo = 0, nsfx = 0;
        for (int c = 0; c < NVOICE + NSTREAM; ++c) if (mixer_ch_playing(c)) { ativo = 1; if (c < NVOICE) ++nsfx; }
        if (nsfx != g_duck_n) { g_duck_n = nsfx; duck_apply(); }
        if (ativo) mixer_poll(buf, n);
        else memset(buf, 0, (size_t)n * 2u * sizeof(short)); /* nada tocando: silencio direto, sem acordar o RSP */
        audio_write_end();
    }
}

void gta2_n64_sfx_start(int id, const void *data, int len, int rate, int format, int loops, int loop_start,
                        int loop_end, int volume, int pan)
{
    if (!g_inited || !id || len < 4) return;
    if (!(format & 1) || (format & 2)) return;          /* so PCM 16 bits mono */
    if (!in_rom(data)) return;                           /* banco ainda nao mapeado da ROM */
    if (loops != 0) { /* TETOS: efeitos que acabam sozinhos (tiro, batida, explosao): descarta os inaudiveis e as rajadas */
        const uint32_t agora = timer_ticks();
        if (volume < TETO_SFX_VOL_MIN) { return; }
        if ((uint32_t)(agora - g_ini_t0) > TETO_MS(33)) { g_ini_t0 = agora; g_ini_n = 0; }
        if (g_ini_n >= TETO_SFX_INICIOS) { return; }
        ++g_ini_n;
    }
    Wave *w = wave_get(data, (uint32_t)len & ~1u, loops, loop_start, loop_end);
    if (!w) return;
    fxcache_touch(w);
    int ch = voice_find(id);
    if (ch < 0) {
        int ativas = 0;
        for (int c = 0; c < NVOICE; ++c) if (mixer_ch_playing(c)) ++ativas;
        if (ativas >= TETO_SFX_VOZES && loops != 0) { return; } /* teto: descarta efeito novo (em loop infinito, tipo motor/sirene, rouba) */
    }
    if (ch < 0) for (int c = 0; c < NVOICE; ++c) if (!mixer_ch_playing(c)) { ch = c; break; }
    if (ch < 0) { /* todos ocupados: rouba o mais antigo */
        ch = 0;
        for (int c = 1; c < NVOICE; ++c) if (g_voice[c].order < g_voice[ch].order) ch = c;
    }
    for (int c = 0; c < NVOICE; ++c) if (c != ch && g_voice[c].id == id) g_voice[c].id = 0;
    g_voice[ch].id = id;
    g_voice[ch].order = ++g_order;
    g_voice[ch].upd_t = timer_ticks();
    mixer_ch_play(ch, &w->wf);
    apply_mix(ch, volume, pan, rate);
}

void gta2_n64_sfx_update(int id, int volume, int pan, int rate)
{
    if (!g_inited) return;
    const int ch = voice_find(id);
    if (ch >= 0 && mixer_ch_playing(ch)) {
        const uint32_t agora = timer_ticks();
        if ((uint32_t)(agora - g_voice[ch].upd_t) < TETO_MS(TETO_SFX_UPD_MS)) return; /* teto: atualizacao frequente demais */
        g_voice[ch].upd_t = agora;
        apply_mix(ch, volume, pan, rate);
    }
}

void gta2_n64_sfx_stop(int id)
{
    if (!g_inited) return;
    const int ch = voice_find(id);
    if (ch >= 0) { mixer_ch_stop(ch); g_voice[ch].id = 0; }
}

void gta2_n64_master_volume(int volume)
{
    if (!g_inited) return;
    float v = (float)volume / 127.0f;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    g_master = v;
    duck_apply();
}

/* ---- Streams (radios em gtaudio, arquivos .wav64 convertidos no build) ---- */
typedef struct {
    int id;          /* handle do Miles (0 = livre) */
    int open;
    wav64_t wav;
    int volume, rate;
    int vocal;       /* announcer vocal: the game already scales its volume by the sfx setting */
} Strm;
static Strm g_st[NSTREAM];

static Strm *stream_find(int id)
{
    for (int i = 0; i < NSTREAM; ++i) if (g_st[i].open && g_st[i].id == id) return &g_st[i];
    return NULL;
}
static inline int stream_ch(const Strm *s) { return NVOICE + (int)(s - g_st); }
static void stream_apply(Strm *s);
static void streams_refresh(void)
{
    for (int i = 0; i < NSTREAM; ++i)
        if (g_st[i].open && mixer_ch_playing(stream_ch(&g_st[i]))) stream_apply(&g_st[i]);
}

static void stream_apply(Strm *s)
{
    const int ch = stream_ch(s);
    float v = (float)s->volume / 127.0f * (s->vocal ? 1.0f : (float)g_vol_music * 0.1f) * GANHO_MUSICA;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    mixer_ch_set_vol(ch, v, v);
    if (s->rate > 0 && s->rate != 22050) {
        float f = s->wav.wave.frequency * (float)s->rate / 22050.0f;
        if (f > MAX_FREQ) f = MAX_FREQ;
        mixer_ch_set_freq(ch, f);
    }
}

#define MISS_MAX 64
static uint32_t g_miss[MISS_MAX];
static int g_nmiss;

/* path ja normalizado pelo port ("rom:/data/gtaudio/1.wav"); o arquivo no DFS e' o .wav64 */
int gta2_n64_stream_open(int id, const char *path)
{
    if (!g_inited || !id || !path) return 0;
    char p[200];
    const size_t n = strlen(path);
    if (n + 3 >= sizeof(p)) return 0;
    memcpy(p, path, n + 1);
    if (n >= 4 && strcmp(p + n - 4, ".wav") == 0) strcat(p, "64");
    uint32_t h = 2166136261u;           /* arquivos que nao existem (falas, d/m.wav64): lembra e nao tenta abrir de novo */
    for (const char *q = p; *q; ++q) h = (h ^ (uint8_t)*q) * 16777619u;
    for (int i = 0; i < g_nmiss; ++i) if (g_miss[i] == h) return 0;
    FILE *f = fopen(p, "rb");           /* wav64_open para o jogo se o arquivo nao existir */
    if (!f) { if (g_nmiss < MISS_MAX) g_miss[g_nmiss++] = h; return 0; }
    fclose(f);
    Strm *s = stream_find(id);
    if (!s) for (int i = 0; i < NSTREAM; ++i) if (!g_st[i].open) { s = &g_st[i]; break; }
    if (!s) return 0;
    if (s->open) { mixer_ch_stop(stream_ch(s)); wav64_close(&s->wav); s->open = 0; }
    wav64_open(&s->wav, p);
    s->id = id; s->open = 1; s->volume = 127; s->rate = 22050;
    s->vocal = strstr(p, "/vocals/") != NULL;
    return 1;
}

void gta2_n64_stream_close(int id)
{
    Strm *s = stream_find(id);
    if (!s) return;
    mixer_ch_stop(stream_ch(s));
    wav64_close(&s->wav);
    s->open = 0; s->id = 0;
}

void gta2_n64_stream_start(int id, int loops)
{
    Strm *s = stream_find(id);
    if (!s) return;
    wav64_set_loop(&s->wav, loops == 0); /* Miles: 0 = repete sempre (radio) */
    wav64_play(&s->wav, stream_ch(s));
    stream_apply(s);
}

int gta2_n64_stream_playing(int id)
{
    Strm *s = stream_find(id);
    return s && mixer_ch_playing(stream_ch(s));
}

void gta2_n64_stream_set(int id, int volume, int rate)
{
    Strm *s = stream_find(id);
    if (!s) return;
    s->volume = volume; s->rate = rate;
    if (mixer_ch_playing(stream_ch(s))) stream_apply(s);
}

void gta2_n64_stream_seek(int id, int ms)
{
    Strm *s = stream_find(id);
    if (!s || ms < 0) return;
    if (mixer_ch_playing(stream_ch(s))) wav64_seek(&s->wav, stream_ch(s), (double)ms / 1000.0);
}

/* which: 0 = duracao total, 1 = posicao atual (ms) */
int gta2_n64_stream_ms(int id, int which)
{
    Strm *s = stream_find(id);
    if (!s || s->wav.wave.frequency <= 0.0f) return 0;
    const double f = s->wav.wave.frequency;
    if (which == 0) return s->wav.wave.len > 0 ? (int)(s->wav.wave.len * 1000.0 / f) : 0;
    return mixer_ch_playing(stream_ch(s)) ? (int)(mixer_ch_get_pos(stream_ch(s)) * 1000.0 / f) : 0;
}
