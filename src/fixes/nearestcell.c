// findNearestCell runs the caller's costly filter on every navmesh cell; measuring first runs it only on a would-be
// nearest cell. Same result: the filters only write scratch state and caches.
#include "../fix.h"

static const WtTarget targets[] = {
    {"world.Pathfinder.findNearestCell", 0x34a1d81e52f48531ull},
};

// The function as built: 12 loop head; 24-29 cell's collide bits, else continue; 30-34 the filter, else continue;
// 35-46 squared distance; 47 not nearer: continue; 48-49 keep it; 50 continue; 51 return.
static int apply(HlCode *c, HlEdit *e, const int *fn) {
    HlFun *f = hl_fun_decode(c, fn[0]);
    if (!f || f->nops != 52 || !wt_expect(c, f, 12, OLabel, NULL) || !wt_expect(c, f, 28, OJEq, NULL) ||
        !wt_expect(c, f, 30, OJNull, NULL) || !wt_expect(c, f, 32, OCallClosure, NULL) ||
        !wt_expect(c, f, 33, OJTrue, NULL) || !wt_expect(c, f, 35, OCall1, "shiro.navmesh.Cell.getCenter") ||
        !wt_expect(c, f, 47, OJNotLt, NULL) || !wt_expect(c, f, 50, OJAlways, NULL) || f->ops[28].a[2] != 30 ||
        f->ops[30].a[1] != 35 || f->ops[33].a[1] != 35 || f->ops[47].a[2] != 50) {
        hl_fun_free(f);
        return -1;
    }
    int order[52], n = 0;
    for (int i = 0; i <= 29; i++) order[n++] = i;
    for (int i = 35; i <= 47; i++) order[n++] = i;
    for (int i = 30; i <= 34; i++) order[n++] = i;
    for (int i = 48; i <= 51; i++) order[n++] = i;
    int at[52];
    for (int i = 0; i < n; i++) at[order[i]] = i;
    if (hl_fun_reorder(f, order, n)) {
        hl_fun_free(f);
        return -1;
    }
    *hl_jump_target(&f->ops[at[28]], 0) = at[35]; // collide bits match: measure first
    *hl_jump_target(&f->ops[at[30]], 0) = at[48]; // no filter: keep it
    *hl_jump_target(&f->ops[at[33]], 0) = at[48]; // filter passed: keep it
    int r = hl_edit_replace(e, fn[0], f);
    hl_fun_free(f);
    return r;
}

const WtFix fix_nearestcell = {"nearestcell", "nearest navmesh cell measures before filtering", targets, 1, apply};
