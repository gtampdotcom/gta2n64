// Ponte C++ -> C: o main (main_n64.c, libdragon) chama estas funcoes. Replica o que o
// shell web faz (semear o registro, gta2_start) sem emscripten.
#include <string.h>
#include <stdio.h>
#include <windows.h>

#include "winmain.hpp"

int __stdcall WinMain_5E53F0(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd);
extern "C" int Gta2Port_RunLoopSlice();
extern "C" void Gta2Port_StaticInit();
extern "C" void gta2_port_reg_set_dword(const char* path, const char* name, DWORD value);
extern "C" void gta2_port_reg_set_string(const char* path, const char* name, const char* value);
extern "C" void gta2_port_key(int scancode, int vk, int down, int ch);
extern "C" int Gta2Port_InFrontend(); // winmain.cpp
extern "C" int gta2_n64_boot_map; // main_n64.c: -1 = menu, 0..2 = mapa direto

namespace
{
constexpr int kRunning = 0x7FFFFFFF;
const char kRegRoot[] = "hklm\\software\\dma design ltd\\gta2";

void SeedRegistry()
{
    char path[96];
    snprintf(path, sizeof(path), "%s\\screen", kRegRoot);
    gta2_port_reg_set_dword(path, "start_mode", 0);
    gta2_port_reg_set_dword(path, "do_play_movie", 0); // sem Bink no N64
    // Controles (scan codes DirectInput), na ordem dos slots do jogo (igual ao port web).
    static const int controls[] = {0x11, 0x1f, 0x1e, 0x20, 0x26, 0x1c, 0x39, 0x24, 0x25, 0x17, 0x18, 0x19};
    snprintf(path, sizeof(path), "%s\\control", kRegRoot);
    for (int i = 0; i < 12; ++i)
    {
        char name[8];
        snprintf(name, sizeof(name), "%d", i);
        gta2_port_reg_set_dword(path, name, controls[i]);
    }
    gta2_port_reg_set_dword("hklm\\software\\freeloader.com\\gta2", "cityinstalled", 2);
    snprintf(path, sizeof(path), "%s\\debug", kRegRoot);
    // trains on: no "skip_trains" key at all -- the engine only checks that the key EXISTS (Get_Debug_Setting), so even 0 turned them off
    // N64: por padrao abre o menu original do GTA 2 (o motor escolhe o mapa pelo data\\test1.seq: City 1 = wil, 2 = ste, 3 = bil).
    // Se o jogador segurou Z ao ligar (main_n64.c), pula o menu e entra direto no mapa escolhido.
    static const char* const kMaps[3] = {"bil", "ste", "wil"};
    const int boot = gta2_n64_boot_map;
    const char* m = (boot >= 0 && boot < 3) ? kMaps[boot] : "bil";
    char name[16];
    if (boot >= 0 && boot < 3)
    {
        // ATENCAO: o motor so testa se a chave "skip_frontend" EXISTE (valor ignorado): para abrir o menu ela nao pode ser criada.
        gta2_port_reg_set_dword(path, "skip_frontend", 1);
    }
    snprintf(name, sizeof(name), "%s.gmp", m);
    gta2_port_reg_set_string(path, "mapname", name);
    snprintf(name, sizeof(name), "%s.sty", m);
    gta2_port_reg_set_string(path, "stylename", name);
    snprintf(name, sizeof(name), "%s.scr", m);
    gta2_port_reg_set_string(path, "scriptname", name);
}
bool gStarted = false;
} // namespace

// 0 = rodando; != 0 = o jogo terminou na inicializacao (codigo de saida).
extern "C" int gta2_game_init(void)
{
    SeedRegistry();
    Gta2Port_StaticInit(); // o que o start-up da CRT original fazia
    static char cmd[4] = "";
    const int r = WinMain_5E53F0(reinterpret_cast<HINSTANCE>(0x400000), nullptr, cmd, SW_SHOWNORMAL);
    if (r != kRunning)
    {
        return r ? r : 1;
    }
    gStarted = true;
    return 0;
}

// -1 = continua; outro valor = codigo de saida do jogo.
extern "C" int gta2_game_frame(void)
{
    return gStarted ? Gta2Port_RunLoopSlice() : 1;
}

extern "C" void gta2_game_key(int scancode, int vk, int down)
{
    gta2_port_key(scancode, vk, down, 0);
}

// 1 = o menu do GTA 2 esta aberto (main_n64.c muda o mapeamento dos botoes entre menu e jogo)
extern "C" int gta2_game_in_menu(void)
{
    return gStarted ? Gta2Port_InFrontend() : 0;
}

