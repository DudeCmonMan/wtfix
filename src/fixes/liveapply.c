// Panel changes reach a battle within a frame: at Battle.updateCamera's end the zoom constants are written into the
// game's data (it reads them each time) and the HUDs already on screen are rescaled.
#include "../asm.h"
#include "../live.h"

static const WtTarget targets[] = {
    {"battle.Battle.updateCamera", 0}, // its returns are found
    {"$Const.getConst", 0},            // its lookup ops are checked and reused
    {"h2d.Object.setScale", 0},
};

enum { RB, RDT, RVOID, RK, RT, RDATA, RIDX, RMAP, RID, RNAME, RDYN, RROW, RV, RNULL, RSTATE, RARR, RI, RLEN, RRAW, RDYN2,
       RUNIT, RHUD, RFLAG, RERR, NREGS };
enum { L_HUDS, L_NEXT0, L_NEXT1, L_LOOP, L_BARS, L_SCALE, L_RET, L_CATCH };

static int lookup_ok(HlCode *c, const HlFun *g) {
    if (!(wt_expect(c, g, 0, OGetGlobal, NULL) && wt_expect(c, g, 1, OField, NULL) && wt_expect(c, g, 3, OField, NULL) &&
          wt_expect(c, g, 5, OCall2, "haxe.ds.StringMap.get") && wt_expect(c, g, 6, OToVirtual, NULL)))
        return 0;
    const HlOp *o = g->ops;
    return o[1].a[1] == o[0].a[0] && o[3].a[1] == o[1].a[0] && o[5].a[2] == o[3].a[0] && o[6].a[1] == o[5].a[0] &&
           hl_type_field(c, g->regs[o[0].a[0]], "constant") == o[1].a[2] &&
           hl_type_field(c, g->regs[o[1].a[0]], "byId") == o[3].a[2];
}

static int field(HlCode *c, int type, const char *name, int *ftype) {
    *ftype = -1;
    if (type < 0) return -1;
    int fid = hl_type_field(c, type, name);
    *ftype = hl_field_type(c, type, fid);
    if (fid < 0) wt_log("  %s has no field %s", hl_type_name(c, type), name);
    return fid;
}

static int apply(HlCode *c, HlEdit *e, const int *fn) {
    if (!wt_signature(c, fn[0], "(battle.Battle,f64)->void") || !wt_signature(c, fn[2], "(h2d.Object,f64)->void"))
        return -1;
    HlFun *cam = hl_fun_decode(c, fn[0]), *get = hl_fun_decode(c, fn[1]);
    int ok = cam && get && lookup_ok(c, get);
    int tbattle = ok ? cam->regs[0] : -1, tstate, tarrobj, tarr, tunit, thud, tx, tvalue;
    int trow = ok ? get->regs[get->ops[6].a[0]] : -1;
    int f_state = field(c, tbattle, "state", &tstate), f_units = field(c, tstate, "units", &tarrobj);
    int f_len = field(c, tarrobj, "length", &tx), f_arr = field(c, tarrobj, "array", &tarr);
    int f_hud = -1, f_closed = -1, f_minimal = -1, f_value = field(c, trow, "value", &tvalue);
    tunit = hl_find_type(c, HOBJ, "battle.Unit");
    f_hud = field(c, tunit, "unitHud", &thud), f_closed = field(c, thud, "closed", &tx);
    f_minimal = field(c, thud, "minimal", &tx);
    int names[2] = {wt_string_global(c, wt_live_const_names[0]), wt_string_global(c, wt_live_const_names[1])};
    int tvoid = hl_find_type(c, HVOID, NULL), tbool = hl_find_type(c, HBOOL, NULL), ti32 = hl_find_type(c, HI32, NULL);
    int tf64 = hl_find_type(c, HF64, NULL), tdyn = hl_find_type(c, HDYN, NULL);
    int t_number = wt_fun_type(c, 1, ti32, -1, tf64), t_flag = wt_fun_type(c, 1, ti32, -1, tbool);
    int t_error = wt_fun_type(c, 1, tdyn, -1, tvoid), t_apply = wt_fun_type(c, 1, tbattle, -1, tvoid);
    ok = ok && f_state >= 0 && f_units >= 0 && f_len >= 0 && f_arr >= 0 && f_hud >= 0 && f_closed >= 0 &&
         f_minimal >= 0 && f_value >= 0 && c->types[tvalue].kind == HNULL && names[0] >= 0 && names[1] >= 0 &&
         t_number >= 0 && t_flag >= 0 && t_error >= 0 && t_apply >= 0;
    if (!ok) {
        hl_fun_free(cam), hl_fun_free(get);
        return -1;
    }
    int f_number = hl_edit_native(e, "wtfix", "live_number", t_number);
    int f_known = hl_edit_native(e, "wtfix", "live_known", t_flag);
    int f_take = hl_edit_native(e, "wtfix", "live_take", t_flag);
    int f_error = hl_edit_native(e, "wtfix", "live_error", t_error);
    int f_scale = wt_findex(c, fn[2]), map_get = get->ops[5].a[1];
    const int regs[NREGS] = {tbattle, tf64, tvoid, ti32, tbool, get->regs[get->ops[0].a[0]], get->regs[get->ops[1].a[0]],
                             get->regs[get->ops[3].a[0]], ti32, get->regs[0], get->regs[get->ops[5].a[0]], trow, tf64,
                             tvalue, tstate, tarrobj, ti32, ti32, tarr, tdyn, tunit, thud, tbool, tdyn};
    Asm a;
    asm_begin(&a, 96);
    jump(&a, wt_op(OTrap, 2, RERR, 0), L_CATCH);
    op(&a, wt_op(OInt, 2, RK, hl_edit_int(e, LIVE_TAKE_CONSTS)));
    op(&a, wt_op(OCall1, 3, RT, f_take, RK));
    jump(&a, wt_op(OJFalse, 2, RT, 0), L_HUDS);
    op(&a, wt_op(OGetGlobal, 2, RDATA, get->ops[0].a[1]));
    op(&a, wt_op(OField, 3, RIDX, RDATA, get->ops[1].a[2]));
    jump(&a, wt_op(OJNull, 2, RIDX, 0), L_HUDS);
    op(&a, wt_op(OField, 3, RMAP, RIDX, get->ops[3].a[2]));
    jump(&a, wt_op(OJNull, 2, RMAP, 0), L_HUDS);
    for (int k = 0; k < 2; k++) {
        int next = k ? L_NEXT1 : L_NEXT0;
        op(&a, wt_op(OInt, 2, RID, hl_edit_int(e, k ? LIVE_ZOOM_MIN : LIVE_ZOOM_MAX)));
        op(&a, wt_op(OCall1, 3, RT, f_known, RID));
        jump(&a, wt_op(OJFalse, 2, RT, 0), next);
        op(&a, wt_op(OGetGlobal, 2, RNAME, names[k]));
        op(&a, wt_op(OCall2, 4, RDYN, map_get, RMAP, RNAME));
        op(&a, wt_op(OToVirtual, 2, RROW, RDYN));
        jump(&a, wt_op(OJNull, 2, RROW, 0), next);
        op(&a, wt_op(OCall1, 3, RV, f_number, RID));
        op(&a, wt_op(OToDyn, 2, RNULL, RV));
        op(&a, wt_op(OSetField, 3, RROW, f_value, RNULL));
        mark(&a, next);
    }
    mark(&a, L_HUDS);
    op(&a, wt_op(OInt, 2, RK, hl_edit_int(e, LIVE_TAKE_HUDS)));
    op(&a, wt_op(OCall1, 3, RT, f_take, RK));
    jump(&a, wt_op(OJFalse, 2, RT, 0), L_RET);
    op(&a, wt_op(OField, 3, RSTATE, RB, f_state));
    jump(&a, wt_op(OJNull, 2, RSTATE, 0), L_RET);
    op(&a, wt_op(OField, 3, RARR, RSTATE, f_units));
    jump(&a, wt_op(OJNull, 2, RARR, 0), L_RET);
    op(&a, wt_op(OInt, 2, RI, hl_edit_int(e, 0)));
    mark(&a, L_LOOP);
    op(&a, wt_op(OLabel, 0));
    op(&a, wt_op(OField, 3, RLEN, RARR, f_len));
    jump(&a, wt_op(OJSGte, 3, RI, RLEN, 0), L_RET);
    op(&a, wt_op(OField, 3, RRAW, RARR, f_arr));
    op(&a, wt_op(OGetArray, 3, RDYN2, RRAW, RI));
    op(&a, wt_op(OIncr, 1, RI));
    op(&a, wt_op(OUnsafeCast, 2, RUNIT, RDYN2));
    jump(&a, wt_op(OJNull, 2, RUNIT, 0), L_LOOP);
    op(&a, wt_op(OField, 3, RHUD, RUNIT, f_hud));
    jump(&a, wt_op(OJNull, 2, RHUD, 0), L_LOOP);
    op(&a, wt_op(OField, 3, RFLAG, RHUD, f_closed));
    jump(&a, wt_op(OJTrue, 2, RFLAG, 0), L_BARS);
    op(&a, wt_op(OField, 3, RFLAG, RHUD, f_minimal));
    jump(&a, wt_op(OJTrue, 2, RFLAG, 0), L_BARS);
    op(&a, wt_op(OInt, 2, RID, hl_edit_int(e, LIVE_HUD_FULL)));
    jump(&a, wt_op(OJAlways, 1, 0), L_SCALE);
    mark(&a, L_BARS);
    op(&a, wt_op(OInt, 2, RID, hl_edit_int(e, LIVE_HUD_BARS)));
    mark(&a, L_SCALE);
    op(&a, wt_op(OCall1, 3, RV, f_number, RID));
    op(&a, wt_op(OCall2, 4, RVOID, f_scale, RHUD, RV));
    jump(&a, wt_op(OJAlways, 1, 0), L_LOOP);
    mark(&a, L_RET);
    op(&a, wt_op(OEndTrap, 1, 1));
    op(&a, wt_op(ORet, 1, RVOID));
    mark(&a, L_CATCH);
    op(&a, wt_op(OCall1, 3, RVOID, f_error, RERR));
    op(&a, wt_op(ORet, 1, RVOID));
    resolve(&a);
    HlFun *live = hl_fun_new(t_apply, NREGS, regs, a.ops, a.n, cam->files[cam->nops - 1], cam->lines[cam->nops - 1]);
    free(a.ops);
    int live_findex = live ? hl_edit_add(e, live) : -1, returns = 0;
    hl_fun_free(live);

    // Each return's own slot takes the call, so a jump to it still runs it; the return follows.
    for (int i = cam->nops - 1; live_findex >= 0 && i >= 0; i--) {
        if (cam->ops[i].op != ORet) continue;
        HlOp ret = cam->ops[i];
        cam->ops[i] = wt_op(OCall1, 3, ret.a[0], live_findex, 0);
        hl_fun_insert(cam, i + 1, &ret, 1);
        returns++;
    }
    int result = returns && !hl_edit_replace(e, fn[0], cam) ? 0 : -1;
    hl_fun_free(cam), hl_fun_free(get);
    return result;
}

const WtFix fix_live = {"live", "overlay settings reach the battle as they change", targets, 3, apply};
