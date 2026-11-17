/* The game's intro movie. The PC game plays data/movie/intro.bik with Bink, which cannot be decoded here (bink_n64.cpp
 * makes the engine believe there is no movie). The build (tools/stage_data.sh) converts it once with libdragon's
 * videoconv64 to rom:/data/intro.m1v + intro.wav64: MPEG-1, 320x240, about 10 MB. It is played at power-on, before the
 * engine starts (so before any of the game's memory is taken); A, B or Start skips it. A ROM built without the movie
 * (INCLUDE_INTRO=0, or no ffmpeg on the build machine) simply starts at the menu.
 *
 * Why MPEG-1 and why not libdragon's fmv_play(): RAM. Cartridge space is plentiful (64 MB) and RAM is not (8 MB, with
 * ~300 KB to spare in game), and whatever code plays the movie stays in RAM for the whole game. The MPEG-1 decoder is
 * ~26 KB where the H.264 one is ~83 KB (its file would be half the size, which does not matter); fmv_play() also pulls
 * in the subtitle renderer and its font code (~30 KB) that would never run. This file is the same playback loop
 * without subtitles. Everything it uses is linked as one block (tools/make_hot_ld.py) that main_n64.c turns into buffer
 * space once the movie is over. */
#include <libdragon.h>
#include <video_sync.h>
#include <stdio.h>
#include <sys/stat.h>

#define INTRO_VIDEO    "rom:/data/intro.m1v"
#define INTRO_AUDIO    "rom:/data/intro.wav64"
#define INTRO_AUDIO_HZ 22050 /* what stage_data.sh asks videoconv64 for */

#ifdef N64_INTRO_TEST
/* test builds (-DN64_INTRO_TEST=frame): log the playback rate, dump that frame as "[fb]" hex lines (the format of the
   game's N64_PROF_DUMP), then stop the movie a little later */
static int intro_test(int frame_idx, float time_sec)
{
    static uint32_t t0;
    if (frame_idx == 0) t0 = TICKS_READ();
    if ((frame_idx % 60) == 59) debugf("[intro] frame %d movie %.2fs wall %.2fs\n", frame_idx, (double)time_sec, (double)(uint32_t)(TICKS_READ() - t0) / (double)TICKS_PER_SECOND);
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
    return frame_idx >= (N64_INTRO_TEST) + 20;
}
#endif

/* Returns 1 if the movie was played: the display was closed for it and the caller must set its own mode up again. */
int gta2_n64_play_intro(int crt_margin)
{
    struct stat st;
    if (stat(INTRO_VIDEO, &st) != 0) return 0;

    display_close(); /* the movie gets the mode that fits it */
    rdpq_init();
    audio_init(INTRO_AUDIO_HZ, 4);
    mixer_init(2);
    video_register_codec(&mpeg1_codec);

    video_t *video = video_open(INTRO_VIDEO, &(video_parms_t){ .buffered_pics = 8 });
    const video_info_t info = video_get_info(video);
    display_init((resolution_t){ .width = info.width, .height = info.height, .aspect_ratio = info.aspect_ratio,
                                 .overscan_margin = crt_margin ? VI_CRT_MARGIN : 0 },
                 DEPTH_32_BPP, 2, GAMMA_NONE, FILTERS_RESAMPLE);
    yuv_init();
    yuv_blitter_t yuv = yuv_blitter_new_fmv(info.width, info.height, display_get_width(), display_get_height(),
                                            &(yuv_fmv_parms_t){ .cs = &info.colorspace, .video_aspect_ratio = info.aspect_ratio,
                                                                .display_aspect_ratio = vi_get_aspect_ratio() });
    display_set_fps_limit(info.framerate);

    wav64_t *audio = stat(INTRO_AUDIO, &st) == 0 ? wav64_load(INTRO_AUDIO, NULL) : NULL;
    video_sync_t *vsync = NULL;
    if (audio) {
        mixer_ch_play(0, &audio->wave);
        vsync = video_sync_create(video, NULL); /* frames are dropped, if need be, to stay with the sound */
    }

    for (int frame_idx = 0;; ++frame_idx) {
        int skip_render = 0;
        mixer_try_play();
        if (vsync && mixer_ch_playing(0)) {
            const double master = mixer_ch_get_pos(0) / (double)audio->wave.frequency;
            const video_sync_action_t a = video_sync_step(vsync, master, frame_idx);
            if (a.kind == VIDEO_SYNC_SKIP_NEXT) {
                skip_render = 1;
            } else if (a.kind == VIDEO_SYNC_SEEK_AND_RENDER) {
                const int idx = video_seek(video, a.seek_frame);
                if (idx >= 0) frame_idx = idx;
            }
        }
        if (!video_next_frame(video)) break;
        mixer_try_play();

        joypad_poll();
        const joypad_buttons_t b = joypad_get_buttons_pressed(JOYPAD_PORT_1);
        int stop = frame_idx > 8 && (b.start || b.a || b.b);

        if (!skip_render) {
            surface_t *disp = display_try_get();
            while (!disp) { /* no framebuffer free yet: decode ahead meanwhile */
                if (!video_poll(video)) { disp = display_get(); break; }
                disp = display_try_get();
            }
            rdpq_attach(disp, NULL);
            yuv_frame_t frame = video_get_frame(video);
            yuv_blitter_run(&yuv, &frame);
#ifdef N64_INTRO_TEST
            stop |= intro_test(frame_idx, (float)frame_idx / info.framerate);
#endif
            rdpq_detach_show();
        }
        if (stop) break;
    }

    rspq_wait();
    if (audio) { mixer_ch_stop(0); wav64_close(audio); }
    yuv_blitter_free(&yuv);
    yuv_close();
    if (vsync) video_sync_destroy(vsync);
    video_close(video);
    display_set_fps_limit(0);
    display_close();
    /* leave everything as the game expects to find it: it initialises rdpq and the audio itself, later */
    mixer_close();
    audio_close();
    rdpq_close();
    return 1;
}
