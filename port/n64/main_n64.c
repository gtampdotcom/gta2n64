// Ponto de entrada N64 do motor gta2_re (decomp do GTA 2): inicializa o hardware, le o controle e roda o motor.
// O jogo (WinMain_5E53F0) roda por fatias: gta2_game_frame() = Gta2Port_RunLoopSlice().
#include <libdragon.h>
#include <malloc.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int gta2_game_init(void);
int gta2_game_frame(void);
void gta2_game_key(int scancode, int vk, int down);
void gta2_port_flush_key_events(int with_latched);
int gta2_game_in_menu(void);
void gta2_install_exception_fixups(void);

#include "prof_n64.h"
#if defined(N64_PAD) && N64_PAD > 0
/* measurement aid: N64_PAD bytes of dead space in the code segment, to see how much a timing depends on where the code and
 * data that follow happen to land in the (direct-mapped) caches rather than on the change being measured */
__attribute__((used, section(".text.n64pad"))) static const unsigned char g_n64_pad[N64_PAD] = { 1 };
#endif
#ifdef N64_PROF
/* ---- frame profiler (see prof_n64.h) ---- */
#ifndef N64_BENCH_F0
#define N64_BENCH_F0 300 /* the bench window, in presented frames: the same frames (same game ticks) in every build */
#endif
#ifndef N64_BENCH_F1
#define N64_BENCH_F1 1100
#endif
#define PROF_REPORT 60
static const char *const kPfName[PF_N] = { "logic", "draw", "present", "texconv", "wait", "audio", "hash", "p.upload", "p.tri", "p.rect", "p.mode" };
static const char *const kPcName[PC_N] = { "ticks", "cmds", "verts", "tris", "rects", "uploads", "conv", "areaKpx", "unaligned", "fpconv" };
static uint32_t pf_acc[PF_N], pf_tot[PF_N], pc_acc[PC_N], pc_tot[PC_N];
static uint8_t pf_stk[16];
static int pf_sp;
static uint32_t pf_last, pf_t_first, pf_t_rep, pf_t_b0, pf_frames, pf_bframes, pf_max, pf_nframe;
uint32_t gta2_n64_prof_hash = 2166136261u; /* running checksum of everything drawn (render_n64.c) */
uint32_t gta2_n64_prof_fbhash = 2166136261u; /* running checksum of sampled framebuffers */
static uint32_t pf_hash0, pf_fbhash0;
static int pf_state; /* 0 = before the window, 1 = inside, 2 = done */
void gta2_n64_prof_enter(int id)
{
    const uint32_t now = TICKS_READ();
    if (pf_sp > 0) pf_acc[pf_stk[pf_sp - 1]] += now - pf_last;
    if (pf_sp < 16) pf_stk[pf_sp] = (uint8_t)id;
    ++pf_sp;
    pf_last = now;
}
void gta2_n64_prof_leave(void)
{
    const uint32_t now = TICKS_READ();
    if (pf_sp <= 0) return;
    --pf_sp;
    if (pf_sp < 16) pf_acc[pf_stk[pf_sp]] += now - pf_last;
    pf_last = now;
}
void gta2_n64_prof_count(int id, unsigned n) { pc_acc[id] += n; }
/* Sampling profiler: a ~2 kHz timer interrupt records where the CPU was (EPC), as a histogram over the code in 32-byte buckets
 * (functions are aligned to 32). Only samples inside the bench window count. Dumped at the end as "[smp] addr:count ..." lines;
 * the host turns them into the hot-function link order (tools/prof_order.py). */
#ifdef N64_PROF_SAMPLE
extern char __text_start[], __text_end[];
#define SMP_SHIFT 5
#define SMP_MAX (0x1E0000 >> SMP_SHIFT)
static uint16_t smp_hist[SMP_MAX];
static uint8_t smp_phase[SMP_MAX]; /* profiler section of the last sample in the bucket */
static uint32_t smp_total, smp_out, smp_sec[PF_N + 1];
static void smp_cb(int ovfl)
{
    (void)ovfl;
    if (pf_state != 1) return;
    const uint32_t i = ((uint32_t)C0_EPC() - (uint32_t)(uintptr_t)__text_start) >> SMP_SHIFT;
    const int sec = pf_sp > 0 && pf_sp <= 16 ? pf_stk[pf_sp - 1] : PF_N;
    if (i < SMP_MAX) { if (smp_hist[i] != 0xFFFF) ++smp_hist[i]; smp_phase[i] = (uint8_t)sec; } else ++smp_out;
    ++smp_total;
    ++smp_sec[sec];
}
static void smp_dump(void)
{
    char b[256];
    int n = 0;
    n = snprintf(b, sizeof(b), "[smpinfo] total=%u outside=%u text=%p", (unsigned)smp_total, (unsigned)smp_out, (void *)__text_start);
    for (int i = 0; i <= PF_N; ++i) if (smp_sec[i]) n += snprintf(b + n, sizeof(b) - n, " %s=%u", i < PF_N ? kPfName[i] : "none", (unsigned)smp_sec[i]);
    debugf("%s\n", b);
    n = 0;
    for (uint32_t i = 0; i < SMP_MAX; ++i) {
        if (!smp_hist[i]) continue;
        n += snprintf(b + n, sizeof(b) - n, " %x:%u:%u", (unsigned)((uint32_t)(uintptr_t)__text_start + (i << SMP_SHIFT)), (unsigned)smp_hist[i], (unsigned)smp_phase[i]);
        if (n > 220) { debugf("[smp]%s\n", b); n = 0; }
    }
    if (n) debugf("[smp]%s\n", b);
}
#endif
static void pf_line(const char *tag, const uint32_t *t, const uint32_t *c, uint32_t wall, uint32_t frames, uint32_t maxf)
{
    char b[512];
    int n = 0;
    uint32_t sum = 0;
    wall -= t[PF_HASH]; /* the checksum is measuring overhead, not part of the game */
    const double k = 1000.0 / (double)TICKS_PER_SECOND / (double)frames;
    n += snprintf(b + n, sizeof(b) - n, "[%s] frames=%u wall_ms=%.0f ms/frame=%.2f fps=%.2f max=%.1f |", tag, (unsigned)frames,
                  (double)wall * 1000.0 / (double)TICKS_PER_SECOND, (double)wall * k, (double)frames * (double)TICKS_PER_SECOND / (double)wall,
                  (double)maxf * 1000.0 / (double)TICKS_PER_SECOND);
    for (int i = 0; i < PF_N; ++i) { if (i == PF_HASH) continue; sum += t[i]; if (t[i]) n += snprintf(b + n, sizeof(b) - n, " %s=%.2f", kPfName[i], (double)t[i] * k); }
    n += snprintf(b + n, sizeof(b) - n, " other=%.2f |", (double)(int32_t)(wall - sum) * k);
    for (int i = 0; i < PC_N; ++i) if (c[i]) n += snprintf(b + n, sizeof(b) - n, " %s=%.1f", kPcName[i], (double)c[i] / (double)frames);
    debugf("%s\n", b);
}
void gta2_n64_prof_frame(void)
{
    static uint32_t prev;
    const uint32_t now = TICKS_READ();
    { extern uint32_t gta2_n64_excfix_n[2]; static uint32_t e0, e1; pc_acc[PC_X1] += gta2_n64_excfix_n[0] - e0; pc_acc[PC_X2] += gta2_n64_excfix_n[1] - e1; e0 = gta2_n64_excfix_n[0]; e1 = gta2_n64_excfix_n[1]; }
#ifdef N64_PROF_SAMPLE
    if (!pf_t_first) new_timer(TICKS_PER_SECOND / 1987, TF_CONTINUOUS, smp_cb);
#endif
    if (!pf_t_first) { pf_t_first = pf_t_rep = prev = now | 1u; memset(pf_acc, 0, sizeof(pf_acc)); memset(pc_acc, 0, sizeof(pc_acc)); return; }
    const uint32_t ft = now - prev;
    prev = now;
    ++pf_frames;
    ++pf_nframe;
    if (pf_state == 0 && pf_nframe >= N64_BENCH_F0) { pf_hash0 = gta2_n64_prof_hash; pf_fbhash0 = gta2_n64_prof_fbhash; pf_state = 1; pf_t_b0 = now; pf_bframes = 0; pf_max = 0; memset(pf_tot, 0, sizeof(pf_tot)); memset(pc_tot, 0, sizeof(pc_tot)); memset(pf_acc, 0, sizeof(pf_acc)); memset(pc_acc, 0, sizeof(pc_acc)); pf_frames = 0; pf_t_rep = now; return; }
    if (pf_state == 1) { ++pf_bframes; if (ft > pf_max) pf_max = ft; }
    if (pf_frames >= PROF_REPORT || (pf_state == 1 && pf_nframe >= N64_BENCH_F1)) {
        pf_line("prof", pf_acc, pc_acc, now - pf_t_rep, pf_frames, 0);
        if (pf_state == 1) { for (int i = 0; i < PF_N; ++i) pf_tot[i] += pf_acc[i]; for (int i = 0; i < PC_N; ++i) pc_tot[i] += pc_acc[i]; }
        memset(pf_acc, 0, sizeof(pf_acc)); memset(pc_acc, 0, sizeof(pc_acc));
        pf_frames = 0; pf_t_rep = now;
        if (pf_state == 1 && pf_nframe >= N64_BENCH_F1) {
            struct mallinfo mi = mallinfo();
            extern char *__heap_top;
            pf_line("bench", pf_tot, pc_tot, now - pf_t_b0, pf_bframes, pf_max);
            debugf("[benchsum] cmds=%u verts=%u tris=%u rects=%u uploads=%u conv=%u ticks=%u hash=%08x (from %08x) fbhash=%08x (from %08x)\n", (unsigned)pc_tot[PC_CMDS], (unsigned)pc_tot[PC_VERTS], (unsigned)pc_tot[PC_TRIS],
                   (unsigned)pc_tot[PC_RECTS], (unsigned)pc_tot[PC_UPLOADS], (unsigned)pc_tot[PC_CONV], (unsigned)pc_tot[PC_LOGIC], (unsigned)gta2_n64_prof_hash, (unsigned)pf_hash0, (unsigned)gta2_n64_prof_fbhash, (unsigned)pf_fbhash0);
            debugf("[benchmem] heap_used=%dKB heap_free_in_arena=%dKB arena=%dKB unclaimed=%ldKB\n", mi.uordblks / 1024, mi.fordblks / 1024, mi.arena / 1024, (long)(__heap_top - (char *)sbrk(0)) / 1024);
#ifdef N64_PROF_SAMPLE
            smp_dump();
#endif
            debugf("[benchdone]\n");
            pf_state = 2;
        }
    }
}
#endif

// Relogio monotonico em ms para o motor (timeGetTime, QueryPerformanceCounter...).
double gta2_n64_now_ms(void)
{
    return (double)timer_ticks() * 1000.0 / (double)TICKS_PER_SECOND;
}

#ifndef N64_LAB_KEYS
#define N64_LAB_KEYS 0 /* 1 = L/R/C-cima alternam filtro/perspectiva/depuracao do render (em vez de proxima arma/freio/entrar no carro) */
#endif
typedef struct { int sc, vk; } KeyDef;
enum { K_UP, K_DOWN, K_LEFT, K_RIGHT, K_W, K_S, K_A, K_D, K_ENTER, K_ATTACK, K_JUMP, K_ESC, K_PREV, K_NEXT, K_RADIO, K_SPECIAL, K_PAUSE, K_COUNT };
static const KeyDef kKeys[K_COUNT] = {
    {0xC8, 0x26}, {0xD0, 0x28}, {0xCB, 0x25}, {0xCD, 0x27}, // setas (menus)
    {0x11, 0x57}, {0x1F, 0x53}, {0x1E, 0x41}, {0x20, 0x44}, // W S A D (jogo)
    {0x1C, 0x0D}, {0x26, 0x4C}, {0x39, 0x20}, {0x01, 0x1B}, // Enter, ataque L, pulo/freio, Esc
    {0x24, 0x4A}, {0x25, 0x4B},                             // arma anterior J / proxima K
    {0x3B, 0x70},                                           // F1: proxima estacao de radio
    {0x17, 0x49},                                           // I: especial (buzina etc.; slot 9 dos controles)
    {0x40, 0x75},                                           // F6: pausa (TogglePause do motor)
};
static uint8_t gKeyDown[K_COUNT];

static void set_key(int k, int down)
{
    if (gKeyDown[k] != (uint8_t)down)
    {
        gKeyDown[k] = (uint8_t)down;
        gta2_game_key(kKeys[k].sc, kKeys[k].vk, down);
    }
}

/* pause menu (Hud.cpp): buttons newly pressed since the last call. 1 up, 2 down, 4 left, 8 right, 16 A, 32 B */
int gta2_n64_menu_pad(void)
{
    static int prev = -1;
    joypad_inputs_t in = joypad_get_inputs(JOYPAD_PORT_1);
    int now = 0;
    if (in.btn.d_up || in.stick_y > 40) now |= 1;
    if (in.btn.d_down || in.stick_y < -40) now |= 2;
    if (in.btn.d_left || in.stick_x < -40) now |= 4;
    if (in.btn.d_right || in.stick_x > 40) now |= 8;
    if (in.btn.a) now |= 16;
    if (in.btn.b) now |= 32;
    const int pressed = prev < 0 ? 0 : now & ~prev; /* first call: whatever is held (Start) does not count */
    prev = now;
    return pressed;
}

static void poll_input(void)
{
    joypad_poll();
    joypad_inputs_t in = joypad_get_inputs(JOYPAD_PORT_1);
#ifdef N64_BENCH
    {   /* scripted pad for repeatable measurements: walk forward all the time, turn left 1 s in every 5 */
        extern uint32_t gta2_n64_frames(void);
        const uint32_t bf = gta2_n64_frames(); /* frame-indexed, so every build plays the same game */
        memset(&in, 0, sizeof(in));
        in.btn.d_up = bf >= 60u;
        in.btn.d_left = bf >= 60u && ((bf / 20u) % 5u) == 0u;
    }
#endif
    /* Controle estilo PS1 (manual do GTA 2 no PS1). No JOGO:
     *   direcional/analogico = mover/virar   A = acelera (X)   B = re (Quadrado)   C-cima = entra/sai do carro (Triangulo)
     *   C-direita = especial (Circulo)   R = freia/pula (R1)   Z = ataca (R2)   L = proxima arma (L1)   C-esquerda = arma anterior (L2)
     *   C-baixo = proxima radio   Start = pausa.
     *   Nao ha "sair da fase" pelo controle (o ESC do PC nao e mapeado dentro do jogo).
     * No MENU: direcional/analogico navega, A ou Start confirma, B volta. */
    const int menu = gta2_game_in_menu();
    /* Protecao contra "botao fantasma": ao trocar menu <-> jogo, Enter/Esc/Pausa ficam bloqueados ate soltar A, B, Start e C-cima.
     * Sem isso, um botao ainda apertado na troca era lido pela tela nova como um aperto novo e escolhia opcoes sozinho. Start tambem so vale depois de 2 leituras seguidas (filtro de ruido). */
    static int prev_menu = -1, guard, start_prev;
    if (menu != prev_menu) { if (prev_menu != -1) guard = 1; prev_menu = menu; }
    if (guard && !(in.btn.a || in.btn.b || in.btn.start || in.btn.c_up)) guard = 0;
    const int start_ok = in.btn.start && start_prev;
    start_prev = in.btn.start;
    /* BLINDAGEM DA ENTRADA NA PARTIDA: ao entrar no jogo (ou ligar direto no mapa) todas as teclas do jogo ficam desligadas ate o
     * controle ficar 100% neutro (sem botao, analogico perto do centro) por 6 leituras seguidas e passarem 0,8 s; no maximo 5 s. */
#ifdef N64_BENCH
    const uint32_t now_ms = 0; /* no input blanking in bench builds: it is timed in real ms */
#else
    const uint32_t now_ms = (uint32_t)get_ticks_ms();
#endif
    static int bp_menu = -1, neutral_n;
    static uint32_t blank_t0;
    const int pad_neutral = !(in.btn.a || in.btn.b || in.btn.z || in.btn.start || in.btn.l || in.btn.r || in.btn.c_up || in.btn.c_down ||
                              in.btn.c_left || in.btn.c_right || in.btn.d_up || in.btn.d_down || in.btn.d_left || in.btn.d_right) &&
                            in.stick_x > -15 && in.stick_x < 15 && in.stick_y > -15 && in.stick_y < 15;
    if (!menu && bp_menu != 0) { blank_t0 = now_ms | 1u; neutral_n = 0; }
    bp_menu = menu;
    neutral_n = pad_neutral ? (neutral_n < 100 ? neutral_n + 1 : neutral_n) : 0;
    int blank = 0;
    if (!menu && blank_t0)
    {
        const uint32_t el = now_ms - blank_t0;
        if (el >= 5000u || (el >= 800u && neutral_n >= 6)) blank_t0 = 0;
        else blank = 1;
    }
    const int dup = in.btn.d_up || in.stick_y > 40, ddown = in.btn.d_down || in.stick_y < -40;
    const int left = !blank && (in.btn.d_left || in.stick_x < -40), right = !blank && (in.btn.d_right || in.stick_x > 40);
    const int up = !blank && (dup || (!menu && in.btn.a)), down = !blank && (ddown || (!menu && in.btn.b));
    set_key(K_UP, up);   set_key(K_W, !menu && up);
    set_key(K_DOWN, down); set_key(K_S, !menu && down);
    set_key(K_LEFT, left); set_key(K_A, !menu && left);
    set_key(K_RIGHT, right); set_key(K_D, !menu && right);
    set_key(K_ENTER, !guard && !blank && (menu ? (in.btn.a || in.btn.start) : in.btn.c_up));
    set_key(K_ESC, !guard && !blank && menu && in.btn.b); /* ESC so existe nos menus */
    set_key(K_PAUSE, !menu && !blank && !guard && start_ok);
    set_key(K_ATTACK, !menu && !blank && in.btn.z);
    set_key(K_JUMP, !menu && !blank && in.btn.r);
    set_key(K_NEXT, !menu && !blank && in.btn.l);
    set_key(K_PREV, !menu && !blank && in.btn.c_left);
    set_key(K_SPECIAL, !menu && !blank && in.btn.c_right);
    set_key(K_RADIO, !blank && in.btn.c_down); /* C-baixo troca a radio */
    /* Fila de eventos de teclado do jogo: no menu e no bloqueio de entrada ela e esvaziada todo quadro. Sem isso, as teclas apertadas no menu
     * (B = ESC, Enter, setas) ficavam guardadas e o jogo as lia todas de uma vez ao comecar a partida: o ESC abria "sair do jogo", o Enter
     * seguinte confirmava e o jogo voltava ao menu; as setas guardadas faziam o personagem/carro "enlouquecer". */
    if (menu || blank) gta2_port_flush_key_events(!menu);
#if N64_LAB_KEYS
    {   /* laboratorio de render: L = filtro, R = perspectiva, C-cima = depuracao (so na borda de subida). Desligado: os botoes sao do jogo */
        extern void gta2_n64_render_toggle(int which);
        static int pl, pr, pc;
        if (in.btn.l && !pl) gta2_n64_render_toggle(0);
        if (in.btn.r && !pr) gta2_n64_render_toggle(1);
        if (in.btn.c_up && !pc) gta2_n64_render_toggle(2);
        pl = in.btn.l; pr = in.btn.r; pc = in.btn.c_up;
    }
#endif
}

int gta2_n64_gfx_active(void);
/* ---- memory of the intro movie's code, reused after the movie ----
 * The linker script (tools/make_hot_ld.py) puts everything only the intro uses (libdragon's H.264 decoder and movie
 * player, ~80 KB) between these two symbols. The movie plays once, at power-on; after that the block is plain RAM and is
 * handed out here to buffers that live for the whole game (frame_n64.cpp), instead of them coming out of the heap: with
 * the decoder resident, the game had 130 KB less. Never free() what this returns. */
extern char __n64_intro_text_start[] __attribute__((weak)), __n64_intro_text_end[] __attribute__((weak));
static char *g_reclaim, *g_reclaim_end;
static void reclaim_init(void)
{
    char *const a = (char *)__n64_intro_text_start, *const b = (char *)__n64_intro_text_end;
    if (!a || (uintptr_t)b <= (uintptr_t)a) return;
    g_reclaim = a;
    g_reclaim_end = b;
    inst_cache_hit_invalidate(g_reclaim, g_reclaim_end - g_reclaim); /* it is data from now on */
}
void *gta2_n64_reclaim_alloc(size_t n)
{
    n = (n + 63u) & ~(size_t)63u;
    if (!g_reclaim || (size_t)(g_reclaim_end - g_reclaim) < n) return NULL;
    void *p = g_reclaim;
    g_reclaim += n;
    return p;
}

/* Escolha de inicio, lida por game_entry.cpp: -1 = menu original do GTA 2; 0 = bil (Industrial), 1 = ste (Residential), 2 = wil (Downtown)
 * direto no mapa (sem menu). Segurar Z ao ligar pula o menu: Z = bil, Z + C-baixo = ste, Z + C-esquerda = wil. */
#ifndef N64_BOOT_MAP
#define N64_BOOT_MAP -1 /* build with -DN64_BOOT_MAP=0..2 to always start in that map, as if Z were held */
#endif
int gta2_n64_boot_map = N64_BOOT_MAP;
static void read_boot_choice(void)
{
    joypad_inputs_t in;
    memset(&in, 0, sizeof(in));
    for (int i = 0; i < 4; ++i) { joypad_poll(); wait_ms(20); } /* o 1o poll pode vir vazio */
    in = joypad_get_inputs(JOYPAD_PORT_1);
    if (in.btn.z) gta2_n64_boot_map = in.btn.c_left ? 2 : in.btn.c_down ? 1 : 0;
}

#ifdef N64_MICROBENCH
/* What the machine (or the emulator's model of it) charges for the basic things: plain instructions, a call that misses the
 * instruction cache, a load that misses the data cache, float maths. Printed once at boot. */
static volatile uint32_t mb_sink;
static void microbench(void)
{
    const double ns = 1e9 / (double)TICKS_PER_SECOND;
    uint32_t t0, t1;
    uint8_t *buf = (uint8_t *)memalign(16384, 5 * 16384);
    if (!buf) { debugf("[mb] no memory\n"); return; }
    /* 1) integer loop, everything cached: 4 instructions per iteration */
    { uint32_t a = 1; t0 = TICKS_READ(); for (int i = 0; i < 200000; ++i) { a = a * 3u + (uint32_t)i; } t1 = TICKS_READ(); mb_sink = a; debugf("[mb] int loop: %.1f ns/iter\n", (double)(t1 - t0) * ns / 200000.0); }
    /* 2) float loop */
    { volatile float fa = 1.0f; float f = fa; t0 = TICKS_READ(); for (int i = 0; i < 200000; ++i) { f = f * 1.0001f + 0.5f; } t1 = TICKS_READ(); fa = f; debugf("[mb] float mul+add: %.1f ns/iter\n", (double)(t1 - t0) * ns / 200000.0); }
    { volatile float fa = 3.0f; float f = fa, g = 1.0f; t0 = TICKS_READ(); for (int i = 0; i < 50000; ++i) { g = g / f + 1.0f; } t1 = TICKS_READ(); fa = g; debugf("[mb] float div+add: %.1f ns/iter\n", (double)(t1 - t0) * ns / 50000.0); }
    { volatile float fa = 3.7f; float f = fa; int k = 0; t0 = TICKS_READ(); for (int i = 0; i < 100000; ++i) { k += (int)(f + (float)i); } t1 = TICKS_READ(); mb_sink = (uint32_t)k; debugf("[mb] int->float->int: %.1f ns/iter\n", (double)(t1 - t0) * ns / 100000.0); }
    /* 3) data cache: one load per 16-byte line over 2 KB (all hits after the first pass) and over 64 KB (all misses) */
    memset(buf, 1, 5 * 16384);
    { uint32_t a = 0; for (int r = 0; r < 2; ++r) for (int i = 0; i < 2048; i += 16) a += buf[i]; t0 = TICKS_READ(); for (int r = 0; r < 400; ++r) for (int i = 0; i < 2048; i += 16) a += buf[i]; t1 = TICKS_READ(); mb_sink = a; debugf("[mb] load, cache hit: %.1f ns\n", (double)(t1 - t0) * ns / (400.0 * 128.0)); }
    { uint32_t a = 0; t0 = TICKS_READ(); for (int r = 0; r < 12; ++r) for (int i = 0; i < 65536; i += 16) a += buf[i]; t1 = TICKS_READ(); mb_sink = a; debugf("[mb] load, cache miss: %.1f ns\n", (double)(t1 - t0) * ns / (12.0 * 4096.0)); }
    { t0 = TICKS_READ(); for (int r = 0; r < 12; ++r) for (int i = 0; i < 65536; i += 16) buf[i] = (uint8_t)r; t1 = TICKS_READ(); debugf("[mb] store, cache miss: %.1f ns\n", (double)(t1 - t0) * ns / (12.0 * 4096.0)); }
    /* 4) instruction cache: 4 tiny functions ("jr ra; nop") called in turn, first 32 bytes apart, then 16 KB apart (same cache line index) */
    for (int k = 0; k < 2; ++k) {
        void (*fn[4])(void);
        for (int i = 0; i < 4; ++i) {
            uint32_t *c = (uint32_t *)(buf + (k ? i * 16384 : i * 32));
            c[0] = 0x03E00008u; c[1] = 0;
            fn[i] = (void (*)(void))c;
        }
        data_cache_hit_writeback_invalidate(buf, 5 * 16384);
        inst_cache_hit_invalidate(buf, 5 * 16384);
        for (int i = 0; i < 8; ++i) fn[i & 3]();
        t0 = TICKS_READ(); for (int i = 0; i < 40000; ++i) fn[i & 3](); t1 = TICKS_READ();
        debugf("[mb] call+return, %s: %.1f ns\n", k ? "icache conflict" : "icache hit", (double)(t1 - t0) * ns / 40000.0);
    }
    free(buf);
}
#endif

int main(void)
{
    debug_init_isviewer();
#if defined(N64_PAD) && N64_PAD > 0
    { static const unsigned char *volatile keep; keep = g_n64_pad; (void)keep; } /* so the linker keeps the padding */
#endif
    debug_init_usblog();
    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_RESAMPLE);
    console_init();
    console_set_debug(true);
    console_set_render_mode(RENDER_MANUAL);
    /* margem de seguranca (overscan): sem ela, upscalers/TVs cortam as bordas (HUD, cantos). Valores em pontos do VI (640x480);
       ajuste com -DN64_OVERSCAN_X=.. -DN64_OVERSCAN_Y=.. (0 = imagem inteira, como no emulador). Tem que vir DEPOIS do console_init(). */
#ifndef N64_OVERSCAN_X
#define N64_OVERSCAN_X 36
#endif
#ifndef N64_OVERSCAN_Y
#define N64_OVERSCAN_Y 28
#endif
    vi_set_borders((vi_borders_t){ N64_OVERSCAN_X, N64_OVERSCAN_X, N64_OVERSCAN_Y, N64_OVERSCAN_Y });
    /* resolucao: console_init() deixa a tela em 640x240 (mesma configuracao do build que ficou sem linhas) */
    timer_init();
#ifdef N64_MICROBENCH
    microbench();
#endif
    gta2_install_exception_fixups();
    joypad_init();
    const int dfs = dfs_init(DFS_DEFAULT_LOCATION);
    if (dfs != 0)
    {
        printf("Falha ao abrir o sistema de arquivos da ROM (dfs_init=%d)\n", dfs);
        for (;;) { console_render(); }
    }
    console_render();

    read_boot_choice();
#if !defined(N64_NO_INTRO) && (!defined(N64_PROF) || defined(N64_INTRO_TEST))
    {   /* the intro movie (intro_n64.c), unless the menu is being skipped (Z held). Afterwards the display goes back to
           the mode console_init() left it in, which is the one the game is drawn in (640x240) */
        extern int gta2_n64_play_intro(int crt_margin);
        if (gta2_n64_boot_map < 0 && gta2_n64_play_intro(N64_OVERSCAN_X > 0 || N64_OVERSCAN_Y > 0)) {
            display_init(RESOLUTION_640x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_DISABLED);
            vi_set_borders((vi_borders_t){ N64_OVERSCAN_X, N64_OVERSCAN_X, N64_OVERSCAN_Y, N64_OVERSCAN_Y });
        }
    }
#endif
    reclaim_init(); /* the movie (if any) is over: its code becomes buffer space */
    console_render();

    { /* whole-game buffers first, at the bottom of the heap (see gta2_n64_render_prealloc) */
        extern void gta2_n64_render_prealloc(void);
        extern void gta2_frame_prealloc(void);
        extern void gta2_n64_big_prealloc(void);
        gta2_n64_render_prealloc();
        gta2_n64_big_prealloc();
        gta2_frame_prealloc();
    }
    const int rc = gta2_game_init();
    if (rc != 0)
    {
        printf("Motor saiu na inicializacao: codigo %d\n", rc);
        for (;;) { console_render(); }
    }

    for (;;)
    {
        poll_input();
        const int r = gta2_game_frame();
#ifdef N64_MEMLOG
        { static unsigned fr; if ((++fr % 10) == 0) { struct mallinfo mi = mallinfo(); { extern unsigned g_smallN, g_smallB; { extern char *__heap_end, *__heap_top; extern uint32_t gta2_n64_frames(void); debugf("[mem] f=%u pres=%u arena=%d used=%d free=%d brk=%p sp=%p heap_end=%p heap_top=%p\n", fr, (unsigned)gta2_n64_frames(), mi.arena, mi.uordblks, mi.fordblks, sbrk(0), (void*)&mi, (void*)__heap_end, (void*)__heap_top); } (void)g_smallN; (void)g_smallB; } } }
#endif
        if (r != -1)
        {
            printf("Jogo terminou: codigo %d\n", r);
            for (;;) { console_render(); }
        }
        if (!gta2_n64_gfx_active()) console_render();
    }
}


/* ---- listagem de pasta do DragonFS com curinga (usada por _findfirst) ---- */
static int glob_match(const char *p, const char *s)
{
    for (; *p; ++p, ++s) {
        if (*p == '*') {
            while (p[1] == '*') ++p;
            if (!p[1]) return 1;
            for (; *s; ++s) if (glob_match(p + 1, s)) return 1;
            return glob_match(p + 1, s);
        }
        if (!*s) return 0;
        if (*p != '?' && *p != *s) return 0;
    }
    return !*s;
}

/* devolve 1 e copia o 1o nome que casa com 'glob' (minusculas) em 'out'; 0 se nada. */
int gta2_n64_dir_find(const char *dirpath, const char *glob, char *out, int outsz)
{
    dir_t d;
    int r = dir_findfirst(dirpath, &d);
    while (r == 0) {
        if (d.d_type != DT_DIR && glob_match(glob, d.d_name)) {
            strncpy(out, d.d_name, outsz - 1);
            out[outsz - 1] = 0;
            return 1;
        }
        r = dir_findnext(dirpath, &d);
    }
    return 0;
}

/* endereco CPU (KSEG1, sem cache) de um arquivo do DragonFS + offset; NULL se nao achar */
const void *gta2_n64_rom_ptr(const char *romPath, unsigned offset)
{
    uint32_t a = dfs_rom_addr(romPath);
    if (!a && strncmp(romPath, "rom:/", 5) == 0) a = dfs_rom_addr(romPath + 5);
    if (!a) return NULL;
    uint32_t phys = a & 0x1FFFFFFF;
    if (phys < 0x10000000u) phys += 0x10000000u;      /* veio como offset na ROM */
    return (const void *)(uintptr_t)(0xA0000000u | (phys + offset));
}

#ifdef N64_MEMTRACE
void gta2_n64_dbg_mm(unsigned n, void *ra) { debugf("[mm] %u ra=%p\n", n, ra); }
/* depuracao: registra alocacoes grandes (>= 16 KB) com o endereco de quem chamou (build com -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc) */
#include <stdlib.h>
void *__real_malloc(size_t n); void *__real_calloc(size_t a, size_t b); void *__real_realloc(void *p, size_t n);
unsigned g_smallN, g_smallB;
void *__wrap_malloc(size_t n) { void *p = __real_malloc(n); if (n < 512) { g_smallN++; g_smallB += n; } if (n >= 512) debugf("[big] malloc %u -> %p ra=%p\n", (unsigned)n, p, __builtin_return_address(0)); return p; }
void *__wrap_calloc(size_t a, size_t b) { void *p = __real_calloc(a, b); if (a * b < 512) { g_smallN++; g_smallB += a * b; } if (a * b >= 512) debugf("[big] calloc %u -> %p ra=%p\n", (unsigned)(a * b), p, __builtin_return_address(0)); return p; }
void *__wrap_realloc(void *q, size_t n) { void *p = __real_realloc(q, n); if (n >= 512) debugf("[big] realloc %u -> %p ra=%p\n", (unsigned)n, p, __builtin_return_address(0)); return p; }
void *__real_memalign(size_t a, size_t n); void *__real_malloc_uncached(size_t n); void *__real_malloc_uncached_aligned(int a, size_t n);
void *__wrap_memalign(size_t a, size_t n) { void *p = __real_memalign(a, n); debugf("[big] memalign %u -> %p ra=%p\n", (unsigned)n, p, __builtin_return_address(0)); return p; }
void *__wrap_malloc_uncached(size_t n) { void *p = __real_malloc_uncached(n); debugf("[big] uncached %u -> %p ra=%p\n", (unsigned)n, p, __builtin_return_address(0)); return p; }
void *__wrap_malloc_uncached_aligned(int a, size_t n) { void *p = __real_malloc_uncached_aligned(a, n); debugf("[big] uncached_al %u -> %p ra=%p\n", (unsigned)n, p, __builtin_return_address(0)); return p; }
#endif
