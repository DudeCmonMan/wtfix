// Battle distance blur is tuned for the game's 40 zoom limit, so zoomed out further the whole field blurs. In battle,
// the four distances handed to the shader move out by how far the camera is past 40; the prefabs stay unchanged.
#include "../fix.h"
#include "../live.h"
#include "../zoom.h"
#include <stdio.h>

static const WtTarget targets[] = {
    {"hrt.prefab.rfx.DistanceBlur.end", 0}, // its four distance hand-offs to the shader are found and checked
};

static const char *const distances[] = {"nearStartDistance", "nearEndDistance", "farStartDistance", "farEndDistance"};

// The op storing this.<name> into the shader's <name>__, right after it is read.
static int handoff(HlCode *c, const HlFun *f, const char *name, int tf64) {
    int own = hl_type_field(c, f->regs[0], name), found = -1;
    char shader_name[64];
    snprintf(shader_name, sizeof shader_name, "%s__", name);
    for (int i = 0; i + 1 < f->nops; i++) {
        const HlOp *get = &f->ops[i], *set = &f->ops[i + 1];
        int reads_own = (get->op == OGetThis && get->a[1] == own) || (get->op == OField && get->a[1] == 0 && get->a[2] == own);
        if (!reads_own || set->op != OSetField || set->a[2] != get->a[0]) continue;
        if (set->a[1] != hl_type_field(c, f->regs[set->a[0]], shader_name) || f->regs[get->a[0]] != tf64) continue;
        if (found >= 0) {
            wt_log("  %s is handed to the shader twice", name);
            return -1;
        }
        found = i + 1;
    }
    if (found < 0) wt_log("  %s is no longer handed to the shader", name);
    return found;
}

static int apply(HlCode *c, HlEdit *e, const int *fn) {
    HlFun *f = hl_fun_decode(c, fn[0]);
    int tf64 = hl_find_type(c, HF64, NULL), at[4], ok = f && tf64 >= 0;
    for (int k = 0; ok && k < 4; k++) ok = (at[k] = handoff(c, f, distances[k], tf64)) >= 0;
    if (!ok) {
        hl_fun_free(f);
        return -1;
    }

    // Every jump in the block goes to op 0 as it was, the function's own start: shift stays 0 unless all of it holds.
    Zoom z;
    zoom_begin(&z, c, e, f);
    int rshift = zoom_float(&z, 0);
    zoom_live(&z, LIVE_BLUR, 0);
    zoom_battle_mode(&z, 0);
    zoom_op(&z, wt_op(OJNull, 2, 1, 0));
    int rdist = zoom_distance(&z, zoom_object(&z, zoom_object(&z, 1, "ctx", 0), "camera", 0), 0);
    zoom_op(&z, wt_op(OSub, 3, rshift, rdist, zoom_float(&z, VANILLA_MAX_ZOOM)));
    zoom_op(&z, wt_op(OJSGte, 3, rshift, zoom_float(&z, 0), 0));
    zoom_op(&z, wt_op(OFloat, 2, rshift, hl_edit_float(e, 0)));

    // Last first, so the earlier hand-offs are still where they were found; the block goes in after them.
    for (int i = 1; i < 4; i++)
        for (int j = i; j > 0 && at[j] > at[j - 1]; j--) {
            int t = at[j];
            at[j] = at[j - 1], at[j - 1] = t;
        }
    if (!z.bad)
        for (int k = 0; k < 4; k++) {
            int value = f->ops[at[k] - 1].a[0];
            HlOp add = wt_op(OAdd, 3, value, value, rshift);
            hl_fun_insert(f, at[k], &add, 1);
        }
    int result = zoom_insert(&z, 0) || hl_edit_replace(e, fn[0], f) ? -1 : 0;
    hl_fun_free(f);
    return result;
}

const WtFix fix_battleblur = {"battleblur", "battle distance blur follows the zoom past 40", targets, 1, apply};
