// Gerado de gta2-browser/port por tools/gen_n64_platform.py e adaptado para N64 (sem emscripten).
// The DMA video driver interface (dmavideo.dll) for the browser.
//
// One device and one 640x480 mode: the game renders in its reference space
// and the WebGL2 renderer scales the result to the canvas. For widescreen the
// shell sets a wider view (gta2_port_set_view_width); the screen rectangle
// reported to the game is then that wide, still 480 high. Presenting a
// frame hands the draw list to JavaScript (render/frame.cpp).
#include <stdlib.h>
#include <string.h>

#include "3rdParty/GTA2Hax/DmaVideo/DmaVideo.hpp"
#include "dma_video.hpp"
#include "render/frame.h"
#include "Game.hpp"
#include "winmain.hpp"

namespace
{
SVideo* gVideo = nullptr;
SVideoFunctions* gFuncs = nullptr;
SDisplayMode gMode = {};
SDevice gDevice = {};
s32 gEnumIdx = 0;
SVidVersion gVersion = {0x3FE7AE14, "GTA2 browser port video driver"};
constexpr s32 kModeId = 1;
s32 gViewWidth = 640;
} // namespace

// Read by the game's camera (Camera.hpp): the activation margin (D07/D17).
extern "C" s32 gGta2PortActivationMargin = 1;
// Defined with the Modern camera (Source/port_modern_camera.cpp).
extern "C" s32 gGta2PortCameraMode;
extern "C" f32 gGta2PortCameraHeightBias;

extern "C" {

// Reference width of gameplay frames: 640 (4:3) up to 1280 (8:3), even. Can
// change at any time (window resize, full screen): the screen rectangle is
// updated and a running game re-applies its screen size, as the original does
// after a mode change. Called between frames.
int gta2_port_set_view_width(int width)
{
    width = width < 640 ? 640 : width > 1280 ? 1280 : width;
    width &= ~1;
    if (width == gViewWidth)
    {
        return gViewWidth;
    }
    gViewWidth = width;
    if (gVideo && gVideo->field_48_rect_right)
    {
        gVideo->field_48_rect_right = gViewWidth;
    }
    if (gGame_0x40_67E008 && gGame_0x40_67E008->field_38_local_player)
    {
        gCachedScreenDimension_706C58 = 0; // the original only re-applies on a height change
        UpdateGameScreenSize_5D8E00();
    }
    return gViewWidth;
}

void gta2_port_set_activation_margin(int on)
{
    gGta2PortActivationMargin = on ? 1 : 0;
}

// 0 = Classic (the original camera), 1 = Modern (D17). heightBias: blocks added to Modern's height.
void gta2_port_set_camera(int mode, float heightBias)
{
    gGta2PortCameraMode = mode ? 1 : 0;
    gGta2PortCameraHeightBias = heightBias < -2.0f ? -2.0f : heightBias > 4.0f ? 4.0f : heightBias;
}

SVidVersion* CC Vid_GetVersion()
{
    return &gVersion;
}

s32 CC Vid_InitDLL(HINSTANCE, SVideoFunctions* funcs)
{
    gFuncs = funcs;
    return 0;
}

SVideo* CC Vid_Init_SYS(HINSTANCE param1, u16 flags)
{
    gVideo = static_cast<SVideo*>(calloc(1, sizeof(SVideo)));
    gVideo->field_0 = 1;
    gVideo->field_4_flags = flags | 0x200;
    gVideo->field_14_display_mode_count_2_q = 1;
    gVideo->field_18_num_guids = 2;
    gVideo->field_78 = static_cast<u32>(reinterpret_cast<uintptr_t>(param1));
    gVideo->field_84_from_initDLL = gFuncs;
    gMode.field_0_display_mode_idx = kModeId;
    gMode.field_8_width = 640;
    gMode.field_C_height = 480;
    gMode.field_10_pitch = 640 * 2;
    gMode.field_14_rgb_bit_count = 16;
    gDevice.field_0_id = 1;
    gDevice.field_4_flags = 1; // keeps the game from minimising on focus loss
    gVideo->field_1C_num_display_modes = 1;
    return gVideo;
}

void CC Vid_ShutDown_SYS(SVideo* v)
{
    free(v);
    if (v == gVideo)
    {
        gVideo = nullptr;
    }
}

s32 CC Vid_CheckMode(SVideo* v, s32 width, s32 height, s32)
{
    return v && width == 640 && height == 480 ? kModeId : 0;
}

SDevice* CC Vid_FindDevice(SVideo*, s32)
{
    return &gDevice;
}

s32 CC Vid_SetDevice(SVideo*, s32)
{
    return 0;
}

SDisplayMode* CC Vid_FindMode(SVideo* v, s32 modeId)
{
    return v && modeId == kModeId ? &gMode : nullptr;
}

s32 CC Vid_FindFirstMode(SVideo* v, s32)
{
    gEnumIdx = 0;
    return Vid_FindNextMode(v);
}

s32 CC Vid_FindNextMode(SVideo* v)
{
    if (!v || gEnumIdx >= 1)
    {
        return 0;
    }
    return ++gEnumIdx;
}

s32 CC Vid_SetMode(SVideo* v, HWND hwnd, s32)
{
    if (!v)
    {
        return 1;
    }
    v->field_8_found_width = 640;
    v->field_C_found_height = 480;
    v->field_10_found_rgb_bit_count = 16;
    v->field_48_rect_right = gViewWidth;
    v->field_4C_rect_bottom = 480;
    v->field_5C = 5; // RGB 5:6:5 component widths, read by the Bink path
    v->field_60_g = 6;
    v->field_64_r = 5;
    v->field_68_b = 5;
    v->field_6C = 5;
    v->field_4C0_hwnd = hwnd;
    return 0;
}

void CC Vid_CloseScreen(SVideo*)
{
}
void CC Vid_GrabSurface(SVideo*)
{
}
void CC Vid_ReleaseSurface(SVideo*)
{
}
s32 CC Vid_EnableWrites(SVideo*)
{
    return 0;
}
s32 CC Vid_DisableWrites(SVideo*)
{
    return 0;
}
s32 CC Vid_GetSurface(SVideo*)
{
    return 0;
}
s32 CC Vid_FreeSurface(SVideo*)
{
    return 0;
}

void CC Vid_FlipBuffers(SVideo*)
{
    gta2_frame_present();
}

s32 CC Vid_ClearScreen(SVideo*, u8 r, u8 g, u8 b, s32 left, s32 top, s32 right, s32 bottom)
{
    gta2_frame_clear(r, g, b, left, top, right, bottom);
    return 0;
}

s32 CC Vid_SetGamma(SVideo*, f32, f32, f32)
{
    return 0;
}

s32 CC Vid_WindowProc(SVideo*, HWND, u32, WPARAM, LPARAM)
{
    return 0;
}

} // extern "C"
