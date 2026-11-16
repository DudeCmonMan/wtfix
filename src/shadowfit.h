// shadowcache's rules: the static map holds only while the light camera does, so Heaps' fit is widened and held.
#pragma once
#include <math.h>
#include <stdbool.h>

// shadow_put / shadow_get slots.
enum {
    SHADOW_POS = 0,
    SHADOW_TARGET = 3,
    SHADOW_BOX = 6,     // orthoBounds xMin, xMax, yMin, yMax, zMin, zMax (light view space)
    SHADOW_ROT = 12,    // mcam _11 _12 _13 _21 _22 _23 _31 _32 _33
    SHADOW_COUNT = 21,
    SHADOW_SIG = 22,
    SHADOW_SLOTS = 24
};

// shadow_flag / shadow_set.
enum {
    SHADOW_ACTIVE,
    SHADOW_KEEP,      // set: the next processShadowMap starts from the static map
    SHADOW_KEEP_TAKE, // read and clear, in place of processShadowMap's depth clear
    SHADOW_COPY_FAILED
};

// Each refit leaves this much room for the fit to shrink (zooming in) before a sharper one is made.
#define SHADOW_SHRINK 0.15

typedef struct {
    double pos[3], target[3], box[6];
} ShadowCam;

typedef struct {
    ShadowCam held;
    bool valid;
} ShadowFit;

static inline bool shadow_cam_usable(const ShadowCam *c) {
    for (int i = 0; i < 3; i++)
        if (!isfinite(c->pos[i]) || !isfinite(c->target[i])) return false;
    // An empty fit (no caster) is +-1e20.
    for (int i = 0; i < 6; i += 2)
        if (!isfinite(c->box[i]) || !isfinite(c->box[i + 1]) || c->box[i + 1] <= c->box[i] || c->box[i + 1] - c->box[i] > 1e6)
            return false;
    return true;
}

// 0: unusable fit, the game's frame; 1: hold (*out = held); 2: a new held camera (*out = the fit widened by margin).
// rot multiplies row vectors as Heaps does: a world offset d moves the box by d * rot.
static inline int shadow_fit_decide(ShadowFit *s, const ShadowCam *in, const double rot[9], double margin, ShadowCam *out) {
    if (!shadow_cam_usable(in)) {
        s->valid = false;
        return 0;
    }
    if (s->valid) {
        const ShadowCam *h = &s->held;
        bool same_sun = true;
        for (int i = 0; i < 3; i++)
            if (fabs((in->target[i] - in->pos[i]) - (h->target[i] - h->pos[i])) > 1e-7) same_sun = false;
        double d[3] = {in->pos[0] - h->pos[0], in->pos[1] - h->pos[1], in->pos[2] - h->pos[2]};
        bool inside = same_sun;
        for (int axis = 0; axis < 3 && inside; axis++) {
            double shift = d[0] * rot[axis] + d[1] * rot[3 + axis] + d[2] * rot[6 + axis];
            double lo = in->box[2 * axis] + shift, hi = in->box[2 * axis + 1] + shift;
            double slack = 1e-9 * (h->box[2 * axis + 1] - h->box[2 * axis]);
            if (lo < h->box[2 * axis] - slack || hi > h->box[2 * axis + 1] + slack) inside = false;
        }
        double keep = (1 - SHADOW_SHRINK) / (1 + 2 * margin);
        for (int axis = 0; axis < 2 && inside; axis++)
            if (in->box[2 * axis + 1] - in->box[2 * axis] < keep * (h->box[2 * axis + 1] - h->box[2 * axis])) inside = false;
        if (inside) {
            *out = *h;
            return 1;
        }
    }
    *out = *in;
    for (int axis = 0; axis < 3; axis++) {
        double grow = margin * (in->box[2 * axis + 1] - in->box[2 * axis]);
        out->box[2 * axis] -= grow;
        out->box[2 * axis + 1] += grow;
    }
    s->held = *out;
    s->valid = true;
    return 2;
}

typedef struct {
    bool valid;
    int count;
    double sig;
} ShadowStatic;

// 2: redraw the static map; 1: reuse it. *moved: the same casters at other places (a static mesh that moves).
static inline int shadow_plan_decide(ShadowStatic *s, bool new_texture, bool refit, int count, double sig, bool *moved) {
    *moved = s->valid && !new_texture && !refit && count == s->count && sig != s->sig;
    if (s->valid && !new_texture && !refit && count == s->count && sig == s->sig) return 1;
    s->valid = true;
    s->count = count;
    s->sig = sig;
    return 2;
}
