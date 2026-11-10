/* The game's intro movie. The PC game plays data/movie/intro.bik with Bink, which cannot be decoded here (bink_n64.cpp
 * makes the engine believe there is no movie). The build (tools/stage_data.sh) converts it once with libdragon's
 * videoconv64 to rom:/data/intro.h264 + intro.wav64: 320x240 at the movie's own 15 frames per second, about 4 MB. It is
 * played at power-on, before the engine starts (so before any of the game's memory is taken), with libdragon's movie
 * player; A, B or Start skips it. A ROM built without the movie (INCLUDE_INTRO=0, or no ffmpeg on the build machine)
 * simply starts at the menu. */
#include <libdragon.h>
#include <stdio.h>

#define INTRO_VIDEO    "rom:/data/intro.h264"
#define INTRO_AUDIO_HZ 22050 /* what stage_data.sh asks videoconv64 for */

static void intro_osd(void *ctx, int frame_idx, float time_sec, fmv_control_t *ctrl)
{
    (void)ctx; (void)time_sec;
    joypad_poll();
    const joypad_buttons_t b = joypad_get_buttons_pressed(JOYPAD_PORT_1);
    if (frame_idx > 4 && (b.start || b.a || b.b)) ctrl->stop(ctrl);
#ifdef N64_INTRO_TEST
    {   /* test builds (-DN64_INTRO_TEST=frame): log the playback rate, dump that frame as "[fb]" hex lines (tools: the
           same format as the game's N64_PROF_DUMP), then stop the movie a little later */
        static uint32_t t0;
        if (frame_idx == 0) t0 = TICKS_READ();
        if ((frame_idx % 30) == 29) debugf("[intro] frame %d movie %.2fs wall %.2fs\n", frame_idx, (double)time_sec, (double)(uint32_t)(TICKS_READ() - t0) / (double)TICKS_PER_SECOND);
        if (frame_idx == (N64_INTRO_TEST)) {
            rspq_wait();
            const surface_t *fb = rdpq_get_attached();
            static const char hx[] = "0123456789abcdef";
            char line[64 * 4 + 1];
            if (fb) {
                debugf("[fbinfo] %d %d\n", (int)fb->width, (int)fb->height);
                for (int y = 0; y < fb->height; ++y) {
                    const uint8_t *row = (const uint8_t *)fb->buffer + (size_t)y * fb->stride;
                    for (int x0 = 0; x0 < fb->width; x0 += 64) {
                        int n = 0;
                        for (int x = x0; x < x0 + 64 && x < fb->width; ++x) {
                            unsigned v;
                            if (TEX_FORMAT_BITDEPTH(surface_get_format(fb)) == 32) { const uint8_t *p = row + x * 4; v = ((p[0] >> 3) << 11) | ((p[1] >> 3) << 6) | ((p[2] >> 3) << 1) | 1; }
                            else v = ((const uint16_t *)row)[x];
                            line[n++] = hx[v >> 12]; line[n++] = hx[(v >> 8) & 15]; line[n++] = hx[(v >> 4) & 15]; line[n++] = hx[v & 15];
                        }
                        line[n] = 0;
                        debugf("[fb] %s\n", line);
                    }
                }
                debugf("[introdump]\n");
            }
        }
        if (frame_idx == (N64_INTRO_TEST) + 20) ctrl->stop(ctrl);
    }
#endif
}

/* Returns 1 if the movie was played: the display was closed for it and the caller must set its own mode up again. */
int gta2_n64_play_intro(int crt_margin)
{
    FILE *f = fopen(INTRO_VIDEO, "rb");
    if (!f) return 0;
    fclose(f);

    display_close(); /* the player opens the mode that fits the movie */
    rdpq_init();
    audio_init(INTRO_AUDIO_HZ, 4);
    mixer_init(2);
    video_register_codec(&h264_codec);
    fmv_play(INTRO_VIDEO, &(fmv_parms_t){ .osd_callback = intro_osd, .crt_margin = crt_margin != 0 });
    /* leave everything as the game expects to find it: it initialises rdpq and the audio itself, later */
    mixer_close();
    audio_close();
    rdpq_close();
    return 1;
}
