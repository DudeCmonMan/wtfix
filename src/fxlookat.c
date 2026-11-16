// fxlookat's natives, called once per camera-facing FX plane per frame.
#include <stdio.h>
#include "fix.h"
#include "fxlookat.h"

#define HL_PRIM(name) \
    __declspec(dllexport) void *hlp_##name(const char **sign) { *sign = NULL; return (void *)name; }

static FxLook look;

static void fxlook_camera(int frame, double px, double py, double pz, double tx, double ty, double tz, double ux,
                          double uy, double uz) {
    const double cam[9] = {px, py, pz, tx, ty, tz, ux, uy, uz};
    fxlook_camera_at(&look, frame, cam);
}

// state: the plane's lastFrame << 1 | 1 when it is marked moved or has a fixed position.
static bool fxlook_skip(void *obj, int state) { return fxlook_skip_at(&look, obj, state >> 1, state & 1); }
HL_PRIM(fxlook_camera)
HL_PRIM(fxlook_skip)

int wt_fxlook_stats(char *out, int size) {
    return snprintf(out, size, "%lld of %lld planes kept last frame", look.last_skipped, look.last_asked);
}
