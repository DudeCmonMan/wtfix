// Battle culls the engine misses: static props get culling colliders, so off-screen ones leave the main and shadow
// passes, and small props' models a cullingScreenRatio (Mesh.emit's size cull), so tiny ones are not drawn at all.
// Runs once battleInit is set and no loading screen is left: updateCulling already runs while the map is generated,
// and walking that half-built scene stops the battle loading.
#include "../asm.h"
#include "../live.h"
#include <string.h>

static const WtTarget targets[] = {
    {"battle.Battle.updateCulling", 0}, // its returns are found
    {"$String.__alloc__", 0},
    {"hscript.$Parser.__constructor__", 0},
    {"hscript.Parser.parseString", 0},
    {"hscript.$Interp.__constructor__", 0},
    {"hscript.Interp.execute", 0},
    {"haxe.ds.StringMap.set", 0},
    {"haxe.ds.$StringMap.__constructor__", 0},
    {"$Type.resolveClass", 0},
};
enum { F_CULLING, F_ALLOC, F_PARSER, F_PARSE, F_INTERP, F_EXECUTE, F_SET, F_MAP, F_RESOLVE };

enum { RB, RZERO, RLEN, RBATTLE, RGAMECLS, RGAME, RLOADING, RBYTES, RSOURCE, RPARSER, RORIGIN, RLINE, REXPR, RINTERP,
       RVARS, RKEYB, RKEYL, RKEY, RVAL, RRESOLVE, RID, RNUM, RSTATE, RMAP, RRESULT, RERR, RERR2, RNULL, RVOID, RREADY,
       RMAINCLS, RLOADING2D, NREGS };
enum { L_OUT, L_CATCH, L_CATCH2, L_STATE };

#define EXPR "virtual<e,line,origin,pmax,pmin>"

typedef struct {
    Asm *a;
    HlEdit *e;
    int f_alloc, f_set;
} Binder;

static void bind(Binder *b, const char *name, int value) {
    op(b->a, wt_op(OString, 2, RKEYB, hl_edit_string(b->e, name)));
    op(b->a, wt_op(OInt, 2, RKEYL, hl_edit_int(b->e, (int)strlen(name))));
    op(b->a, wt_op(OCall2, 4, RKEY, b->f_alloc, RKEYB, RKEYL));
    op(b->a, wt_op(OCall3, 5, RVOID, b->f_set, RVARS, RKEY, value));
}

static int apply(HlCode *c, HlEdit *e, const int *fn) {
    if (!wt_signature(c, fn[F_CULLING], "(battle.Battle)->void") ||
        !wt_signature(c, fn[F_ALLOC], "(bytes,i32)->String") ||
        !wt_signature(c, fn[F_PARSER], "(hscript.Parser)->void") ||
        !wt_signature(c, fn[F_PARSE], "(hscript.Parser,String,String,null<i32>)->" EXPR) ||
        !wt_signature(c, fn[F_INTERP], "(hscript.Interp)->void") ||
        !wt_signature(c, fn[F_EXECUTE], "(hscript.Interp," EXPR ")->dynamic") ||
        !wt_signature(c, fn[F_SET], "(haxe.ds.StringMap,String,dynamic)->void") ||
        !wt_signature(c, fn[F_MAP], "(haxe.ds.StringMap)->void") ||
        !wt_signature(c, fn[F_RESOLVE], "(String)->hl.Class"))
        return -1;
    const HlType *parse = &c->types[c->functions[fn[F_PARSE]].type];
    int tvoid = hl_find_type(c, HVOID, NULL), ti32 = hl_find_type(c, HI32, NULL), tf64 = hl_find_type(c, HF64, NULL);
    int tbytes = hl_find_type(c, HBYTES, NULL), tdyn = hl_find_type(c, HDYN, NULL);
    int tbattle = c->types[c->functions[fn[F_CULLING]].type].args[0];
    int tinterp = c->types[c->functions[fn[F_INTERP]].type].args[0];
    int f_vars = hl_type_field(c, tinterp, "variables"), tvars = hl_field_type(c, tinterp, f_vars);
    int tgamecls = hl_find_type(c, HOBJ, "$Game"), f_inst = hl_type_field(c, tgamecls, "inst");
    int tgame = hl_field_type(c, tgamecls, f_inst), g_game = tgame >= 0 ? c->types[tgame].global - 1 : -1;
    int f_loading = hl_type_field(c, tgame, "loadingScreen"), tloading = hl_field_type(c, tgame, f_loading);
    int f_ready = hl_type_field(c, tbattle, "battleInit"), tbool = hl_find_type(c, HBOOL, NULL);
    int tmaincls = hl_find_type(c, HOBJ, "$Main"), f_loading2d = hl_type_field(c, tmaincls, "loadingS2d");
    int tloading2d = hl_field_type(c, tmaincls, f_loading2d), g_main = -1;
    for (int g = 0; tmaincls >= 0 && g < c->nglobals; g++)
        if (c->globals[g] == tmaincls) g_main = g;
    int t_due = wt_fun_type(c, 1, tdyn, -1, ti32), t_source = wt_fun_type(c, 1, ti32, -1, tbytes);
    int t_give = wt_fun_type(c, 0, -1, -1, tdyn), t_take = wt_fun_type(c, 1, tdyn, -1, tvoid);
    int t_number = wt_fun_type(c, 1, ti32, -1, tf64), t_hook = wt_fun_type(c, 1, tbattle, -1, tvoid);
    if (tvars != c->types[c->functions[fn[F_SET]].type].args[0] || f_inst < 0 || g_game < 0 || f_loading < 0 ||
        f_ready < 0 || f_loading2d < 0 || g_main < 0 || t_due < 0 || t_source < 0 || t_give < 0 || t_take < 0 ||
        t_number < 0 || t_hook < 0) {
        wt_log("  hscript.Interp.variables, Game.inst, Game.loadingScreen, Battle.battleInit, Main.loadingS2d or a "
               "function type battlecull needs is missing");
        return -1;
    }
    int f_due = hl_edit_native(e, "wtfix", "cull_due", t_due);
    int f_source = hl_edit_native(e, "wtfix", "cull_source", t_source);
    int f_state_get = hl_edit_native(e, "wtfix", "cull_state_get", t_give);
    int f_state_set = hl_edit_native(e, "wtfix", "cull_state_set", t_take);
    int f_done = hl_edit_native(e, "wtfix", "cull_done", t_take);
    int f_failed = hl_edit_native(e, "wtfix", "cull_failed", t_take);
    int f_number = hl_edit_native(e, "wtfix", "live_number", t_number);
    int f_alloc = wt_findex(c, fn[F_ALLOC]);
    const int regs[NREGS] = {tbattle, ti32,      ti32,           tdyn,       tgamecls,        tgame,
                             tloading, tbytes,   parse->args[1], parse->args[0], parse->args[1], parse->args[3],
                             parse->ret, tinterp, tvars,         tbytes,     ti32,            parse->args[1],
                             tdyn,     c->functions[fn[F_RESOLVE]].type, ti32, tf64, tdyn, tvars,
                             tdyn,     tdyn,     tdyn,           tdyn,       tvoid, tbool, tmaincls, tloading2d};

    Asm a;
    asm_begin(&a, 96);
    Binder b = {&a, e, f_alloc, wt_findex(c, fn[F_SET])};
    jump(&a, wt_op(OTrap, 2, RERR, 0), L_CATCH);
    op(&a, wt_op(OInt, 2, RZERO, hl_edit_int(e, 0)));
    op(&a, wt_op(OMov, 2, RBATTLE, RB)); // objects already are dynamics; ToDyn would box
    op(&a, wt_op(OCall1, 3, RLEN, f_due, RBATTLE));
    jump(&a, wt_op(OJSLt, 3, RLEN, RZERO, 0), L_OUT);
    op(&a, wt_op(OGetGlobal, 2, RGAMECLS, g_game));
    op(&a, wt_op(OField, 3, RGAME, RGAMECLS, f_inst));
    jump(&a, wt_op(OJNull, 2, RGAME, 0), L_OUT);
    op(&a, wt_op(OField, 3, RLOADING, RGAME, f_loading));
    jump(&a, wt_op(OJNotNull, 2, RLOADING, 0), L_OUT);
    op(&a, wt_op(OField, 3, RREADY, RB, f_ready));
    jump(&a, wt_op(OJFalse, 2, RREADY, 0), L_OUT);
    op(&a, wt_op(OGetGlobal, 2, RMAINCLS, g_main));
    op(&a, wt_op(OField, 3, RLOADING2D, RMAINCLS, f_loading2d));
    jump(&a, wt_op(OJNotNull, 2, RLOADING2D, 0), L_OUT);
    op(&a, wt_op(OCall1, 3, RBYTES, f_source, RLEN));
    op(&a, wt_op(OCall2, 4, RSOURCE, f_alloc, RBYTES, RLEN));
    op(&a, wt_op(ONew, 1, RPARSER));
    op(&a, wt_op(OCall1, 3, RVOID, wt_findex(c, fn[F_PARSER]), RPARSER));
    op(&a, wt_op(ONull, 1, RORIGIN));
    op(&a, wt_op(ONull, 1, RLINE));
    op(&a, wt_op(OCall4, 6, REXPR, wt_findex(c, fn[F_PARSE]), RPARSER, RSOURCE, RORIGIN, RLINE));
    op(&a, wt_op(ONew, 1, RINTERP));
    op(&a, wt_op(OCall1, 3, RVOID, wt_findex(c, fn[F_INTERP]), RINTERP));
    op(&a, wt_op(OField, 3, RVARS, RINTERP, f_vars));
    bind(&b, "battle", RBATTLE);
    op(&a, wt_op(OMov, 2, RVAL, RGAME));
    bind(&b, "game", RVAL);
    op(&a, wt_op(OStaticClosure, 2, RRESOLVE, wt_findex(c, fn[F_RESOLVE])));
    op(&a, wt_op(OMov, 2, RVAL, RRESOLVE));
    bind(&b, "cls", RVAL);
    op(&a, wt_op(OInt, 2, RID, hl_edit_int(e, LIVE_CULL_OFFSCREEN)));
    op(&a, wt_op(OCall1, 3, RNUM, f_number, RID));
    op(&a, wt_op(OToDyn, 2, RVAL, RNUM));
    bind(&b, "offscreen", RVAL);
    op(&a, wt_op(OInt, 2, RID, hl_edit_int(e, LIVE_CULL_SMALL)));
    op(&a, wt_op(OCall1, 3, RNUM, f_number, RID));
    op(&a, wt_op(OToDyn, 2, RVAL, RNUM));
    bind(&b, "smallPx", RVAL);
    op(&a, wt_op(OCall0, 2, RSTATE, f_state_get));
    jump(&a, wt_op(OJNotNull, 2, RSTATE, 0), L_STATE);
    op(&a, wt_op(ONew, 1, RMAP));
    op(&a, wt_op(OCall1, 3, RVOID, wt_findex(c, fn[F_MAP]), RMAP));
    op(&a, wt_op(OMov, 2, RSTATE, RMAP));
    op(&a, wt_op(OCall1, 3, RVOID, f_state_set, RSTATE));
    mark(&a, L_STATE);
    bind(&b, "state", RSTATE);
    op(&a, wt_op(OCall2, 4, RRESULT, wt_findex(c, fn[F_EXECUTE]), RINTERP, REXPR));
    op(&a, wt_op(OCall1, 3, RVOID, f_done, RRESULT));
    mark(&a, L_OUT);
    op(&a, wt_op(OEndTrap, 1, 1));
    op(&a, wt_op(ORet, 1, RVOID));
    // An error that fails to print is still reported, without its text.
    mark(&a, L_CATCH);
    jump(&a, wt_op(OTrap, 2, RERR2, 0), L_CATCH2);
    op(&a, wt_op(OCall1, 3, RVOID, f_failed, RERR));
    op(&a, wt_op(OEndTrap, 1, 1));
    op(&a, wt_op(ORet, 1, RVOID));
    mark(&a, L_CATCH2);
    op(&a, wt_op(ONull, 1, RNULL));
    op(&a, wt_op(OCall1, 3, RVOID, f_failed, RNULL));
    op(&a, wt_op(ORet, 1, RVOID));
    resolve(&a);

    HlFun *culling = hl_fun_decode(c, fn[F_CULLING]);
    HlFun *hook = culling ? hl_fun_new(t_hook, NREGS, regs, a.ops, a.n, culling->files[0], culling->lines[0]) : NULL;
    free(a.ops);
    int f_hook = hook ? hl_edit_add(e, hook) : -1, returns = 0;
    hl_fun_free(hook);
    // Each return's own slot takes the call, so a jump to it still runs it; the return follows.
    for (int i = culling && f_hook >= 0 ? culling->nops - 1 : -1; i >= 0; i--) {
        if (culling->ops[i].op != ORet) continue;
        HlOp ret = culling->ops[i];
        culling->ops[i] = wt_op(OCall1, 3, ret.a[0], f_hook, 0);
        hl_fun_insert(culling, i + 1, &ret, 1);
        returns++;
    }
    int result = returns && !hl_edit_replace(e, fn[F_CULLING], culling) ? 0 : -1;
    hl_fun_free(culling);
    return result;
}

const WtFix fix_battlecull = {"battlecull", "battle props: off-screen ones culled, ones under a few pixels not drawn (live)",
                              targets, 9, apply};
