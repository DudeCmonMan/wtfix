// fxlookat's rules: a look-at plane's transform depends only on the camera and on inputs that mark it moved, so
// after FXLOOK_STILL synced frames with neither changing, a recompute gives what it already holds.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define FXLOOK_SLOTS 4096 // a power of two, well over the planes in a battle
#define FXLOOK_PROBES 16
#define FXLOOK_STILL 3 // a non-uniformly scaled parent's scale settles a frame late, prevAbsPos one more

typedef struct {
    const void *obj;
    int frame, streak;
    uint32_t gen;
} FxLookEntry;

typedef struct {
    double cam[9]; // pos, target, up
    bool cam_known;
    uint32_t gen; // changes with the camera
    int frame;
    long long asked, skipped;
    long long frame_asked, frame_skipped, last_asked, last_skipped;
    FxLookEntry slot[FXLOOK_SLOTS];
} FxLook;

// Before each plane's question; several cameras in a frame read as moves.
static inline void fxlook_camera_at(FxLook *s, int frame, const double cam[9]) {
    if (!s->cam_known || memcmp(cam, s->cam, sizeof s->cam)) {
        memcpy(s->cam, cam, sizeof s->cam);
        s->cam_known = true;
        s->gen++;
    }
    if (frame != s->frame) {
        s->last_asked = s->frame_asked, s->last_skipped = s->frame_skipped;
        s->frame_asked = s->frame_skipped = 0;
        s->frame = frame;
    }
}

static inline FxLookEntry *fxlook_entry(FxLook *s, const void *obj) {
    uintptr_t h = (uintptr_t)obj;
    h ^= h >> 17, h *= 0x9E3779B97F4A7C15ull, h ^= h >> 29;
    FxLookEntry *free_slot = NULL;
    for (int i = 0; i < FXLOOK_PROBES; i++) {
        FxLookEntry *e = &s->slot[(h + i) & (FXLOOK_SLOTS - 1)];
        if (e->obj == obj) return e;
        // An entry not synced in the last frame has no streak left to keep.
        if (!free_slot && (!e->obj || e->frame < s->frame - 1 || e->frame > s->frame)) free_slot = e;
    }
    if (free_slot) free_slot->obj = NULL;
    return free_slot;
}

// last_frame: the plane's lastFrame before this sync; moved: its posChanged as the sync starts. Once per sync.
static inline bool fxlook_skip_at(FxLook *s, const void *obj, int last_frame, bool moved) {
    FxLookEntry *e = fxlook_entry(s, obj);
    s->asked++, s->frame_asked++;
    if (!e) return false;
    bool same = e->obj == obj && e->frame == s->frame - 1 && last_frame == s->frame - 1 && e->gen == s->gen && !moved;
    e->streak = !same ? 0 : e->streak < FXLOOK_STILL ? e->streak + 1 : FXLOOK_STILL;
    e->obj = obj, e->frame = s->frame, e->gen = s->gen;
    if (e->streak < FXLOOK_STILL) return false;
    s->skipped++, s->frame_skipped++;
    return true;
}
