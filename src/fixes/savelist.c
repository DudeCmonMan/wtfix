// The save list reads, de-obfuscates, inflates and hashes every save whole only to show its header. It reads the
// header chunks only and skips the integrity check, so a damaged save shows in the list and fails when loaded.
#include "../fix.h"

static const WtTarget targets[] = {
    {"ui.win.$LoadGame.~loadGames.3", 0x43a821c723ed865cull}, // the list's per-file loop
    {"mpman.$Storage.read", 0x2bbb1c5d5f161a9cull},
    {"sys.io.$File.getBytes", 0xfb45cddce182f8d3ull},
    {"st.$GameState.readSave", 0x0e9360645c1c4322ull},
};

static int fail(HlFun *a, HlFun *b, HlFun *c, HlFun *d) {
    hl_fun_free(a), hl_fun_free(b), hl_fun_free(c), hl_fun_free(d);
    return -1;
}

static int apply(HlCode *c, HlEdit *e, const int *fn) {
    HlFun *list = hl_fun_decode(c, fn[0]), *read = hl_fun_decode(c, fn[1]), *bytes = hl_fun_decode(c, fn[2]),
          *save = hl_fun_decode(c, fn[3]);
    if (!list || !read || !bytes || !save) return fail(list, read, bytes, save);

    // getBytes, reading the header chunks only (wtfix@file_head has file_contents' type and contract).
    int contents = hl_find_native(c, "std", "file_contents");
    if (contents < 0 || !wt_expect(c, bytes, 3, OCall2, "std@file_contents")) return fail(list, read, bytes, save);
    bytes->ops[3].a[1] = hl_edit_native(e, "wtfix", "file_head", c->natives[contents].type);
    int head_bytes = hl_edit_add(e, bytes);

    // Storage.read through it: both getBytes calls (game folder and user data path).
    if (!wt_expect(c, read, 16, OCall1, "sys.io.$File.getBytes") || !wt_expect(c, read, 28, OCall1, "sys.io.$File.getBytes"))
        return fail(list, read, bytes, save);
    read->ops[16].a[1] = read->ops[28].a[1] = head_bytes;
    int head_read = hl_edit_add(e, read);

    // readSave, returning the header once it finds the data chunk empty (r4 = data at op 98; op 183 builds the
    // result from r1 header, r4 data, r8 corrupted). r14 and r23 are free i32 scratch registers there.
    int length = hl_type_field(c, save->regs[4], "length"), zero = hl_find_int(c, 0);
    if (!wt_expect(c, save, 98, OMov, NULL) || save->ops[98].a[0] != 4 || !wt_expect(c, save, 183, ONew, NULL) ||
        length < 0 || zero < 0 || c->types[save->regs[14]].kind != HI32 || c->types[save->regs[23]].kind != HI32 ||
        c->types[save->regs[8]].kind != HBOOL)
        return fail(list, read, bytes, save);
    HlOp early[] = {
        wt_op(OJNull, 2, 4, 99),
        wt_op(OField, 3, 14, 4, length),
        wt_op(OInt, 2, 23, zero),
        wt_op(OJNotEq, 3, 14, 23, 99),
        wt_op(OBool, 2, 8, 0),
        wt_op(OJAlways, 1, 183),
    };
    // Uncompressed saves jump to 99 too.
    if (hl_fun_insert_entry(save, 99, early, 6)) return fail(list, read, bytes, save);
    int head_save = hl_edit_add(e, save);

    // Ops 32/47 read and parse each file; 151/154 do it again to report a failure other than "Not a save". Both use
    // the header-only copies, which reject the same files again.
    if (!wt_expect(c, list, 32, OCall2, "mpman.$Storage.read") || !wt_expect(c, list, 47, OCall1, "st.$GameState.readSave") ||
        !wt_expect(c, list, 151, OCall2, "mpman.$Storage.read") || !wt_expect(c, list, 154, OCall1, "st.$GameState.readSave") ||
        head_bytes < 0 || head_read < 0 || head_save < 0)
        return fail(list, read, bytes, save);
    list->ops[32].a[1] = list->ops[151].a[1] = head_read;
    list->ops[47].a[1] = list->ops[154].a[1] = head_save;
    int r = hl_edit_replace(e, fn[0], list);
    fail(list, read, bytes, save);
    return r;
}

const WtFix fix_savelist = {"savelist", "save list reads headers only", targets, 4, apply};
