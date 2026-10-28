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
    const uint32_t now_ms = (uint32_t)get_ticks_ms();
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

int main(void)
{
    debug_init_isviewer();
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
