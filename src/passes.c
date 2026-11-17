#include "passes.h"
#include <stdint.h>
#include <stdio.h>
#include "objlayout.h"

static int off_current = -1, off_next = -1, off_pass = -1, off_name = -1, off_bytes = -1, off_length = -1;

static int offset_of(HlCode *c, const char *type, const char *name) {
    int t = hl_find_type(c, HOBJ, type), f = t < 0 ? -1 : hl_type_field(c, t, name);
    return f < 0 ? -1 : wt_field_offset(c, t, f);
}

int wt_pass_layout(HlCode *c) {
    off_current = offset_of(c, "h3d.pass.PassList", "current");
    off_next = offset_of(c, "h3d.pass.PassObject", "next");
    off_pass = offset_of(c, "h3d.pass.PassObject", "pass");
    off_name = offset_of(c, "h3d.mat.Pass", "name");
    off_bytes = offset_of(c, "String", "bytes");
    off_length = offset_of(c, "String", "length");
    return off_current < 0 || off_next < 0 || off_pass < 0 || off_name < 0 || off_bytes < 0 || off_length < 0 ? -1 : 0;
}

static void *field(void *obj, int off) { return obj && off >= 0 ? *(void **)((char *)obj + off) : NULL; }

void wt_pass_name(void *passes, char *out, int size) {
    void *first = field(passes, off_current), *pass = field(first, off_pass), *name = field(pass, off_name);
    const uint16_t *chars = (const uint16_t *)field(name, off_bytes);
    int length = name && off_length >= 0 ? *(int *)((char *)name + off_length) : 0, n = 0;
    if (!first) {
        out[0] = 0;
        return;
    }
    for (; chars && n < length && n < size - 1; n++) out[n] = chars[n] > 32 && chars[n] < 127 ? (char)chars[n] : '?';
    if (!n) snprintf(out, size, "(no name)");
    else out[n] = 0;
}

int wt_pass_ranges(void *passes, int off_obj, int parts, void **firsts, int *counts) {
    int n = wt_pass_count(passes, 1 << 20), r = 0, i = 0;
    if (parts < 2 || n < parts * 4) return n ? 1 : 0;
    void *before = NULL;
    for (void *p = field(passes, off_current); p; p = field(p, off_next), i++) {
        void *obj = field(p, off_obj);
        if (!i) firsts[0] = p, counts[0] = 0;
        else if (r + 1 < parts && i >= (int)((long long)n * (r + 1) / parts) && obj != before) firsts[++r] = p, counts[r] = 0;
        counts[r]++;
        before = obj;
    }
    return r + 1;
}

int wt_pass_count(void *passes, int limit) {
    int n = 0;
    for (void *p = field(passes, off_current); p && n < limit; p = field(p, off_next)) n++;
    return n;
}
