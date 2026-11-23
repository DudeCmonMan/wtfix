// Battle sun shadows ([perf] shadow_cache): static casters, marked isStatic by a per-battle hscript setup, are drawn
// into their own map only when they or the held light camera change; the dynamic ones are drawn over a copy of it.
#include "../asm.h"
#include "../live.h"
#include "../shadowfit.h"
#include <string.h>

static const WtTarget targets[] = {
    {"h3d.pass.DirShadowMap.draw", 0},             // its allocation, cull and camera update are found and checked
    {"h3d.pass.DirShadowMap.processShadowMap", 0}, // its depth clear is found and checked
    {"h3d.pass.DirShadowMap.syncShader", 0},
    {"h3d.pass.Shadows.filterPasses", 0},          // a call is put first; nothing else in it is relied on
    {"h3d.Camera.update", 0},
    {"h3d.mat.$Texture.__constructor__", 0},
    {"h3d.mat.Texture.dispose", 0},
    {"hl.BaseType.check", 0},
    {"h3d.impl.TextureCache.allocTarget", 0},
    {"$String.__alloc__", 0},
    {"hscript.$Parser.__constructor__", 0},
    {"hscript.Parser.parseString", 0},
    {"hscript.$Interp.__constructor__", 0},
    {"hscript.Interp.execute", 0},
    {"haxe.ds.StringMap.set", 0},
    {"haxe.ds.$StringMap.__constructor__", 0},
    {"$Type.resolveClass", 0},
};
enum { F_DRAW, F_PROCESS, F_SYNC, F_FILTER, F_CAMUPDATE, F_TEXNEW, F_TEXDISPOSE, F_CHECK, F_ALLOCTARGET, F_ALLOC,
       F_PARSER, F_PARSE, F_INTERP, F_EXECUTE, F_SET, F_MAP, F_RESOLVE, F_COUNT };

#define EXPR "virtual<e,line,origin,pmax,pmin>"

typedef struct {
    HlCode *c;
    int ok;
} Look;

static int fld(Look *l, int type, const char *name, int *ftype) {
    int fid = type >= 0 ? hl_type_field(l->c, type, name) : -1;
    int t = fid >= 0 ? hl_field_type(l->c, type, fid) : -1;
    if (ftype) *ftype = t;
    if (fid < 0 || t < 0) {
        if (l->ok) wt_log("  %s has no field %s", type >= 0 ? hl_type_name(l->c, type) : "(missing type)", name);
        l->ok = 0;
    }
    return fid;
}

static int ty(Look *l, int kind, const char *name) {
    int t = hl_find_type(l->c, kind, name);
    if (t < 0) {
        if (l->ok) wt_log("  type %s is missing", name ? name : "(unnamed)");
        l->ok = 0;
    }
    return t;
}

static int fun_type(Look *l, int nargs, int a0, int a1, int ret, const char *what) {
    int t = wt_fun_type(l->c, nargs, a0, a1, ret);
    if (t < 0) {
        if (l->ok) wt_log("  no function type %s in this build", what);
        l->ok = 0;
    }
    return t;
}

static int class_global(Look *l, int type) {
    int g = type >= 0 ? l->c->types[type].global - 1 : -1;
    if (g < 0) {
        if (l->ok) wt_log("  %s has no class global", type >= 0 ? hl_type_name(l->c, type) : "(missing type)");
        l->ok = 0;
    }
    return g;
}

static int construct(Look *l, int type, const char *name) {
    const HlType *t = type >= 0 ? &l->c->types[type] : NULL;
    for (int i = 0; t && i < t->nconstructs; i++) {
        const HlStr *s = &l->c->strings[t->constructs[i]];
        if (s->len == (int)strlen(name) && !memcmp(s->s, name, s->len)) return i;
    }
    if (l->ok) wt_log("  enum %s has no %s", t ? hl_type_name(l->c, type) : "(missing)", name);
    l->ok = 0;
    return -1;
}

static int method_slot(Look *l, int type, const char *name) {
    for (int t = type; t >= 0; t = l->c->types[t].super) {
        const HlType *h = &l->c->types[t];
        for (int i = 0; i < h->nproto; i++) {
            const HlStr *s = &l->c->strings[h->proto[3 * i]];
            if (s->len == (int)strlen(name) && !memcmp(s->s, name, s->len)) return h->proto[3 * i + 2];
        }
    }
    if (l->ok) wt_log("  %s has no method %s", type >= 0 ? hl_type_name(l->c, type) : "(missing type)", name);
    l->ok = 0;
    return -1;
}

static int findex_of(Look *l, const char *name) {
    int f = hl_find_function(l->c, name);
    if (f < 0) {
        if (l->ok) wt_log("  %s is not in this build", name);
        l->ok = 0;
        return -1;
    }
    return wt_findex(l->c, f);
}

typedef struct {
    int f_due, f_source, f_on, f_check, f_state_get, f_state_set, f_done, f_error, f_begin, f_put, f_get, f_fit, f_plan,
        f_flag, f_set, f_marked;
} Natives;

// A catch block whose error may itself fail to print: the second trap reports it without its text.
static void catch_block(Asm *a, const Natives *n, int l_catch, int l_catch2, int rerr, int rerr2, int rnull, int rvoid) {
    mark(a, l_catch);
    jump(a, wt_op(OTrap, 2, rerr2, 0), l_catch2);
    op(a, wt_op(OCall1, 3, rvoid, n->f_error, rerr));
    op(a, wt_op(OEndTrap, 1, 1));
    op(a, wt_op(ORet, 1, rvoid));
    mark(a, l_catch2);
    op(a, wt_op(ONull, 1, rnull));
    op(a, wt_op(OCall1, 3, rvoid, n->f_error, rnull));
    op(a, wt_op(ORet, 1, rvoid));
}

// Setup: (battle.Battle)->void, runs the setup script when the native half says it is due; null puts marks back.
enum { S_B, S_VOID, S_ZERO, S_LEN, S_BATTLE, S_BYTES, S_SOURCE, S_PARSER, S_ORIGIN, S_LINE, S_EXPR, S_INTERP, S_VARS,
       S_KEYB, S_KEYL, S_KEY, S_VAL, S_RESOLVE, S_NUM, S_STATE, S_MAP, S_RESULT, S_ERR, S_ERR2, S_NULL, S_NREGS };

typedef struct {
    Asm *a;
    HlEdit *e;
    int f_alloc, f_set;
} Binder;

static void bind(Binder *b, const char *name, int value) {
    op(b->a, wt_op(OString, 2, S_KEYB, hl_edit_string(b->e, name)));
    op(b->a, wt_op(OInt, 2, S_KEYL, hl_edit_int(b->e, (int)strlen(name))));
    op(b->a, wt_op(OCall2, 4, S_KEY, b->f_alloc, S_KEYB, S_KEYL));
    op(b->a, wt_op(OCall3, 5, S_VOID, b->f_set, S_VARS, S_KEY, value));
}

static int add_setup(HlCode *c, HlEdit *e, const int *fn, const Natives *n, Look *l, int tbattle, int file, int line) {
    const HlType *parse = &c->types[c->functions[fn[F_PARSE]].type];
    int tvoid = ty(l, HVOID, NULL), ti32 = ty(l, HI32, NULL), tbytes = ty(l, HBYTES, NULL), tdyn = ty(l, HDYN, NULL);
    int tinterp = c->types[c->functions[fn[F_INTERP]].type].args[0], tvars;
    int f_vars = fld(l, tinterp, "variables", &tvars);
    int tmap = c->types[c->functions[fn[F_MAP]].type].args[0];
    int t_setup = fun_type(l, 1, tbattle, -1, tvoid, "(battle.Battle)->void");
    if (!l->ok || tvars != c->types[c->functions[fn[F_SET]].type].args[0]) return -1;
    const int regs[S_NREGS] = {tbattle, tvoid, ti32, ti32, tdyn, tbytes, parse->args[1], parse->args[0],
                               parse->args[1], parse->args[3], parse->ret, tinterp, tvars, tbytes, ti32,
                               parse->args[1], tdyn, c->functions[fn[F_RESOLVE]].type, ti32, tdyn, tmap, tdyn, tdyn,
                               tdyn, tdyn};
    enum { L_OUT, L_HAVE, L_CATCH, L_CATCH2 };
    Asm a;
    asm_begin(&a, 128);
    Binder b = {&a, e, wt_findex(c, fn[F_ALLOC]), wt_findex(c, fn[F_SET])};
    jump(&a, wt_op(OTrap, 2, S_ERR, 0), L_CATCH);
    op(&a, wt_op(OInt, 2, S_ZERO, hl_edit_int(e, 0)));
    op(&a, wt_op(OMov, 2, S_BATTLE, S_B)); // objects already are dynamics; ToDyn would box
    op(&a, wt_op(OCall1, 3, S_LEN, n->f_due, S_BATTLE));
    jump(&a, wt_op(OJSLt, 3, S_LEN, S_ZERO, 0), L_OUT);
    op(&a, wt_op(OCall1, 3, S_BYTES, n->f_source, S_LEN));
    op(&a, wt_op(OCall2, 4, S_SOURCE, b.f_alloc, S_BYTES, S_LEN));
    op(&a, wt_op(ONew, 1, S_PARSER));
    op(&a, wt_op(OCall1, 3, S_VOID, wt_findex(c, fn[F_PARSER]), S_PARSER));
    op(&a, wt_op(ONull, 1, S_ORIGIN));
    op(&a, wt_op(ONull, 1, S_LINE));
    op(&a, wt_op(OCall4, 6, S_EXPR, wt_findex(c, fn[F_PARSE]), S_PARSER, S_SOURCE, S_ORIGIN, S_LINE));
    op(&a, wt_op(ONew, 1, S_INTERP));
    op(&a, wt_op(OCall1, 3, S_VOID, wt_findex(c, fn[F_INTERP]), S_INTERP));
    op(&a, wt_op(OField, 3, S_VARS, S_INTERP, f_vars));
    bind(&b, "battle", S_BATTLE);
    op(&a, wt_op(OStaticClosure, 2, S_RESOLVE, wt_findex(c, fn[F_RESOLVE])));
    op(&a, wt_op(OMov, 2, S_VAL, S_RESOLVE));
    bind(&b, "cls", S_VAL);
    op(&a, wt_op(OCall0, 2, S_NUM, n->f_on));
    op(&a, wt_op(OToDyn, 2, S_VAL, S_NUM));
    bind(&b, "on", S_VAL);
    op(&a, wt_op(OCall0, 2, S_NUM, n->f_check));
    op(&a, wt_op(OToDyn, 2, S_VAL, S_NUM));
    bind(&b, "check", S_VAL);
    op(&a, wt_op(OCall0, 2, S_STATE, n->f_state_get));
    jump(&a, wt_op(OJNotNull, 2, S_STATE, 0), L_HAVE);
    op(&a, wt_op(ONew, 1, S_MAP));
    op(&a, wt_op(OCall1, 3, S_VOID, wt_findex(c, fn[F_MAP]), S_MAP));
    op(&a, wt_op(OMov, 2, S_STATE, S_MAP));
    op(&a, wt_op(OCall1, 3, S_VOID, n->f_state_set, S_STATE));
    mark(&a, L_HAVE);
    bind(&b, "state", S_STATE);
    op(&a, wt_op(OCall2, 4, S_RESULT, wt_findex(c, fn[F_EXECUTE]), S_INTERP, S_EXPR));
    op(&a, wt_op(OCall1, 3, S_VOID, n->f_done, S_RESULT));
    mark(&a, L_OUT);
    op(&a, wt_op(OEndTrap, 1, 1));
    op(&a, wt_op(ORet, 1, S_VOID));
    catch_block(&a, n, L_CATCH, L_CATCH2, S_ERR, S_ERR2, S_NULL, S_VOID);
    resolve(&a);
    HlFun *f = hl_fun_new(t_setup, S_NREGS, regs, a.ops, a.n, file, line);
    free(a.ops);
    int findex = f ? hl_edit_add(e, f) : -1;
    hl_fun_free(f);
    return findex;
}

// P: ()->void, first in Shadows.filterPasses: outside a battle, the marks go back, or static shadow modes draw them.
enum { P_VOID, P_MARKED, P_GAMECLS, P_GAME, P_BATTLE, P_MODE, P_BMODE, P_NREGS };

static int add_putback(HlCode *c, HlEdit *e, const Natives *n, Look *l, int f_setup, int tbattle, int file, int line) {
    int tvoid = ty(l, HVOID, NULL), tbool = ty(l, HBOOL, NULL), tgamecls = ty(l, HOBJ, "$Game"), tgame, tmode, tx;
    int f_inst = fld(l, tgamecls, "inst", &tgame), f_battle = fld(l, tgame, "battle", &tx);
    int f_gmode = fld(l, tgame, "mode", &tmode);
    int g_game = class_global(l, tgame);
    int t_putback = fun_type(l, 0, -1, -1, tvoid, "()->void");
    if (!l->ok) return -1;
    const int regs[P_NREGS] = {tvoid, tbool, tgamecls, tgame, tbattle, tmode, tmode};
    enum { L_OUT, L_PUTBACK };
    Asm a;
    asm_begin(&a, 16);
    op(&a, wt_op(OCall0, 2, P_MARKED, n->f_marked));
    jump(&a, wt_op(OJFalse, 2, P_MARKED, 0), L_OUT);
    op(&a, wt_op(OGetGlobal, 2, P_GAMECLS, g_game));
    op(&a, wt_op(OField, 3, P_GAME, P_GAMECLS, f_inst));
    jump(&a, wt_op(OJNull, 2, P_GAME, 0), L_PUTBACK);
    op(&a, wt_op(OField, 3, P_BATTLE, P_GAME, f_battle));
    op(&a, wt_op(OField, 3, P_MODE, P_GAME, f_gmode));
    op(&a, wt_op(OUnsafeCast, 2, P_BMODE, P_BATTLE));
    jump(&a, wt_op(OJEq, 3, P_MODE, P_BMODE, 0), L_OUT);
    mark(&a, L_PUTBACK);
    op(&a, wt_op(ONull, 1, P_BATTLE));
    op(&a, wt_op(OCall1, 3, P_VOID, f_setup, P_BATTLE));
    mark(&a, L_OUT);
    op(&a, wt_op(ORet, 1, P_VOID));
    resolve(&a);
    HlFun *f = hl_fun_new(t_putback, P_NREGS, regs, a.ops, a.n, file, line);
    free(a.ops);
    int findex = f ? hl_edit_add(e, f) : -1;
    hl_fun_free(f);
    return findex;
}

// A: (DirShadowMap)->void, after Heaps fits the light camera and before the cull uses it.
enum { A_THIS, A_VOID, A_ZERO, A_ID, A_ERR, A_ERR2, A_NULL, A_GAMECLS, A_GAME, A_BATTLE, A_MODE, A_BMODE, A_LOADING,
       A_READY, A_MAINCLS, A_LOAD2D, A_LIGHT, A_LCLS, A_LDYN, A_ISL, A_PL, A_MAIN, A_SMODE, A_IDX, A_TWO, A_BLUR, A_RAD,
       A_ZF, A_CTX, A_FRAME, A_BDYN, A_ACT, A_CAM, A_VEC, A_BOX, A_MAT, A_F, A_FIT, A_NREGS };

typedef struct {
    Asm *a;
    HlEdit *e;
    int f_put, f_get;
} Slots;

static void put(Slots *s, int slot, int obj, int fid) {
    op(s->a, wt_op(OField, 3, A_F, obj, fid));
    op(s->a, wt_op(OInt, 2, A_ID, hl_edit_int(s->e, slot)));
    op(s->a, wt_op(OCall2, 4, A_VOID, s->f_put, A_ID, A_F));
}

static void get(Slots *s, int slot, int obj, int fid) {
    op(s->a, wt_op(OInt, 2, A_ID, hl_edit_int(s->e, slot)));
    op(s->a, wt_op(OCall1, 3, A_F, s->f_get, A_ID));
    op(s->a, wt_op(OSetField, 3, obj, fid, A_F));
}

static int add_fit(HlCode *c, HlEdit *e, const int *fn, const Natives *n, Look *l, int f_setup, int tdsm, int tbattle,
                   int file, int line) {
    int tvoid = ty(l, HVOID, NULL), ti32 = ty(l, HI32, NULL), tf64 = ty(l, HF64, NULL), tdyn = ty(l, HDYN, NULL);
    int tbool = ty(l, HBOOL, NULL), tgamecls = ty(l, HOBJ, "$Game"), tmaincls = ty(l, HOBJ, "$Main");
    int tpbrlight = ty(l, HOBJ, "h3d.scene.pbr.Light");
    int tgame, tmode, tbmode, tloading, tx, tload2d, tlight, tsmode, tblur, tctx, tcam, tvec, tbox, tmat;
    int f_inst = fld(l, tgamecls, "inst", &tgame), f_battle = fld(l, tgame, "battle", &tx);
    int f_gmode = fld(l, tgame, "mode", &tmode), f_loading = fld(l, tgame, "loadingScreen", &tloading);
    int f_ready = fld(l, tbattle, "battleInit", &tx), f_load2d = fld(l, tmaincls, "loadingS2d", &tload2d);
    int f_light = fld(l, tdsm, "light", &tlight), f_ismain = fld(l, tpbrlight, "isMainLight", &tx);
    int f_smode = fld(l, tdsm, "mode", &tsmode), f_blur = fld(l, tdsm, "blur", &tblur);
    int f_radius = fld(l, tblur, "radius", &tx), f_ctx = fld(l, tdsm, "ctx", &tctx), f_frame = fld(l, tctx, "frame", &tx);
    int f_cam = fld(l, tdsm, "lightCamera", &tcam), f_pos = fld(l, tcam, "pos", &tvec);
    int f_target = fld(l, tcam, "target", &tx), f_box = fld(l, tcam, "orthoBounds", &tbox);
    int f_mcam = fld(l, tcam, "mcam", &tmat);
    int f_xyz[3] = {fld(l, tvec, "x", NULL), fld(l, tvec, "y", NULL), fld(l, tvec, "z", NULL)};
    static const char *const box_names[6] = {"xMin", "xMax", "yMin", "yMax", "zMin", "zMax"};
    static const char *const rot_names[9] = {"_11", "_12", "_13", "_21", "_22", "_23", "_31", "_32", "_33"};
    int f_boxes[6], f_rot[9];
    for (int i = 0; i < 6; i++) f_boxes[i] = fld(l, tbox, box_names[i], NULL);
    for (int i = 0; i < 9; i++) f_rot[i] = fld(l, tmat, rot_names[i], NULL);
    int g_game = class_global(l, tgame), g_main = -1, g_light = class_global(l, tpbrlight);
    for (int g = 0; tmaincls >= 0 && g < c->nglobals; g++)
        if (c->globals[g] == tmaincls) g_main = g;
    int i_dynamic = construct(l, tsmode, "Dynamic");
    int t_fit = fun_type(l, 1, tdsm, -1, tvoid, "(h3d.pass.DirShadowMap)->void");
    if (!l->ok || g_main < 0) {
        if (g_main < 0) wt_log("  $Main has no global");
        return -1;
    }
    tbmode = tmode;
    const int regs[A_NREGS] = {tdsm,     tvoid,   ti32,     ti32,    tdyn,   tdyn,  tdyn,   tgamecls, tgame, tbattle,
                               tmode,    tbmode,  tloading, tbool,   tmaincls, tload2d, tlight, c->globals[g_light],
                               tdyn,     tbool,   tpbrlight, tbool,  tsmode, ti32,  ti32,   tblur,    tf64,  tf64,
                               tctx,     ti32,    tdyn,     ti32,    tcam,   tvec,  tbox,   tmat,     tf64,  ti32};
    enum { L_OUT, L_NOBLUR, L_CATCH, L_CATCH2 };
    Asm a;
    asm_begin(&a, 256);
    Slots s = {&a, e, n->f_put, n->f_get};
    jump(&a, wt_op(OTrap, 2, A_ERR, 0), L_CATCH);
    op(&a, wt_op(OInt, 2, A_ZERO, hl_edit_int(e, 0)));
    // Only in a battle that is built and shown.
    op(&a, wt_op(OGetGlobal, 2, A_GAMECLS, g_game));
    op(&a, wt_op(OField, 3, A_GAME, A_GAMECLS, f_inst));
    jump(&a, wt_op(OJNull, 2, A_GAME, 0), L_OUT);
    op(&a, wt_op(OField, 3, A_BATTLE, A_GAME, f_battle));
    jump(&a, wt_op(OJNull, 2, A_BATTLE, 0), L_OUT);
    op(&a, wt_op(OField, 3, A_MODE, A_GAME, f_gmode));
    op(&a, wt_op(OUnsafeCast, 2, A_BMODE, A_BATTLE));
    jump(&a, wt_op(OJNotEq, 3, A_MODE, A_BMODE, 0), L_OUT);
    op(&a, wt_op(OField, 3, A_LOADING, A_GAME, f_loading));
    jump(&a, wt_op(OJNotNull, 2, A_LOADING, 0), L_OUT);
    op(&a, wt_op(OField, 3, A_READY, A_BATTLE, f_ready));
    jump(&a, wt_op(OJFalse, 2, A_READY, 0), L_OUT);
    op(&a, wt_op(OGetGlobal, 2, A_MAINCLS, g_main));
    op(&a, wt_op(OField, 3, A_LOAD2D, A_MAINCLS, f_load2d));
    jump(&a, wt_op(OJNotNull, 2, A_LOAD2D, 0), L_OUT);
    // The sun is the main pbr light.
    op(&a, wt_op(OField, 3, A_LIGHT, A_THIS, f_light));
    jump(&a, wt_op(OJNull, 2, A_LIGHT, 0), L_OUT);
    op(&a, wt_op(OGetGlobal, 2, A_LCLS, g_light));
    op(&a, wt_op(OMov, 2, A_LDYN, A_LIGHT));
    op(&a, wt_op(OCall2, 4, A_ISL, wt_findex(c, fn[F_CHECK]), A_LCLS, A_LDYN));
    jump(&a, wt_op(OJFalse, 2, A_ISL, 0), L_OUT);
    op(&a, wt_op(OUnsafeCast, 2, A_PL, A_LIGHT));
    op(&a, wt_op(OField, 3, A_MAIN, A_PL, f_ismain));
    jump(&a, wt_op(OJFalse, 2, A_MAIN, 0), L_OUT);
    op(&a, wt_op(OCall1, 3, A_VOID, f_setup, A_BATTLE));
    // Heaps' own static modes and a blurred map are left as they are.
    op(&a, wt_op(OField, 3, A_SMODE, A_THIS, f_smode));
    jump(&a, wt_op(OJNull, 2, A_SMODE, 0), L_OUT);
    op(&a, wt_op(OEnumIndex, 2, A_IDX, A_SMODE));
    op(&a, wt_op(OInt, 2, A_TWO, hl_edit_int(e, i_dynamic)));
    jump(&a, wt_op(OJNotEq, 3, A_IDX, A_TWO, 0), L_OUT);
    op(&a, wt_op(OField, 3, A_BLUR, A_THIS, f_blur));
    jump(&a, wt_op(OJNull, 2, A_BLUR, 0), L_NOBLUR);
    op(&a, wt_op(OField, 3, A_RAD, A_BLUR, f_radius));
    op(&a, wt_op(OFloat, 2, A_ZF, hl_edit_float(e, 0)));
    jump(&a, wt_op(OJSLt, 3, A_ZF, A_RAD, 0), L_OUT);
    mark(&a, L_NOBLUR);
    op(&a, wt_op(OField, 3, A_CTX, A_THIS, f_ctx));
    jump(&a, wt_op(OJNull, 2, A_CTX, 0), L_OUT);
    op(&a, wt_op(OField, 3, A_FRAME, A_CTX, f_frame));
    op(&a, wt_op(OMov, 2, A_BDYN, A_BATTLE));
    op(&a, wt_op(OCall2, 4, A_ACT, n->f_begin, A_BDYN, A_FRAME));
    jump(&a, wt_op(OJSLte, 3, A_ACT, A_ZERO, 0), L_OUT);
    op(&a, wt_op(OField, 3, A_CAM, A_THIS, f_cam));
    jump(&a, wt_op(OJNull, 2, A_CAM, 0), L_OUT);
    // The JIT does not check OField for null; the compiler's NullCheck does.
    op(&a, wt_op(OField, 3, A_VEC, A_CAM, f_pos));
    jump(&a, wt_op(OJNull, 2, A_VEC, 0), L_OUT);
    for (int i = 0; i < 3; i++) put(&s, SHADOW_POS + i, A_VEC, f_xyz[i]);
    op(&a, wt_op(OField, 3, A_VEC, A_CAM, f_target));
    jump(&a, wt_op(OJNull, 2, A_VEC, 0), L_OUT);
    for (int i = 0; i < 3; i++) put(&s, SHADOW_TARGET + i, A_VEC, f_xyz[i]);
    op(&a, wt_op(OField, 3, A_BOX, A_CAM, f_box));
    jump(&a, wt_op(OJNull, 2, A_BOX, 0), L_OUT);
    for (int i = 0; i < 6; i++) put(&s, SHADOW_BOX + i, A_BOX, f_boxes[i]);
    op(&a, wt_op(OField, 3, A_MAT, A_CAM, f_mcam));
    jump(&a, wt_op(OJNull, 2, A_MAT, 0), L_OUT);
    for (int i = 0; i < 9; i++) put(&s, SHADOW_ROT + i, A_MAT, f_rot[i]);
    op(&a, wt_op(OCall0, 2, A_FIT, n->f_fit));
    jump(&a, wt_op(OJSLte, 3, A_FIT, A_ZERO, 0), L_OUT);
    op(&a, wt_op(OField, 3, A_VEC, A_CAM, f_pos));
    for (int i = 0; i < 3; i++) get(&s, SHADOW_POS + i, A_VEC, f_xyz[i]);
    op(&a, wt_op(OField, 3, A_VEC, A_CAM, f_target));
    for (int i = 0; i < 3; i++) get(&s, SHADOW_TARGET + i, A_VEC, f_xyz[i]);
    op(&a, wt_op(OField, 3, A_BOX, A_CAM, f_box));
    for (int i = 0; i < 6; i++) get(&s, SHADOW_BOX + i, A_BOX, f_boxes[i]);
    op(&a, wt_op(OCall1, 3, A_VOID, wt_findex(c, fn[F_CAMUPDATE]), A_CAM));
    mark(&a, L_OUT);
    op(&a, wt_op(OEndTrap, 1, 1));
    op(&a, wt_op(ORet, 1, A_VOID));
    catch_block(&a, n, L_CATCH, L_CATCH2, A_ERR, A_ERR2, A_NULL, A_VOID);
    resolve(&a);
    if (!l->ok) {
        free(a.ops);
        return -1;
    }
    HlFun *f = hl_fun_new(t_fit, A_NREGS, regs, a.ops, a.n, file, line);
    free(a.ops);
    int findex = f ? hl_edit_add(e, f) : -1;
    hl_fun_free(f);
    return findex;
}

// B: draw's type, after the light frustum cull: static casters leave the list, drawn into the static map when due.
enum { B_THIS, B_PASSES, B_SORT, B_VOID, B_ERR, B_ERR2, B_NULL, B_ID, B_ZERO, B_ACT, B_P, B_NXT, B_PASS, B_ST, B_SH,
       B_STT, B_DH, B_DT, B_NULLP, B_COUNT, B_SIG, B_OBJ, B_M, B_F, B_K, B_S, B_TT, B_SIZE, B_W, B_FMT, B_FMT2,
       B_CREATED, B_N, B_TY, B_RAW, B_TGT, B_I, B_ARR, B_LEN, B_FC, B_PLAN, B_TWO, B_TMP, B_RES, B_Q, B_DISC, B_LD,
       B_NREGS };

static int add_split(HlCode *c, HlEdit *e, const int *fn, const Natives *n, Look *l, int tdsm, int g_format, int file,
                     int line) {
    const HlType *draw = &c->types[c->functions[fn[F_DRAW]].type];
    int tpasslist = draw->args[1], tsort = draw->args[2];
    int tvoid = ty(l, HVOID, NULL), ti32 = ty(l, HI32, NULL), tf64 = ty(l, HF64, NULL), tdyn = ty(l, HDYN, NULL);
    int tbool = ty(l, HBOOL, NULL), ttype = ty(l, HTYPE, NULL), tarray = ty(l, HARRAY, NULL);
    int tarrobj = ty(l, HOBJ, "hl.types.ArrayObj"), tflags = ty(l, HENUM, "h3d.mat.TextureFlags");
    int tpassobj, tpass, tobj, tmat, ttex, ttt, tfmt, tx;
    int f_current = fld(l, tpasslist, "current", &tpassobj), f_discarded = fld(l, tpasslist, "discarded", &tx);
    int f_lastdisc = fld(l, tpasslist, "lastDisc", &tx), f_next = fld(l, tpassobj, "next", &tx);
    int f_pass = fld(l, tpassobj, "pass", &tpass), f_obj = fld(l, tpassobj, "obj", &tobj);
    int f_isstatic = fld(l, tpass, "isStatic", &tx), f_abspos = fld(l, tobj, "absPos", &tmat);
    int f_static = fld(l, tdsm, "staticTexture", &ttex), f_size = fld(l, tdsm, "size", &tx);
    int f_t = fld(l, ttex, "t", &ttt), f_width = fld(l, ttex, "width", &tx), f_height = fld(l, ttex, "height", &tx);
    int f_format = fld(l, ttex, "format", &tfmt), f_array = fld(l, tarrobj, "array", &tx);
    int f_length = fld(l, tarrobj, "length", &tx);
    static const char *const sig_names[6] = {"_41", "_42", "_43", "_11", "_22", "_33"};
    static const double sig_weights[6] = {1, 3, 7, 11, 13, 17};
    int f_sig[6];
    for (int i = 0; i < 6; i++) f_sig[i] = fld(l, tmat, sig_names[i], NULL);
    int i_target = construct(l, tflags, "Target");
    int na = hl_find_native(c, "std", "alloc_array"), f_alloc_array = na >= 0 ? c->natives[na].findex : -1;
    if (!l->ok || f_alloc_array < 0 || c->globals[g_format] != tfmt ||
        c->types[c->functions[fn[F_TEXNEW]].type].args[3] != tarrobj) {
        wt_log("  std@alloc_array, the shadow map's format or Texture's constructor is not as expected");
        return -1;
    }
    const int regs[B_NREGS] = {tdsm,     tpasslist, tsort,    tvoid,    tdyn,     tdyn,     tdyn,     ti32,
                               ti32,     tbool,     tpassobj, tpassobj, tpass,    tbool,    tpassobj, tpassobj,
                               tpassobj, tpassobj,  tpassobj, ti32,     tf64,     tobj,     tmat,     tf64,
                               tf64,     ttex,      ttt,      ti32,     ti32,     tfmt,     tfmt,     ti32,
                               ti32,     ttype,     tarray,   tflags,   ti32,     tarrobj,  ti32,     tf64,
                               ti32,     ti32,      tpasslist, ttex,    tpassobj, tpassobj, tpassobj};
    enum { L_OUT, L_LOOP, L_DONE, L_S2, L_S3, L_DYN, L_D2, L_D3, L_NEXT, L_N1, L_N2, L_MAKE, L_MK2, L_HAVE, L_RESTORE,
           L_TAIL, L_ATTACH, L_AT1, L_AT2, L_AT3, L_KEEP, L_CATCH, L_CATCH2 };
    Asm a;
    asm_begin(&a, 256);
    jump(&a, wt_op(OTrap, 2, B_ERR, 0), L_CATCH);
    op(&a, wt_op(OInt, 2, B_ZERO, hl_edit_int(e, 0)));
    op(&a, wt_op(OInt, 2, B_ID, hl_edit_int(e, SHADOW_ACTIVE)));
    op(&a, wt_op(OCall1, 3, B_ACT, n->f_flag, B_ID));
    jump(&a, wt_op(OJFalse, 2, B_ACT, 0), L_OUT);
    op(&a, wt_op(ONull, 1, B_SH));
    op(&a, wt_op(ONull, 1, B_STT));
    op(&a, wt_op(ONull, 1, B_DH));
    op(&a, wt_op(ONull, 1, B_DT));
    op(&a, wt_op(ONull, 1, B_NULLP));
    op(&a, wt_op(OInt, 2, B_COUNT, hl_edit_int(e, 0)));
    op(&a, wt_op(OFloat, 2, B_SIG, hl_edit_float(e, 0)));
    op(&a, wt_op(OField, 3, B_P, B_PASSES, f_current));
    // The static casters' count and places fold into a signature that shows when they change.
    mark(&a, L_LOOP);
    op(&a, wt_op(OLabel, 0));
    jump(&a, wt_op(OJNull, 2, B_P, 0), L_DONE);
    op(&a, wt_op(OField, 3, B_NXT, B_P, f_next));
    op(&a, wt_op(OField, 3, B_PASS, B_P, f_pass));
    jump(&a, wt_op(OJNull, 2, B_PASS, 0), L_DYN);
    op(&a, wt_op(OField, 3, B_ST, B_PASS, f_isstatic));
    jump(&a, wt_op(OJFalse, 2, B_ST, 0), L_DYN);
    jump(&a, wt_op(OJNotNull, 2, B_SH, 0), L_S2);
    op(&a, wt_op(OMov, 2, B_SH, B_P));
    jump(&a, wt_op(OJAlways, 1, 0), L_S3);
    mark(&a, L_S2);
    op(&a, wt_op(OSetField, 3, B_STT, f_next, B_P));
    mark(&a, L_S3);
    op(&a, wt_op(OMov, 2, B_STT, B_P));
    op(&a, wt_op(OIncr, 1, B_COUNT));
    op(&a, wt_op(OField, 3, B_OBJ, B_P, f_obj));
    jump(&a, wt_op(OJNull, 2, B_OBJ, 0), L_NEXT);
    op(&a, wt_op(OField, 3, B_M, B_OBJ, f_abspos));
    jump(&a, wt_op(OJNull, 2, B_M, 0), L_NEXT);
    for (int i = 0; i < 6; i++) {
        op(&a, wt_op(OField, 3, B_F, B_M, f_sig[i]));
        if (sig_weights[i] != 1) {
            op(&a, wt_op(OFloat, 2, B_K, hl_edit_float(e, sig_weights[i])));
            op(&a, wt_op(OMul, 3, B_F, B_F, B_K));
        }
        op(&a, wt_op(OAdd, 3, B_SIG, B_SIG, B_F));
    }
    jump(&a, wt_op(OJAlways, 1, 0), L_NEXT);
    mark(&a, L_DYN);
    jump(&a, wt_op(OJNotNull, 2, B_DH, 0), L_D2);
    op(&a, wt_op(OMov, 2, B_DH, B_P));
    jump(&a, wt_op(OJAlways, 1, 0), L_D3);
    mark(&a, L_D2);
    op(&a, wt_op(OSetField, 3, B_DT, f_next, B_P));
    mark(&a, L_D3);
    op(&a, wt_op(OMov, 2, B_DT, B_P));
    mark(&a, L_NEXT);
    op(&a, wt_op(OMov, 2, B_P, B_NXT));
    jump(&a, wt_op(OJAlways, 1, 0), L_LOOP);
    mark(&a, L_DONE);
    jump(&a, wt_op(OJNull, 2, B_STT, 0), L_N1);
    op(&a, wt_op(OSetField, 3, B_STT, f_next, B_NULLP));
    mark(&a, L_N1);
    jump(&a, wt_op(OJNull, 2, B_DT, 0), L_N2);
    op(&a, wt_op(OSetField, 3, B_DT, f_next, B_NULLP));
    mark(&a, L_N2);
    op(&a, wt_op(OSetField, 3, B_PASSES, f_current, B_DH));
    // staticTexture: unused in Dynamic mode, disposed with the map; made like the shadow map so it can be copied in.
    op(&a, wt_op(OInt, 2, B_CREATED, hl_edit_int(e, 0)));
    op(&a, wt_op(OField, 3, B_S, B_THIS, f_static));
    op(&a, wt_op(OField, 3, B_SIZE, B_THIS, f_size));
    op(&a, wt_op(OGetGlobal, 2, B_FMT, g_format));
    jump(&a, wt_op(OJNull, 2, B_S, 0), L_MAKE);
    op(&a, wt_op(OField, 3, B_TT, B_S, f_t));
    jump(&a, wt_op(OJNull, 2, B_TT, 0), L_MAKE);
    op(&a, wt_op(OField, 3, B_W, B_S, f_width));
    jump(&a, wt_op(OJNotEq, 3, B_W, B_SIZE, 0), L_MAKE);
    op(&a, wt_op(OField, 3, B_W, B_S, f_height));
    jump(&a, wt_op(OJNotEq, 3, B_W, B_SIZE, 0), L_MAKE);
    op(&a, wt_op(OField, 3, B_FMT2, B_S, f_format));
    jump(&a, wt_op(OJNotEq, 3, B_FMT2, B_FMT, 0), L_MAKE);
    jump(&a, wt_op(OJAlways, 1, 0), L_HAVE);
    mark(&a, L_MAKE);
    jump(&a, wt_op(OJNull, 2, B_S, 0), L_MK2);
    op(&a, wt_op(OCall1, 3, B_VOID, wt_findex(c, fn[F_TEXDISPOSE]), B_S));
    mark(&a, L_MK2);
    op(&a, wt_op(OInt, 2, B_N, hl_edit_int(e, 1)));
    op(&a, wt_op(OType, 2, B_TY, tflags));
    op(&a, wt_op(OCall2, 4, B_RAW, f_alloc_array, B_TY, B_N));
    op(&a, wt_op(OMakeEnum, 3, B_TGT, i_target, 0));
    op(&a, wt_op(OInt, 2, B_I, hl_edit_int(e, 0)));
    op(&a, wt_op(OSetArray, 3, B_RAW, B_I, B_TGT));
    op(&a, wt_op(ONew, 1, B_ARR));
    op(&a, wt_op(OSetField, 3, B_ARR, f_array, B_RAW));
    op(&a, wt_op(OArraySize, 2, B_LEN, B_RAW));
    op(&a, wt_op(OSetField, 3, B_ARR, f_length, B_LEN));
    op(&a, wt_op(ONew, 1, B_S));
    op(&a, wt_op(OCallN, 8, B_VOID, wt_findex(c, fn[F_TEXNEW]), 5, B_S, B_SIZE, B_SIZE, B_ARR, B_FMT));
    op(&a, wt_op(OSetField, 3, B_THIS, f_static, B_S));
    op(&a, wt_op(OInt, 2, B_CREATED, hl_edit_int(e, 1)));
    mark(&a, L_HAVE);
    op(&a, wt_op(OToSFloat, 2, B_FC, B_COUNT));
    op(&a, wt_op(OInt, 2, B_ID, hl_edit_int(e, SHADOW_COUNT)));
    op(&a, wt_op(OCall2, 4, B_VOID, n->f_put, B_ID, B_FC));
    op(&a, wt_op(OInt, 2, B_ID, hl_edit_int(e, SHADOW_SIG)));
    op(&a, wt_op(OCall2, 4, B_VOID, n->f_put, B_ID, B_SIG));
    op(&a, wt_op(OCall1, 3, B_PLAN, n->f_plan, B_CREATED));
    jump(&a, wt_op(OJSLte, 3, B_PLAN, B_ZERO, 0), L_RESTORE);
    op(&a, wt_op(OInt, 2, B_TWO, hl_edit_int(e, 2)));
    jump(&a, wt_op(OJNotEq, 3, B_PLAN, B_TWO, 0), L_ATTACH);
    // The static casters alone, through the game's processShadowMap, which clears the target.
    op(&a, wt_op(ONew, 1, B_TMP));
    op(&a, wt_op(OSetField, 3, B_TMP, f_current, B_SH));
    op(&a, wt_op(OCall4, 6, B_RES, wt_findex(c, fn[F_PROCESS]), B_THIS, B_TMP, B_S, B_SORT));
    op(&a, wt_op(OField, 3, B_SH, B_TMP, f_current)); // sorted: a new head and tail
    op(&a, wt_op(OMov, 2, B_STT, B_SH));
    jump(&a, wt_op(OJNull, 2, B_STT, 0), L_ATTACH);
    mark(&a, L_TAIL);
    op(&a, wt_op(OLabel, 0));
    op(&a, wt_op(OField, 3, B_Q, B_STT, f_next));
    jump(&a, wt_op(OJNull, 2, B_Q, 0), L_ATTACH);
    op(&a, wt_op(OMov, 2, B_STT, B_Q));
    jump(&a, wt_op(OJAlways, 1, 0), L_TAIL);
    // The static casters go to the discarded list, which LightSystem.drawShadows puts back after the draw.
    mark(&a, L_ATTACH);
    jump(&a, wt_op(OJNull, 2, B_SH, 0), L_KEEP);
    op(&a, wt_op(OField, 3, B_DISC, B_PASSES, f_discarded));
    op(&a, wt_op(OField, 3, B_LD, B_PASSES, f_lastdisc));
    jump(&a, wt_op(OJNull, 2, B_DISC, 0), L_AT1);
    jump(&a, wt_op(OJNotNull, 2, B_LD, 0), L_AT2);
    mark(&a, L_AT1);
    op(&a, wt_op(OSetField, 3, B_PASSES, f_discarded, B_SH));
    jump(&a, wt_op(OJAlways, 1, 0), L_AT3);
    mark(&a, L_AT2);
    op(&a, wt_op(OSetField, 3, B_LD, f_next, B_SH));
    mark(&a, L_AT3);
    op(&a, wt_op(OSetField, 3, B_PASSES, f_lastdisc, B_STT));
    mark(&a, L_KEEP);
    op(&a, wt_op(OInt, 2, B_ID, hl_edit_int(e, SHADOW_KEEP)));
    op(&a, wt_op(OInt, 2, B_N, hl_edit_int(e, 1)));
    op(&a, wt_op(OCall2, 4, B_VOID, n->f_set, B_ID, B_N));
    mark(&a, L_OUT);
    op(&a, wt_op(OEndTrap, 1, 1));
    op(&a, wt_op(ORet, 1, B_VOID));
    // Not the cache's frame: the list as it was, for the game's draw.
    mark(&a, L_RESTORE);
    jump(&a, wt_op(OJNull, 2, B_SH, 0), L_OUT);
    op(&a, wt_op(OSetField, 3, B_STT, f_next, B_DH));
    op(&a, wt_op(OSetField, 3, B_PASSES, f_current, B_SH));
    jump(&a, wt_op(OJAlways, 1, 0), L_OUT);
    catch_block(&a, n, L_CATCH, L_CATCH2, B_ERR, B_ERR2, B_NULL, B_VOID);
    resolve(&a);
    HlFun *f = hl_fun_new(c->functions[fn[F_DRAW]].type, B_NREGS, regs, a.ops, a.n, file, line);
    free(a.ops);
    int findex = f ? hl_edit_add(e, f) : -1;
    hl_fun_free(f);
    return findex;
}

// C: syncShader's type, first in processShadowMap: copies the kept static map in; only a depth map skips its clear.
enum { C_THIS, C_TEX, C_VOID, C_ERR, C_ERR2, C_NULL, C_ID, C_N, C_K, C_S, C_CTX, C_ENG, C_DRV, C_OK, C_NREGS };

static int add_copy(HlCode *c, HlEdit *e, const int *fn, const Natives *n, Look *l, int tdsm, int file, int line) {
    int tvoid = ty(l, HVOID, NULL), ti32 = ty(l, HI32, NULL), tdyn = ty(l, HDYN, NULL), tbool = ty(l, HBOOL, NULL);
    int ttex, tctx, teng, tdrv;
    int f_static = fld(l, tdsm, "staticTexture", &ttex), f_ctx = fld(l, tdsm, "ctx", &tctx);
    int f_engine = fld(l, tctx, "engine", &teng), f_driver = fld(l, teng, "driver", &tdrv);
    int slot = method_slot(l, tdrv, "copyTexture"), f_isdepth = findex_of(l, "h3d.mat.Texture.isDepth");
    const HlType *sync = &c->types[c->functions[fn[F_SYNC]].type];
    if (!l->ok || sync->nargs != 2 || sync->args[0] != tdsm || sync->args[1] != ttex) {
        wt_log("  syncShader is not (DirShadowMap, Texture)");
        return -1;
    }
    const int regs[C_NREGS] = {tdsm, ttex, tvoid, tdyn, tdyn, tdyn, ti32, ti32, tbool, ttex, tctx, teng, tdrv, tbool};
    enum { L_OUT, L_FAIL, L_CATCH, L_CATCH2 };
    Asm a;
    asm_begin(&a, 64);
    jump(&a, wt_op(OTrap, 2, C_ERR, 0), L_CATCH);
    op(&a, wt_op(OInt, 2, C_ID, hl_edit_int(e, SHADOW_KEEP)));
    op(&a, wt_op(OCall1, 3, C_K, n->f_flag, C_ID));
    jump(&a, wt_op(OJFalse, 2, C_K, 0), L_OUT);
    jump(&a, wt_op(OJNull, 2, C_TEX, 0), L_FAIL);
    op(&a, wt_op(OCall1, 3, C_K, f_isdepth, C_TEX));
    jump(&a, wt_op(OJFalse, 2, C_K, 0), L_FAIL);
    op(&a, wt_op(OField, 3, C_S, C_THIS, f_static));
    jump(&a, wt_op(OJNull, 2, C_S, 0), L_FAIL);
    op(&a, wt_op(OField, 3, C_CTX, C_THIS, f_ctx));
    jump(&a, wt_op(OJNull, 2, C_CTX, 0), L_FAIL);
    op(&a, wt_op(OField, 3, C_ENG, C_CTX, f_engine));
    jump(&a, wt_op(OJNull, 2, C_ENG, 0), L_FAIL);
    op(&a, wt_op(OField, 3, C_DRV, C_ENG, f_driver));
    jump(&a, wt_op(OJNull, 2, C_DRV, 0), L_FAIL);
    op(&a, wt_op(OCallMethod, 6, C_OK, slot, 3, C_DRV, C_S, C_TEX));
    jump(&a, wt_op(OJTrue, 2, C_OK, 0), L_OUT);
    mark(&a, L_FAIL);
    op(&a, wt_op(OInt, 2, C_ID, hl_edit_int(e, SHADOW_COPY_FAILED)));
    op(&a, wt_op(OInt, 2, C_N, hl_edit_int(e, 1)));
    op(&a, wt_op(OCall2, 4, C_VOID, n->f_set, C_ID, C_N));
    mark(&a, L_OUT);
    op(&a, wt_op(OEndTrap, 1, 1));
    op(&a, wt_op(ORet, 1, C_VOID));
    catch_block(&a, n, L_CATCH, L_CATCH2, C_ERR, C_ERR2, C_NULL, C_VOID);
    resolve(&a);
    HlFun *f = hl_fun_new(c->functions[fn[F_SYNC]].type, C_NREGS, regs, a.ops, a.n, file, line);
    free(a.ops);
    int findex = f ? hl_edit_add(e, f) : -1;
    hl_fun_free(f);
    return findex;
}

static int is_call_to(const HlOp *o, int findex) {
    return o->op >= OCall0 && o->op <= OCallN && o->a[1] == findex;
}

static int targeted(const HlFun *f, int at) {
    for (int i = 0; i < f->nops; i++) {
        HlOp o = f->ops[i];
        if (!hl_is_jump(o.op)) continue;
        int k = o.op == OSwitch ? o.a[1] : 0;
        for (int j = o.op == OSwitch ? -1 : 0; j < k; j++)
            if (*hl_jump_target(&o, j) == at) return 1;
    }
    return 0;
}

static int add_reg(HlFun *f, int type) {
    f->regs[f->nregs] = type;
    return f->nregs++;
}

static int apply(HlCode *c, HlEdit *e, const int *fn) {
    Look l = {c, 1};
    if (!wt_signature(c, fn[F_DRAW], "(h3d.pass.DirShadowMap,h3d.pass.PassList,(h3d.pass.PassList)->void)->void") ||
        !wt_signature(c, fn[F_PROCESS],
                      "(h3d.pass.DirShadowMap,h3d.pass.PassList,h3d.mat.Texture,(h3d.pass.PassList)->void)->h3d.mat.Texture") ||
        !wt_signature(c, fn[F_SYNC], "(h3d.pass.DirShadowMap,h3d.mat.Texture)->void") ||
        !wt_signature(c, fn[F_FILTER], "(h3d.pass.Shadows,h3d.pass.PassList)->bool") ||
        !wt_signature(c, fn[F_CAMUPDATE], "(h3d.Camera)->void") ||
        !wt_signature(c, fn[F_TEXNEW], "(h3d.mat.Texture,i32,i32,hl.types.ArrayObj,hxd.PixelFormat)->void") ||
        !wt_signature(c, fn[F_TEXDISPOSE], "(h3d.mat.Texture)->void") ||
        !wt_signature(c, fn[F_CHECK], "(hl.BaseType,dynamic)->bool") ||
        !wt_signature(c, fn[F_ALLOC], "(bytes,i32)->String") ||
        !wt_signature(c, fn[F_PARSER], "(hscript.Parser)->void") ||
        !wt_signature(c, fn[F_PARSE], "(hscript.Parser,String,String,null<i32>)->" EXPR) ||
        !wt_signature(c, fn[F_INTERP], "(hscript.Interp)->void") ||
        !wt_signature(c, fn[F_EXECUTE], "(hscript.Interp," EXPR ")->dynamic") ||
        !wt_signature(c, fn[F_SET], "(haxe.ds.StringMap,String,dynamic)->void") ||
        !wt_signature(c, fn[F_MAP], "(haxe.ds.StringMap)->void") ||
        !wt_signature(c, fn[F_RESOLVE], "(String)->hl.Class"))
        return -1;
    int tvoid = ty(&l, HVOID, NULL), ti32 = ty(&l, HI32, NULL), tf64 = ty(&l, HF64, NULL), tdyn = ty(&l, HDYN, NULL);
    int tbool = ty(&l, HBOOL, NULL), tbytes = ty(&l, HBYTES, NULL), tbattle = ty(&l, HOBJ, "battle.Battle");
    int tdsm = c->types[c->functions[fn[F_DRAW]].type].args[0];
    int t_int_void = fun_type(&l, 2, ti32, ti32, tvoid, "(i32,i32)->void");
    int t_flag = fun_type(&l, 1, ti32, -1, tbool, "(i32)->bool");
    int t_put = fun_type(&l, 2, ti32, tf64, tvoid, "(i32,f64)->void");
    int t_get = fun_type(&l, 1, ti32, -1, tf64, "(i32)->f64");
    int t_int = fun_type(&l, 0, -1, -1, ti32, "()->i32");
    int t_plan = fun_type(&l, 1, ti32, -1, ti32, "(i32)->i32");
    int t_begin = fun_type(&l, 2, tdyn, ti32, ti32, "(dynamic,i32)->i32");
    int t_due = fun_type(&l, 1, tdyn, -1, ti32, "(dynamic)->i32");
    int t_source = fun_type(&l, 1, ti32, -1, tbytes, "(i32)->bytes");
    int t_give = fun_type(&l, 0, -1, -1, tdyn, "()->dynamic");
    int t_take = fun_type(&l, 1, tdyn, -1, tvoid, "(dynamic)->void");
    int t_marked = fun_type(&l, 0, -1, -1, tbool, "()->bool");
    if (!l.ok) return -1;
    Natives n = {
        hl_edit_native(e, "wtfix", "shadow_due", t_due),     hl_edit_native(e, "wtfix", "shadow_source", t_source),
        hl_edit_native(e, "wtfix", "shadow_on", t_int),      hl_edit_native(e, "wtfix", "shadow_check", t_int),
        hl_edit_native(e, "wtfix", "shadow_state_get", t_give), hl_edit_native(e, "wtfix", "shadow_state_set", t_take),
        hl_edit_native(e, "wtfix", "shadow_done", t_take),   hl_edit_native(e, "wtfix", "shadow_error", t_take),
        hl_edit_native(e, "wtfix", "shadow_begin", t_begin), hl_edit_native(e, "wtfix", "shadow_put", t_put),
        hl_edit_native(e, "wtfix", "shadow_get", t_get),     hl_edit_native(e, "wtfix", "shadow_fit", t_int),
        hl_edit_native(e, "wtfix", "shadow_plan", t_plan),   hl_edit_native(e, "wtfix", "shadow_flag", t_flag),
        hl_edit_native(e, "wtfix", "shadow_set", t_int_void), hl_edit_native(e, "wtfix", "shadow_marked", t_marked),
    };

    HlFun *draw = hl_fun_decode(c, fn[F_DRAW]), *process = hl_fun_decode(c, fn[F_PROCESS]);
    if (!draw || !process) {
        hl_fun_free(draw), hl_fun_free(process);
        return -1;
    }
    // draw: the map's allocation and format, the cull's last store before it, the camera update before that.
    int alloc_findex = wt_findex(c, fn[F_ALLOCTARGET]), update_findex = wt_findex(c, fn[F_CAMUPDATE]);
    int i_alloc = -1, i_cull_end = -1, i_update = -1, g_format = -1, f_lastdisc = hl_type_field(c, draw->regs[1], "lastDisc");
    for (int i = 0; i < draw->nops; i++)
        if (is_call_to(&draw->ops[i], alloc_findex)) i_alloc = i_alloc < 0 ? i : -2;
    if (i_alloc >= 0 && draw->ops[i_alloc].op == OCallN && draw->ops[i_alloc].a[2] == 7) {
        int rformat = draw->ops[i_alloc].a[3 + 5];
        for (int i = i_alloc - 1; i >= 0 && g_format < 0; i--)
            if (draw->ops[i].a[0] == rformat && draw->ops[i].op != OSetField) g_format = draw->ops[i].op == OGetGlobal ? draw->ops[i].a[1] : -2;
        for (int i = i_alloc - 1; i >= 0 && i_cull_end < 0; i--) {
            const HlOp *o = &draw->ops[i];
            if (o->op == OSetField && o->a[0] == 1 && o->a[1] == f_lastdisc) i_cull_end = i;
            else if (hl_is_jump(o->op) || (o->op >= OCall0 && o->op <= OCallClosure)) break;
        }
        for (int i = i_cull_end - 1; i >= 0 && i_update < 0; i--)
            if (is_call_to(&draw->ops[i], update_findex)) i_update = i;
    }
    int at_split = i_cull_end + 1, at_fit = i_update + 1;
    if (i_alloc < 0 || g_format < 0 || i_cull_end < 0 || i_update < 0 || targeted(draw, at_split) || at_fit >= at_split) {
        wt_log("  DirShadowMap.draw: shadow map allocation, cull or camera update not found as expected (%d %d %d %d)",
               i_alloc, g_format, i_cull_end, i_update);
        hl_fun_free(draw), hl_fun_free(process);
        return -1;
    }
    // processShadowMap: its depth clear, right after pushDepth, skipped by a jump over it.
    int push_findex = findex_of(&l, "h3d.Engine.pushDepth"), clear_findex = findex_of(&l, "h3d.Engine.clear");
    int i_push = -1, i_clear = -1;
    for (int i = 0; l.ok && i < process->nops; i++)
        if (is_call_to(&process->ops[i], push_findex)) i_push = i_push < 0 ? i : -2;
    for (int i = i_push + 1; i_push >= 0 && i < process->nops && i_clear < 0; i++) {
        const HlOp *o = &process->ops[i];
        if (is_call_to(o, clear_findex)) i_clear = i;
        else if (hl_is_jump(o->op) || (o->op >= OCall0 && o->op <= OCallClosure)) break;
    }
    if (i_push < 0 || i_clear < 0 || process->ops[i_clear + 1].op != OJAlways || targeted(process, i_push + 1)) {
        wt_log("  processShadowMap: the depth clear after pushDepth is not as expected (%d %d)", i_push, i_clear);
        hl_fun_free(draw), hl_fun_free(process);
        return -1;
    }

    int file = draw->files[0], line = draw->lines[0];
    int f_setup = add_setup(c, e, fn, &n, &l, tbattle, file, line);
    int f_putback = f_setup >= 0 ? add_putback(c, e, &n, &l, f_setup, tbattle, file, line) : -1;
    int f_fit = f_putback >= 0 ? add_fit(c, e, fn, &n, &l, f_setup, tdsm, tbattle, file, line) : -1;
    int f_split = f_fit >= 0 ? add_split(c, e, fn, &n, &l, tdsm, g_format, file, line) : -1;
    int f_copy = f_split >= 0 ? add_copy(c, e, fn, &n, &l, tdsm, file, line) : -1;
    HlFun *filter = f_copy >= 0 ? hl_fun_decode(c, fn[F_FILTER]) : NULL;
    if (!filter) {
        hl_fun_free(draw), hl_fun_free(process);
        return -1;
    }

    // Later inserts first: each keeps the indices before it.
    int dv = add_reg(draw, tvoid), di = add_reg(draw, ti32), dz = add_reg(draw, ti32);
    HlOp split_call = wt_op(OCall3, 5, dv, f_split, 0, 1, 2);
    hl_fun_insert(draw, at_split, &split_call, 1);
    HlOp fit_call = wt_op(OCall1, 3, dv, f_fit, 0);
    hl_fun_insert(draw, at_fit, &fit_call, 1);
    // Every draw starts inactive; only the fit makes the battle sun's the cache's.
    HlOp reset[] = {wt_op(OInt, 2, di, hl_edit_int(e, SHADOW_ACTIVE)), wt_op(OInt, 2, dz, hl_edit_int(e, 0)),
                    wt_op(OCall2, 4, dv, n.f_set, di, dz)};
    hl_fun_insert(draw, 0, reset, 3);

    int pv = add_reg(process, tvoid), pi = add_reg(process, ti32), pb = add_reg(process, tbool);
    HlOp skip[] = {wt_op(OInt, 2, pi, hl_edit_int(e, SHADOW_KEEP_TAKE)), wt_op(OCall1, 3, pb, n.f_flag, pi),
                   wt_op(OJTrue, 2, pb, i_clear + 1)};
    hl_fun_insert(process, i_push + 1, skip, 3);
    HlOp copy_call = wt_op(OCall2, 4, pv, f_copy, 0, 2);
    hl_fun_insert(process, 0, &copy_call, 1);

    HlOp putback_call = wt_op(OCall0, 2, add_reg(filter, tvoid), f_putback);
    hl_fun_insert(filter, 0, &putback_call, 1);

    int result = !hl_edit_replace(e, fn[F_DRAW], draw) && !hl_edit_replace(e, fn[F_PROCESS], process) &&
                         !hl_edit_replace(e, fn[F_FILTER], filter)
                     ? 0
                     : -1;
    hl_fun_free(draw), hl_fun_free(process), hl_fun_free(filter);
    return result;
}

const WtFix fix_shadowcache = {"shadowcache",
                               "battle sun shadows: static casters drawn only when they or the light camera change (live)",
                               targets, F_COUNT, apply};
