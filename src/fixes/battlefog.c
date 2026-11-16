// Battle fog bands are in world units, so zoomed out past the game's 40 they cover much of the ground. In battle, the
// start distance and distance scale handed to the shader scale by zoom/40; the blend between setups is not scaled.
#include "../fix.h"
#include "../live.h"
#include "../zoom.h"
#include <stdio.h>

static const WtTarget targets[] = {
    {"hrt.prefab.rfx.GenFog.end", 0}, // its startDistance__ and distanceScale__ stores are found and checked
};

// The one op storing an f64 into a GenFogShader's <name>__.
static int store(HlCode *c, const HlFun *f, int tshader, const char *name, int tf64) {
    char shader_name[64];
    snprintf(shader_name, sizeof shader_name, "%s__", name);
    int fid = hl_type_field(c, tshader, shader_name), found = -1;
    for (int i = 0; fid >= 0 && i < f->nops; i++) {
        const HlOp *o = &f->ops[i];
        if (o->op != OSetField || o->a[1] != fid || f->regs[o->a[0]] != tshader || f->regs[o->a[2]] != tf64) continue;
        if (found >= 0) {
            wt_log("  %s is handed to the shader twice", name);
            return -1;
        }
        found = i;
    }
    if (found < 0) wt_log("  %s is no longer handed to the shader", name);
    return found;
}

static int apply(HlCode *c, HlEdit *e, const int *fn) {
    HlFun *f = hl_fun_decode(c, fn[0]);
    int tf64 = hl_find_type(c, HF64, NULL), tshader = hl_find_type(c, HOBJ, "hrt.prefab.rfx.GenFogShader");
    int start = -1, scale = -1;
    if (f && tf64 >= 0 && tshader >= 0) {
        start = store(c, f, tshader, "startDistance", tf64);
        scale = store(c, f, tshader, "distanceScale", tf64);
    } else if (f) {
        wt_log("  hrt.prefab.rfx.GenFogShader is missing");
    }
    if (start < 0 || scale < 0) {
        hl_fun_free(f);
        return -1;
    }

    // Every jump in the block goes to op 0 as it was: the factor stays 1 unless all of it holds.
    Zoom z;
    zoom_begin(&z, c, e, f);
    int rfactor = zoom_float(&z, 1);
    zoom_live(&z, LIVE_FOG, 0);
    zoom_battle_mode(&z, 0);
    zoom_op(&z, wt_op(OJNull, 2, 1, 0));
    int rdist = zoom_distance(&z, zoom_object(&z, zoom_object(&z, 1, "ctx", 0), "camera", 0), 0);
    int rzoomed = zoom_reg(&z, tf64);
    zoom_op(&z, wt_op(OSDiv, 3, rzoomed, rdist, zoom_float(&z, VANILLA_MAX_ZOOM)));
    zoom_op(&z, wt_op(OJSLte, 3, rzoomed, rfactor, 0));
    zoom_op(&z, wt_op(OMov, 2, rfactor, rzoomed));

    // The later store first, so the earlier one is still where it was found; the block goes in after both.
    int at[2] = {start, scale}, ops[2] = {OMul, OSDiv};
    if (start < scale) at[0] = scale, at[1] = start, ops[0] = OSDiv, ops[1] = OMul;
    if (!z.bad)
        for (int k = 0; k < 2; k++) {
            int value = f->ops[at[k]].a[2];
            HlOp o = wt_op(ops[k], 3, value, value, rfactor);
            hl_fun_insert(f, at[k], &o, 1);
        }
    int result = zoom_insert(&z, 0) || hl_edit_replace(e, fn[0], f) ? -1 : 0;
    hl_fun_free(f);
    return result;
}

const WtFix fix_battlefog = {"battlefog", "battle distance fog follows the zoom past 40", targets, 1, apply};
