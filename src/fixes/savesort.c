// Load Game lists saves newest first but puts each campaign's autosave in front unsorted; the combined list is sorted
// again with the game's comparator (stable, so equal times keep their order). Continue sorts its own copy.
#include "../fix.h"

static const WtTarget targets[] = {
    {"ui.win.$LoadGame.loadGames", 0x63af51f1b74c10a2ull},
};

static int apply(HlCode *c, HlEdit *e, const int *fn) {
    HlFun *f = hl_fun_decode(c, fn[0]);
    if (!f) return -1;
    // Ops 100-105 build the comparator and sort the main list; op 190 puts the autosaves in front of it into r27.
    if (!wt_expect(c, f, 100, OStaticClosure, "ui.win.$LoadGame.~loadGames.7") || !wt_expect(c, f, 104, OInstanceClosure, NULL) ||
        !wt_expect(c, f, 105, OCall2, "hl.types.ArrayObj.sort") || !wt_expect(c, f, 190, OCall2, "hl.types.ArrayObj.concat") ||
        f->ops[105].a[2] != 21 || f->ops[105].a[3] != 33 || f->ops[104].a[0] != 33 || f->ops[100].a[0] != 32 ||
        f->ops[190].a[0] != 27) {
        hl_fun_free(f);
        return -1;
    }
    HlOp sort[] = {
        wt_op(OStaticClosure, 2, 32, f->ops[100].a[1]),
        wt_op(OInstanceClosure, 3, 33, f->ops[104].a[1], 32),
        wt_op(OCall2, 4, f->ops[105].a[0], f->ops[105].a[1], 27, 33),
    };
    hl_fun_insert(f, 191, sort, 3);
    int r = hl_edit_replace(e, fn[0], f);
    hl_fun_free(f);
    return r;
}

const WtFix fix_savesort = {"savesort", "Load Game: autosaves sorted with the other saves, newest first", targets, 1, apply};
