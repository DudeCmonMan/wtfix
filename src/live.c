#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fix.h"
#include "live.h"

#define HL_PRIM(name) \
    __declspec(dllexport) void *hlp_##name(const char **sign) { *sign = NULL; return (void *)name; }

// The zoom defaults are the game's, replaced by data.cdb's own when it loads (live_seed).
WtLive wt_live[LIVE_COUNT] = {
    {"Max zoom out", "const", "CameraMaxDistance", "%.0f", 40, 40, 40, 20, 200, false, false},
    {"Max zoom in", "const", "CameraMinDistance", "%.0f", 30, 30, 30, 5, 100, false, false},
    {"Full HUD size", "ui", "unit_hud_scale", "%.2f", 0.8, 1, 0.8, 0.2, 2, false, false},
    {"Small bars size", "ui", "unit_bars_scale", "%.2f", 1, 1, 1, 0.2, 2, false, false},
    {"Blur follows zoom", "ui", "blur_follows_zoom", "%.0f", 1, 0, 1, 0, 1, true, false},
    {"Fog follows zoom", "ui", "fog_follows_zoom", "%.0f", 1, 0, 1, 0, 1, true, false},
    {"Sound follows zoom", "ui", "sound_follows_zoom", "%.0f", 1, 0, 1, 0, 1, true, false},
    {"Cull props off screen", "perf", "cull_offscreen_props", "%.0f", 1, 0, 1, 0, 1, true, false},
    {"Hide props under (px)", "perf", "cull_small_props_px", "%.0f", 6, 0, 6, 0, 8, false, false},
    {"Cache static shadows", "perf", "shadow_cache", "%.0f", 1, 0, 1, 0, 1, true, false},
    {"Shadow cache margin (%)", "perf", "shadow_cache_margin", "%.0f", 5, 0, 5, 0, 25, false, false},
    {"Keep still FX look-at planes", "perf", "fx_lookat_cache", "%.0f", 0, 0, 0, 0, 1, true, false},
    {"Armour joints once a frame", "perf", "subskin_once", "%.0f", 1, 0, 1, 0, 1, true, false},
    {"Skin joints computed in C", "perf", "skin_joints_c", "%.0f", 1, 0, 1, 0, 1, true, false},
    {"Draw threads", "perf", "draw_threads", "%.0f", 3, 0, 3, 0, 7, false, false},
    {"NVIDIA DLSS", "dlss", "enable", "%.0f", 1, 0, 1, 0, 1, true, false},
    {"DLSS mode", "dlss", "mode", "%.0f", 1, 0, 1, 0, 4, false, false},
    {"Game sharpen with DLSS", "dlss", "sharpen", "%.0f", 0, 0, 0, 0, 1, true, false},
    {"Sharper textures with DLSS", "dlss", "mip_bias", "%.0f", 1, 0, 1, 0, 1, true, false},
    {"Panel scale", "ui", "panel_scale", "%.1fx", 1, 1, 1, 0.5, 2, false, false},
};

static wchar_t ini_path[MAX_PATH];
static volatile LONG pending;
static bool seeded[2], dirty;

static LONG takers_of(int id) {
    if (id == LIVE_CULL_OFFSCREEN || id == LIVE_CULL_SMALL) return 1L << LIVE_TAKE_CULL;
    if (id == LIVE_SHADOW_CACHE) return 1L << LIVE_TAKE_SHADOW;
    // None: read each frame or on each call.
    if (id == LIVE_SHADOW_MARGIN) return 0;
    if (id == LIVE_FX_LOOKAT) return 0;
    if (id == LIVE_SUBSKIN_ONCE) return 0;
    if (id == LIVE_SKIN_C) return 0;
    if (id == LIVE_DRAW_THREADS) return 0;
    if (id == LIVE_DLSS_MODE || id == LIVE_DLSS_MIP_BIAS) return 0;
    if (id == LIVE_PANEL_SCALE) return 0;
    if (id == LIVE_DLSS || id == LIVE_DLSS_SHARPEN) return 1L << LIVE_TAKE_DLSS;
    return (1L << LIVE_TAKE_CONSTS) | (1L << LIVE_TAKE_HUDS);
}

static void changed(LONG takers) { InterlockedOr(&pending, takers); }

static double clamp(const WtLive *s, double v) { return v < s->low ? s->low : v > s->high ? s->high : v; }

void wt_live_load(const wchar_t *ini) {
    wcsncpy(ini_path, ini, MAX_PATH - 1);
    for (int i = 0; i < LIVE_COUNT; i++) {
        WtLive *s = &wt_live[i];
        double v = wt_ini_number(ini, s->section, s->key, NAN);
        s->set = !isnan(v);
        if (s->set && clamp(s, v) != v) wt_log("live: [%s] %s=%g is outside %g-%g", s->section, s->key, v, s->low, s->high);
        s->value = s->set ? clamp(s, v) : s->fallback;
    }
    char line[512];
    int at = snprintf(line, sizeof line, "live:");
    for (int i = 0; i < LIVE_COUNT && at < (int)sizeof line; i++) {
        const WtLive *s = &wt_live[i];
        if (s->toggle) at += snprintf(line + at, sizeof line - at, "%s %s %s", i ? "," : "", s->label, s->value >= 0.5 ? "on" : "off");
        else at += snprintf(line + at, sizeof line - at, "%s %s %g%s", i ? "," : "", s->label, s->value, i < 2 && !s->set ? " (game)" : "");
    }
    wt_log("%s", line);
    dirty = false;
    // The game sets its AA effects itself when it sets up a renderer (initMode); only changes made here re-run it.
    changed(((1L << LIVE_TAKERS) - 1) & ~(1L << LIVE_TAKE_DLSS));
}

void wt_live_change(int id, double value) {
    if (id < 0 || id >= LIVE_COUNT) return;
    wt_live[id].value = clamp(&wt_live[id], value);
    wt_live[id].set = true;
    dirty = true;
    changed(takers_of(id));
}

// Stays set (saved) only where wtfix's default differs from the game's.
void wt_live_reset(int id) {
    if (id < 0 || id >= LIVE_COUNT) return;
    WtLive *s = &wt_live[id];
    s->value = s->game;
    s->set = s->game != s->fallback;
    dirty = true;
    changed(takers_of(id));
}

bool wt_live_dirty(void) { return dirty; }
const wchar_t *wt_live_ini(void) { return ini_path; }

static double live_number(int id) { return id >= 0 && id < LIVE_COUNT ? wt_live[id].value : 0; }
// A zoom constant is written only once the game's value is known or one was set.
static bool live_known(int id) { return id >= 0 && id < 2 && (seeded[id] || wt_live[id].set); }
static bool live_take(int taker) {
    LONG bit = 1L << taker;
    return taker >= 0 && taker < LIVE_TAKERS && (InterlockedAnd(&pending, ~bit) & bit);
}
bool wt_live_take(int taker) { return live_take(taker); }
static void live_seed(int id, double game) {
    if (id < 0 || id >= 2) return;
    wt_live[id].game = game;
    seeded[id] = true;
    if (!wt_live[id].set) wt_live[id].value = game;
    changed(takers_of(id)); // also after a language switch reloads data.cdb over what was set
}
static void live_error(void *exc) {
    static int n;
    if (n++ < 3) wt_log("live: applying the settings in battle threw (%p)", exc);
}
HL_PRIM(live_number)
HL_PRIM(live_known)
HL_PRIM(live_take)
HL_PRIM(live_seed)
HL_PRIM(live_error)

typedef struct {
    char **line;
    int n, cap;
} Lines;

static void insert_line(Lines *l, int at, const char *text) {
    if (l->n == l->cap) l->cap = l->cap * 2 + 16, l->line = (char **)realloc(l->line, l->cap * sizeof(char *));
    memmove(&l->line[at + 1], &l->line[at], (l->n - at) * sizeof(char *));
    l->line[at] = _strdup(text);
    l->n++;
}

static const char *skip_space(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static int is_key(const char *line, const char *key) {
    const char *s = skip_space(line);
    size_t n = strlen(key);
    return !_strnicmp(s, key, n) && *skip_space(s + n) == '=';
}

static void update(Lines *l, const char *section, const char *key, const char *value) {
    int head = -1, found = -1, last = -1;
    for (int i = 0; i < l->n; i++) {
        const char *s = skip_space(l->line[i]);
        if (*s == '[') {
            if (head >= 0) break;
            size_t n = strlen(section);
            if (!_strnicmp(s + 1, section, n) && s[1 + n] == ']') head = i;
            continue;
        }
        if (head < 0) continue;
        if (is_key(l->line[i], key)) found = i;
        if (*s && *s != ';' && *s != '#') last = i;
    }
    char text[512];
    if (found >= 0) {
        if (!value) {
            free(l->line[found]);
            memmove(&l->line[found], &l->line[found + 1], (l->n - found - 1) * sizeof(char *));
            l->n--;
            return;
        }
        const char *comment = strchr(strchr(l->line[found], '=') + 1, ';');
        snprintf(text, sizeof text, "%s=%s%s%s", key, value, comment ? " " : "", comment ? comment : "");
        free(l->line[found]);
        l->line[found] = _strdup(text);
        return;
    }
    if (!value) return;
    snprintf(text, sizeof text, "%s=%s", key, value);
    if (head < 0) {
        char header[96];
        snprintf(header, sizeof header, "[%s]", section);
        insert_line(l, l->n, "");
        insert_line(l, l->n, header);
        insert_line(l, l->n, text);
    } else {
        insert_line(l, (last >= 0 ? last : head) + 1, text);
    }
}

int wt_live_save(const wchar_t *ini) {
    Lines l = {0};
    FILE *f = _wfopen(ini, L"rb");
    if (f) {
        char buf[1024];
        while (fgets(buf, sizeof buf, f)) {
            buf[strcspn(buf, "\r\n")] = 0;
            insert_line(&l, l.n, buf);
        }
        fclose(f);
    }
    for (int i = 0; i < LIVE_COUNT; i++) {
        const WtLive *s = &wt_live[i];
        char value[64];
        snprintf(value, sizeof value, s->toggle ? "%.0f" : "%.4g", s->value);
        update(&l, s->section, s->key, s->set ? value : NULL);
    }
    while (l.n && !*skip_space(l.line[l.n - 1])) free(l.line[--l.n]);
    f = _wfopen(ini, L"wb");
    int ok = f != NULL;
    for (int i = 0; ok && i < l.n; i++) ok = fprintf(f, "%s\r\n", l.line[i]) >= 0;
    if (f) ok = !fclose(f) && ok;
    for (int i = 0; i < l.n; i++) free(l.line[i]);
    free(l.line);
    if (!ok) {
        wt_log("live: could not write wtfix.ini");
        return -1;
    }
    dirty = false;
    wt_log("live: saved to wtfix.ini");
    return 0;
}
