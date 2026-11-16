// Loads raise the GC trigger ([gc] load_threshold) for fewer full collections; lib.GC.gc in gameplayStart cleans up.
// Each body moves to an added function: gc_load(true); try body catch(e) { gc_load(false); rethrow e }; gc_load(false).
#include "../fix.h"

static const WtTarget targets[] = {
    {"Game.start", 0},
    {"Game.switchToMode", 0},
};

static int wrap(HlCode *c, HlEdit *e, int function, int gc_load) {
    const HlType *t = &c->types[c->functions[function].type];
    int tvoid = hl_find_type(c, HVOID, NULL), tbool = hl_find_type(c, HBOOL, NULL), tdyn = hl_find_type(c, HDYN, NULL);
    if (t->kind != HFUN || t->nargs > 8 || tvoid < 0 || tbool < 0 || tdyn < 0) return -1;
    HlFun *body = hl_fun_decode(c, function);
    if (!body) return -1;
    int file = body->files[0], line = body->lines[0];
    int copy = hl_edit_add(e, body);
    hl_fun_free(body);
    if (copy < 0) return -1;

    int regs[16], n = t->nargs;
    for (int i = 0; i < n; i++) regs[i] = t->args[i];
    int rr = n, rb = n + 1, rv = n + 2, re = n + 3;
    regs[rr] = t->ret, regs[rb] = tbool, regs[rv] = tvoid, regs[re] = tdyn;

    HlOp call = wt_op(n <= 4 ? OCall0 + n : OCallN, 0);
    call.a[0] = rr;
    call.a[1] = copy;
    if (n <= 4) {
        for (int i = 0; i < n; i++) call.a[2 + i] = i;
        call.n = 2 + n;
    } else {
        call.a[2] = n;
        for (int i = 0; i < n; i++) call.a[3 + i] = i;
        call.n = 3 + n;
    }
    HlOp ops[] = {
        wt_op(OBool, 2, rb, 1),
        wt_op(OCall1, 3, rv, gc_load, rb),
        wt_op(OTrap, 2, re, 8),
        call,
        wt_op(OEndTrap, 1, 1),
        wt_op(OBool, 2, rb, 0),
        wt_op(OCall1, 3, rv, gc_load, rb),
        wt_op(ORet, 1, rr),
        wt_op(OBool, 2, rb, 0), // 8: an exception left the body
        wt_op(OCall1, 3, rv, gc_load, rb),
        wt_op(ORethrow, 1, re),
    };
    HlFun *w = hl_fun_new(c->functions[function].type, n + 4, regs, ops, sizeof ops / sizeof ops[0], file, line);
    int r = w ? hl_edit_replace(e, function, w) : -1;
    hl_fun_free(w);
    return r;
}

static int apply(HlCode *c, HlEdit *e, const int *fn) {
    if (!wt_native_ready || !wt_native_ready("gc")) {
        wt_log("  the GC trigger in libhl.dll was not found (see the gc: line above)");
        return -1;
    }
    int tbool = hl_find_type(c, HBOOL, NULL), tvoid = hl_find_type(c, HVOID, NULL), type = -1;
    for (int i = 0; i < c->ntypes && type < 0; i++) {
        const HlType *t = &c->types[i];
        if (t->kind == HFUN && t->nargs == 1 && t->args[0] == tbool && t->ret == tvoid) type = i;
    }
    if (type < 0) return -1;
    int gc_load = hl_edit_native(e, "wtfix", "gc_load", type);
    for (int i = 0; i < 2; i++)
        if (wrap(c, e, fn[i], gc_load)) return -1;
    return 0;
}

const WtFix fix_gcload = {"gcload", "fewer GC collections while loading", targets, 2, apply};
