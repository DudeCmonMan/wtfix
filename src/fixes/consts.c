// The game reads its constants from data.cdb by name each time. Rows set right after Main.setLang loads it change them
// game-wide and leave data.cdb alone (the game hashes it to mark saves and bug reports as modded).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../fix.h"
#include "../live.h"

WtConst wt_consts[WT_MAX_CONSTS];
int wt_nconsts;

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) *--end = 0;
    return s;
}

int wt_read_consts(const wchar_t *ini_path) {
    wchar_t full[MAX_PATH], buf[16384];
    wt_nconsts = 0;
    if (!GetFullPathNameW(ini_path, MAX_PATH, full, NULL)) return 0;
    DWORD n = GetPrivateProfileSectionW(L"const", buf, sizeof buf / sizeof buf[0], full);
    for (wchar_t *wline = buf; wline < buf + n && *wline; wline += wcslen(wline) + 1) {
        char line[256], copy[256];
        if (!WideCharToMultiByte(CP_UTF8, 0, wline, -1, line, sizeof line, NULL, NULL)) line[0] = 0;
        snprintf(copy, sizeof copy, "%s", line);
        char *eq = strchr(copy, '=');
        if (copy[0] == ';' || copy[0] == '#') continue;
        if (!eq) {
            wt_log("consts: ignored [const] line \"%s\" (no =)", line);
            continue;
        }
        *eq = 0;
        char *key = trim(copy), *value = eq + 1, *comment = strchr(value, ';'), *end;
        if (comment) *comment = 0;
        value = trim(value);
        double v = strtod(value, &end);
        int named = *key && strlen(key) < sizeof wt_consts[0].name;
        for (char *k = key; named && *k; k++)
            named = (*k >= 'A' && *k <= 'Z') || (*k >= 'a' && *k <= 'z') || (*k >= '0' && *k <= '9') || *k == '_';
        if (!named || end == value || *end) {
            wt_log("consts: ignored [const] line \"%s\" (want Name=number)", line);
            continue;
        }
        if (wt_nconsts == WT_MAX_CONSTS) {
            wt_log("consts: more than %d [const] lines, the rest ignored", WT_MAX_CONSTS);
            break;
        }
        snprintf(wt_consts[wt_nconsts].name, sizeof wt_consts[0].name, "%s", key);
        wt_consts[wt_nconsts++].value = v;
    }
    return wt_nconsts;
}

double wt_ini_number(const wchar_t *ini_path, const char *section, const char *key, double fallback) {
    wchar_t full[MAX_PATH], s[64], k[64], v[128];
    if (!GetFullPathNameW(ini_path, MAX_PATH, full, NULL)) return fallback;
    swprintf(s, 64, L"%hs", section);
    swprintf(k, 64, L"%hs", key);
    GetPrivateProfileStringW(s, k, L"", v, 128, full);
    wchar_t *comment = wcschr(v, L';'), *end;
    if (comment) *comment = 0;
    double d = wcstod(v, &end);
    while (*end == L' ' || *end == L'\t') end++;
    return end != v && !*end ? d : fallback;
}

static const WtTarget targets[] = {
    {"$Main.setLang", 0},   // the op that calls $Data.load is found and checked
    {"$Const.getConst", 0}, // its lookup ops are checked and reused
};

static int data_load_call(HlCode *c, const HlFun *f) {
    int found = -1;
    for (int i = 0; i < f->nops; i++) {
        if (f->ops[i].op < OCall0 || f->ops[i].op > OCallN) continue;
        char name[512];
        hl_function_name(c, f->ops[i].a[1], 0, name, sizeof name);
        if (strcmp(name, "$Data.load")) continue;
        if (found >= 0) {
            wt_log("  $Data.load is called twice in $Main.setLang");
            return -1;
        }
        found = i;
    }
    if (found < 0) wt_log("  $Main.setLang no longer calls $Data.load");
    return found;
}

// getConst begins: data = global Data; idx = data.constant; byId = idx.byId; d = byId.get(name); row = (virtual)d.
static int lookup_ok(HlCode *c, const HlFun *g) {
    if (!(wt_expect(c, g, 0, OGetGlobal, NULL) && wt_expect(c, g, 1, OField, NULL) && wt_expect(c, g, 2, ONullCheck, NULL) &&
          wt_expect(c, g, 3, OField, NULL) && wt_expect(c, g, 4, ONullCheck, NULL) &&
          wt_expect(c, g, 5, OCall2, "haxe.ds.StringMap.get") && wt_expect(c, g, 6, OToVirtual, NULL)))
        return 0;
    const HlOp *o = g->ops;
    int chained = o[1].a[1] == o[0].a[0] && o[3].a[1] == o[1].a[0] && o[5].a[2] == o[3].a[0] && o[5].a[3] == 0 &&
                  o[6].a[1] == o[5].a[0];
    int fields = hl_type_field(c, g->regs[o[0].a[0]], "constant") == o[1].a[2] &&
                 hl_type_field(c, g->regs[o[1].a[0]], "byId") == o[3].a[2];
    if (!chained || !fields) wt_log("  $Const.getConst no longer looks constants up in Data.constant.byId");
    return chained && fields;
}

static int apply(HlCode *c, HlEdit *e, const int *fn) {
    HlFun *lang = hl_fun_decode(c, fn[0]), *get = hl_fun_decode(c, fn[1]);
    int ok = lang && get && lookup_ok(c, get);
    int call = ok ? data_load_call(c, lang) : -1;
    int row_type = ok ? get->regs[get->ops[6].a[0]] : -1, tf64 = hl_find_type(c, HF64, NULL);
    int value = ok && c->types[row_type].kind == HVIRTUAL ? hl_type_field(c, row_type, "value") : -1;
    int value_type = value >= 0 ? c->types[row_type].fields[2 * value + 1] : -1;
    if (call < 0 || value_type < 0 || c->types[value_type].kind != HNULL ||
        c->types[c->types[value_type].param].kind != HF64 || tf64 < 0) {
        if (ok && call >= 0) wt_log("  a constant row's value is no longer a nullable float");
        hl_fun_free(lang), hl_fun_free(get);
        return -1;
    }
    int names[WT_MAX_CONSTS], nset = 0;
    for (int k = 0; k < wt_nconsts; k++) {
        names[k] = wt_string_global(c, wt_consts[k].name);
        if (names[k] < 0) wt_log("  %s: no game code names this constant (typo?), not set", wt_consts[k].name);
        else wt_log("  %s = %g", wt_consts[k].name, wt_consts[k].value), nset++;
    }
    // The overlay's zoom settings learn the game's own values here, before wtfix.ini's are set over them.
    int ti32 = hl_find_type(c, HI32, NULL), tvoid = hl_find_type(c, HVOID, NULL);
    int seed_type = wt_fun_type(c, 2, ti32, tf64, tvoid), seeds[2];
    for (int k = 0; k < 2; k++) seeds[k] = seed_type >= 0 ? wt_string_global(c, wt_live_const_names[k]) : -1;
    if (!nset && seeds[0] < 0 && seeds[1] < 0) {
        wt_log("  wtfix.ini [const] is empty: nothing to set");
        hl_fun_free(lang), hl_fun_free(get);
        return 0;
    }

    int r = lang->nregs;
    int rdata = r, ridx = r + 1, rmap = r + 2, rname = r + 3, rdyn = r + 4, rrow = r + 5, rf = r + 6, rnull = r + 7;
    int rid = r + 8, rvoid = r + 9;
    const int types[] = {get->regs[get->ops[0].a[0]], get->regs[get->ops[1].a[0]], get->regs[get->ops[3].a[0]],
                         get->regs[0],                get->regs[get->ops[5].a[0]], row_type, tf64, value_type,
                         ti32,                        tvoid};
    for (int i = 0; i < 10; i++) lang->regs[lang->nregs++] = types[i];

    // Inserted last first after the load: each block's skip (no such row) lands on the next block or the game's op.
    int at = call + 1, map_get = get->ops[5].a[1];
    for (int k = wt_nconsts - 1; k >= 0; k--) {
        if (names[k] < 0) continue;
        HlOp set[] = {
            wt_op(OGetGlobal, 2, rname, names[k]),
            wt_op(OCall2, 4, rdyn, map_get, rmap, rname),
            wt_op(OToVirtual, 2, rrow, rdyn),
            wt_op(OJNull, 2, rrow, at),
            wt_op(OFloat, 2, rf, hl_edit_float(e, wt_consts[k].value)),
            wt_op(OToDyn, 2, rnull, rf),
            wt_op(OSetField, 3, rrow, value, rnull),
        };
        hl_fun_insert(lang, at, set, 7);
    }
    int f_seed = seed_type >= 0 ? hl_edit_native(e, "wtfix", "live_seed", seed_type) : -1;
    for (int k = 1; k >= 0; k--) {
        if (seeds[k] < 0) continue;
        HlOp seed[] = {
            wt_op(OGetGlobal, 2, rname, seeds[k]),
            wt_op(OCall2, 4, rdyn, map_get, rmap, rname),
            wt_op(OToVirtual, 2, rrow, rdyn),
            wt_op(OJNull, 2, rrow, at),
            wt_op(OField, 3, rnull, rrow, value),
            wt_op(OJNull, 2, rnull, at),
            wt_op(OSafeCast, 2, rf, rnull),
            wt_op(OInt, 2, rid, hl_edit_int(e, k ? LIVE_ZOOM_MIN : LIVE_ZOOM_MAX)),
            wt_op(OCall2, 4, rvoid, f_seed, rid, rf),
        };
        hl_fun_insert(lang, at, seed, 9);
    }
    HlOp lookup[] = {
        wt_op(OGetGlobal, 2, rdata, get->ops[0].a[1]),
        wt_op(OField, 3, ridx, rdata, get->ops[1].a[2]),
        wt_op(ONullCheck, 1, ridx),
        wt_op(OField, 3, rmap, ridx, get->ops[3].a[2]),
        wt_op(ONullCheck, 1, rmap),
    };
    hl_fun_insert(lang, at, lookup, 5);

    int result = hl_edit_replace(e, fn[0], lang) ? -1 : 0;
    hl_fun_free(lang), hl_fun_free(get);
    return result;
}

const WtFix fix_consts = {"consts", "game constants set from wtfix.ini [const]", targets, 2, apply};
