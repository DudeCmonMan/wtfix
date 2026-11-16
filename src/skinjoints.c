// The joint loops of Skin.syncJoints (with JointData.sync) and SubSkin.syncJoints in C, bit-identical to the game's.
// A skin is planned first: anything the game would throw on, allocate or dispatch answers false before any write.
// Joints and operations keep the game's order, each step reads its inputs before writing (matrices can be shared),
// and no FMA (-ffp-contract=off). Main thread only; never calls HL code or allocates.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fix.h"
#include "live.h"
#include "objlayout.h"
#include "skinjoints.h"

#define HL_PRIM(name) \
    __declspec(dllexport) void *hlp_##name(const char **sign) { *sign = NULL; return (void *)name; }

extern void *hl_get_obj_rt(void *type);

#define MAX_JOINTS 512
#define HL_KIND_OBJ 11

enum { STEP_SKIP, STEP_OWN, STEP_FOLLOW };
enum { PAL_NONE, PAL_TIMES_ABS, PAL_COPY };

typedef struct {
    unsigned char kind, retarget, palette;
    double *abs, *rel, *def, *parent, *add, *pal, *trans, *from, *from_pal, *target, *origin;
} Step;

static Step steps[MAX_JOINTS];
static void *jointdata_type;
static bool checked[WT_SKIN_ROLES], broken;
static long long in_c, left;
static unsigned reasons_logged;

enum { R_NULL, R_ALLOC, R_DYNAMIC, R_MANY, R_EMPTY, R_LAYOUT, R_REASONS };
static const char *const reason_text[R_REASONS] = {
    "a null the game's code would throw on", "a matrix the game allocates on first use",
    "dynamic joints (not a plain h3d.scene.JointData)", "over 512 joints", "no joints",
    "the field offsets disagree with libhl",
};

static bool give_up(int reason) {
    left++;
    if (!(reasons_logged & 1u << reason)) {
        reasons_logged |= 1u << reason;
        wt_log("skinjoints: a skin left to the game's loop: %s", reason_text[reason]);
    }
    return false;
}

static inline void *ptr(const void *o, int f) {
    return *(void *const *)((const char *)o + wt_skin_layout.off[f]);
}
static inline int i32(const void *o, int f) { return *(const int *)((const char *)o + wt_skin_layout.off[f]); }
static inline bool flag(const void *o, int f) {
    return *(const unsigned char *)((const char *)o + wt_skin_layout.off[f]) != 0;
}
static inline double *mat(void *m) { return m ? (double *)((char *)m + wt_skin_layout.off[M_11]) : NULL; }
static inline double *mat_of(const void *o, int f) { return mat(ptr(o, f)); }

// arr[i] as the bytecode reads an ArrayObj: null at or past its length (compared unsigned).
static void *item(const void *arr, int i) {
    if ((unsigned)i >= (unsigned)i32(arr, AR_LENGTH)) return NULL;
    void *data = ptr(arr, AR_ARRAY);
    return data ? ((void **)((char *)data + WT_VARRAY_DATA))[i] : NULL;
}

// The offsets against libhl's for the object's class, once per role; a mismatch turns the C off for the session.
static int rt_check(int role, const void *type) {
    if (!type || *(const int *)type != HL_KIND_OBJ) return -1;
    const char *rt = (const char *)hl_get_obj_rt((void *)type);
    if (!rt || *(void *const *)rt != type) return -1;
    int nfields = *(const int *)(rt + 8);
    const int *index = *(const int *const *)(rt + 0x28);
    for (int f = 0; f < WT_SKIN_FIELDS; f++) {
        if (wt_skin_role[f] != role) continue;
        int count = f == M_11 ? 16 : 1;
        for (int k = 0; k < count; k++) {
            int fid = wt_skin_layout.fid[f] + k;
            if (fid >= nfields || index[fid] != wt_skin_layout.off[f] + 8 * k) return -1;
        }
    }
    return 0;
}

static bool known(int role, const void *obj) {
    if (checked[role]) return true;
    if (broken || rt_check(role, *(void *const *)obj)) {
        broken = true;
        return false;
    }
    return checked[role] = true;
}

static bool usable(void) {
    return wt_live[LIVE_SKIN_C].value >= 0.5 && wt_skin_layout.ready && !broken;
}

#define NEED(cond, reason) \
    do {                   \
        if (!(cond)) return give_up(reason); \
    } while (0)
#define KNOWN(role, obj) NEED(known(role, obj), R_LAYOUT)

// The joint's own pose and its parent's absolute pose, found as JointData.sync and SubSkin.syncJoints do.
static bool plan_own(Step *s, void *skin, void *jdata, void *j, void *jd, double *tmp, bool retarget_on) {
    void *rel = ptr(jd, JD_REL);
    s->retarget = 0;
    if (!rel) {
        rel = ptr(j, J_DEFMAT);
    } else if (flag(j, J_RETARGET) && retarget_on) {
        s->def = mat_of(j, J_DEFMAT);
        NEED(tmp && s->def, R_NULL);
        s->retarget = 1;
    }
    NEED(rel, R_NULL);
    s->rel = mat(rel);
    void *parent = ptr(j, J_PARENT);
    if (parent) {
        KNOWN(ROLE_JOINT, parent);
        void *pjd = item(jdata, i32(parent, J_INDEX));
        NEED(pjd, R_NULL);
        s->parent = mat_of(pjd, JD_ABS);
    } else {
        s->parent = mat_of(skin, SK_ABSPOS);
    }
    NEED(s->parent, R_NULL);
    return true;
}

static bool plan_palette(Step *s, void *palette, void *j, int bind) {
    NEED(palette, R_NULL);
    KNOWN(ROLE_ARRAY, palette);
    s->pal = mat(item(palette, bind));
    s->trans = mat_of(j, J_TRANSPOS);
    NEED(s->pal && s->trans, R_NULL);
    s->palette = PAL_TIMES_ABS;
    return true;
}

static bool plan_joints(void *skin, void **joints, int *n) {
    void *anim = ptr(skin, SK_SKINDATA);
    NEED(anim, R_NULL);
    KNOWN(ROLE_ANIM, anim);
    *joints = ptr(anim, AS_ALLJOINTS);
    NEED(*joints, R_NULL);
    KNOWN(ROLE_ARRAY, *joints);
    *n = i32(*joints, AR_LENGTH);
    NEED(*n <= MAX_JOINTS, R_MANY);
    NEED(*n > 0, R_EMPTY); // the game's loop does nothing either
    void *jdata = ptr(skin, SK_JOINTSDATA);
    NEED(jdata, R_NULL);
    KNOWN(ROLE_ARRAY, jdata);
    return true;
}

// Skin.syncJoints' loop calls JointData.sync virtually: only plain JointData are done here.
static bool plan_skin(void *skin, double *tmp, int *count) {
    void *joints, *jdata = NULL;
    int n;
    KNOWN(ROLE_SKIN, skin);
    if (!plan_joints(skin, &joints, &n)) return false;
    jdata = ptr(skin, SK_JOINTSDATA);
    void *palette = ptr(skin, SK_PALETTE);
    bool retarget_on = flag(skin, SK_RETARGET);
    for (int i = 0; i < n; i++) {
        Step *s = &steps[i];
        void *j = item(joints, i);
        NEED(j, R_NULL);
        KNOWN(ROLE_JOINT, j);
        void *jd = item(jdata, i32(j, J_INDEX));
        NEED(jd, R_NULL);
        NEED(*(void **)jd == jointdata_type, R_DYNAMIC);
        KNOWN(ROLE_JOINTDATA, jd);
        if (ptr(j, J_FOLLOW)) {
            s->kind = STEP_SKIP;
            continue;
        }
        s->kind = STEP_OWN;
        s->abs = mat_of(jd, JD_ABS);
        if (s->abs) KNOWN(ROLE_MATRIX, ptr(jd, JD_ABS));
        if (!plan_own(s, skin, jdata, j, jd, tmp, retarget_on)) return false;
        NEED(s->abs, R_NULL);
        s->add = mat_of(jd, JD_ADD);
        s->palette = PAL_NONE;
        int bind = i32(j, J_BIND);
        if (bind >= 0 && !plan_palette(s, palette, j, bind)) return false;
        s->target = mat_of(jd, JD_TARGET);
        s->origin = mat_of(jd, JD_ORIGIN);
        NEED(s->target && s->origin, R_ALLOC);
    }
    *count = n;
    return true;
}

static bool plan_subskin(void *sub, double *tmp, int *count) {
    void *joints, *jdata;
    int n;
    KNOWN(ROLE_SKIN, sub);
    KNOWN(ROLE_SUBSKIN, sub);
    void *base = ptr(sub, SK_BASE);
    NEED(base, R_NULL);
    KNOWN(ROLE_SKIN, base);
    if (!plan_joints(sub, &joints, &n)) return false;
    jdata = ptr(sub, SK_JOINTSDATA);
    void *palette = ptr(sub, SK_PALETTE), *base_data = ptr(base, SK_JOINTSDATA), *base_palette = ptr(base, SK_PALETTE);
    bool retarget_on = flag(sub, SK_RETARGET);
    for (int i = 0; i < n; i++) {
        Step *s = &steps[i];
        void *j = item(joints, i);
        NEED(j, R_NULL);
        KNOWN(ROLE_JOINT, j);
        int bind = i32(j, J_BIND);
        void *jd = item(jdata, i32(j, J_INDEX));
        NEED(jd, R_NULL);
        KNOWN(ROLE_JOINTDATA, jd);
        s->abs = mat_of(jd, JD_ABS);
        NEED(s->abs, R_ALLOC);
        KNOWN(ROLE_MATRIX, ptr(jd, JD_ABS));
        s->palette = PAL_NONE;
        void *follow = ptr(j, J_FOLLOW);
        if (follow) {
            s->kind = STEP_FOLLOW;
            KNOWN(ROLE_JOINT, follow);
            NEED(base_data, R_NULL);
            KNOWN(ROLE_ARRAY, base_data);
            void *bjd = item(base_data, i32(follow, J_INDEX));
            NEED(bjd, R_NULL);
            s->from = mat_of(bjd, JD_ABS);
            if (bind < 0) continue;
            NEED(palette, R_NULL);
            KNOWN(ROLE_ARRAY, palette);
            s->pal = mat(item(palette, bind));
            NEED(s->pal, R_ALLOC);
            s->trans = mat_of(j, J_TRANSPOS);
            if (s->trans) {
                s->palette = PAL_TIMES_ABS;
            } else {
                NEED(base_palette, R_NULL);
                KNOWN(ROLE_ARRAY, base_palette);
                s->from_pal = mat(item(base_palette, i32(follow, J_BIND)));
                NEED(s->from_pal, R_NULL);
                s->palette = PAL_COPY;
            }
            continue;
        }
        s->kind = STEP_OWN;
        if (!plan_own(s, sub, jdata, j, jd, tmp, retarget_on)) return false;
        if (bind >= 0 && !plan_palette(s, palette, j, bind)) return false;
    }
    *count = n;
    return true;
}

static inline void copy16(double *out, const double *m) {
    double t[16];
    for (int k = 0; k < 16; k++) t[k] = m[k];
    for (int k = 0; k < 16; k++) out[k] = t[k];
}

// As h3d.MatrixImpl.multiply3x4inline, in its order for identical rounding: ((a1 * b1 + a2 * b2) + a3 * b3) [+ b4].
static inline void mul34(double *out, const double *a, const double *b) {
    double a11 = a[0], a12 = a[1], a13 = a[2], a21 = a[4], a22 = a[5], a23 = a[6];
    double a31 = a[8], a32 = a[9], a33 = a[10], a41 = a[12], a42 = a[13], a43 = a[14];
    double b11 = b[0], b12 = b[1], b13 = b[2], b21 = b[4], b22 = b[5], b23 = b[6];
    double b31 = b[8], b32 = b[9], b33 = b[10], b41 = b[12], b42 = b[13], b43 = b[14];
    out[0] = a11 * b11 + a12 * b21 + a13 * b31;
    out[1] = a11 * b12 + a12 * b22 + a13 * b32;
    out[2] = a11 * b13 + a12 * b23 + a13 * b33;
    out[3] = 0.0;
    out[4] = a21 * b11 + a22 * b21 + a23 * b31;
    out[5] = a21 * b12 + a22 * b22 + a23 * b32;
    out[6] = a21 * b13 + a22 * b23 + a23 * b33;
    out[7] = 0.0;
    out[8] = a31 * b11 + a32 * b21 + a33 * b31;
    out[9] = a31 * b12 + a32 * b22 + a33 * b32;
    out[10] = a31 * b13 + a32 * b23 + a33 * b33;
    out[11] = 0.0;
    out[12] = a41 * b11 + a42 * b21 + a43 * b31 + b41;
    out[13] = a41 * b12 + a42 * b22 + a43 * b32 + b42;
    out[14] = a41 * b13 + a42 * b23 + a43 * b33 + b43;
    out[15] = 1.0;
}

// As h3d.MatrixImpl.multiply: (((a1 * b1 + a2 * b2) + a3 * b3) + a4 * b4).
static inline void mul44(double *out, const double *a, const double *b) {
    double x[16], y[16];
    for (int k = 0; k < 16; k++) x[k] = a[k];
    for (int k = 0; k < 16; k++) y[k] = b[k];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            out[4 * r + c] = x[4 * r] * y[c] + x[4 * r + 1] * y[4 + c] + x[4 * r + 2] * y[8 + c] + x[4 * r + 3] * y[12 + c];
}

// Retargeted joints: TMP_MAT.load(rel) with the default pose's translation, as the game does.
static inline const double *own_pose(const Step *s, double *tmp) {
    if (!s->retarget) return s->rel;
    copy16(tmp, s->rel);
    tmp[12] = s->def[12];
    tmp[13] = s->def[13];
    tmp[14] = s->def[14];
    return tmp;
}

static void compute_skin(int n, double *tmp) {
    for (int i = 0; i < n; i++) {
        const Step *s = &steps[i];
        if (s->kind == STEP_SKIP) continue;
        mul34(s->abs, own_pose(s, tmp), s->parent);
        if (s->add) mul34(s->abs, s->add, s->abs);
        if (s->palette) mul34(s->pal, s->trans, s->abs);
        copy16(s->target, s->abs);
        copy16(s->origin, s->abs);
    }
}

static void compute_subskin(int n, double *tmp) {
    for (int i = 0; i < n; i++) {
        const Step *s = &steps[i];
        if (s->kind == STEP_FOLLOW) {
            if (s->from) copy16(s->abs, s->from);
            if (s->palette == PAL_TIMES_ABS) mul44(s->pal, s->trans, s->abs);
            else if (s->palette == PAL_COPY) copy16(s->pal, s->from_pal);
            continue;
        }
        mul34(s->abs, own_pose(s, tmp), s->parent);
        if (s->palette) mul34(s->pal, s->trans, s->abs);
    }
}

static bool done(void) {
    if (++in_c == 100000) wt_log("skinjoints: the first 100000 skins computed in C, %lld left to the game", left);
    return true;
}

// The patched Skin.syncJoints calls this first with h3d.scene.JointData's type.
static bool skin_type(void *type) {
    jointdata_type = type;
    return usable();
}

static bool skin_joints(void *skin, void *tmp_mat) {
    int n;
    if (!usable()) return false;
    if (tmp_mat && !known(ROLE_MATRIX, tmp_mat)) return give_up(R_LAYOUT);
    if (!plan_skin(skin, mat(tmp_mat), &n)) return false;
    compute_skin(n, mat(tmp_mat));
    return done();
}

static bool subskin_joints(void *sub, void *tmp_mat) {
    int n;
    if (!usable()) return false;
    if (tmp_mat && !known(ROLE_MATRIX, tmp_mat)) return give_up(R_LAYOUT);
    if (!plan_subskin(sub, mat(tmp_mat), &n)) return false;
    compute_subskin(n, mat(tmp_mat));
    return done();
}

HL_PRIM(skin_type)
HL_PRIM(skin_joints)
HL_PRIM(subskin_joints)

int wt_skin_stats(char *out, int size) {
    return snprintf(out, size, "%lld skins computed in C, %lld left to the game%s", in_c, left,
                    broken ? " (off: field offsets)" : "");
}
