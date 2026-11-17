/* Frame profiler for the N64 port. Compiled in only with -DN64_PROF (EXTRA_CFLAGS); otherwise every macro is empty.
 * Exclusive timing: time goes to the innermost section entered. Report on the debug channel (ISViewer / USB) every
 * PROF_REPORT presents, plus one "[bench]" summary for the frame window N64_BENCH_F0..N64_BENCH_F1 and a "[benchsum]"
 * line whose totals and checksum of the draw lists must not change when an optimisation is behaviour-preserving. */
#ifndef PROF_N64_H
#define PROF_N64_H
#ifdef __cplusplus
extern "C" {
#endif

enum { PF_LOGIC, PF_DRAW, PF_PRESENT, PF_TEXCONV, PF_WAIT, PF_AUDIO, PF_HASH, PF_UPLOAD, PF_TRI, PF_RECT, PF_MODE, PF_N };
enum { PC_LOGIC, PC_CMDS, PC_VERTS, PC_TRIS, PC_RECTS, PC_UPLOADS, PC_CONV, PC_AREA, PC_X1, PC_X2, PC_N };

#ifdef N64_PROF
void gta2_n64_prof_enter(int id);
void gta2_n64_prof_leave(void);
void gta2_n64_prof_count(int id, unsigned n);
void gta2_n64_prof_frame(void);
#define PROF_ENTER(id) gta2_n64_prof_enter(id)
#define PROF_LEAVE() gta2_n64_prof_leave()
#define PROF_COUNT(id, n) gta2_n64_prof_count(id, (unsigned)(n))
#define PROF_FRAME() gta2_n64_prof_frame()
#else
#define PROF_ENTER(id) ((void)0)
#define PROF_LEAVE() ((void)0)
#define PROF_COUNT(id, n) ((void)0)
#define PROF_FRAME() ((void)0)
#endif

#ifdef __cplusplus
}
#endif
#endif
