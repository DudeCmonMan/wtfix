// hrt's LookAtObject.syncRec marks camera-facing FX planes moved every frame; [perf] fx_lookat_cache leaves the mark
// off while wtfix@fxlook_skip says a recompute would change nothing. Planes aimed at an object are never skipped.
// The added ops read only fields they null-check and call natives: nothing new can throw, so no trap.
#include "../fix.h"
#include "../live.h"
#include <string.h>

static const WtTarget targets[] = {
    {"hrt.prefab.fx.LookAtObject.syncRec", 0x801b8d497d131674ull},
    {"h3d.scene.Object.getScene", 0}, // called as is: (Object)->Scene
};

static int fun_type(HlCode *c, int nargs, const int *args, int ret) {
    for (int i = 0; i < c->ntypes; i++) {
        const HlType *t = &c->types[i];
        if (t->kind == HFUN && t->nargs == nargs && t->ret == ret && !memcmp(t->args, args, nargs * sizeof(int)))
            return i;
    }
    return -1;
}

static int add_reg(HlFun *f, int type) {
    f->regs[f->nregs] = type;
    return f->nregs++;
}

static int field_of(HlCode *c, int type, const char *name, int want, int *ok) {
    int fid = hl_type_field(c, type, name);
    if (fid < 0 || hl_field_type(c, type, fid) != want) {
        wt_log("  %s.%s is missing or of another type", hl_type_name(c, type), name);
        *ok = 0;
    }
    return fid;
}

static int apply(HlCode *c, HlEdit *e, const int *fn) {
    if (!wt_signature(c, fn[0], "(hrt.prefab.fx.LookAtObject,h3d.scene.RenderContext)->void") ||
        !wt_signature(c, fn[1], "(h3d.scene.Object)->h3d.scene.Scene"))
        return -1;
    int ti32 = hl_find_type(c, HI32, NULL), tf64 = hl_find_type(c, HF64, NULL), tbool = hl_find_type(c, HBOOL, NULL);
    int tvoid = hl_find_type(c, HVOID, NULL), tdyn = hl_find_type(c, HDYN, NULL);
    int tlook = hl_find_type(c, HOBJ, "hrt.prefab.fx.LookAtObject"), tobj = hl_find_type(c, HOBJ, "h3d.scene.Object");
    int tctx = hl_find_type(c, HOBJ, "h3d.scene.RenderContext"), tscene = hl_find_type(c, HOBJ, "h3d.scene.Scene");
    int tcam = hl_find_type(c, HOBJ, "h3d.Camera"), tvec = hl_find_type(c, HOBJ, "h3d.VectorImpl");
    int ok = tlook >= 0 && tobj >= 0 && tctx >= 0 && tscene >= 0 && tcam >= 0 && tvec >= 0;
    if (!ok) {
        wt_log("  a Heaps type is missing");
        return -1;
    }
    int f_target = field_of(c, tlook, "target", tobj, &ok), f_last = field_of(c, tlook, "lastFrame", ti32, &ok);
    int f_flags = field_of(c, tlook, "flags", ti32, &ok), f_frame = field_of(c, tctx, "frame", ti32, &ok);
    int f_camera = field_of(c, tscene, "camera", tcam, &ok);
    int f_vec[3] = {field_of(c, tcam, "pos", tvec, &ok), field_of(c, tcam, "target", tvec, &ok),
                    field_of(c, tcam, "up", tvec, &ok)};
    int f_xyz[3] = {field_of(c, tvec, "x", tf64, &ok), field_of(c, tvec, "y", tf64, &ok),
                    field_of(c, tvec, "z", tf64, &ok)};
    int cam_args[10] = {ti32, tf64, tf64, tf64, tf64, tf64, tf64, tf64, tf64, tf64}, skip_args[2] = {tdyn, ti32};
    int t_number = wt_fun_type(c, 1, ti32, -1, tf64), t_cam = fun_type(c, 10, cam_args, tvoid);
    int t_skip = fun_type(c, 2, skip_args, tbool);
    if (!ok || t_number < 0 || t_cam < 0 || t_skip < 0) {
        wt_log("  no function type for live_number, fxlook_camera or fxlook_skip");
        return -1;
    }
    HlFun *f = hl_fun_decode(c, fn[0]);
    if (!f || !wt_expect(c, f, 22, OCall2, "h3d.scene.Object.syncRec") || f->ops[22].a[2] != 0 ||
        f->ops[22].a[3] != 1 || !wt_expect(c, f, 23, ORet, NULL)) {
        wt_log("  LookAtObject.syncRec no longer ends in Object.syncRec(this, ctx)");
        hl_fun_free(f);
        return -1;
    }
    int rid = add_reg(f, ti32), rnum = add_reg(f, tf64), rhalf = add_reg(f, tf64), rscene = add_reg(f, tscene);
    int rcam = add_reg(f, tcam), rtarget = add_reg(f, tobj), rvec = add_reg(f, tvec), rframe = add_reg(f, ti32);
    int rvoid = add_reg(f, tvoid), rlast = add_reg(f, ti32), rflags = add_reg(f, ti32), rone = add_reg(f, ti32);
    int rbit = add_reg(f, ti32), rstate = add_reg(f, ti32), rdyn = add_reg(f, tdyn), rskip = add_reg(f, tbool);
    int rshift = add_reg(f, ti32), rfixed = add_reg(f, ti32);
    int rcamv[9];
    for (int i = 0; i < 9; i++) rcamv[i] = add_reg(f, tf64);

    // Targets 0 and 22 name the original ops: the stock path (mark moved) and the Object.syncRec call.
    HlOp b[48];
    int n = 0;
    b[n++] = wt_op(OInt, 2, rid, hl_edit_int(e, LIVE_FX_LOOKAT));
    b[n++] = wt_op(OCall1, 3, rnum, hl_edit_native(e, "wtfix", "live_number", t_number), rid);
    b[n++] = wt_op(OFloat, 2, rhalf, hl_edit_float(e, 0.5));
    b[n++] = wt_op(OJSLt, 3, rnum, rhalf, 0);
    b[n++] = wt_op(OJNull, 2, 1, 0);
    b[n++] = wt_op(OCall1, 3, rscene, wt_findex(c, fn[1]), 0);
    b[n++] = wt_op(OJNull, 2, rscene, 0);
    b[n++] = wt_op(OField, 3, rcam, rscene, f_camera);
    b[n++] = wt_op(OJNull, 2, rcam, 0);
    b[n++] = wt_op(OField, 3, rtarget, 0, f_target);
    b[n++] = wt_op(OJNotNull, 2, rtarget, 0);
    for (int v = 0; v < 3; v++) {
        b[n++] = wt_op(OField, 3, rvec, rcam, f_vec[v]);
        b[n++] = wt_op(OJNull, 2, rvec, 0);
        for (int k = 0; k < 3; k++) b[n++] = wt_op(OField, 3, rcamv[3 * v + k], rvec, f_xyz[k]);
    }
    b[n++] = wt_op(OField, 3, rframe, 1, f_frame);
    b[n++] = wt_op(OCallN, 13, rvoid, hl_edit_native(e, "wtfix", "fxlook_camera", t_cam), 10, rframe, rcamv[0],
                   rcamv[1], rcamv[2], rcamv[3], rcamv[4], rcamv[5], rcamv[6], rcamv[7], rcamv[8]);
    b[n++] = wt_op(OField, 3, rlast, 0, f_last);
    b[n++] = wt_op(OField, 3, rflags, 0, f_flags);
    // Asked as moved: posChanged (1), or fixedPosition (8192), whose syncRec returns early when not changed.
    b[n++] = wt_op(OInt, 2, rone, hl_edit_int(e, 1));
    b[n++] = wt_op(OInt, 2, rshift, hl_edit_int(e, 13));
    b[n++] = wt_op(OSShr, 3, rfixed, rflags, rshift);
    b[n++] = wt_op(OOr, 3, rbit, rflags, rfixed);
    b[n++] = wt_op(OAnd, 3, rbit, rbit, rone);
    b[n++] = wt_op(OShl, 3, rstate, rlast, rone);
    b[n++] = wt_op(OOr, 3, rstate, rstate, rbit);
    b[n++] = wt_op(OMov, 2, rdyn, 0);
    b[n++] = wt_op(OCall2, 4, rskip, hl_edit_native(e, "wtfix", "fxlook_skip", t_skip), rdyn, rstate);
    b[n++] = wt_op(OJTrue, 2, rskip, 22);
    hl_fun_insert(f, 0, b, n);
    int result = hl_edit_replace(e, fn[0], f) ? -1 : 0;
    hl_fun_free(f);
    return result;
}

const WtFix fix_fxlookat = {"fxlookat",
                            "camera-facing FX planes keep their transform while the camera and they hold still "
                            "(live, [perf] fx_lookat_cache)",
                            targets, 2, apply};
