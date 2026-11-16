// Unit skins: joint loops in C ([perf] skin_joints_c) and sub-skins once a frame ([perf] subskin_once); one fix
// because both patch the same functions.
// subskin_once keeps a sub-skin's first result in a frame while its jointsUpdated, enableRetargeting and its base's
// joints hold: skins computing twice in a frame count in wtfix@joints_repeat, and the sub-skin keeps that count in
// forceJointsUpdateOnFrame (never used on a gfx.SubSkin). The counting runs with the switch off so it can change live.
// The added ops read fields and call natives that neither throw nor call HL: nothing new can throw.
#include "../fix.h"
#include "../live.h"
#include "../objlayout.h"
#include "../skinjoints.h"
#include <string.h>

static const WtTarget targets[] = {
    {"gfx.SubSkin.syncJoints", 0xe2945e7c5ad7fef0ull},
    {"h3d.scene.Skin.syncJoints", 0x3c630434b3a550d3ull},
    {"gfx.WeightedSubSkin.syncJoints", 0x7e759ff67a078301ull},
};

// What the C also stands in for, per joint; a build that changed any of them keeps the game's loops.
static const WtTarget kernel_calls[] = {
    {"h3d.scene.JointData.sync", 0xd1cee302cd288ebaull},
    {"h3d.MatrixImpl.multiply", 0x903bdddee235dac3ull},
    {"h3d.MatrixImpl.load", 0x903785446fa5f841ull},
};

#define COMPUTE 10   // each function's first op past "if (nothing to do) return"
#define SKIN_LOOP 27 // Skin.syncJoints: the joint loop, past the accumulator
#define SKIN_END 57  // Skin.syncJoints: "jointsUpdated = false; buffersDirty = true; jointsFrame = ..."
#define SUB_END 590  // the same in gfx.SubSkin.syncJoints

WtSkinLayout wt_skin_layout;

const int wt_skin_role[WT_SKIN_FIELDS] = {
    ROLE_SKIN,  ROLE_SKIN,  ROLE_SKIN,  ROLE_SKIN,  ROLE_SKIN,  ROLE_SUBSKIN, ROLE_ANIM, ROLE_ARRAY,
    ROLE_ARRAY, ROLE_JOINT, ROLE_JOINT, ROLE_JOINT, ROLE_JOINT, ROLE_JOINT,   ROLE_JOINT, ROLE_JOINT,
    ROLE_JOINTDATA, ROLE_JOINTDATA, ROLE_JOINTDATA, ROLE_JOINTDATA, ROLE_JOINTDATA, ROLE_MATRIX,
};

static const struct {
    const char *cls, *name, *type;
} skin_fields[WT_SKIN_FIELDS] = {
    {"h3d.scene.Skin", "skinData", "h3d.anim.Skin"},
    {"h3d.scene.Skin", "jointsData", "hl.types.ArrayObj"},
    {"h3d.scene.Skin", "currentPalette", "hl.types.ArrayObj"},
    {"h3d.scene.Skin", "absPos", "h3d.MatrixImpl"},
    {"h3d.scene.Skin", "enableRetargeting", "bool"},
    {"gfx.SubSkin", "baseSkin", "h3d.scene.Skin"},
    {"h3d.anim.Skin", "allJoints", "hl.types.ArrayObj"},
    {"hl.types.ArrayObj", "length", "i32"},
    {"hl.types.ArrayObj", "array", "array"},
    {"h3d.anim.Joint", "index", "i32"},
    {"h3d.anim.Joint", "bindIndex", "i32"},
    {"h3d.anim.Joint", "parent", "h3d.anim.Joint"},
    {"h3d.anim.Joint", "follow", "h3d.anim.Joint"},
    {"h3d.anim.Joint", "defMat", "h3d.MatrixImpl"},
    {"h3d.anim.Joint", "transPos", "h3d.MatrixImpl"},
    {"h3d.anim.Joint", "retargetAnim", "bool"},
    {"h3d.scene.JointData", "currentRelPos", "h3d.MatrixImpl"},
    {"h3d.scene.JointData", "currentAbsPos", "h3d.MatrixImpl"},
    {"h3d.scene.JointData", "additivePose", "h3d.MatrixImpl"},
    {"h3d.scene.JointData", "targetMat", "h3d.MatrixImpl"},
    {"h3d.scene.JointData", "originMat", "h3d.MatrixImpl"},
    {"h3d.MatrixImpl", "_11", "f64"},
};

int wt_skin_layout_build(HlCode *c, WtSkinLayout *out) {
    memset(out, 0, sizeof *out);
    for (int i = 0; i < WT_SKIN_FIELDS; i++) {
        int t = hl_find_type(c, HOBJ, skin_fields[i].cls);
        int fid = t < 0 ? -1 : hl_type_field(c, t, skin_fields[i].name);
        int ft = fid < 0 ? -1 : hl_field_type(c, t, fid);
        int at = ft < 0 ? -1 : wt_field_offset(c, t, fid);
        if (at < 0 || strcmp(hl_type_name(c, ft), skin_fields[i].type)) {
            wt_log("  %s.%s is not a %s at a known place", skin_fields[i].cls, skin_fields[i].name, skin_fields[i].type);
            return -1;
        }
        out->off[i] = at;
        out->fid[i] = fid;
    }
    int m = hl_find_type(c, HOBJ, "h3d.MatrixImpl");
    for (int k = 0; k < 16; k++) {
        char name[4] = {'_', (char)('1' + k / 4), (char)('1' + k % 4), 0};
        int fid = hl_type_field(c, m, name);
        if (fid != out->fid[M_11] + k || hl_field_type(c, m, fid) != hl_field_type(c, m, out->fid[M_11]) ||
            wt_field_offset(c, m, fid) != out->off[M_11] + 8 * k) {
            wt_log("  h3d.MatrixImpl's _11.._44 are not 16 doubles in a row");
            return -1;
        }
    }
    out->ready = 1;
    return 0;
}

typedef struct {
    int f_type, f_skin, f_sub;
    int tdyn, ttype, tbool, tmat, tclass;
    int jointdata, tmp_field;
} Kernel;

static int kernel_setup(HlCode *c, HlEdit *e, Kernel *k) {
    for (int i = 0; i < (int)(sizeof kernel_calls / sizeof kernel_calls[0]); i++) {
        int fn = hl_find_function(c, kernel_calls[i].name);
        if (fn < 0 || hl_shape(c, fn) != kernel_calls[i].shape) {
            wt_log("subskin: joints in C off, %s %s in this build", kernel_calls[i].name, fn < 0 ? "is not" : "changed");
            return -1;
        }
    }
    if (wt_skin_layout_build(c, &wt_skin_layout)) {
        wt_log("subskin: joints in C off, a field it reads changed");
        return -1;
    }
    k->tdyn = hl_find_type(c, HDYN, NULL);
    k->ttype = hl_find_type(c, HTYPE, NULL);
    k->tbool = hl_find_type(c, HBOOL, NULL);
    k->tmat = hl_find_type(c, HOBJ, "h3d.MatrixImpl");
    k->tclass = hl_find_type(c, HOBJ, "h3d.scene.$Skin");
    k->jointdata = hl_find_type(c, HOBJ, "h3d.scene.JointData");
    k->tmp_field = k->tclass < 0 ? -1 : hl_type_field(c, k->tclass, "TMP_MAT");
    int t1 = wt_fun_type(c, 1, k->tdyn, -1, k->tbool), t2 = wt_fun_type(c, 2, k->tdyn, k->tdyn, k->tbool);
    if (k->ttype < 0 || k->tmp_field < 0 || hl_field_type(c, k->tclass, k->tmp_field) != k->tmat || t1 < 0 || t2 < 0) {
        wt_log("subskin: joints in C off, no Skin.TMP_MAT, type, (dynamic)->bool or (dynamic,dynamic)->bool here");
        wt_skin_layout.ready = 0;
        return -1;
    }
    k->f_type = hl_edit_native(e, "wtfix", "skin_type", t1);
    k->f_skin = hl_edit_native(e, "wtfix", "skin_joints", t2);
    k->f_sub = hl_edit_native(e, "wtfix", "subskin_joints", t2);
    return 0;
}

static int add_reg(HlFun *f, int type) {
    f->regs[f->nregs] = type;
    return f->nregs++;
}

static int field_of(HlCode *c, int type, const char *name) {
    int i = hl_type_field(c, type, name);
    if (i < 0) wt_log("  %s has no field %s", hl_type_name(c, type), name);
    return i;
}

// "rclass = global h3d.scene.Skin; rtmp = rclass.TMP_MAT" at op `at` of the unpatched function.
static int reads_tmp_mat(HlCode *c, const HlFun *f, int at, const Kernel *k) {
    if (!wt_expect(c, f, at, OGetGlobal, NULL) || !wt_expect(c, f, at + 1, OField, NULL) ||
        f->regs[f->ops[at].a[0]] != k->tclass || f->ops[at + 1].a[1] != f->ops[at].a[0] ||
        f->ops[at + 1].a[2] != k->tmp_field) {
        wt_log("  op %d is no longer the read of h3d.scene.Skin.TMP_MAT", at);
        return 0;
    }
    return 1;
}

// Ops 6-9 of all three: "r5 = global hxd.Timer; r4 = r5.frameCount; <jump to COMPUTE>; Ret".
static int entry_as_expected(HlCode *c, const HlFun *f, int tvoid) {
    int ttimer = hl_find_type(c, HOBJ, "hxd.$Timer");
    if (!wt_expect(c, f, 6, OGetGlobal, NULL) || !wt_expect(c, f, 7, OField, NULL) || !wt_expect(c, f, 9, ORet, NULL))
        return 0;
    const HlOp *g = &f->ops[6], *fc = &f->ops[7], *j = &f->ops[8];
    if (ttimer < 0 || f->regs[g->a[0]] != ttimer || fc->a[1] != g->a[0] ||
        fc->a[2] != hl_type_field(c, ttimer, "frameCount") || !hl_is_jump(j->op) || *hl_jump_target((HlOp *)j, 0) != COMPUTE ||
        f->regs[1] != tvoid || f->ops[9].a[0] != 1) {
        wt_log("  ops 6-9 are no longer the frame check before the joints are computed");
        return 0;
    }
    return 1;
}

// Last part first, each at COMPUTE, so a part's jump to COMPUTE + k lands on the parts after it.
static int insert_parts(HlFun *f, HlOp *const *parts, const int *sizes, int nparts) {
    int entering[8], n = 0, total = 0;
    for (int i = 0; i < f->nops; i++) {
        HlOp *o = &f->ops[i];
        if (!hl_is_jump(o->op)) continue;
        int ntargets = o->op == OSwitch ? o->a[1] + 1 : 1;
        for (int k = 0; k < ntargets; k++) {
            if (*hl_jump_target(o, o->op == OSwitch ? k - 1 : 0) != COMPUTE) continue;
            if (i >= COMPUTE || o->op == OSwitch || n == 8) {
                wt_log("  op %d also jumps to op %d", i, COMPUTE);
                return -1;
            }
            entering[n++] = i;
        }
    }
    for (int p = nparts - 1; p >= 0; p--) {
        hl_fun_insert(f, COMPUTE, parts[p], sizes[p]);
        total += sizes[p];
    }
    for (int i = 0; i < n; i++) {
        int *t = hl_jump_target(&f->ops[entering[i]], 0);
        if (*t != COMPUTE + total) return -1;
        *t = COMPUTE;
    }
    return n ? 0 : -1;
}

// At Skin.syncJoints' joint loop: "if (skin_type(JointData) && skin_joints(this, Skin.TMP_MAT)) goto end".
static int insert_skin_call(HlCode *c, HlFun *f, const Kernel *k) {
    if (!wt_expect(c, f, SKIN_LOOP, OInt, NULL) || !wt_expect(c, f, SKIN_END, OBool, NULL) ||
        !wt_expect(c, f, SKIN_END - 1, OJAlways, NULL) || !wt_expect(c, f, 17, OGetGlobal, NULL) ||
        f->regs[f->ops[17].a[0]] != k->tclass || f->ops[SKIN_END].a[1] != 0 || f->ops[SKIN_END - 1].a[0] <= SKIN_LOOP) {
        wt_log("  h3d.scene.Skin.syncJoints: ops 17, %d or %d are not the ones expected", SKIN_LOOP, SKIN_END);
        return -1;
    }
    int entering[4], n = 0;
    for (int i = 0; i < f->nops; i++) {
        HlOp *o = &f->ops[i];
        if (!hl_is_jump(o->op) || o->op == OSwitch || *hl_jump_target(o, 0) != SKIN_LOOP) continue;
        if (i >= SKIN_LOOP || n == 4) {
            wt_log("  op %d also jumps to op %d", i, SKIN_LOOP);
            return -1;
        }
        entering[n++] = i;
    }
    int rtype = add_reg(f, k->ttype), rd1 = add_reg(f, k->tdyn), rd2 = add_reg(f, k->tdyn), rok = add_reg(f, k->tbool);
    int rclass = add_reg(f, k->tclass), rtmp = add_reg(f, k->tmat);
    HlOp block[] = {
        wt_op(OType, 2, rtype, k->jointdata),
        wt_op(OMov, 2, rd1, rtype),
        wt_op(OCall1, 3, rok, k->f_type, rd1),
        wt_op(OJFalse, 2, rok, SKIN_LOOP),
        wt_op(OGetGlobal, 2, rclass, f->ops[17].a[1]),
        wt_op(OField, 3, rtmp, rclass, k->tmp_field),
        wt_op(OMov, 2, rd1, 0),
        wt_op(OMov, 2, rd2, rtmp),
        wt_op(OCall2, 4, rok, k->f_skin, rd1, rd2),
        wt_op(OJFalse, 2, rok, SKIN_LOOP),
        wt_op(OJAlways, 1, SKIN_END),
    };
    int size = (int)(sizeof block / sizeof block[0]);
    hl_fun_insert(f, SKIN_LOOP, block, size);
    for (int i = 0; i < n; i++) {
        int *t = hl_jump_target(&f->ops[entering[i]], 0);
        if (*t != SKIN_LOOP + size) return -1;
        *t = SKIN_LOOP;
    }
    return 0;
}

// Skin and WeightedSubSkin: "if (jointsFrame == hxd.Timer.frameCount) joints_repeat()" as they start computing.
static int patch_counter(HlCode *c, HlEdit *e, int fn, int f_repeat, int tvoid, int ti32, const Kernel *k) {
    HlFun *f = hl_fun_decode(c, fn);
    if (!f || !entry_as_expected(c, f, tvoid) || (k && insert_skin_call(c, f, k))) {
        hl_fun_free(f);
        return -1;
    }
    int f_frame = field_of(c, f->regs[0], "jointsFrame");
    if (f_frame < 0) {
        hl_fun_free(f);
        return -1;
    }
    int rtimer = add_reg(f, f->regs[f->ops[6].a[0]]), rnow = add_reg(f, ti32), rlast = add_reg(f, ti32);
    HlOp block[] = {
        wt_op(OGetGlobal, 2, rtimer, f->ops[6].a[1]),
        wt_op(OField, 3, rnow, rtimer, f->ops[7].a[2]),
        wt_op(OField, 3, rlast, 0, f_frame),
        wt_op(OJNotEq, 3, rlast, rnow, COMPUTE),
        wt_op(OCall0, 2, 1, f_repeat),
    };
    HlOp *parts[] = {block};
    int sizes[] = {5};
    int r = insert_parts(f, parts, sizes, 1) || hl_edit_replace(e, fn, f) ? -1 : 0;
    hl_fun_free(f);
    return r;
}

// gfx.SubSkin, past its base's frame check (r4 holds the frame count): return while this frame's joints still hold.
static int patch_subskin(HlCode *c, HlEdit *e, int fn, int f_repeat, int f_gen, int f_fresh, int tvoid, int ti32,
                         int tbool, const Kernel *k) {
    HlFun *f = hl_fun_decode(c, fn);
    if (!f || !entry_as_expected(c, f, tvoid)) {
        hl_fun_free(f);
        return -1;
    }
    int t = f->regs[0];
    int f_frame = field_of(c, t, "jointsFrame"), f_updated = field_of(c, t, "jointsUpdated");
    int f_retarget = field_of(c, t, "enableRetargeting"), f_prev = field_of(c, t, "prevEnableRetargeting");
    int f_seen = field_of(c, t, "forceJointsUpdateOnFrame");
    if (f_frame < 0 || f_updated < 0 || f_retarget < 0 || f_prev < 0 || f_seen < 0 || f->ops[7].a[0] != 4 ||
        c->types[f->regs[4]].kind != HI32 || hl_field_type(c, t, f_seen) != ti32) {
        if (f) wt_log("  gfx.SubSkin.syncJoints: the frame count is not in r4, or a field changed type");
        hl_fun_free(f);
        return -1;
    }
    if (k && (!reads_tmp_mat(c, f, COMPUTE, k) || !wt_expect(c, f, SUB_END, OBool, NULL) ||
              !wt_expect(c, f, SUB_END - 1, OJAlways, NULL) || f->ops[SUB_END].a[1] != 0)) {
        hl_fun_free(f);
        return -1;
    }
    int rlast = add_reg(f, ti32), rflag = add_reg(f, tbool), rprev = add_reg(f, tbool), rseen = add_reg(f, ti32);
    // check: jumps to COMPUTE land on count, COMPUTE + 1 on remember (count is one op).
    HlOp check[] = {
        wt_op(OField, 3, rlast, 0, f_frame),
        wt_op(OJNotEq, 3, rlast, 4, COMPUTE + 1),
        wt_op(OField, 3, rflag, 0, f_updated),
        wt_op(OJTrue, 2, rflag, COMPUTE),
        wt_op(OField, 3, rflag, 0, f_retarget),
        wt_op(OField, 3, rprev, 0, f_prev),
        wt_op(OJNotEq, 3, rflag, rprev, COMPUTE),
        wt_op(OField, 3, rseen, 0, f_seen),
        wt_op(OCall1, 3, rflag, f_fresh, rseen),
        wt_op(OJFalse, 2, rflag, COMPUTE),
        wt_op(ORet, 1, 1),
    };
    HlOp count[] = {wt_op(OCall0, 2, 1, f_repeat)};
    HlOp remember[] = {
        wt_op(OCall0, 2, rseen, f_gen),
        wt_op(OSetField, 3, 0, f_seen, rseen),
    };
    HlOp native[7];
    if (k) {
        int rd1 = add_reg(f, k->tdyn), rd2 = add_reg(f, k->tdyn), rok = add_reg(f, k->tbool);
        int rclass = add_reg(f, k->tclass), rtmp = add_reg(f, k->tmat);
        native[0] = wt_op(OGetGlobal, 2, rclass, f->ops[COMPUTE].a[1]);
        native[1] = wt_op(OField, 3, rtmp, rclass, k->tmp_field);
        native[2] = wt_op(OMov, 2, rd1, 0);
        native[3] = wt_op(OMov, 2, rd2, rtmp);
        native[4] = wt_op(OCall2, 4, rok, k->f_sub, rd1, rd2);
        native[5] = wt_op(OJFalse, 2, rok, COMPUTE);
        native[6] = wt_op(OJAlways, 1, SUB_END);
    }
    HlOp *parts[] = {check, count, remember, native};
    int sizes[] = {11, 1, 2, 7};
    int r = insert_parts(f, parts, sizes, k ? 4 : 3) || hl_edit_replace(e, fn, f) ? -1 : 0;
    hl_fun_free(f);
    return r;
}

static int apply(HlCode *c, HlEdit *e, const int *fn) {
    if (!wt_signature(c, fn[0], "(gfx.SubSkin)->void") || !wt_signature(c, fn[1], "(h3d.scene.Skin)->void") ||
        !wt_signature(c, fn[2], "(gfx.WeightedSubSkin)->void"))
        return -1;
    int tvoid = hl_find_type(c, HVOID, NULL), ti32 = hl_find_type(c, HI32, NULL), tbool = hl_find_type(c, HBOOL, NULL);
    int t_repeat = wt_fun_type(c, 0, -1, -1, tvoid), t_gen = wt_fun_type(c, 0, -1, -1, ti32);
    int t_fresh = wt_fun_type(c, 1, ti32, -1, tbool);
    if (t_repeat < 0 || t_gen < 0 || t_fresh < 0) {
        wt_log("  no ()->void, ()->i32 or (i32)->bool type for the natives");
        return -1;
    }
    int f_repeat = hl_edit_native(e, "wtfix", "joints_repeat", t_repeat);
    int f_gen = hl_edit_native(e, "wtfix", "joints_gen", t_gen);
    int f_fresh = hl_edit_native(e, "wtfix", "joints_fresh", t_fresh);
    Kernel kernel;
    const Kernel *k = kernel_setup(c, e, &kernel) ? NULL : &kernel;
    if (patch_subskin(c, e, fn[0], f_repeat, f_gen, f_fresh, tvoid, ti32, tbool, k) ||
        patch_counter(c, e, fn[1], f_repeat, tvoid, ti32, k) || patch_counter(c, e, fn[2], f_repeat, tvoid, ti32, NULL)) {
        wt_skin_layout.ready = 0;
        return -1;
    }
    return 0;
}

const WtFix fix_subskin = {"subskin",
                           "unit skins: joints computed in C (live, [perf] skin_joints_c), and armour and clothes "
                           "(gfx.SubSkin) once a frame unless something they follow changed (live, [perf] "
                           "subskin_once)",
                           targets, 3, apply};
