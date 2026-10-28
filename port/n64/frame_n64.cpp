// Gerado de gta2-browser/port por tools/gen_n64_platform.py e adaptado para N64 (sem emscripten).
// Draw-list storage and presentation (see frame.h).
#include "render/frame.h"
#include "render/n64tex.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "platform/port_api.h"

extern "C" void gta2_n64_present(const void* cmds, int cmdCount, const void* verts, int vertCount, int refWidth);
static inline void gta2_js_present(const void* c, int cc, const void* v, int vc, int w) { gta2_n64_present(c, cc, v, vc, w); }

namespace
{
std::vector<FrameVertex> gVerts;
std::vector<FrameCommand> gCmds;
uint32_t gPresents = 0;

// Gameplay frames are as wide as the selected view (widescreen); everything
// else (menus, loading screens, movies) is laid out in the 640x480 box and
// shown pillarboxed. The game sets the layout per frame (Game::Draw).
int32_t gFrameWidth = 640;
float gHudOffsetX = 0.0f;

// The HUD part of the last full draw, replayed by interpolated draws between
// game updates (Game::Draw, port patch; D04).
std::vector<FrameVertex> gHudVerts;
std::vector<FrameCommand> gHudCmds;
size_t gHudCmdStart = 0, gHudVertStart = 0;

// N64: cache de pedacos do HUD que so mudam de vez em quando (ex.: barras de respeito das gangues). O pedaco e' desenhado
// normalmente e os comandos/vertices gerados sao guardados; nos quadros seguintes, se a chave (entradas do desenho) for a
// mesma, os comandos guardados sao recolocados sem passar pelo motor de sprites (que custa ~40 us por sprite).
// So guarda pedacos feitos apenas de triangulos (kind 0). Vale so para quadros consecutivos: se o pedaco ficar quadros
// sem ser pedido, os slots de textura dos comandos podem ter sido reaproveitados e o cache e' descartado.
struct HudCacheSlot
{
    std::vector<FrameVertex> v;
    std::vector<FrameCommand> c;
    uint32_t key = 0, frame = 0;
    float offx = 0.0f;
    bool valid = false;
    size_t cs = 0, vs = 0;
};
HudCacheSlot gHudCache[4];

// N64: a RAM e' o limite (a pilha de heap encosta no fim da memoria). Se uma tela de missao com muito texto pede mais vertices
// do que o vetor comporta, o vector dobra de tamanho e, sem RAM para a copia, o operator new devolve nulo e o jogo trava
// (escrita em endereco invalido). Antes de crescer, testa se a copia maior cabe; se nao cabe, descarta o que passou do limite.
template <class T>
bool ensureRoom(std::vector<T>& v, size_t extra)
{
    if (v.size() + extra <= v.capacity())
    {
        return true;
    }
    // Fixed size, allocated once on first use (in the front end, before any map is loaded) and never grown: a list that
    // doubled during play (explosions, briefings) left big blocks in the middle of the heap, and after a few level loads
    // the map's 224 KB column buffer no longer found a free run (out of memory with 900 KB free). A frame that needs more
    // than this simply does not draw the extra sprites.
    const size_t kBytes = sizeof(T) == sizeof(FrameVertex) ? 144 * 1024 : 48 * 1024; // ~6000 vertices, ~1200 commands
    if (v.capacity() == 0)
    {
        v.reserve(kBytes / sizeof(T));
    }
    return v.size() + extra <= v.capacity();
}
} // namespace

// ---------------------------------------------------------------------------
// N64: agrupamento e cache de texto (usado por Draw.cpp::DrawText_5D8A10).
// Cada letra e' um quadrilatero com textura propria; uma frase de 150 letras gerava ~150 comandos (um upload de
// textura cada) e custava ~25 ms por quadro enquanto o briefing da missao ficava na tela.
//  1) as letras de uma chamada de DrawText sao reagrupadas por textura (uma letra repetida vira um comando so);
//  2) o resultado fica guardado: se o quadro seguinte desenha exatamente o mesmo texto (mesma frase, posicao, fonte,
//     paleta), ele e' recolocado direto, sem repetir o trabalho do motor por letra.
// Os tokens de textura guardados continuam validos enquanto o texto for desenhado a cada quadro (o renderer marca o
// slot como em uso); se passar um quadro sem ele, a entrada e' descartada e refeita.
namespace
{
constexpr uint32_t kTxtMaxVerts = 1200; // ~28 KB: ate 200 letras guardadas
constexpr uint32_t kTxtMinVerts = 48;   // menos de 8 letras: nao compensa
constexpr int kTxtMaxCmds = 48;         // texturas distintas por frase
constexpr int kTxtEntries = 8;
constexpr uint32_t kTokBase = 0x10000u; // mesmo valor de TEX_TOKEN_BASE (render_n64.c)
constexpr uint32_t kTokCount = N64_CACHE_N; // mesmo valor de CACHE_N (render_n64.c)

struct TxtCmd
{
    uint32_t texture, count;
};
struct TxtEntry
{
    uint64_t key;
    uint32_t last, vfirst, nverts, blend;
    uint32_t stamp; // muda a cada vez que a entrada e' (re)gravada: o renderer usa para saber se o bloco gravado ainda vale
    int ncmds;
    bool used;
    TxtCmd cmds[kTxtMaxCmds];
};

FrameVertex gTxtPool[kTxtMaxVerts];
TxtEntry gTxtEnt[kTxtEntries];
uint32_t gTxtPoolUsed = 0;
bool gTxtActive = false;
bool gForceNewCmd = false;
uint64_t gTxtKey = 0;
size_t gTxtCmd0 = 0, gTxtVert0 = 0;
uint32_t gTxtStamp = 0;

constexpr uint32_t kTextPoolSize = 3072; // frases do quadro (fonte embutida)
char gTextPool[kTextPoolSize];
uint32_t gTextPoolUsed = 0;

inline uint64_t txtMix(uint64_t h, uint32_t v)
{
    h ^= v;
    return h * 1099511628211ULL;
}
inline uint32_t txtBits(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    return u;
}
void txtReset()
{
    for (int i = 0; i < kTxtEntries; ++i)
    {
        gTxtEnt[i].used = false;
    }
    gTxtPoolUsed = 0;
}
} // namespace

extern "C" {

// N64: the draw lists live for the whole game; reserved at boot (main_n64.c), before any level memory, so they do not
// split the heap between level loads
void gta2_frame_prealloc()
{
    ensureRoom(gVerts, 1);
    ensureRoom(gCmds, 1);
}

void gta2_frame_push_vertex(uint32_t texture, uint32_t blend, const FrameVertex& v)
{
    if (!ensureRoom(gVerts, 1))
    {
        return;
    }
    if (gForceNewCmd || gCmds.empty() || gCmds.back().kind != 0 || gCmds.back().texture != texture || gCmds.back().blend != blend)
    {
        if (!ensureRoom(gCmds, 1))
        {
            return;
        }
        FrameCommand c = {0, texture, blend, static_cast<uint32_t>(gVerts.size()), 0, 0, {0, 0, 0, 0}};
        gCmds.push_back(c);
        gForceNewCmd = false;
    }
    gCmds.back().count++;
    gVerts.push_back(v);
    gVerts.back().x += gHudOffsetX;
}

// A whole quad or triangle at once (gbh_n64.cpp Draw): one room check and one command check for all its vertices
void gta2_frame_push_vertices(uint32_t texture, uint32_t blend, const FrameVertex* v, int n)
{
    if (!ensureRoom(gVerts, static_cast<size_t>(n)))
    {
        return;
    }
    if (gForceNewCmd || gCmds.empty() || gCmds.back().kind != 0 || gCmds.back().texture != texture || gCmds.back().blend != blend)
    {
        if (!ensureRoom(gCmds, 1))
        {
            return;
        }
        FrameCommand c = {0, texture, blend, static_cast<uint32_t>(gVerts.size()), 0, 0, {0, 0, 0, 0}};
        gCmds.push_back(c);
        gForceNewCmd = false;
    }
    gCmds.back().count += static_cast<uint32_t>(n);
    const size_t base = gVerts.size();
    gVerts.insert(gVerts.end(), v, v + n);
    if (gHudOffsetX != 0.0f)
    {
        for (int i = 0; i < n; ++i)
        {
            gVerts[base + i].x += gHudOffsetX;
        }
    }
}

// Texto com a fonte embutida da libdragon: o motor entrega a frase (ASCII, com ^00/^01 para a cor) e a posicao;
// o renderer desenha com rdpq_text na hora certa da lista (comando kind 2).
// first = deslocamento da frase em gTextPool, count = bytes, colour = numero de linhas,
// rect = {x*256, y*256, altura da linha*256 (espaco 640x480), 0}.
const char* gta2_frame_text_pool()
{
    return gTextPool;
}

void gta2_frame_text(const char* utf8, int nbytes, float x, float y, float lineRef, int nlines)
{
    if (!utf8 || nbytes <= 0 || gTextPoolUsed + static_cast<uint32_t>(nbytes) > kTextPoolSize)
    {
        return; // pool cheio: a frase deste quadro e' descartada
    }
    memcpy(gTextPool + gTextPoolUsed, utf8, static_cast<size_t>(nbytes));
    FrameCommand c = {2,
                      0,
                      0,
                      gTextPoolUsed,
                      static_cast<uint32_t>(nbytes),
                      static_cast<uint32_t>(nlines),
                      {static_cast<int32_t>((x + gHudOffsetX) * 256.0f), static_cast<int32_t>(y * 256.0f), static_cast<int32_t>(lineRef * 256.0f), 0}};
    if (!ensureRoom(gCmds, 1))
    {
        return;
    }
    gCmds.push_back(c);
    gTextPoolUsed += static_cast<uint32_t>(nbytes);
}

uint64_t gta2_text_key(const uint16_t* s, float x, float y, unsigned font, float scale, int ptype, unsigned pal, int alpha, unsigned flags)
{
    if (!s)
    {
        return 0;
    }
    uint64_t h = 1469598103934665603ULL;
    for (const uint16_t* p = s; *p; ++p) // wchar_t do jogo = 16 bits (-fshort-wchar)
    {
        h = txtMix(h, *p);
    }
    h = txtMix(h, txtBits(x));
    h = txtMix(h, txtBits(y));
    h = txtMix(h, font);
    h = txtMix(h, txtBits(scale));
    h = txtMix(h, static_cast<uint32_t>(ptype));
    h = txtMix(h, pal);
    h = txtMix(h, static_cast<uint32_t>(alpha));
    h = txtMix(h, flags);
    h = txtMix(h, txtBits(gHudOffsetX));
    return h;
}

// 1 se a frase foi recolocada do cache (o chamador nao deve desenhar de novo)
int gta2_text_replay(uint64_t key)
{
    for (int i = 0; i < kTxtEntries; ++i)
    {
        TxtEntry& e = gTxtEnt[i];
        if (!e.used || e.key != key)
        {
            continue;
        }
        if (e.last + 1u < gPresents) // passou um quadro sem desenhar: os slots de textura podem ter sido reciclados
        {
            e.used = false;
            return 0;
        }
        e.last = gPresents;
        if (!ensureRoom(gVerts, e.nverts) || !ensureRoom(gCmds, static_cast<size_t>(e.ncmds)))
        {
            return 1; // sem RAM: a frase some neste quadro (nao e' recolocada nem refeita)
        }
        const uint32_t base = static_cast<uint32_t>(gVerts.size());
        gVerts.insert(gVerts.end(), gTxtPool + e.vfirst, gTxtPool + e.vfirst + e.nverts);
        uint32_t first = base;
        for (int j = 0; j < e.ncmds; ++j)
        {
            // rect[2] = stamp da entrada, rect[3] = indice da entrada + 1: marca "texto estatico" para o renderer gravar um bloco do RSP
            FrameCommand c = {0, e.cmds[j].texture, e.blend, first, e.cmds[j].count, 0,
                              {0, 0, static_cast<int32_t>(e.stamp), i + 1}};
            gCmds.push_back(c);
            first += e.cmds[j].count;
        }
        gForceNewCmd = true; // o proximo triangulo desenhado nao pode ser anexado a um comando marcado
        return 1;
    }
    return 0;
}

void gta2_text_begin(uint64_t key)
{
    gTxtActive = true;
    gTxtKey = key;
    gTxtCmd0 = gCmds.size();
    gTxtVert0 = gVerts.size();
    gForceNewCmd = true; // as letras desta frase nunca entram num comando anterior
}

void gta2_text_end()
{
    if (!gTxtActive)
    {
        return;
    }
    gTxtActive = false;
    gForceNewCmd = false;

    const size_t c0 = gCmds.size() >= gTxtCmd0 ? gTxtCmd0 : gCmds.size();
    const size_t v0 = gTxtVert0;
    const size_t nc = gCmds.size() - c0;
    const size_t nv = gVerts.size() - v0;
    if (nc == 0 || nv < kTxtMinVerts || nv > kTxtMaxVerts)
    {
        return;
    }

    // so frases limpas: triangulos, mesma mistura, vertices em sequencia
    const uint32_t blend = gCmds[c0].blend;
    uint32_t expect = static_cast<uint32_t>(v0);
    for (size_t i = 0; i < nc; ++i)
    {
        const FrameCommand& c = gCmds[c0 + i];
        if (c.kind != 0 || c.blend != blend || c.first != expect)
        {
            return;
        }
        expect += c.count;
    }
    if (expect != static_cast<uint32_t>(gVerts.size()))
    {
        return;
    }

    // texturas distintas, na ordem em que aparecem, e o total de vertices de cada uma
    uint32_t tex[kTxtMaxCmds];
    uint32_t cnt[kTxtMaxCmds];
    int nt = 0;
    bool cacheable = true;
    for (size_t i = 0; i < nc; ++i)
    {
        const FrameCommand& c = gCmds[c0 + i];
        if (c.texture - kTokBase >= kTokCount) // sem token de textura valido: nao guarda
        {
            cacheable = false;
        }
        int j = 0;
        while (j < nt && tex[j] != c.texture)
        {
            ++j;
        }
        if (j == nt)
        {
            if (nt == kTxtMaxCmds)
            {
                return;
            }
            tex[nt] = c.texture;
            cnt[nt] = 0;
            ++nt;
        }
        cnt[j] += c.count;
    }

    // entrada do cache: a da mesma frase, senao uma livre, senao zera tudo
    int ei = -1;
    for (int i = 0; i < kTxtEntries; ++i)
    {
        if (gTxtEnt[i].used && gTxtEnt[i].key == gTxtKey)
        {
            ei = i;
            break;
        }
    }
    if (ei < 0)
    {
        for (int i = 0; i < kTxtEntries; ++i)
        {
            if (!gTxtEnt[i].used)
            {
                ei = i;
                break;
            }
        }
    }
    if (ei < 0 || gTxtPoolUsed + static_cast<uint32_t>(nv) > kTxtMaxVerts)
    {
        txtReset();
        ei = 0;
    }

    // a area livre do pool serve de copia da ordem original; depois recebe a versao agrupada
    FrameVertex* tmp = gTxtPool + gTxtPoolUsed;
    memcpy(tmp, &gVerts[v0], nv * sizeof(FrameVertex));
    size_t out = v0;
    for (int j = 0; j < nt; ++j)
    {
        for (size_t i = 0; i < nc; ++i)
        {
            const FrameCommand& c = gCmds[c0 + i];
            if (c.texture != tex[j])
            {
                continue;
            }
            memcpy(&gVerts[out], tmp + (c.first - static_cast<uint32_t>(v0)), c.count * sizeof(FrameVertex));
            out += c.count;
        }
    }
    FrameCommand merged = gCmds[c0];
    uint32_t first = static_cast<uint32_t>(v0);
    for (int j = 0; j < nt; ++j)
    {
        merged.texture = tex[j];
        merged.first = first;
        merged.count = cnt[j];
        gCmds[c0 + static_cast<size_t>(j)] = merged;
        first += cnt[j];
    }
    gCmds.resize(c0 + static_cast<size_t>(nt));

    if (!cacheable)
    {
        gTxtEnt[ei].used = false;
        return;
    }
    memcpy(tmp, &gVerts[v0], nv * sizeof(FrameVertex)); // versao agrupada
    TxtEntry& e = gTxtEnt[ei];
    e.key = gTxtKey;
    e.last = gPresents;
    e.vfirst = gTxtPoolUsed;
    e.nverts = static_cast<uint32_t>(nv);
    e.blend = blend;
    e.ncmds = nt;
    e.stamp = ++gTxtStamp;
    for (int j = 0; j < nt; ++j)
    {
        e.cmds[j].texture = tex[j];
        e.cmds[j].count = cnt[j];
    }
    e.used = true;
    gTxtPoolUsed += static_cast<uint32_t>(nv);
}

void gta2_frame_clear(uint8_t r, uint8_t g, uint8_t b, int32_t left, int32_t top, int32_t right, int32_t bottom)
{
    FrameCommand c = {1, 0, 0, 0, 0, static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8) | (static_cast<uint32_t>(b) << 16),
                      {left, top, right, bottom}};
    if (!ensureRoom(gCmds, 1))
    {
        return;
    }
    gCmds.push_back(c);
}

// N64: desenha a imagem 'img' (tabela de imagens do gbh, menu) -- origem left,top,right,bottom em pixels da imagem, destino x,y no espaco 640x480.
// kind 3: texture = indice, first/count = x/y (inteiros com sinal), rect = origem.
void gta2_frame_blit(int img, int l, int t, int r, int b, int x, int y)
{
    FrameCommand c = {3, static_cast<uint32_t>(img), 0, static_cast<uint32_t>(x), static_cast<uint32_t>(y), 0, {l, t, r, b}};
    if (!ensureRoom(gCmds, 1))
    {
        return;
    }
    gCmds.push_back(c);
}

void gta2_frame_present()
{
    gta2_js_present(gCmds.data(), static_cast<int>(gCmds.size()), gVerts.data(), static_cast<int>(gVerts.size()), gFrameWidth);
    gCmds.clear();
    gVerts.clear();
    gFrameWidth = 640;
    gHudOffsetX = 0.0f;
    gTextPoolUsed = 0;
    ++gPresents;
}

// Read by the game's HUD (Hud.cpp, port patch): anchor HUD parts to the screen edges in widescreen.
int32_t gGta2PortHudEdges = 1;

void gta2_port_set_hud_edges(int on)
{
    gGta2PortHudEdges = on ? 1 : 0;
}

void Gta2Port_HudBegin()
{
    gHudCmdStart = gCmds.size();
    gHudVertStart = gVerts.size();
}

void Gta2Port_HudEnd()
{
    gHudVerts.assign(gVerts.begin() + static_cast<long>(gHudVertStart), gVerts.end());
    gHudCmds.assign(gCmds.begin() + static_cast<long>(gHudCmdStart), gCmds.end());
    for (FrameCommand& c : gHudCmds)
    {
        if (c.kind == 0)
        {
            c.first -= static_cast<uint32_t>(gHudVertStart);
        }
    }
}

// 1 = os comandos guardados foram recolocados (o chamador nao deve desenhar o pedaco)
int Gta2Port_CacheTry(int slot, uint32_t key, uint32_t frame)
{
    HudCacheSlot& s = gHudCache[slot & 3];
    if (!s.valid || s.key != key || s.offx != gHudOffsetX || static_cast<uint32_t>(frame - s.frame) > 1u)
    {
        return 0;
    }
    if (!ensureRoom(gVerts, s.v.size()) || !ensureRoom(gCmds, s.c.size()))
    {
        return 0;
    }
    const uint32_t base = static_cast<uint32_t>(gVerts.size());
    gVerts.insert(gVerts.end(), s.v.begin(), s.v.end());
    for (FrameCommand c : s.c)
    {
        c.first += base;
        gCmds.push_back(c);
    }
    s.frame = frame;
    return 1;
}

void Gta2Port_CacheBegin(int slot, uint32_t key, uint32_t frame)
{
    HudCacheSlot& s = gHudCache[slot & 3];
    s.valid = false;
    s.key = key;
    s.frame = frame;
    s.offx = gHudOffsetX;
    s.cs = gCmds.size();
    s.vs = gVerts.size();
    gForceNewCmd = true; // o primeiro vertice do pedaco nao pode se juntar a um comando anterior
}

void Gta2Port_CacheEnd(int slot)
{
    HudCacheSlot& s = gHudCache[slot & 3];
    if (gCmds.size() < s.cs || gVerts.size() < s.vs)
    {
        return;
    }
    s.v.assign(gVerts.begin() + static_cast<long>(s.vs), gVerts.end());
    s.c.assign(gCmds.begin() + static_cast<long>(s.cs), gCmds.end());
    bool ok = !s.c.empty() && s.v.size() < 600 && s.c.size() < 60;
    for (FrameCommand& c : s.c)
    {
        if (c.kind != 0)
        {
            ok = false;
            break;
        }
        c.first -= static_cast<uint32_t>(s.vs);
    }
    s.valid = ok;
}

void Gta2Port_HudReplay()
{
    if (!ensureRoom(gVerts, gHudVerts.size()) || !ensureRoom(gCmds, gHudCmds.size()))
    {
        return;
    }
    const uint32_t base = static_cast<uint32_t>(gVerts.size());
    gVerts.insert(gVerts.end(), gHudVerts.begin(), gHudVerts.end());
    for (FrameCommand c : gHudCmds)
    {
        if (c.kind == 0)
        {
            c.first += base;
        }
        gCmds.push_back(c);
    }
}

// Read by the game (Source/port_interpolation.cpp): display-rate interpolation on/off.
extern "C" int32_t gGta2PortInterpolate;
void gta2_port_set_interpolation(int on)
{
    gGta2PortInterpolate = on ? 1 : 0;
}

// ---- HUD layer ----
// The HUD is laid out for 640x480 and the framebuffer is 320x240, so at 1:1 everything would be drawn at half size
// (blurry text). While Hud::DrawGui draws, each element is scaled by N64_HUD_SCALE around its anchor (the screen
// corner/edge it belongs to), then the whole HUD is pulled inside a TV-safe margin (N64_HUD_SAFE_X/Y, in 640x480 units).
#ifndef N64_HUD_SCALE
#define N64_HUD_SCALE 1.33f       /* briefing text and face, centred messages */
#endif
#ifndef N64_HUD_SCALE_TOP
#define N64_HUD_SCALE_TOP 1.18f   /* top-corner stats: money, hearts, gang bars, wanted level */
#endif
#ifndef N64_HUD_SAFE_X
#define N64_HUD_SAFE_X 16.0f
#endif
#ifndef N64_HUD_SAFE_Y
#define N64_HUD_SAFE_Y 12.0f
#endif
int gN64HudOn;
static float gHudK = 1.0f, gHudAx, gHudAy;

float gta2_n64_hud_scale(int which)
{
    return which == 2 ? N64_HUD_SCALE_TOP : which == 1 ? N64_HUD_SCALE : 1.0f;
}

void gta2_n64_hud_begin(float ax, float ay, int which)
{
    gN64HudOn = 1;
    gHudK = gta2_n64_hud_scale(which);
    gHudAx = ax;
    gHudAy = ay;
}

void gta2_n64_hud_end(void)
{
    gN64HudOn = 0;
}

void gta2_n64_hud_xform(float* x, float* y)
{
    const float sx = gHudAx + (*x - gHudAx) * gHudK;
    const float sy = gHudAy + (*y - gHudAy) * gHudK;
    *x = N64_HUD_SAFE_X + sx * ((640.0f - 2.0f * N64_HUD_SAFE_X) / 640.0f);
    *y = N64_HUD_SAFE_Y + sy * ((480.0f - 2.0f * N64_HUD_SAFE_Y) / 480.0f);
}

// Translucent rectangle (pause menu panel and bars), 640x480 units; rgba = r | g << 8 | b << 16 | a << 24 (a 1..254).
void gta2_n64_rect(float x0, float y0, float x1, float y1, uint32_t rgba)
{
    if (gN64HudOn)
    {
        gta2_n64_hud_xform(&x0, &y0);
        gta2_n64_hud_xform(&x1, &y1);
    }
    FrameCommand c = {1, 0, 0, 0, 0, rgba,
                      {static_cast<int32_t>(x0 + gHudOffsetX), static_cast<int32_t>(y0), static_cast<int32_t>(x1 + gHudOffsetX), static_cast<int32_t>(y1)}};
    if (!ensureRoom(gCmds, 1))
    {
        return;
    }
    gCmds.push_back(c);
}

void Gta2Port_SetFrameLayout(int32_t width, int32_t hudOffsetX)
{
    gFrameWidth = width > 0 ? width : 640;
    gHudOffsetX = static_cast<float>(hudOffsetX);
}

uint32_t gta2_port_present_count()
{
    return gPresents;
}

} // extern "C"
