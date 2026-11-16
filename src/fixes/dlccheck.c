// BasePlayer.globalUpdate asks Steam about every DLC each frame, only to spot a change: past its me check it goes on
// once a second (dlc_due). A change still lands within a second; the game caches its own DLC answers longer anyway.
#include "../fix.h"
#include <string.h>

static const WtTarget targets[] = {
    {"ent.BasePlayer.globalUpdate", 0}, // the me check and the getDlcs call are found; it must do nothing else
};

// All globalUpdate may call: anything else might need every frame.
static const char *const dlc_calls[] = {"$Const.getDlcs", "ent.BasePlayer.getDlcSignature", "ent.BasePlayer.setDlcs"};

static int only_the_dlc_check(HlCode *c, const HlFun *f) {
    for (int i = 0; i < f->nops; i++) {
        int op = f->ops[i].op;
        if (op == OSetField || op == OSetThis || op == OSetGlobal || op == ODynSet || op == OSetArray ||
            op == OSetMem || op == OSetI8 || op == OSetI16 || op == OSetref || op == OSetEnumField) {
            wt_log("  op %d stores a value (%d): globalUpdate now does more than the DLC check", i, op);
            return 0;
        }
        if (op == OCallMethod || op == OCallThis || op == OCallClosure) {
            wt_log("  op %d makes a dynamic call: globalUpdate now does more than the DLC check", i);
            return 0;
        }
        if (op < OCall0 || op > OCallN) continue;
        char name[512];
        hl_function_name(c, f->ops[i].a[1], 0, name, sizeof name);
        int known = 0;
        for (size_t k = 0; k < sizeof dlc_calls / sizeof dlc_calls[0]; k++) known |= !strcmp(name, dlc_calls[k]);
        if (!known) {
            wt_log("  op %d calls %s: globalUpdate now does more than the DLC check", i, name);
            return 0;
        }
    }
    return 1;
}

static int jumps_to(HlOp *o, int at) {
    if (!hl_is_jump(o->op)) return 0;
    if (o->op != OSwitch) return *hl_jump_target(o, 0) == at;
    for (int k = -1; k < o->a[1]; k++)
        if (*hl_jump_target(o, k) == at) return 1;
    return 0;
}

// The one $Const.getDlcs call, right after "if (this.game.me != this) return".
static int find_check(HlCode *c, HlFun *f, int tgame) {
    int at = -1;
    for (int i = 0; i < f->nops; i++) {
        if (f->ops[i].op != OCall0) continue;
        char name[512];
        hl_function_name(c, f->ops[i].a[1], 0, name, sizeof name);
        if (strcmp(name, "$Const.getDlcs")) continue;
        if (at >= 0) {
            wt_log("  globalUpdate calls $Const.getDlcs twice");
            return -1;
        }
        at = i;
    }
    if (at < 2) {
        wt_log("  globalUpdate no longer starts the DLC check with $Const.getDlcs past its me check");
        return -1;
    }
    const HlOp *me = &f->ops[at - 2], *skip = &f->ops[at - 1];
    if (!wt_expect(c, f, at - 2, OField, NULL) || !wt_expect(c, f, at - 1, OJNotEq, NULL)) return -1;
    int rme = me->a[0], target = skip->a[2];
    if (f->regs[me->a[1]] != tgame || me->a[2] != hl_type_field(c, tgame, "me") ||
        !((skip->a[0] == rme && skip->a[1] == 0) || (skip->a[0] == 0 && skip->a[1] == rme)) || target <= at ||
        !wt_expect(c, f, target, ORet, NULL)) {
        wt_log("  the ops before $Const.getDlcs are no longer \"if (this.game.me != this) return\"");
        return -1;
    }
    // A jump straight to the call would pass the gate.
    for (int i = 0; i < f->nops; i++)
        if (jumps_to(&f->ops[i], at)) {
            wt_log("  op %d jumps to $Const.getDlcs", i);
            return -1;
        }
    return at;
}

static int apply(HlCode *c, HlEdit *e, const int *fn) {
    if (!wt_signature(c, fn[0], "(ent.BasePlayer,f64)->void")) return -1;
    int tgame = hl_find_type(c, HOBJ, "Game"), tbool = hl_find_type(c, HBOOL, NULL);
    int tvoid = hl_find_type(c, HVOID, NULL), tdue = wt_fun_type(c, 0, -1, -1, tbool);
    if (tgame < 0 || tdue < 0) {
        wt_log("  no Game type, or no ()->bool type for wtfix@dlc_due");
        return -1;
    }
    HlFun *f = hl_fun_decode(c, fn[0]);
    int at = f && only_the_dlc_check(c, f) ? find_check(c, f, tgame) : -1;
    if (at < 0) {
        hl_fun_free(f);
        return -1;
    }
    int rdue = f->nregs, rvoid = f->nregs + 1;
    f->regs[f->nregs++] = tbool;
    f->regs[f->nregs++] = tvoid;
    HlOp gate[] = {
        wt_op(OCall0, 2, rdue, hl_edit_native(e, "wtfix", "dlc_due", tdue)),
        wt_op(OJTrue, 2, rdue, at), // at: the getDlcs call, past this block
        wt_op(ORet, 1, rvoid),
    };
    hl_fun_insert(f, at, gate, 3);
    int result = hl_edit_replace(e, fn[0], f) ? -1 : 0;
    hl_fun_free(f);
    return result;
}

const WtFix fix_dlccheck = {"dlccheck", "the local player's DLC check (8 Steam calls) once a second, not every frame",
                            targets, 1, apply};
