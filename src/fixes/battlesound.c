// Battle sound is heard from ListenerDistanceFight in front of the camera; zoomed out past the game's 40 that point
// rises and distance reverb takes over, so the distance grows by however far the camera is past 40.
#include "../fix.h"
#include "../live.h"
#include "../zoom.h"

static const WtTarget targets[] = {
    {"battle.Battle.getSoundListenerDistance", 0}, // its last op returns the constant's value
};

static int apply(HlCode *c, HlEdit *e, const int *fn) {
    HlFun *f = hl_fun_decode(c, fn[0]);
    int tf64 = hl_find_type(c, HF64, NULL), name = wt_string_global(c, "ListenerDistanceFight");
    int at = f ? f->nops - 1 : -1, reads = 0, jumped_to = 0;
    for (int i = 0; f && i < f->nops; i++) {
        if (f->ops[i].op == OGetGlobal && f->ops[i].a[1] == name) reads++;
        if (hl_is_jump(f->ops[i].op) && *hl_jump_target(&f->ops[i], 0) == at) jumped_to++;
    }
    if (!f || name < 0 || reads != 1 || f->ops[at].op != ORet || f->regs[f->ops[at].a[0]] != tf64 || jumped_to) {
        wt_log("  getSoundListenerDistance no longer ends by returning ListenerDistanceFight");
        hl_fun_free(f);
        return -1;
    }

    // Every jump in the block goes to the Ret: the constant's value is returned as it is unless all of it holds.
    int rvalue = f->ops[at].a[0];
    Zoom z;
    zoom_begin(&z, c, e, f);
    zoom_live(&z, LIVE_SOUND, at);
    int rdist = zoom_distance(&z, zoom_object(&z, zoom_object(&z, 0, "s3d", at), "camera", at), at);
    int rpast = zoom_reg(&z, tf64);
    zoom_op(&z, wt_op(OSub, 3, rpast, rdist, zoom_float(&z, VANILLA_MAX_ZOOM)));
    zoom_op(&z, wt_op(OJSLte, 3, rpast, zoom_float(&z, 0), at));
    zoom_op(&z, wt_op(OAdd, 3, rvalue, rvalue, rpast));
    int result = zoom_insert(&z, at) || hl_edit_replace(e, fn[0], f) ? -1 : 0;
    hl_fun_free(f);
    return result;
}

const WtFix fix_battlesound = {"battlesound", "battle sound listener follows the zoom past 40", targets, 1, apply};
