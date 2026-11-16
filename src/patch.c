#include "fix.h"
#include <stdio.h>
#include <stdlib.h>

int wt_patch(const unsigned char *in, size_t size, WtEnabled enabled, unsigned char **out, size_t *out_size) {
    *out = NULL;
    *out_size = 0;
    HlCode c;
    if (hl_read(&c, in, size)) {
        wt_log("bytecode not read (%s): nothing patched", c.error);
        return -1;
    }
    wt_log("bytecode v%d: %d functions, %d natives, %d types", c.version, c.nfunctions, c.nnatives, c.ntypes);
    HlEdit *e = hl_edit_begin(&c);
    int applied = 0;
    for (int i = 0; i < wt_nfixes; i++) {
        const WtFix *fix = wt_fixes[i];
        if (enabled && !enabled(fix->name)) {
            wt_log("%s: off in wtfix.ini", fix->name);
            continue;
        }
        int fn[16], ok = 1;
        for (int t = 0; t < fix->ntargets; t++) {
            fn[t] = hl_find_function(&c, fix->targets[t].name);
            if (fn[t] < 0) {
                wt_log("%s: OFF, %s is not in this build", fix->name, fix->targets[t].name);
                ok = 0;
                break;
            }
            uint64_t shape = hl_shape(&c, fn[t]);
            if (fix->targets[t].shape && shape != fix->targets[t].shape) {
                wt_log("%s: OFF, %s changed in this build (shape %016llx, fix written for %016llx)", fix->name,
                       fix->targets[t].name, (unsigned long long)shape, (unsigned long long)fix->targets[t].shape);
                ok = 0;
                break;
            }
        }
        if (!ok) continue;
        int mark = hl_edit_mark(e);
        if (fix->apply(&c, e, fn)) {
            hl_edit_rollback(e, mark);
            wt_log("%s: OFF, could not be applied", fix->name);
            continue;
        }
        wt_log("%s: on (%s)", fix->name, fix->summary);
        applied++;
    }
    if (!applied) {
        hl_edit_free(e);
        hl_free(&c);
        return 0;
    }
    unsigned char *result = hl_edit_write(e, out_size);
    hl_edit_free(e);
    HlCode check;
    if (hl_read(&check, result, *out_size)) {
        wt_log("patched bytecode does not read back (%s): running the game unpatched", check.error);
        free(result);
        hl_free(&c);
        *out_size = 0;
        return -1;
    }
    wt_log("patched: %d fixes, %zu -> %zu bytes, %d functions", applied, size, *out_size, check.nfunctions);
    hl_free(&check);
    hl_free(&c);
    *out = result;
    return applied;
}
