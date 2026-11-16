// Spline.copy is this.load(other.save()); load takes a point object as is, so during copy the base save hands over
// the points themselves, not dynamic copies. other.save() stays virtual, so subclasses that save more still do.
#include "../fix.h"

static const WtTarget targets[] = {
    {"hrt.prefab.l3d.Spline.copy", 0x0b682ec428800f2cull},
    {"hrt.prefab.l3d.Spline.save", 0xae83520d523d0c5dull},
    {"hrt.prefab.l3d.Spline.load", 0x6443e1710f5b7f78ull},                // gives each point to SplinePoint.load
    {"hrt.prefab.l3d.SplinePoint.load", 0xd670812ffcb2b7f7ull},           // copies a SplinePoint it is given
    {"hrt.prefab.l3d.$SplinePoint.__constructor__", 0xa9dce3483dac142dull}, // allocates the vectors load copies into
};

static int fun_type(HlCode *c, int nargs, int arg, int ret) {
    for (int i = 0; i < c->ntypes; i++) {
        const HlType *t = &c->types[i];
        if (t->kind == HFUN && t->nargs == nargs && t->ret == ret && (!nargs || t->args[0] == arg)) return i;
    }
    return -1;
}

// Before the save of a point at `at` (r1 = SplinePoint.save(r12)): if copying, r1 = the point itself.
static void point_or_save(HlFun *f, int at, int copying, int flag) {
    HlOp ops[] = {
        wt_op(OCall0, 2, flag, copying),
        wt_op(OJFalse, 2, flag, at),     // not copying: the save as built
        wt_op(OUnsafeCast, 2, 1, 12),
        wt_op(OJAlways, 1, at + 1),      // past the save
    };
    hl_fun_insert(f, at, ops, 4);
}

static int apply(HlCode *c, HlEdit *e, const int *fn) {
    int tbool = hl_find_type(c, HBOOL, NULL), tvoid = hl_find_type(c, HVOID, NULL), tdyn = hl_find_type(c, HDYN, NULL);
    int set_type = fun_type(c, 1, tbool, tvoid), ask_type = fun_type(c, 0, -1, tbool);
    HlFun *copy = hl_fun_decode(c, fn[0]), *save = hl_fun_decode(c, fn[1]);
    int ok = copy && save && set_type >= 0 && ask_type >= 0 && tdyn >= 0 && copy->nops == 6 &&
             wt_expect(c, copy, 0, OCall2, "hrt.prefab.Prefab.copy") && wt_expect(c, copy, 3, OCallMethod, NULL) &&
             copy->ops[3].a[3] == 3 && copy->ops[3].a[0] == 4 && wt_expect(c, copy, 4, OCallThis, NULL) &&
             wt_expect(c, copy, 5, ORet, NULL) && c->types[copy->regs[2]].kind == HVOID &&
             wt_expect(c, save, 26, OCall1, "hrt.prefab.l3d.SplinePoint.save") &&
             wt_expect(c, save, 59, OCall1, "hrt.prefab.l3d.SplinePoint.save") && save->ops[26].a[0] == 1 &&
             save->ops[26].a[2] == 12 && save->ops[59].a[0] == 1 && save->ops[59].a[2] == 12 &&
             c->types[save->regs[1]].kind == HDYN;
    if (!ok) {
        hl_fun_free(copy), hl_fun_free(save);
        return -1;
    }
    int mode = hl_edit_native(e, "wtfix", "spline_copy_mode", set_type);
    int copying = hl_edit_native(e, "wtfix", "spline_copying", ask_type);

    int flag = save->nregs;
    save->regs[save->nregs++] = tbool;
    point_or_save(save, 59, copying, flag); // the samples first, so op 26 is still where it was
    point_or_save(save, 26, copying, flag);

    // copy: Prefab.copy; cast; null check; mode(true); try { d = other.save() } catch { mode(false); rethrow };
    // mode(false); this.load(d); return.
    int rb = copy->nregs, re = copy->nregs + 1;
    copy->regs[copy->nregs++] = tbool;
    copy->regs[copy->nregs++] = tdyn;
    HlOp after[] = {wt_op(OEndTrap, 1, 1), wt_op(OBool, 2, rb, 0), wt_op(OCall1, 3, 2, mode, rb)};
    hl_fun_insert(copy, 4, after, 3);
    int handler = copy->nops;
    HlOp failed[] = {wt_op(OBool, 2, rb, 0), wt_op(OCall1, 3, 2, mode, rb), wt_op(ORethrow, 1, re)};
    hl_fun_insert(copy, handler, failed, 3);
    HlOp before[] = {wt_op(OBool, 2, rb, 1), wt_op(OCall1, 3, 2, mode, rb), wt_op(OTrap, 2, re, handler)};
    hl_fun_insert(copy, 3, before, 3);

    int r = hl_edit_replace(e, fn[1], save) || hl_edit_replace(e, fn[0], copy) ? -1 : 0;
    hl_fun_free(copy), hl_fun_free(save);
    return r;
}

const WtFix fix_splinecopy = {"splinecopy", "spline copies skip the save/load round trip", targets, 5, apply};
