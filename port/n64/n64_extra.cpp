// Pecas da plataforma N64 que nao existem no port web.
#include <stdio.h>
#include <windows.h>
#include <string.h>
#include "ImGuiDebug.hpp"
#include "NetUi.hpp"
#include "platform/port_api.h"

// ImGuiDebug.cpp nao entra no build (depende de ImGui); winmain.cpp referencia o callback.
void CC ImGuiDebugDraw()
{
}

// Gancho de apresentacao: o renderer Tiny3D (render_t3d.c) sobrescreve este simbolo fraco.
extern "C" __attribute__((weak)) void gta2_n64_present(const void* cmds, int cmdCount, const void* verts, int vertCount, int refWidth)
{
    (void)cmds; (void)cmdCount; (void)verts; (void)vertCount; (void)refWidth;
}

// Lobby DirectPlay: sem rede no N64.
s32 NetUi::cb_FillSessionList_519D30(NetUi*, Network_NameList*)
{
    return 0;
}

// Globais que no port web vinham dos patches 0010+ (camera moderna, interpolacao, co-op).
extern "C" {
int32_t gGta2PortInterpolate = 0;
int32_t gGta2PortCameraMode = 0;
float gGta2PortCameraHeightBias = 0.0f;
int32_t gGta2PortFriendlyFire = 0;

// Win32 restante
HRESULT CoCreateInstance(REFCLSID, void*, DWORD, REFIID, LPVOID*)
{
    return -1; // sem DirectPlay/DirectMusic
}
UINT GetDriveTypeA(LPCSTR)
{
    return 3; // DRIVE_FIXED // sem CD: o audio fica no cartucho
}
int WideCharToMultiByte(UINT, DWORD, LPCWSTR w, int wl, LPSTR out, int ol, LPCSTR, LPBOOL)
{
    int n = 0;
    for (; (wl < 0 || n < wl) && w[n]; ++n)
    {
        if (out && n < ol)
            out[n] = w[n] < 0x100 ? (char)w[n] : '?';
    }
    if (wl < 0)
    {
        if (out && n < ol)
            out[n] = 0;
        ++n;
    }
    return n;
}
LPSTR lstrcatA(LPSTR a, LPCSTR b)
{
    return strcat(a, b);
}
}

// getcwd: o N64 nao tem diretorio de trabalho; o motor so usa isso no start-up.
extern "C" char* getcwd(char* buf, size_t size)
{
    if (!buf || size < 6)
        return 0;
    strcpy(buf, "rom:/");
    return buf;
}
