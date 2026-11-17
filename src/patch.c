#include "fix.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

WtExtend wt_extend;

static int contains(const unsigned char *data, size_t size, const char *text) {
    size_t n = strlen(text);
    for (size_t at = 0; at + n <= size;) {
        const unsigned char *p = memchr(data + at, text[0], size - n + 1 - at);
        if (!p) return 0;
        if (!memcmp(p, text, n)) return 1;
        at = (size_t)(p - data) + 1;
    }
    return 0;
}

// Steam's "Wartales (OpenGL)" boots sdlboot.dat, which has Heaps' GL driver and no DirectX one.
int wt_boot_opengl(const unsigned char *boot, size_t size) {
    return contains(boot, size, "h3d.impl.GlDriver") && !contains(boot, size, "h3d.impl.DirectXDriver");
}

static const WtFix *fix_at(int late, int i) {
    if (late) return wt_extend.late[i];
    return i < wt_nfixes ? wt_fixes[i] : wt_extend.fixes[i - wt_nfixes];
}

static int pass(const unsigned char *in, size_t size, int late, WtEnabled enabled, unsigned char **out,
                size_t *out_size) {
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
    for (int i = 0; i < (late ? wt_extend.nlate : wt_nfixes + wt_extend.nfixes); i++) {
        const WtFix *fix = fix_at(late, i);
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

// Late fixes patch the others' result in a second pass; if that pass fails, the first result stands.
int wt_patch(const unsigned char *in, size_t size, WtEnabled enabled, unsigned char **out, size_t *out_size) {
    int applied = pass(in, size, 0, enabled, out, out_size);
    if (applied < 0 || !wt_extend.nlate) return applied;
    unsigned char *first = *out;
    size_t first_size = *out_size;
    int late = pass(first ? first : in, first ? first_size : size, 1, enabled, out, out_size);
    if (late <= 0) {
        *out = first, *out_size = first_size;
        return applied;
    }
    free(first);
    return applied + late;
}
