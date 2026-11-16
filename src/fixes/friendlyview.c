// A friendly-only view: the player's and allies' HUDs and outlines, on through deployment (while the battle has unit
// slots) and on Shift+ALT in battle. ALT alone stays the game's, so Battle.update's ALT checks ignore it under Shift.
#include "../asm.h"
#include <string.h>


static const WtTarget targets[] = {
    {"battle.Battle.update", 0}, // its ALT checks are found by their 'Outlines' argument; its last op is its return
    {"lib.$Input.isDown", 0},
    {"hxd.$Key.isDown", 0},
    {"battle.Unit.isAlive", 0},
    {"battle.Unit.showUnitHud", 0},
    {"battle.Unit.hideUnitHud", 0},
    {"battle.Unit.showOutline", 0},
    {"ent.Entity.setOutline", 0},
    {"battle.ui.comp.LiteUnitHud.show", 0},
};
#define NTARGETS (int)(sizeof targets / sizeof targets[0])

static const char *const signatures[] = {
    "(battle.Battle,f64)->void",      "(String)->bool",      "(i32)->bool",
    "(battle.Unit)->bool",            "(battle.Unit,null<bool>)->void",
    "(battle.Unit)->void",            "(battle.Unit)->void", "(ent.Entity,null<i32>)->void",
    "(battle.ui.comp.LiteUnitHud,bool,bool)->void",
};

#define KEY_SHIFT 16

static int field(HlCode *c, int type, const char *name, int *ftype) {
    *ftype = -1;
    if (type < 0) return -1;
    int fid = hl_type_field(c, type, name);
    *ftype = hl_field_type(c, type, fid);
    if (fid < 0) wt_log("  %s has no field %s", hl_type_name(c, type), name);
    return fid;
}

typedef struct {
    int f_isdown, f_keydown, f_alive, f_showhud, f_hidehud, f_outline, f_setoutline, f_hudshow;
    int f_was, f_count, f_unit_error, f_frame_error, outlines, game_global;
    int state, cine, over, cur, showall, slots, units, len, arr, foe, neutral, owner, side, data, ustate, unitbattle;
    int hud, closed, minimal, box, ontop, shader, playing, inst, loading;
} Ids;

enum { RB, RSTATE, RARR, RLEN, RZERO, RWANT, RNAME, RTMP, RKEY, RWAS, RI, RRAW, RDYN, RUNIT, RONE, RHUD, RTMP2,
       RSHADER, RVOID, RNULLB, ROTHER, RNULLI, RBOX, RTRUE, RFALSE, RCOUNT, RGAMECLASS, RGAME, RLOADING, RERR, RERR2,
       ROWNER, RDATA, RUSTATE, RUBATTLE, RSIDE, RFOE, RNEUTRAL, NREGS };
enum { L_KEYS, L_CINE, L_EDGE, L_GO, L_LOOP, L_OPEN, L_MINIMAL, L_OUTLINE, L_OFF, L_KEEPOUTLINE, L_NEXT, L_UNITFAIL,
       L_RET, L_CATCH };

static void count(Asm *a, HlEdit *e, const Ids *d, int k) {
    op(a, wt_op(OInt, 2, RCOUNT, hl_edit_int(e, k)));
    op(a, wt_op(OCall1, 3, RVOID, d->f_count, RCOUNT));
}

// In Battle.update every frame: traps keep the game's exceptions out of the battle and one bad unit from the rest.
static void build_view(Asm *a, HlEdit *e, const Ids *d) {
    jump(a, wt_op(OTrap, 2, RERR, 0), L_CATCH);
    op(a, wt_op(OField, 3, RSTATE, RB, d->state));
    jump(a, wt_op(OJNull, 2, RSTATE, 0), L_RET);
    // Nothing while the loading screen is up: Battle.update already runs then, on units still being set up.
    op(a, wt_op(OGetGlobal, 2, RGAMECLASS, d->game_global));
    op(a, wt_op(OField, 3, RGAME, RGAMECLASS, d->inst));
    jump(a, wt_op(OJNull, 2, RGAME, 0), L_RET);
    op(a, wt_op(OField, 3, RLOADING, RGAME, d->loading));
    jump(a, wt_op(OJNotNull, 2, RLOADING, 0), L_RET);
    op(a, wt_op(OBool, 2, RWANT, 0));
    op(a, wt_op(OGetGlobal, 2, RNAME, d->outlines));
    op(a, wt_op(OCall1, 3, RTMP, d->f_isdown, RNAME));
    jump(a, wt_op(OJFalse, 2, RTMP, 0), L_KEYS);
    op(a, wt_op(OInt, 2, RKEY, hl_edit_int(e, KEY_SHIFT)));
    op(a, wt_op(OCall1, 3, RTMP, d->f_keydown, RKEY));
    jump(a, wt_op(OJFalse, 2, RTMP, 0), L_KEYS);
    op(a, wt_op(OBool, 2, RWANT, 1));
    mark(a, L_KEYS); // not held: still on while there are deployment slots
    op(a, wt_op(OField, 3, RARR, RSTATE, d->slots));
    jump(a, wt_op(OJNull, 2, RARR, 0), L_CINE);
    op(a, wt_op(OField, 3, RLEN, RARR, d->len));
    op(a, wt_op(OInt, 2, RZERO, hl_edit_int(e, 0)));
    jump(a, wt_op(OJSLte, 3, RLEN, RZERO, 0), L_CINE);
    op(a, wt_op(OBool, 2, RWANT, 1));
    mark(a, L_CINE);
    op(a, wt_op(OField, 3, RTMP, RB, d->cine));
    jump(a, wt_op(OJFalse, 2, RTMP, 0), L_EDGE);
    op(a, wt_op(OBool, 2, RWANT, 0));
    mark(a, L_EDGE);
    op(a, wt_op(OCall1, 3, RWAS, d->f_was, RWANT));
    jump(a, wt_op(OJTrue, 2, RWANT, 0), L_GO);
    jump(a, wt_op(OJFalse, 2, RWAS, 0), L_RET);
    mark(a, L_GO);
    op(a, wt_op(OField, 3, RFOE, RSTATE, d->foe));
    op(a, wt_op(OField, 3, RNEUTRAL, RSTATE, d->neutral));
    op(a, wt_op(OField, 3, RARR, RSTATE, d->units));
    jump(a, wt_op(OJNull, 2, RARR, 0), L_RET);
    op(a, wt_op(OInt, 2, RI, hl_edit_int(e, 0)));
    op(a, wt_op(OInt, 2, RONE, hl_edit_int(e, 1)));
    // A backward jump's target must be a Label: the JIT drops the registers it caches only there.
    mark(a, L_LOOP);
    op(a, wt_op(OLabel, 0));
    op(a, wt_op(OField, 3, RLEN, RARR, d->len));
    jump(a, wt_op(OJSGte, 3, RI, RLEN, 0), L_RET);
    op(a, wt_op(OField, 3, RRAW, RARR, d->arr));
    op(a, wt_op(OGetArray, 3, RDYN, RRAW, RI));
    op(a, wt_op(OIncr, 1, RI));
    op(a, wt_op(OUnsafeCast, 2, RUNIT, RDYN));
    jump(a, wt_op(OJNull, 2, RUNIT, 0), L_LOOP);
    jump(a, wt_op(OTrap, 2, RERR2, 0), L_UNITFAIL);
    // Set-up characters only: structures have no character, which showOutline's getSide needs.
    op(a, wt_op(OField, 3, ROWNER, RUNIT, d->owner));
    jump(a, wt_op(OJNull, 2, ROWNER, 0), L_NEXT);
    op(a, wt_op(OField, 3, RDATA, RUNIT, d->data));
    jump(a, wt_op(OJNull, 2, RDATA, 0), L_NEXT);
    op(a, wt_op(OField, 3, RUSTATE, RUNIT, d->ustate));
    jump(a, wt_op(OJNull, 2, RUSTATE, 0), L_NEXT);
    op(a, wt_op(OField, 3, RUBATTLE, RUNIT, d->unitbattle));
    jump(a, wt_op(OJNull, 2, RUBATTLE, 0), L_NEXT);
    op(a, wt_op(OField, 3, RSIDE, ROWNER, d->side));
    jump(a, wt_op(OJEq, 3, RSIDE, RFOE, 0), L_NEXT);
    jump(a, wt_op(OJEq, 3, RSIDE, RNEUTRAL, 0), L_NEXT);
    jump(a, wt_op(OJFalse, 2, RWANT, 0), L_OFF);
    op(a, wt_op(OCall1, 3, RTMP, d->f_alive, RUNIT));
    jump(a, wt_op(OJFalse, 2, RTMP, 0), L_NEXT);
    // showUnitHud re-adds the HUD to the scene to put it on top: only when it is closed or not on top.
    // A HUD opened while the camera moved is minimal; LiteUnitHud.show redoes it and rebuilds only on a change.
    op(a, wt_op(OField, 3, RHUD, RUNIT, d->hud));
    jump(a, wt_op(OJNull, 2, RHUD, 0), L_OUTLINE);
    op(a, wt_op(OField, 3, RTMP2, RHUD, d->closed));
    jump(a, wt_op(OJTrue, 2, RTMP2, 0), L_OPEN);
    op(a, wt_op(OField, 3, RBOX, RUNIT, d->box));
    jump(a, wt_op(OJNull, 2, RBOX, 0), L_MINIMAL);
    op(a, wt_op(OField, 3, RTMP2, RBOX, d->ontop));
    jump(a, wt_op(OJFalse, 2, RTMP2, 0), L_OPEN);
    mark(a, L_MINIMAL);
    op(a, wt_op(OField, 3, RTMP2, RHUD, d->minimal));
    jump(a, wt_op(OJFalse, 2, RTMP2, 0), L_OUTLINE);
    op(a, wt_op(OBool, 2, RTRUE, 1));
    op(a, wt_op(OBool, 2, RFALSE, 0));
    op(a, wt_op(OCall3, 5, RVOID, d->f_hudshow, RHUD, RTRUE, RFALSE));
    jump(a, wt_op(OJAlways, 1, 0), L_OUTLINE);
    mark(a, L_OPEN);
    op(a, wt_op(ONull, 1, RNULLB));
    op(a, wt_op(OCall2, 4, RVOID, d->f_showhud, RUNIT, RNULLB));
    mark(a, L_OUTLINE);
    op(a, wt_op(OField, 3, RSHADER, RUNIT, d->shader));
    jump(a, wt_op(OJNotNull, 2, RSHADER, 0), L_NEXT);
    op(a, wt_op(OCall1, 3, RVOID, d->f_outline, RUNIT));
    jump(a, wt_op(OJAlways, 1, 0), L_NEXT);
    // The frame it turns off: close what it opened, as the game's hover-out would.
    mark(a, L_OFF);
    op(a, wt_op(OField, 3, ROTHER, RB, d->over));
    jump(a, wt_op(OJEq, 3, ROTHER, RUNIT, 0), L_NEXT);
    op(a, wt_op(OField, 3, RTMP2, RB, d->showall));
    jump(a, wt_op(OJTrue, 2, RTMP2, 0), L_KEEPOUTLINE);
    op(a, wt_op(OCall1, 3, RVOID, d->f_hidehud, RUNIT));
    mark(a, L_KEEPOUTLINE);
    op(a, wt_op(OField, 3, ROTHER, RB, d->cur));
    jump(a, wt_op(OJEq, 3, ROTHER, RUNIT, 0), L_NEXT);
    op(a, wt_op(OField, 3, RTMP2, RUNIT, d->playing));
    jump(a, wt_op(OJTrue, 2, RTMP2, 0), L_NEXT);
    op(a, wt_op(ONull, 1, RNULLI));
    op(a, wt_op(OCall2, 4, RVOID, d->f_setoutline, RUNIT, RNULLI));
    mark(a, L_NEXT);
    op(a, wt_op(OEndTrap, 1, 1));
    jump(a, wt_op(OJAlways, 1, 0), L_LOOP);
    mark(a, L_UNITFAIL);
    count(a, e, d, 4);
    op(a, wt_op(OCall1, 3, RVOID, d->f_unit_error, RERR2));
    jump(a, wt_op(OJAlways, 1, 0), L_LOOP);
    mark(a, L_RET);
    op(a, wt_op(OEndTrap, 1, 1));
    op(a, wt_op(ORet, 1, RVOID));
    mark(a, L_CATCH);
    count(a, e, d, 4);
    op(a, wt_op(OCall1, 3, RVOID, d->f_frame_error, RERR));
    op(a, wt_op(ORet, 1, RVOID));
    resolve(a);
}

static int apply(HlCode *c, HlEdit *e, const int *fn) {
    for (int t = 0; t < NTARGETS; t++)
        if (!wt_signature(c, fn[t], signatures[t])) return -1;
    Ids d = {0};
    d.f_isdown = wt_findex(c, fn[1]), d.f_keydown = wt_findex(c, fn[2]), d.f_alive = wt_findex(c, fn[3]);
    d.f_showhud = wt_findex(c, fn[4]), d.f_hidehud = wt_findex(c, fn[5]), d.f_outline = wt_findex(c, fn[6]);
    d.f_setoutline = wt_findex(c, fn[7]), d.f_hudshow = wt_findex(c, fn[8]);
    const HlType *showhud = &c->types[c->functions[fn[4]].type], *setoutline = &c->types[c->functions[fn[7]].type];
    int tunit = showhud->args[0], tnullbool = showhud->args[1], tnullint = setoutline->args[1];
    int tstring = c->types[c->functions[fn[1]].type].args[0];
    int tvoid = hl_find_type(c, HVOID, NULL), tbool = hl_find_type(c, HBOOL, NULL), ti32 = hl_find_type(c, HI32, NULL);
    int tdyn = hl_find_type(c, HDYN, NULL);
    d.outlines = wt_string_global(c, "Outlines");
    if (d.outlines < 0) {
        wt_log("  no 'Outlines' input action");
        return -1;
    }

    HlFun *up = hl_fun_decode(c, fn[0]);
    if (!up) return -1;
    int tbattle = up->regs[0], tstate, tarrobj, tarr, tx, thud, tshader, tcur, tover, tbox, towner, tside, tfoe;
    int tneutral, tdata, tustate, tub, tinst, tloading;
    int tgame = hl_find_type(c, HOBJ, "Game");
    d.game_global = tgame >= 0 ? c->types[tgame].global - 1 : -1;
    int tgameclass = d.game_global >= 0 ? c->globals[d.game_global] : -1;
    d.state = field(c, tbattle, "state", &tstate), d.cine = field(c, tbattle, "cinematicMode", &tx);
    d.over = field(c, tbattle, "overUnit", &tover), d.cur = field(c, tbattle, "currentUnit", &tcur);
    d.showall = field(c, tbattle, "showOutlines", &tx), d.slots = field(c, tstate, "unitSlots", &tarrobj);
    d.units = field(c, tstate, "units", &tx), d.len = field(c, tarrobj, "length", &tx);
    d.arr = field(c, tarrobj, "array", &tarr), d.foe = field(c, tstate, "foeSide", &tfoe);
    d.neutral = field(c, tstate, "neutralSide", &tneutral), d.owner = field(c, tunit, "owner", &towner);
    d.side = field(c, towner, "side", &tside), d.data = field(c, tunit, "data", &tdata);
    d.ustate = field(c, tunit, "battleState", &tustate), d.unitbattle = field(c, tunit, "battle", &tub);
    d.hud = field(c, tunit, "unitHud", &thud), d.closed = field(c, thud, "closed", &tx);
    d.minimal = field(c, thud, "minimal", &tx), d.box = field(c, tunit, "hud", &tbox);
    d.ontop = field(c, tbox, "onTop", &tx), d.shader = field(c, tunit, "outlineShader", &tshader);
    d.playing = field(c, tunit, "isCurrentUnitPlaying", &tx), d.inst = field(c, tgameclass, "inst", &tinst);
    d.loading = field(c, tgame, "loadingScreen", &tloading);
    int view_type = wt_fun_type(c, 1, tbattle, -1, tvoid), was_type = wt_fun_type(c, 1, tbool, -1, tbool);
    int count_type = wt_fun_type(c, 1, ti32, -1, tvoid), last = up->nops - 1;
    const int *ids = &d.state;
    int ok = view_type >= 0 && was_type >= 0 && count_type >= 0 && tfoe == tside && tneutral == tside &&
             tub == tbattle && tinst == tgame && tcur == tunit && tover == tunit &&
             c->types[c->functions[fn[8]].type].args[0] == thud && wt_expect(c, up, last, ORet, NULL) &&
             c->types[up->regs[up->ops[last].a[0]]].kind == HVOID;
    for (const int *p = ids; ok && p <= &d.loading; p++) ok = *p >= 0;
    if (!ok) {
        hl_fun_free(up);
        return -1;
    }

    // altOnly(name): Input.isDown(name), but false while Shift is down.
    const int alt_regs[] = {tstring, tbool, ti32, tbool};
    const HlOp alt_ops[] = {
        wt_op(OCall1, 3, 1, d.f_isdown, 0), wt_op(OJFalse, 2, 1, 6),       wt_op(OInt, 2, 2, hl_edit_int(e, KEY_SHIFT)),
        wt_op(OCall1, 3, 3, d.f_keydown, 2), wt_op(OJFalse, 2, 3, 6),       wt_op(OBool, 2, 1, 0),
        wt_op(ORet, 1, 1),
    };
    HlFun *alt = hl_fun_new(c->functions[fn[1]].type, 4, alt_regs, alt_ops, 7, up->files[0], up->lines[0]);
    int alt_findex = alt ? hl_edit_add(e, alt) : -1, retargeted = 0;
    hl_fun_free(alt);
    for (int i = 1; alt_findex >= 0 && i < up->nops; i++) {
        HlOp *o = &up->ops[i], *prev = &up->ops[i - 1];
        if (o->op == OCall1 && o->a[1] == d.f_isdown && prev->op == OGetGlobal && prev->a[0] == o->a[2] &&
            prev->a[1] == d.outlines)
            o->a[1] = alt_findex, retargeted++;
    }
    if (!retargeted) {
        wt_log("  battle.Battle.update no longer checks the Outlines action");
        hl_fun_free(up);
        return -1;
    }
    wt_log("  %d ALT checks in battle.Battle.update ignore ALT while Shift is down", retargeted);

    d.f_was = hl_edit_native(e, "wtfix", "friendly_was", was_type);
    d.f_count = hl_edit_native(e, "wtfix", "friendly_count", count_type);
    int error_type = wt_fun_type(c, 1, tdyn, -1, tvoid);
    if (error_type < 0) {
        wt_log("  no (dynamic)->void type for the error log");
        hl_fun_free(up);
        return -1;
    }
    d.f_unit_error = hl_edit_native(e, "wtfix", "friendly_unit_error", error_type);
    d.f_frame_error = hl_edit_native(e, "wtfix", "friendly_frame_error", error_type);
    const int regs[NREGS] = {tbattle, tstate, tarrobj, ti32, ti32, tbool, tstring, tbool, ti32, tbool, ti32, tarr, tdyn,
                             tunit, ti32, thud, tbool, tshader, tvoid, tnullbool, tunit, tnullint, tbox, tbool, tbool,
                             ti32, tgameclass, tgame, tloading, tdyn, tdyn, towner, tdata, tustate, tbattle, tside,
                             tside, tside};
    Asm a = {0};
    a.ops = (HlOp *)calloc(200, sizeof(HlOp));
    build_view(&a, e, &d);
    HlFun *view = hl_fun_new(view_type, NREGS, regs, a.ops, a.n, up->files[last], up->lines[last]);
    free(a.ops);
    int view_findex = view ? hl_edit_add(e, view) : -1;
    hl_fun_free(view);

    // The last op (the return every frame reaches) becomes the call, so jumps to it still run it; the return follows.
    HlOp ret = up->ops[last];
    up->ops[last] = wt_op(OCall1, 3, ret.a[0], view_findex, 0);
    hl_fun_insert(up, up->nops, &ret, 1);
    int result = view_findex >= 0 && !hl_edit_replace(e, fn[0], up) ? 0 : -1;
    hl_fun_free(up);
    return result;
}

const WtFix fix_friendlyview = {"friendlyview", "friendly names and outlines: always while deploying, Shift+ALT in battle",
                                targets, NTARGETS, apply};
