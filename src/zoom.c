#include "zoom.h"
#include <stdlib.h>

#define ZOOM_MAX_OPS 64

void zoom_begin(Zoom *z, HlCode *c, HlEdit *e, HlFun *f) {
    *z = (Zoom){c, e, f, (HlOp *)calloc(ZOOM_MAX_OPS, sizeof(HlOp)), 0, 0, hl_find_type(c, HF64, NULL)};
    if (!f || z->tf64 < 0) z->bad = 1;
}

int zoom_reg(Zoom *z, int type) {
    if (type < 0 || z->bad || z->f->nregs >= (int)(sizeof z->f->regs / sizeof z->f->regs[0])) {
        z->bad = 1;
        return 0;
    }
    z->f->regs[z->f->nregs] = type;
    return z->f->nregs++;
}

void zoom_op(Zoom *z, HlOp op) {
    if (z->n < ZOOM_MAX_OPS) z->ops[z->n++] = op;
    else z->bad = 1;
}

int zoom_float(Zoom *z, double value) {
    int r = zoom_reg(z, z->tf64);
    zoom_op(z, wt_op(OFloat, 2, r, hl_edit_float(z->e, value)));
    return r;
}

static int field(Zoom *z, int type, const char *name, int *ftype) {
    int fid = type >= 0 ? hl_type_field(z->c, type, name) : -1;
    *ftype = fid >= 0 ? hl_field_type(z->c, type, fid) : -1;
    if (fid < 0 && !z->bad) wt_log("  %s has no field %s", type >= 0 ? hl_type_name(z->c, type) : "?", name);
    if (fid < 0) z->bad = 1;
    return fid;
}

static int load(Zoom *z, int robj, const char *name) {
    int ftype, fid = field(z, z->bad ? -1 : z->f->regs[robj], name, &ftype), r = zoom_reg(z, ftype);
    zoom_op(z, wt_op(OField, 3, r, robj, fid));
    return r;
}

int zoom_object(Zoom *z, int robj, const char *name, int skip) {
    int r = load(z, robj, name);
    zoom_op(z, wt_op(OJNull, 2, r, skip));
    return r;
}

void zoom_live(Zoom *z, int live_id, int skip) {
    int ti32 = hl_find_type(z->c, HI32, NULL), type = wt_fun_type(z->c, 1, ti32, -1, z->tf64);
    int rid = zoom_reg(z, ti32), rflag = zoom_reg(z, z->tf64);
    int number = type >= 0 ? hl_edit_native(z->e, "wtfix", "live_number", type) : -1;
    zoom_op(z, wt_op(OInt, 2, rid, hl_edit_int(z->e, live_id)));
    zoom_op(z, wt_op(OCall1, 3, rflag, number, rid));
    zoom_op(z, wt_op(OJSLt, 3, rflag, zoom_float(z, 0.5), skip));
}

void zoom_battle_mode(Zoom *z, int skip) {
    int tgame = hl_find_type(z->c, HOBJ, "Game");
    if (tgame < 0 || z->c->types[tgame].global <= 0) {
        wt_log("  the Game class is missing");
        z->bad = 1;
        return;
    }
    int global = z->c->types[tgame].global - 1, rclass = zoom_reg(z, z->c->globals[global]);
    zoom_op(z, wt_op(OGetGlobal, 2, rclass, global));
    int rinst = zoom_object(z, rclass, "inst", skip);
    int rbattle = zoom_object(z, rinst, "battle", skip), rmode = zoom_object(z, rinst, "mode", skip);
    zoom_op(z, wt_op(OJNotEq, 3, rmode, rbattle, skip));
}

int zoom_distance(Zoom *z, int rcam, int skip) {
    int sqrt_native = hl_find_native(z->c, "std", "math_sqrt");
    const HlType *t = sqrt_native >= 0 ? &z->c->types[z->c->natives[sqrt_native].type] : NULL;
    if (!t || t->nargs != 1 || t->args[0] != z->tf64 || t->ret != z->tf64) {
        wt_log("  std@math_sqrt is missing or not (f64)->f64");
        z->bad = 1;
        return 0;
    }
    int rpos = zoom_object(z, rcam, "pos", skip), rtarget = zoom_object(z, rcam, "target", skip);
    if (!z->bad && z->f->regs[rpos] != z->f->regs[rtarget]) {
        wt_log("  the camera's pos and target are not the same type");
        z->bad = 1;
    }
    int rsum = zoom_reg(z, z->tf64), rd = zoom_reg(z, z->tf64), rdist = zoom_reg(z, z->tf64);
    static const char *const axes[] = {"x", "y", "z"};
    for (int i = 0; i < 3; i++) {
        int ra = load(z, rpos, axes[i]), rb = load(z, rtarget, axes[i]);
        if (!z->bad && (z->f->regs[ra] != z->tf64 || z->f->regs[rb] != z->tf64)) {
            wt_log("  the camera's %s is not an f64", axes[i]);
            z->bad = 1;
        }
        zoom_op(z, wt_op(OSub, 3, rd, ra, rb));
        zoom_op(z, wt_op(OMul, 3, i ? rd : rsum, rd, rd));
        if (i) zoom_op(z, wt_op(OAdd, 3, rsum, rsum, rd));
    }
    zoom_op(z, wt_op(OCall1, 3, rdist, z->c->natives[sqrt_native].findex, rsum));
    return rdist;
}

int zoom_insert(Zoom *z, int at) {
    int ok = !z->bad;
    if (ok) hl_fun_insert(z->f, at, z->ops, z->n);
    free(z->ops);
    z->ops = NULL;
    return ok ? 0 : -1;
}
