// Helpers for writing fixes, kept apart from the runner (patch.c).
#include "fix.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void (*wt_log_sink)(const char *line);
int (*wt_native_ready)(const char *feature);
double (*wt_number)(const char *section, const char *key, double fallback);

void wt_log(const char *format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    if (wt_log_sink) wt_log_sink(line);
}

HlOp wt_op(int op, int n, ...) {
    HlOp o;
    memset(&o, 0, sizeof o);
    o.op = op;
    o.n = n;
    va_list args;
    va_start(args, n);
    for (int i = 0; i < n; i++) o.a[i] = va_arg(args, int);
    va_end(args);
    return o;
}

int wt_findex(HlCode *c, int function) { return c->functions[function].findex; }

int wt_string_global(HlCode *c, const char *s) {
    int tstring = hl_find_type(c, HOBJ, "String");
    for (int g = 0; g < c->nglobals; g++) {
        int i = c->global_string[g];
        if (i >= 0 && c->globals[g] == tstring && c->strings[i].len == (int)strlen(s) &&
            !memcmp(c->strings[i].s, s, c->strings[i].len))
            return g;
    }
    return -1;
}

int wt_fun_type(HlCode *c, int nargs, int arg0, int arg1, int ret) {
    for (int i = 0; i < c->ntypes; i++) {
        const HlType *t = &c->types[i];
        if (t->kind == HFUN && t->nargs == nargs && t->ret == ret && (nargs < 1 || t->args[0] == arg0) &&
            (nargs < 2 || t->args[1] == arg1))
            return i;
    }
    return -1;
}

int wt_signature(HlCode *c, int function, const char *expected) {
    const char *got = hl_type_name(c, c->functions[function].type);
    if (strcmp(got, expected)) {
        char name[256];
        hl_function_name(c, c->functions[function].findex, 0, name, sizeof name);
        wt_log("  %s is %s, expected %s", name, got, expected);
        return 0;
    }
    return 1;
}

int wt_expect(HlCode *c, const HlFun *f, int at, int op, const char *callee) {
    if (at >= f->nops || f->ops[at].op != op) {
        wt_log("  op %d is not the expected %d", at, op);
        return 0;
    }
    if (callee) {
        char name[512];
        hl_function_name(c, f->ops[at].a[1], 0, name, sizeof name);
        if (strcmp(name, callee)) {
            wt_log("  op %d calls %s, expected %s", at, name, callee);
            return 0;
        }
    }
    return 1;
}
