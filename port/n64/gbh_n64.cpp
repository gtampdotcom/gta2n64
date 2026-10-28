// Gerado de gta2-browser/port por tools/gen_n64_platform.py e adaptado para N64 (sem emscripten).
// The GBH renderer interface (d3ddll.dll) for the browser.
//
// The game hands this layer pre-transformed vertices in a 640x480 reference
// space, 8-bit paletted textures and per-draw flags. Each draw becomes
// triangles in a per-frame vertex array, batched while texture and blend mode
// stay the same; the WebGL2 renderer (web/renderer.js) draws the batches when
// the frame is presented. Textures are converted to RGBA once, on register,
// palette change or unlock. No depth buffer: draw order is the game's (D07).
//
// Semantics (flags, palette layout, lighting, tile UV rotation) follow the
// documented behaviour of the original driver interface.
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#ifdef N64
#include "render/n64tex.h"
extern "C" void gta2_n64_tex_forget(const N64Tex* t);
extern "C" uint32_t gta2_n64_tex_capture(N64Tex* t, float a_scr, float a_tex, int additive);
extern "C" int gta2_n64_tex_token_empty(uint32_t token);
extern "C" void gta2_n64_hud_xform(float* x, float* y);
extern "C" int gN64HudOn;
extern "C" int gta2_game_in_menu(void);
#include <libdragon.h>
#include <malloc.h>
#include <stdio.h>
extern "C" void gta2_frame_blit(int img, int l, int t, int r, int b, int x, int y); /* frame_n64.cpp: comando de blit de imagem (menu) */
#endif

#include "gbh_graphics.hpp"
#include "platform/port_api.h"
#include "render/frame.h"

// ---------------------------------------------------------------------------
// JavaScript side (web/renderer.js via Module.gta2Renderer)
static inline void gta2_js_upload_texture(int id, int w, int h, const void* rgba) {  }
static inline void gta2_js_free_texture(int id) {  }
static inline void gta2_js_free_all_textures() {  }

namespace
{
constexpr u32 kFitToTexture = 0x10000;
constexpr u32 kAdditive = 0x200;
constexpr u32 kAlphaBlend = 0x180;
constexpr u32 kLighting = 0x8000;
constexpr u32 kUseVertexDiffuse = 0x2000;
constexpr u32 kTileUVGiven = 0x4000;

// Palettes: the game passes 256 32-bit colours stored as a column (stride 64
// DWORDs). The 16-bit copy the game can read through STexture is kept, and
// the palette id is stored just before it so textures can find the source.
struct Palette
{
    DWORD* original = nullptr;
    u16* data565 = nullptr; // points 2 u16 past the allocation (id stored there)
    u32 loaded = 0;
};
#ifdef N64
// N64: a capacidade e' dimensionada em tempo de execucao pelo numero REAL de paletas fisicas da fase
// (gta2_n64_palettes_reserve, chamada por TextureCache_15D8::LoadPals). Antes era fixa em 1024: uma textura cuja paleta
// fisica passava de 1024 ficava sem paleta e era desenhada lisa (carro de gangue vermelho sem detalhes).
int kMaxPalettes = 0;
Palette* gPalettes = nullptr;
u16 (*gStub)[4] = nullptr; // [0..1] = id (lido por PaletteIdOf), [2..3] = reservado
unsigned gPalRejects = 0;
void PalReject(const char* who, s32 id)
{
    (void)who;
    (void)id;
    ++gPalRejects;
}
#else
constexpr int kMaxPalettes = 16384;
Palette gPalettes[kMaxPalettes];
#endif

u16 ToRGB565(u32 rgb)
{
    const u32 r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    return static_cast<u16>(((r * 31 / 255) << 11) | ((g * 63 / 255) << 5) | (b * 31 / 255));
}

int PaletteIdOf(const u16* data565)
{
    if (!data565)
    {
        return -1;
    }
    u32 id;
    memcpy(&id, data565 - 2, 4);
    return static_cast<int>(id);
}

struct ImageEntry
{
    BOOL loaded;
    DWORD w, h;
    STexture* tex;
    u8* pixels; // a copy of the 16-bit image, to upload it again after a quick load
};
ImageEntry* gImages = nullptr;
u32 gImageCount = 0;

u32 gNextTextureId = 1;
std::vector<u8> gConvert;

// Paletted textures point into the style file's 256-pixel-wide pages, so
// every row is 256 bytes apart (the game indexes them as data[row * 256]).
constexpr int kPagePitch = 256;

// The game re-fills some STexture objects many times per frame (the shared
// text texture, digits, car damage) while the renderer draws at present time.
// Textures are therefore content-addressed: each distinct (pixels, size,
// palette) gets its own GPU texture, and STexture::field_1C_ptr is pointed at
// the one matching its current content when it is registered or unlocked.
std::unordered_map<uint64_t, intptr_t> gTextureByContent;

// Every texture the game holds, so a quick load can rebuild the GPU side (savestate_web.cpp).
std::unordered_set<STexture*> gLiveTextures;

uint64_t ContentKey(const u8* pix, int w, int h, int palId)
{
    uint64_t hash = 1469598103934665603ull;
    auto mix = [&](uint64_t v) {
        hash ^= v;
        hash *= 1099511628211ull;
    };
    mix(static_cast<uint64_t>(w) << 32 | static_cast<uint32_t>(h));
    mix(static_cast<uint32_t>(palId));
    for (int y = 0; y < h; ++y)
    {
        const u8* row = pix + static_cast<size_t>(y) * kPagePitch;
        for (int x = 0; x < w; ++x)
        {
            mix(row[x]);
        }
    }
    return hash;
}

void UploadPaletted(STexture* t)
{
#ifdef N64
    // N64: nao converte aqui. Guarda so (pixels, paleta, tamanho) num descritor; o renderer converte sob demanda.
    {
        N64Tex* info = reinterpret_cast<N64Tex*>(t->field_1C_ptr);
        if (!info)
        {
            info = static_cast<N64Tex*>(calloc(1, sizeof(N64Tex)));
            if (!info)
            {
                return;
            }
            info->slot = -1;
            t->field_1C_ptr = info;
        }
        const int palId = PaletteIdOf(t->field_18_pPaltData);
        info->pix = t->field_14_original_pixel_data_ptr;
        info->w = t->field_E_width;
        info->h = t->field_10_height;
        info->pal = (palId >= 0 && palId < kMaxPalettes && gPalettes[palId].loaded) ? reinterpret_cast<const uint32_t*>(gPalettes[palId].original) : nullptr;
        info->palid = static_cast<int16_t>(palId);
        info->gen++;
        return;
    }
#endif
    const int w = t->field_E_width, h = t->field_10_height;
    const u8* pix = t->field_14_original_pixel_data_ptr;
    const int palId = PaletteIdOf(t->field_18_pPaltData);
    if (!pix || w <= 0 || h <= 0 || w > kPagePitch)
    {
        return;
    }
    const uint64_t key = ContentKey(pix, w, h, palId);
    auto it = gTextureByContent.find(key);
    if (it != gTextureByContent.end())
    {
        t->field_1C_ptr = reinterpret_cast<void*>(it->second);
        return;
    }
    const intptr_t id = gNextTextureId++;
    gTextureByContent.emplace(key, id);
    t->field_1C_ptr = reinterpret_cast<void*>(id);

    gConvert.resize(static_cast<size_t>(w) * h * 4);
    const DWORD* pal = palId >= 0 && palId < kMaxPalettes && gPalettes[palId].loaded ? gPalettes[palId].original : nullptr;
    for (int y = 0; y < h; ++y)
    {
        const u8* row = pix + static_cast<size_t>(y) * kPagePitch;
        for (int x = 0; x < w; ++x)
        {
            const u8 idx = row[x];
            const u32 rgb = pal ? pal[idx * 64] : (idx * 0x010101u);
            u8* o = &gConvert[(static_cast<size_t>(y) * w + x) * 4];
            o[0] = (rgb >> 16) & 0xFF;
            o[1] = (rgb >> 8) & 0xFF;
            o[2] = rgb & 0xFF;
            o[3] = idx == 0 ? 0 : 255; // colour 0 of every palette is transparent
        }
    }
    gta2_js_upload_texture(static_cast<int>(id), w, h, gConvert.data());
}

// 16-bit RGB555 TGA images (front-end backgrounds), stored bottom-up.
void UploadImage(STexture* t, const u8* src)
{
#ifdef N64
    (void)t; (void)src; // sem renderer ainda: nao converte (evita 1,2 MB temporarios na RAM de 8 MB)
    return;
#endif
    const int w = t->field_E_width, h = t->field_10_height;
    gConvert.resize(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            u16 p;
            memcpy(&p, src + (static_cast<size_t>(y) * w + x) * 2, 2);
            u8* o = &gConvert[(static_cast<size_t>(h - 1 - y) * w + x) * 4];
            o[0] = static_cast<u8>(((p >> 10) & 31) * 255 / 31);
            o[1] = static_cast<u8>(((p >> 5) & 31) * 255 / 31);
            o[2] = static_cast<u8>((p & 31) * 255 / 31);
            o[3] = 255;
        }
    }
    gta2_js_upload_texture(reinterpret_cast<intptr_t>(t->field_1C_ptr), w, h, gConvert.data());
}

STexture* NewTexture(s32 w, s32 h, BYTE* data)
{
    auto* t = static_cast<STexture*>(calloc(1, sizeof(STexture)));
    static u16 sNextId = 0;
    t->field_0_id = sNextId++;
    t->field_E_width = static_cast<u16>(w);
    t->field_10_height = static_cast<u16>(h);
    t->field_14_original_pixel_data_ptr = data;
#ifndef N64
    gLiveTextures.insert(t);
#endif
    return t;
}

// Window and lights
float gWinLeft = 0, gWinTop = 0, gWinRight = 640, gWinBottom = 480;
float gAmbient = 255.0f; // ambient * 255, as the original driver keeps it
// Luzes: ligadas no modo PC, desligadas no modo PSX (opcao MODO do menu): 0 = sem luz (mais rapido), 2 = suave por canto do bloco (padrao, ~9%). O modo 1 (chapada por quadrilatero) existe no codigo mas nao esta no menu.
#ifndef N64_LUZES
#define N64_LUZES 0 /* padrao = modo PSX (sem luzes); o menu liga/desliga junto com o modo PC/PSX */
#endif
int gN64Luzes = N64_LUZES;
extern "C" int gta2_n64_get_luzes(void)
{
    return gN64Luzes >= 2 ? 1 : 0; // menu: 0 = desligadas, 1 = ligadas (suaves)
}
extern "C" void gta2_n64_set_luzes(int v)
{
    gN64Luzes = v > 0 ? 2 : 0;
}

struct Light
{
    u32 flags;
    float brightness, radius, radiusSq, invRadius, x, y, z, r, g, b;
    float invRadiusSq, pr, pg, pb; // N64: 1/raio^2 e cor*brilho (por luz, calculados uma vez)
};
Light gLights[256];
u32 gLightCount = 0;
// N64: so as luzes que passam no filtro de flags (0x30000 == 0x10000) entram na lista; o resto nunca contribui.
u16 gActiveLights[256];
u32 gActiveCount = 0;

// N64: grade espacial das luzes ativas (celulas de 4x4 blocos, 32x32 celulas com repeticao = 128 blocos, mais que a tela).
// Cada luz entra em todas as celulas que o seu raio toca; um desenho so olha as celulas sob a sua caixa em vez de testar todas as
// luzes (antes: ~N luzes x ~centenas de desenhos por quadro). O resultado e' o mesmo: as luzes candidatas passam pelo mesmo teste
// exato de distancia. Dedupe por carimbo (uma luz aparece em varias celulas).
constexpr int kBinDim = 32;
constexpr int kBinPool = 3072;
int16_t gBinHead[kBinDim * kBinDim];
int16_t gBinNext[kBinPool];
uint8_t gBinLight[kBinPool];
u32 gBinUsed = 0;
u32 gLightStamp[256];
u32 gDrawStamp = 0;
inline int BinCell(float v)
{
    const int c = static_cast<int>(v * 0.25f);
    return c < 0 ? 0 : c;
}
void BinInsert(u32 idx)
{
    const Light& l = gLights[idx];
    const int x0 = BinCell(l.x - l.radius), x1 = BinCell(l.x + l.radius);
    const int y0 = BinCell(l.y - l.radius), y1 = BinCell(l.y + l.radius);
    for (int cy = y0; cy <= y1; ++cy)
    {
        for (int cx = x0; cx <= x1; ++cx)
        {
            if (gBinUsed >= static_cast<u32>(kBinPool))
            {
                return; // pool cheio: a luz some so nas celulas restantes (nao deveria acontecer)
            }
            const int cell = (cx & (kBinDim - 1)) + (cy & (kBinDim - 1)) * kBinDim;
            gBinLight[gBinUsed] = static_cast<uint8_t>(idx);
            gBinNext[gBinUsed] = gBinHead[cell];
            gBinHead[cell] = static_cast<int16_t>(gBinUsed++);
        }
    }
}

// 'ls' = luzes que alcancam este desenho (ja cortadas por distancia ao quadrilatero): o resultado e' o mesmo de percorrer todas.
// N64: a soma das luzes e' calculada UMA vez por desenho (no centro do quadrilatero) e aplicada aos 4 vertices: iluminacao "chapada" por
// quadrilatero (estilo PS1), ~4x mais barata que por vertice. Cada vertice ainda multiplica pela sua propria cor (diff).
struct LightSum
{
    float r, g, b;
};
LightSum SumLights(const Vert& world, const Light* const* ls, u32 nls)
{
    LightSum o = {0, 0, 0};
    for (u32 i = 0; i < nls; ++i)
    {
        const Light& l = *ls[i];
        const float dx = world.x - l.x, dy = world.y - l.y, dz = world.z - l.z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        // queda quadratica (1 - d2/r2)^2 no lugar da linear (r - d)/r: sem sqrtf, visual parecido (um pouco mais macio).
        const float t = 1.0f - d2 * l.invRadiusSq;
        if (t > 0.0f)
        {
            const float k = t * t;
            o.r += l.pr * k;
            o.g += l.pg * k;
            o.b += l.pb * k;
        }
    }
    return o;
}
// Cache por quadro da soma de luz por PONTO do mundo: os cantos de blocos vizinhos coincidem (cada canto e' compartilhado por ate 4 quadrilateros),
// entao cada canto e' calculado uma vez. Como a lista de luzes de cada desenho cobre a caixa inteira do quadrilatero, a soma num canto
// nao depende de qual desenho a calculou. Mantem a iluminacao suave (gouraud) pelo custo de ~1 calculo por quadrilatero.
struct LCacheEnt
{
    int16_t qx, qy, qz;
    uint16_t epoch;
    LightSum s;
};
constexpr int kLCacheN = 1024;
LCacheEnt gLCache[kLCacheN];
uint16_t gLEpoch = 1; // sobe a cada ResetLights (uma vez por quadro)
inline const LightSum& CachedSum(const Vert& w, const Light* const* ls, u32 nls)
{
    const int qx = static_cast<int>(w.x * 32.0f + 0.5f), qy = static_cast<int>(w.y * 32.0f + 0.5f), qz = static_cast<int>(w.z * 32.0f + 0.5f);
    const u32 h = (static_cast<u32>(qx) * 73856093u ^ static_cast<u32>(qy) * 19349663u ^ static_cast<u32>(qz) * 83492791u) >> 7;
    LCacheEnt& e = gLCache[h & (kLCacheN - 1)];
    if (e.epoch != gLEpoch || e.qx != static_cast<int16_t>(qx) || e.qy != static_cast<int16_t>(qy) || e.qz != static_cast<int16_t>(qz))
    {
        e.s = SumLights(w, ls, nls);
        e.qx = static_cast<int16_t>(qx), e.qy = static_cast<int16_t>(qy), e.qz = static_cast<int16_t>(qz);
        e.epoch = gLEpoch;
    }
    return e.s;
}
inline u32 LitApply(const LightSum& L, float m, u32 colour)
{
    const float amb = gAmbient;
    float r = m * static_cast<float>((colour >> 16) & 0xFF) * L.r + amb;
    float g = m * static_cast<float>((colour >> 8) & 0xFF) * L.g + amb;
    float b = m * static_cast<float>(colour & 0xFF) * L.b + amb;
    r = r > 255.0f ? 255.0f : r;
    g = g > 255.0f ? 255.0f : g;
    b = b > 255.0f ? 255.0f : b;
    return (colour & 0xFF000000) | (static_cast<u32>(r) << 16) | (static_cast<u32>(g) << 8) | static_cast<u32>(b);
}

void Draw(u32 flags, STexture* tex, Vert* v, int count, u32 baseColour)
{
    if (count < 3 || count > 4 || !tex || v[0].z <= 0.0f)
    {
        return;
    }
    const float tw = tex->field_E_width, th = tex->field_10_height;
    if (flags & kFitToTexture)
    {
        static const float k[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        for (int i = 0; i < count; ++i)
        {
            v[i].x = v[0].x + k[i][0] * tw;
            v[i].y = v[0].y + k[i][1] * th;
            v[i].z = v[0].z;
            v[i].u = k[i][0] * tw;
            v[i].v = k[i][1] * th;
        }
    }
    for (int i = 0; i < count; ++i)
    {
        v[i].w = v[i].z;
    }
    if (!(flags & kUseVertexDiffuse))
    {
        const u32 g = baseColour & 0xFF;
        for (int i = 0; i < count; ++i)
        {
            v[i].diff = 0xFF000000u | (g << 16) | (g << 8) | g;
            v[i].spec = 255;
        }
    }
    if (gN64Luzes && (flags & kLighting) && gAmbient != 255.0f)
    {
        // The game appends the world-space positions after the screen vertices.
        // Caixa dos pontos do mundo do quadrilatero; so as luzes das celulas sob a caixa entram, e entre elas so as cujo raio alcanca a caixa.
        static const Light* near_ls[48]; // static: sem preenchimento do compilador a cada desenho
        u32 nnear = 0;
        if (gActiveCount)
        {
            float bx0 = v[4].x, bx1 = v[4].x, by0 = v[4].y, by1 = v[4].y, bz0 = v[4].z, bz1 = v[4].z;
            for (int i = 1; i < count; ++i)
            {
                const Vert& w = v[i + 4];
                if (w.x < bx0) bx0 = w.x;
                if (w.x > bx1) bx1 = w.x;
                if (w.y < by0) by0 = w.y;
                if (w.y > by1) by1 = w.y;
                if (w.z < bz0) bz0 = w.z;
                if (w.z > bz1) bz1 = w.z;
            }
            ++gDrawStamp;
            const int cx0 = BinCell(bx0), cx1 = BinCell(bx1), cy0 = BinCell(by0), cy1 = BinCell(by1);
            for (int cy = cy0; cy <= cy1 && nnear < 48; ++cy)
            {
                for (int cx = cx0; cx <= cx1; ++cx)
                {
                    const int cell = (cx & (kBinDim - 1)) + (cy & (kBinDim - 1)) * kBinDim;
                    for (int e = gBinHead[cell]; e >= 0 && nnear < 48; e = gBinNext[e])
                    {
                        const u32 li = gBinLight[e];
                        if (gLightStamp[li] == gDrawStamp)
                        {
                            continue;
                        }
                        gLightStamp[li] = gDrawStamp;
                        const Light& l = gLights[li];
                        const float dx = l.x < bx0 ? bx0 - l.x : (l.x > bx1 ? l.x - bx1 : 0.0f);
                        const float dy = l.y < by0 ? by0 - l.y : (l.y > by1 ? l.y - by1 : 0.0f);
                        const float dz = l.z < bz0 ? bz0 - l.z : (l.z > bz1 ? l.z - bz1 : 0.0f);
                        if (dx * dx + dy * dy + dz * dz <= l.radiusSq)
                        {
                            near_ls[nnear++] = &l;
                        }
                    }
                }
            }
        }
        if (nnear == 0)
        {
            // sem nenhuma luz perto: so o ambiente (igual para os 4 vertices)
            const u32 a = static_cast<u32>(gAmbient > 255 ? 255 : gAmbient < 0 ? 0 : gAmbient);
            const u32 amb = a | (a << 8) | (a << 16);
            for (int i = 0; i < count; ++i)
            {
                v[i].diff = (v[i].diff & 0xFF000000) | amb;
            }
        }
        else
        {
            const float m = static_cast<u8>(baseColour) * (1.0f / 255.0f);
            if (gN64Luzes >= 2)
            {
            // modo suave (gouraud): soma por canto do mundo, com cache por quadro
            for (int i = 0; i < count; ++i)
            {
                v[i].diff = LitApply(CachedSum(v[i + 4], near_ls, nnear), m, v[i].diff);
            }
            }
            else
            {
            // modo padrao: uma soma de luz no centro do quadrilatero, aplicada aos 4 vertices (iluminacao chapada por bloco, estilo PS1)
            Vert ctr = v[4];
            for (int i = 1; i < count; ++i)
            {
                ctr.x += v[i + 4].x, ctr.y += v[i + 4].y, ctr.z += v[i + 4].z;
            }
            const float inv = 1.0f / static_cast<float>(count);
            ctr.x *= inv, ctr.y *= inv, ctr.z *= inv;
            const LightSum L = SumLights(ctr, near_ls, nnear);
            for (int i = 0; i < count; ++i)
            {
                v[i].diff = LitApply(L, m, v[i].diff);
            }
            }
        }
    }
    u32 blend = (flags & kAdditive) ? kBlendAdditive : (flags & kAlphaBlend) ? kBlendAlpha : kBlendOpaque;
#ifdef N64
    // N64: HUD layer (Hud::DrawGui): scaled up around the element's anchor and kept inside the TV-safe area
    // front-end menus get the same pixel snapping and point sampling as the HUD (no scaling)
    const bool crisp2d = gN64HudOn || gta2_game_in_menu();
    if (crisp2d)
    {
        for (int i = 0; gN64HudOn && i < count; ++i)
        {
            gta2_n64_hud_xform(&v[i].x, &v[i].y);
        }
        // move the whole quad so its top-left corner sits on a framebuffer pixel (2 units of 640x480), keeping its size:
        // snapping each corner on its own made letters of one line round to different heights
        float mx = v[0].x, my = v[0].y;
        for (int i = 1; i < count; ++i)
        {
            mx = v[i].x < mx ? v[i].x : mx;
            my = v[i].y < my ? v[i].y : my;
        }
        const float dx = 2.0f * floorf(mx * 0.5f + 0.5f) - mx, dy = 2.0f * floorf(my * 0.5f + 0.5f) - my;
        for (int i = 0; i < count; ++i)
        {
            v[i].x += dx;
            v[i].y += dy;
        }
    }
    // N64: converte a textura agora (o motor reaproveita textura compartilhada, de dano e de digitos no mesmo quadro)
    u32 texId;
    {
        const float ax = v[1].x - v[0].x, ay = v[1].y - v[0].y, bx = v[2].x - v[0].x, by = v[2].y - v[0].y;
        const float a_scr = fabsf(ax * by - bx * ay);
        const float a_tex = fabsf((v[1].u - v[0].u) * (v[2].v - v[0].v) - (v[2].u - v[0].u) * (v[1].v - v[0].v));
        texId = gta2_n64_tex_capture(reinterpret_cast<N64Tex*>(tex->field_1C_ptr), a_scr, a_tex, blend == kBlendAdditive);
    }
    if (crisp2d)
    {
        blend |= 0x100; // HUD: point sampling (render_n64.c)
    }
    if (gta2_n64_tex_token_empty(texId)) // textura sem nenhum pixel visivel: nao desenharia nada
    {
        return;
    }
#else
    const u32 texId = static_cast<u32>(reinterpret_cast<intptr_t>(tex->field_1C_ptr));
#endif
    // converts the 3 or 4 corners once (2 divides per quad instead of 12) and pushes the 1 or 2 triangles together
    static const int kFan[6] = {0, 1, 2, 0, 2, 3};
    const float itw = 1.0f / tw, ith = 1.0f / th;
    FrameVertex corner[4];
    for (int i = 0; i < count; ++i)
    {
        const Vert& s = v[i];
        const u32 d = s.diff;
        corner[i] = {s.x - 0.5f, s.y - 0.5f, s.w, s.u * itw, s.v * ith,
                     ((d >> 16) & 0xFF) | (d & 0xFF00) | ((d & 0xFF) << 16) | (d & 0xFF000000u)};
    }
    FrameVertex tri[6];
    const int n = (count - 2) * 3;
    for (int i = 0; i < n; ++i)
    {
        tri[i] = corner[kFan[i]];
    }
    gta2_frame_push_vertices(texId, blend, tri, n);
}

// Tiles: default UVs cover the 64x64 tile; flags rotate (0x20/0x40/0x60) and
// flip (0x08 vertical, 0x10 horizontal) the mapping.
void DrawTile(u32 flags, STexture* tex, Vert* p, u32 colour)
{
    u32 drawFlags = flags;
    if (!(flags & kTileUVGiven))
    {
        const float lo = 0.5f, hi = 63.4999f;
        p[0].u = lo, p[0].v = lo;
        p[1].u = hi, p[1].v = lo;
        p[2].u = hi, p[2].v = hi;
        p[3].u = lo, p[3].v = hi;
    }
    float u[4], w[4];
    for (int i = 0; i < 4; ++i)
    {
        u[i] = p[i].u, w[i] = p[i].v;
    }
    auto assign = [&](const int (&order)[4]) {
        for (int i = 0; i < 4; ++i)
        {
            p[i].u = u[order[i]], p[i].v = w[order[i]];
        }
        for (int i = 0; i < 4; ++i)
        {
            u[i] = p[i].u, w[i] = p[i].v;
        }
    };
    switch (flags & 0x60)
    {
        case 0x20:
            assign({3, 0, 1, 2});
            break;
        case 0x40:
            drawFlags = flags ^ 0x18; // 180 degrees: both flips
            break;
        case 0x60:
            assign({1, 2, 3, 0});
            break;
    }
    if (drawFlags & 0x08)
    {
        assign({1, 0, 3, 2});
    }
    if (drawFlags & 0x10)
    {
        assign({3, 2, 1, 0});
    }
    Draw(drawFlags, tex, p, 4, colour);
}

u32 gGlobals[64];
int gScreenTable[1700];
} // namespace

// ---------------------------------------------------------------------------
// Exported interface. Signatures match the game's T_* typedefs exactly
// (gbh_graphics.hpp); exports.cpp checks that at compile time.
extern "C" {

void CC ConvertColourBank(BYTE*)
{
}
s32 CC DrawLine(s32, s32 a2, s32, s32 a4, s32)
{
    return abs(a4 - a2);
}
void CC SetShadeTableA(s32, s32, s32, s32, s32)
{
}
s32* CC MakeScreenTable(s32 value, s32 step, u32 count)
{
    s32* p = gScreenTable;
    for (u32 i = 0; i < count && i < 1700; ++i)
    {
        *p++ = value;
        value += step;
    }
    return p;
}

u32 CC gbh_InitDLL(SVideo*)
{
    static const u32 kCacheDefaults[12] = {10, 62, 62, 88, 1, 0, 110, 126, 126, 40, 10, 0};
    memcpy(&gGlobals[40], kCacheDefaults, sizeof(kCacheDefaults));
    return 1;
}
s32 CC gbh_Init(s32)
{
    return 0;
}
void CC gbh_CloseDLL()
{
}
void CC gbh_CloseScreen(SVideo*)
{
}
u32* CC gbh_GetGlobals()
{
    return gGlobals;
}
s32 CC gbh_GetUsedCache(s32)
{
    return 0;
}
s32 CC gbh_SetColourDepth()
{
    return 1;
}
s32 CC gbh_SetCamera(float, float, float, float)
{
    return 0;
}
void CC gbh_SetWindow(float left, float top, float right, float bottom)
{
    gWinLeft = left, gWinTop = top, gWinRight = right, gWinBottom = bottom;
}
#ifndef N64_AMBIENT_FLOOR
#define N64_AMBIENT_FLOOR 100 /* 0..255: luz minima (o ambiente do PC e' bem escuro; no N64 a tela fica pequena e a TV escurece mais). 0 = como no PC */
#endif
void CC gbh_SetAmbient(float a)
{
    gAmbient = a * 255.0f;
#ifdef N64
    if (gAmbient < static_cast<float>(N64_AMBIENT_FLOOR))
    {
        gAmbient = static_cast<float>(N64_AMBIENT_FLOOR);
    }
#endif
}
void CC gbh_ResetLights()
{
    gLightCount = 0;
    gActiveCount = 0;
    gBinUsed = 0;
    if (++gLEpoch == 0) { memset(gLCache, 0, sizeof(gLCache)); gLEpoch = 1; }
    memset(gBinHead, 0xFF, sizeof(gBinHead)); // -1 = celula vazia
}
s32 CC gbh_AddLight(SLight* l)
{
    if (gLightCount >= 256)
    {
        return -1;
    }
    Light& d = gLights[gLightCount];
    const float k = 1.0f / 255.0f;
    d.flags = l->field_0;
    d.brightness = (l->field_0 & 0xFF) * k;
    d.x = l->field_4_x, d.y = l->field_8_y, d.z = l->field_C_z;
    d.r = ((l->field_10_colour >> 16) & 0xFF) * k;
    d.g = ((l->field_10_colour >> 8) & 0xFF) * k;
    d.b = (l->field_10_colour & 0xFF) * k;
    d.radius = ((l->field_0 >> 8) & 0xFF) * k * 8.0f;
    d.radiusSq = d.radius * d.radius;
    d.invRadius = d.radius > 0 ? 1.0f / d.radius : 0;
    d.invRadiusSq = d.radiusSq > 0 ? 1.0f / d.radiusSq : 0;
#ifndef N64_LIGHT_GAIN
#define N64_LIGHT_GAIN 0.6f /* forca das luzes coloridas (1.0 = PC). A tela do N64 e menor/mais saturada: abaixo de 1 evita o vermelho/neon estourado */
#endif
    d.pr = d.r * d.brightness * N64_LIGHT_GAIN, d.pg = d.g * d.brightness * N64_LIGHT_GAIN, d.pb = d.b * d.brightness * N64_LIGHT_GAIN;
    if ((d.flags & 0x30000) == 0x10000)
    {
        gActiveLights[gActiveCount++] = static_cast<u16>(gLightCount);
        BinInsert(gLightCount);
    }
    return static_cast<s32>(gLightCount++ * 0x2C);
}

void CC gbh_BeginScene()
{
}
void CC gbh_EndScene()
{
}
void CC gbh_BeginLevel()
{
}
void CC gbh_EndLevel()
{
}

#ifdef N64
// Ponteiro atual da paleta fisica (nullptr se ainda nao foi registrada). O renderer chama a cada desenho: uma textura
// criada ANTES da sua paleta ser registrada (ou com a paleta re-registrada depois) ainda acha a paleta certa.
extern "C" const uint32_t* gta2_n64_palette_ptr(int id)
{
    if (id < 0 || id >= kMaxPalettes || !gPalettes || !gPalettes[id].loaded)
    {
        return nullptr;
    }
    return reinterpret_cast<const uint32_t*>(gPalettes[id].original);
}

// Chamada antes de registrar as paletas de uma fase. Nunca libera nem move as tabelas antigas (ponteiros ja entregues ao
// motor continuam validos); so cresce, copiando as entradas.
extern "C" void gta2_n64_palettes_reserve(int n)
{
    if (n > 16384)
    {
        n = 16384;
    }
    if (n <= kMaxPalettes)
    {
        return;
    }
    Palette* np = static_cast<Palette*>(calloc(static_cast<size_t>(n), sizeof(Palette)));
    u16 (*ns)[4] = static_cast<u16 (*)[4]>(calloc(static_cast<size_t>(n), 4 * sizeof(u16)));
    if (!np || !ns)
    {
        free(np);
        free(ns);
        return;
    }
    if (gPalettes)
    {
        memcpy(np, gPalettes, static_cast<size_t>(kMaxPalettes) * sizeof(Palette));
        memcpy(ns, gStub, static_cast<size_t>(kMaxPalettes) * 4 * sizeof(u16));
        // as paletas antigas ja apontam para o stub antigo: copiamos tambem os ponteiros, entao o stub antigo continua vivo (nao liberado)
    }
    gPalettes = np;
    gStub = ns;
    kMaxPalettes = n;
}
#endif

u32 CC gbh_RegisterPalette(s32 id, DWORD* data)
{
    if (id < 0 || id >= kMaxPalettes)
    {
#ifdef N64
        PalReject("RegisterPalette", id);
#endif
        return 0;
    }
    DWORD* p = data;
#ifdef N64
    // Paletas mapeadas da ROM sao somente leitura: escrever nelas vira escrita no barramento da cartucha
    // ("PI writing in progress"). O renderer ja trata o indice 0 como transparente.
    if (reinterpret_cast<uintptr_t>(data) < 0xB0000000u)
#endif
    for (int i = 0; i < 256; ++i, p += 64)
    {
        if (i == 0)
        {
            *p = 0; // colour 0 is transparent
        }
        else if (*p == 0)
        {
            *p = 0x10000; // keeps other black entries opaque
        }
    }
    Palette& pal = gPalettes[id];
#ifdef N64
    // Sem renderer: nao converte nem aloca 1 KB por paleta; todas apontam para um bloco compartilhado.
    {
        const u32 sid = static_cast<u32>(id);
        memcpy(&gStub[id][0], &sid, 4);
        pal.data565 = &gStub[id][2];
        pal.original = data;
        pal.loaded = 1;
        return static_cast<u32>(id);
    }
#endif
    free(pal.data565 ? pal.data565 - 2 : nullptr);
    u16* block = static_cast<u16*>(malloc((2 + 512) * sizeof(u16)));
    const u32 id32 = static_cast<u32>(id);
    memcpy(block, &id32, 4);
    pal.data565 = block + 2;
    pal.original = data;
    pal.loaded = 1;
    for (int i = 0; i < 256; ++i)
    {
        pal.data565[i] = pal.data565[256 + i] = ToRGB565(data[i * 64]);
    }
    return static_cast<u32>(id);
}
void CC gbh_FreePalette(s32 id)
{
    if (id >= 0 && id < kMaxPalettes && gPalettes[id].data565)
    {
#ifndef N64
        free(gPalettes[id].data565 - 2);
#endif
        gPalettes[id] = Palette();
    }
}


STexture* CC gbh_RegisterTexture(s32 width, s32 height, BYTE* data, s32 palId, char_type)
{
    STexture* t = NewTexture(width, height, data);
    if (palId >= 0 && palId < kMaxPalettes)
    {
        t->field_12_bPalIsValid = static_cast<u8>(gPalettes[palId].loaded);
        t->field_18_pPaltData = gPalettes[palId].data565;
    }
#ifdef N64
    else
    {
        PalReject("RegisterTexture", palId);
    }
#endif
    UploadPaletted(t);
    return t;
}
void CC gbh_AssignPalette(STexture* t, s32 palId)
{
    if (palId < 0 || palId >= kMaxPalettes)
    {
#ifdef N64
        PalReject("AssignPalette", palId);
#endif
        return;
    }
    t->field_18_pPaltData = gPalettes[palId].data565;
    t->field_12_bPalIsValid = static_cast<u8>(gPalettes[palId].loaded);
    if (!(t->field_13_flags & 1))
    {
        UploadPaletted(t);
    }
}
STexture* CC gbh_LockTexture(STexture* t)
{
    t->field_8_locked_pixels_ptr = t->field_14_original_pixel_data_ptr;
    t->field_13_flags |= 1;
    t->field_6_pal_size = 256;
    return t;
}
STexture* CC gbh_UnlockTexture(STexture* t)
{
    t->field_6_pal_size = 0;
    t->field_8_locked_pixels_ptr = nullptr;
    t->field_13_flags &= 0xFE;
    UploadPaletted(t);
    return t;
}
void CC gbh_FreeTexture(STexture* t)
{
    // GPU textures are shared by content; they stay cached (see UploadPaletted).
    gLiveTextures.erase(t);
#ifdef N64
    gta2_n64_tex_forget(reinterpret_cast<const N64Tex*>(t->field_1C_ptr));
    free(t->field_1C_ptr); // descritor N64Tex
#endif
    free(t);
}

} // extern "C"

// ---------------------------------------------------------------------------
// N64: one region reserved at boot and shared by the two largest short-lived users, which are never alive together:
// the map's buffers (column index, column words, block table: ~850 KB on bil) during a level, and the half-size menu
// screens while the front end is open (the game is torn down before the menu opens and the menu before a level loads).
// Allocated from the heap on every level or menu load, they needed one free run of ~360 KB that the heap no longer had
// after some play (out of memory with 1 MB free). Bump allocation; the owner resets it when it is torn down. Anything
// that does not fit (or a second owner) falls back to the heap.
namespace
{
u8* gBig;
size_t gBigCap, gBigUsed;
int gBigOwner; // 0 free, 1 menu, 2 map
} // namespace

extern "C" {
void gta2_n64_big_prealloc(void)
{
    if (!gBig)
    {
        gBigCap = 880 * 1024;
        gBig = static_cast<u8*>(memalign(64, gBigCap));
        if (!gBig)
        {
            gBigCap = 0;
        }
    }
}
void* gta2_n64_big_alloc(int owner, size_t n)
{
    n = (n + 63) & ~static_cast<size_t>(63);
    if (!gBig || (gBigOwner && gBigOwner != owner) || gBigUsed + n > gBigCap)
    {
        return nullptr;
    }
    gBigOwner = owner;
    void* p = gBig + gBigUsed;
    gBigUsed += n;
    return p;
}
int gta2_n64_big_owns(const void* p)
{
    return gBig && static_cast<const u8*>(p) >= gBig && static_cast<const u8*>(p) < gBig + gBigCap;
}
void gta2_n64_big_reset(int owner)
{
    if (gBigOwner == owner)
    {
        gBigOwner = 0;
        gBigUsed = 0;
    }
}

s32 CC gbh_InitImageTable(s32 size)
{
    gImages = static_cast<ImageEntry*>(calloc(size, sizeof(ImageEntry)));
    gImageCount = gImages ? static_cast<u32>(size) : 0;
    return gImages ? 0 : -1;
}
s32 CC gbh_FreeImageTable()
{
    for (u32 i = 0; i < gImageCount; ++i)
    {
        if (gImages[i].loaded)
        {
            gbh_FreeTexture(gImages[i].tex);
            if (!gta2_n64_big_owns(gImages[i].pixels))
            {
                free(gImages[i].pixels);
            }
        }
    }
    gta2_n64_big_reset(1);
    free(gImages);
    gImages = nullptr;
    gImageCount = 0;
    return 0;
}
#ifdef N64
// N64: as telas do menu (TGA 16 bits, ate 640x480) viram, na hora de carregar, uma copia na METADE da resolucao (media 2x2) em
// RGBA5551, alinhada em 64 bytes, que o renderer desenha com o RDP (render_n64.c, comando kind 3). Meia resolucao porque a saida e
// 320x240 (menu em 640x480). O TGA 16 bits e' guardado de baixo para cima e, na ROM, em big-endian (tools/le2be.py).
// A copia so existe enquanto o menu esta aberto (gbh_FreeImageTable libera).
static u8* ImageHalf16(const u8* src, int w, int h)
{
    const int hw = (w + 1) / 2, hh = (h + 1) / 2;
    const size_t bytes = (static_cast<size_t>(hw) * hh * 2 + 63) & ~static_cast<size_t>(63);
    u16* out = static_cast<u16*>(memalign(64, bytes));
    if (!out)
    {
        return nullptr;
    }
    for (int y = 0; y < hh; ++y)
    {
        const int ya = h - 1 - 2 * y;
        const int yb = ya > 0 ? ya - 1 : ya;
        for (int x = 0; x < hw; ++x)
        {
            const int xa = 2 * x;
            const int xb = xa + 1 < w ? xa + 1 : xa;
            const int ys[2] = {ya, yb};
            const int xs[2] = {xa, xb};
            unsigned r = 0, g = 0, b = 0;
            for (int j = 0; j < 2; ++j)
            {
                for (int i = 0; i < 2; ++i)
                {
                    const u8* q = src + (static_cast<size_t>(ys[j]) * w + xs[i]) * 2;
                    const unsigned v = (static_cast<unsigned>(q[0]) << 8) | q[1];
                    r += (v >> 10) & 31;
                    g += (v >> 5) & 31;
                    b += v & 31;
                }
            }
            out[static_cast<size_t>(y) * hw + x] = static_cast<u16>(((r >> 2) << 11) | ((g >> 2) << 6) | ((b >> 2) << 1) | 1);
        }
    }
    data_cache_hit_writeback_invalidate(out, bytes);
    return reinterpret_cast<u8*>(out);
}
// usado por render_n64.c: copia reduzida da imagem 'idx' (largura/altura ja na metade), ou nullptr
extern "C" const uint16_t* gta2_n64_image_get(int idx, int* w, int* h)
{
    if (idx < 0 || static_cast<u32>(idx) >= gImageCount || !gImages[idx].loaded || !gImages[idx].pixels)
    {
        return nullptr;
    }
    *w = static_cast<int>((gImages[idx].w + 1) / 2);
    *h = static_cast<int>((gImages[idx].h + 1) / 2);
    return reinterpret_cast<const uint16_t*>(gImages[idx].pixels);
}
#endif
#ifdef N64
extern "C" const void* gta2_n64_map_file(const char* path, long offset);
extern "C" void gta2_n64_rom_copy(void* dst, const void* src, unsigned len);
// N64: carrega uma tela do menu (TGA 16 bits na ROM, big-endian) DIRETO da ROM, sem o buffer de 600 KB do motor (fopen/malloc/fread/free):
// le so duas linhas por vez (DMA do PI para um buffer fixo alinhado) e ja grava a copia reduzida na metade (ImageHalf16).
// Devolve o indice na tabela de imagens (>= 0) ou < 0 em erro.
extern "C" s32 gta2_n64_image_from_rom(const char* path)
{
    const u8* rom = static_cast<const u8*>(gta2_n64_map_file(path, 0));
    if (!rom)
    {
        return -3;
    }
    u8 hdr[24] __attribute__((aligned(8)));
    gta2_n64_rom_copy(hdr, rom, 20);
    const int idlen = hdr[0];
    const int w = (hdr[12] << 8) | hdr[13];
    const int h = (hdr[14] << 8) | hdr[15];
    if (hdr[1] != 0 || hdr[2] != 2 || hdr[16] != 16 || w <= 0 || h <= 0 || w > 640 || h > 480)
    {
        return -2;
    }
    for (u32 i = 0; i < gImageCount; ++i)
    {
        if (gImages[i].loaded)
        {
            continue;
        }
        const int hw = (w + 1) / 2, hh = (h + 1) / 2;
        const size_t bytes = (static_cast<size_t>(hw) * hh * 2 + 63) & ~static_cast<size_t>(63);
        u16* out = static_cast<u16*>(gta2_n64_big_alloc(1, bytes)); // shared region (see gta2_n64_big_alloc)
        if (!out)
        {
            out = static_cast<u16*>(memalign(64, bytes));
        }
        if (!out)
        {
            return -4;
        }
        static u8 rowbuf[2 * 640 * 2 + 32] __attribute__((aligned(16)));
        const uint32_t pix0 = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(rom) & 0x1FFFFFFFu) + 18u + static_cast<uint32_t>(idlen);
        for (int y = 0; y < hh; ++y)
        {
            const int ya = h - 1 - 2 * y;          // TGA 16 bits: de baixo para cima
            const int yb = ya > 0 ? ya - 1 : ya;
            const uint32_t pi = pix0 + static_cast<uint32_t>(yb) * static_cast<uint32_t>(w) * 2u;
            const unsigned len = static_cast<unsigned>(ya - yb + 1) * static_cast<unsigned>(w) * 2u;
            u8* base = rowbuf + (pi & 7u);          // mesmo alinhamento (mod 8) da ROM, exigencia do dma_read
            data_cache_hit_writeback_invalidate(base, len);
            dma_read(base, pi, len);
            const u8* rowA[2] = {base + static_cast<size_t>(w) * 2u * (ya - yb), base}; // [0] = linha ya, [1] = linha yb
            for (int x = 0; x < hw; ++x)
            {
                const int xa = 2 * x;
                const int xb = xa + 1 < w ? xa + 1 : xa;
                unsigned r = 0, g = 0, b = 0;
                for (int j = 0; j < 2; ++j)
                {
                    const u8* row = rowA[j];
                    const int xs[2] = {xa, xb};
                    for (int k = 0; k < 2; ++k)
                    {
                        const unsigned v = (static_cast<unsigned>(row[xs[k] * 2]) << 8) | row[xs[k] * 2 + 1];
                        r += (v >> 10) & 31;
                        g += (v >> 5) & 31;
                        b += v & 31;
                    }
                }
                out[static_cast<size_t>(y) * hw + x] = static_cast<u16>(((r >> 2) << 11) | ((g >> 2) << 6) | ((b >> 2) << 1) | 1);
            }
        }
        data_cache_hit_writeback_invalidate(out, bytes);
        STexture* t = NewTexture(w, h, nullptr);
        gImages[i] = {TRUE, static_cast<DWORD>(w), static_cast<DWORD>(h), t, reinterpret_cast<u8*>(out)};
        return static_cast<s32>(i);
    }
    return -1;
}
#endif
// SImage is a TGA header followed by the image id and 16-bit pixels.
s32 CC gbh_LoadImage(SImage* img)
{
    if (!img || img->field_1 || img->field_2 != 2 || img->field_10 != 16)
    {
        return -2;
    }
    for (u32 i = 0; i < gImageCount; ++i)
    {
        if (!gImages[i].loaded)
        {
            STexture* t = NewTexture(img->field_C_width, img->field_E_height, nullptr);
#ifndef N64
            t->field_1C_ptr = reinterpret_cast<void*>(static_cast<intptr_t>(gNextTextureId++)); // N64: field_1C_ptr e' o descritor N64Tex (free no gbh_FreeTexture)
#endif
            const u8* src = reinterpret_cast<const u8*>(&img->field_12) + img->field_0;
            UploadImage(t, src);
            const size_t bytes = static_cast<size_t>(img->field_C_width) * img->field_E_height * 2;
#ifdef N64
            u8* copy = ImageHalf16(src, img->field_C_width, img->field_E_height); // copia na metade da resolucao (150 KB no maximo), nao os 600 KB originais
#else
            u8* copy = static_cast<u8*>(malloc(bytes));
#endif
            if (copy)
            {
                memcpy(copy, src, bytes);
            }
            gImages[i] = {TRUE, img->field_C_width, img->field_E_height, t, copy};
            return static_cast<s32>(i);
        }
    }
    return -1;
}
s32 CC gbh_BlitImage(s32 index, s32 left, s32 top, s32 right, s32 bottom, s32 x, s32 y)
{
    if (index < 0 || static_cast<u32>(index) >= gImageCount || !gImages[index].loaded)
    {
        return -1;
    }
#ifdef N64
    if (!gImages[index].pixels)
    {
        return -1;
    }
    gta2_frame_blit(index, left, top, right, bottom, x, y);
    return 0;
#endif
    Vert v[8] = {};
    const float w = static_cast<float>(right - left), h = static_cast<float>(bottom - top);
    const float xs[4] = {0, w, w, 0}, ys[4] = {0, 0, h, h};
    for (int i = 0; i < 4; ++i)
    {
        v[i].x = x + xs[i];
        v[i].y = y + ys[i];
        v[i].z = v[i].w = 1.0f;
        v[i].u = left + xs[i];
        v[i].v = top + ys[i];
        v[i].diff = 0xFFFFFFFF;
    }
    Draw(kUseVertexDiffuse, gImages[index].tex, v, 4, 0xFF);
    return 0;
}

void CC gbh_DrawQuad(s32 flags, STexture* t, Vert* v, s32 colour)
{
    Draw(static_cast<u32>(flags), t, v, 4, static_cast<u32>(colour));
}
void CC gbh_DrawTriangle(s32 flags, STexture* t, Vert* v, u8 colour)
{
    Draw(static_cast<u32>(flags), t, v, 3, colour);
}
s32 CC gbh_DrawTilePart(u32 flags, STexture* t, Vert* v, s32 colour)
{
    DrawTile(flags, t, v, static_cast<u32>(colour));
    return 0;
}
s32 CC gbh_DrawTile(u32 flags, STexture* t, Vert* v, u8 colour)
{
    DrawTile(flags, t, v, colour);
    return 0;
}

// Present in the interface but not used by the game in practice.
s32 CC gbh_BlitBuffer(s32, s32, s32, s32, s32, s32)
{
    return 0;
}
void CC gbh_DrawQuadClipped(s32, s32, s32, s32, s32)
{
}
s32 CC gbh_DrawFlatRect(Vert*, s32)
{
    return 0;
}
void CC gbh_Plot(f32, f32, s32, s32)
{
}
s32 CC gbh_PrintBitmap(s32, s32)
{
    return 0;
}
u32 CC gbh_Convert16BitGraphic(s32, u32, WORD*, s32)
{
    return 0;
}
u32 CC gbh_ConvertColour(u8 r, u8 g, u8 b)
{
    return (static_cast<u32>(r) << 16) | (static_cast<u32>(g) << 8) | b;
}

} // extern "C"

// Quick load (savestate_web.cpp): the game's memory was replaced, so the browser's textures no
// longer match the ids in it (after a page reload there are none). Every texture the game holds
// is uploaded again, under fresh ids, and the browser forgets the rest.
void Gta2Port_ReuploadTextures()
{
    gta2_js_free_all_textures();
    gTextureByContent.clear();
    for (STexture* t : gLiveTextures)
    {
        if (t->field_14_original_pixel_data_ptr)
        {
            UploadPaletted(t);
        }
    }
    for (u32 i = 0; i < gImageCount; ++i)
    {
        if (gImages[i].loaded && gImages[i].pixels)
        {
            UploadImage(gImages[i].tex, gImages[i].pixels);
        }
    }
}
