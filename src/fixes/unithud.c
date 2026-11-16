// Unit HUDs (LiteUnitHud) end each rebuild scaled for the look they took: unit_hud_scale full, unit_bars_scale bars.
// The component is scaled, not its Hud: the Hud centres it by measuring, which counts only a child's scale.
#include "../fix.h"
#include "../live.h"

static const WtTarget targets[] = {
    {"battle.ui.comp.LiteUnitHud.rebuild", 0}, // its returns are found
    {"h2d.Object.setScale", 0},
};

static int apply(HlCode *c, HlEdit *e, const int *fn) {
    if (!wt_signature(c, fn[0], "(battle.ui.comp.LiteUnitHud)->void") ||
        !wt_signature(c, fn[1], "(h2d.Object,f64)->void"))
        return -1;
    int thud = c->types[c->functions[fn[0]].type].args[0];
    int closed = hl_type_field(c, thud, "closed"), minimal = hl_type_field(c, thud, "minimal");
    int tbool = hl_find_type(c, HBOOL, NULL), tf64 = hl_find_type(c, HF64, NULL), tvoid = hl_find_type(c, HVOID, NULL);
    int ti32 = hl_find_type(c, HI32, NULL), t_number = wt_fun_type(c, 1, ti32, -1, tf64);
    if (closed < 0 || minimal < 0 || t_number < 0) {
        wt_log("  battle.ui.comp.LiteUnitHud has no closed/minimal, or no (i32)->f64 type for the live size");
        return -1;
    }
    // scaleFor(hud): setScale(hud, live_number(hud.closed || hud.minimal ? bars : full))
    int f_number = hl_edit_native(e, "wtfix", "live_number", t_number);
    const int regs[] = {thud, tbool, tf64, tvoid, ti32};
    const HlOp ops[] = {
        wt_op(OField, 3, 1, 0, closed),
        wt_op(OJTrue, 2, 1, 6),
        wt_op(OField, 3, 1, 0, minimal),
        wt_op(OJTrue, 2, 1, 6),
        wt_op(OInt, 2, 4, hl_edit_int(e, LIVE_HUD_FULL)),
        wt_op(OJAlways, 1, 7),
        wt_op(OInt, 2, 4, hl_edit_int(e, LIVE_HUD_BARS)),
        wt_op(OCall1, 3, 2, f_number, 4),
        wt_op(OCall2, 4, 3, wt_findex(c, fn[1]), 0, 2),
        wt_op(ORet, 1, 3),
    };
    HlFun *f = hl_fun_decode(c, fn[0]);
    HlFun *scale_for = f ? hl_fun_new(c->functions[fn[0]].type, 5, regs, ops, 10, f->files[0], f->lines[0]) : NULL;
    int scale_findex = scale_for ? hl_edit_add(e, scale_for) : -1, returns = 0;
    hl_fun_free(scale_for);
    int tvoid_reg = f ? f->nregs : 0;
    if (f) f->regs[f->nregs++] = tvoid;
    for (int i = f && scale_findex >= 0 ? f->nops - 1 : -1; i >= 0; i--) {
        if (f->ops[i].op != ORet) continue;
        // The return's own slot takes the call, so a jump to it still scales; the return follows.
        HlOp ret = f->ops[i];
        f->ops[i] = wt_op(OCall1, 3, tvoid_reg, scale_findex, 0);
        hl_fun_insert(f, i + 1, &ret, 1);
        returns++;
    }
    if (f && !returns) wt_log("  battle.ui.comp.LiteUnitHud.rebuild has no return");
    int result = returns && !hl_edit_replace(e, fn[0], f) ? 0 : -1;
    hl_fun_free(f);
    return result;
}

const WtFix fix_unithud = {"unithud", "unit HUDs sized by unit_hud_scale (full) and unit_bars_scale (bars), live",
                           targets, 2, apply};
