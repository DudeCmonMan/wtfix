// Hands DirectXDriver.present's dx_driver to overlay.cpp right before its directx@present call, so the panel draws in
// the game's frame with nothing in DirectX hooked.
#include "../fix.h"

static const WtTarget targets[] = {
    {"h3d.impl.DirectXDriver.present", 0}, // its directx@present call is found
};

static int apply(HlCode *c, HlEdit *e, const int *fn) {
    HlFun *f = hl_fun_decode(c, fn[0]);
    int native = hl_find_native(c, "directx", "present"), present = native >= 0 ? c->natives[native].findex : -1;
    int driver = f ? hl_type_field(c, f->regs[0], "driver") : -1, tdriver = f ? hl_field_type(c, f->regs[0], driver) : -1;
    int tdyn = hl_find_type(c, HDYN, NULL), tvoid = hl_find_type(c, HVOID, NULL);
    int frame_type = wt_fun_type(c, 1, tdyn, -1, tvoid), call = -1;
    for (int i = 0; f && present >= 0 && i < f->nops; i++)
        if (f->ops[i].op >= OCall0 && f->ops[i].op <= OCallN && f->ops[i].a[1] == present) call = call < 0 ? i : -2;
    if (!f || call < 0 || tdriver < 0 || c->types[tdriver].kind != HABSTRACT || frame_type < 0) {
        wt_log("  DirectXDriver.present no longer calls directx@present once with a dx_driver at hand");
        hl_fun_free(f);
        return -1;
    }
    int rdriver = f->nregs, rboxed = f->nregs + 1, rvoid = f->nregs + 2;
    f->regs[f->nregs++] = tdriver;
    f->regs[f->nregs++] = tdyn;
    f->regs[f->nregs++] = tvoid;
    HlOp hand[] = {
        wt_op(OField, 3, rdriver, 0, driver),
        wt_op(OToDyn, 2, rboxed, rdriver),
        wt_op(OCall1, 3, rvoid, hl_edit_native(e, "wtfix", "overlay_frame", frame_type), rboxed),
    };
    hl_fun_insert(f, call, hand, 3);
    int result = hl_edit_replace(e, fn[0], f) ? -1 : 0;
    hl_fun_free(f);
    return result;
}

const WtFix fix_overlay = {"overlay", "settings panel on Insert, drawn from the game's own present", targets, 1, apply};
