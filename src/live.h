// Settings the panel changes while the game runs; the patched game code reads them through natives (live.c).
#pragma once
#include <stdbool.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { LIVE_ZOOM_MAX, LIVE_ZOOM_MIN, LIVE_HUD_FULL, LIVE_HUD_BARS, LIVE_BLUR, LIVE_FOG, LIVE_SOUND, LIVE_CULL_OFFSCREEN,
       LIVE_CULL_SMALL, LIVE_SHADOW_CACHE, LIVE_SHADOW_MARGIN, LIVE_FX_LOOKAT, LIVE_SUBSKIN_ONCE,
       LIVE_SKIN_C, LIVE_DLSS, LIVE_DLSS_MODE, LIVE_DLSS_SHARPEN, LIVE_DLSS_MIP_BIAS, LIVE_PANEL_SCALE,
       LIVE_COUNT };
// What game code re-applies on a change; culling has its own as its rebuild costs a hitch.
enum { LIVE_TAKE_CONSTS, LIVE_TAKE_HUDS, LIVE_TAKE_CULL, LIVE_TAKE_SHADOW, LIVE_TAKE_DLSS, LIVE_TAKERS };
static const char *const wt_dlss_modes[5] = {"DLAA", "Quality", "Balanced", "Performance", "Ultra performance"};

typedef struct {
    const char *label, *section, *key, *format;
    double value, game, fallback, low, high; // fallback: when wtfix.ini has none
    bool toggle;
    bool set; // written by Save; unset ones follow the game's value
} WtLive;

extern WtLive wt_live[LIVE_COUNT];
// Indexed by LIVE_ZOOM_MAX, LIVE_ZOOM_MIN.
static const char *const wt_live_const_names[2] = {"CameraMaxDistance", "CameraMinDistance"};

void wt_live_load(const wchar_t *ini);
void wt_live_change(int id, double value);
void wt_live_reset(int id);
bool wt_live_dirty(void);
int wt_live_save(const wchar_t *ini);
const wchar_t *wt_live_ini(void);
bool wt_live_take(int taker);

#ifdef __cplusplus
}
#endif
