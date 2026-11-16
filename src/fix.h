// A fix names its targets by VM function name and shape; a missing or changed target turns the fix off.
#pragma once
#include "hlcode.h"
#include <wchar.h>

typedef struct {
    const char *name;
    uint64_t shape; // 0: relies only on the name and type, not the code
} WtTarget;

typedef struct {
    const char *name;
    const char *summary;
    const WtTarget *targets;
    int ntargets;
    // fn[i] is targets[i]'s function index; nonzero (reason logged) rolls the edit back.
    int (*apply)(HlCode *c, HlEdit *e, const int *fn);
} WtFix;

extern const WtFix *const wt_fixes[];
extern const int wt_nfixes;

// Steps a build may add around patching; this repository adds none.
typedef struct {
    const WtFix *const *fixes;
    int nfixes;
    void (*boot)(void);
    void (*patched)(const unsigned char *code, size_t size);
} WtExtend;
extern WtExtend wt_extend;

void wt_log(const char *format, ...);
extern void (*wt_log_sink)(const char *line);
extern int (*wt_native_ready)(const char *feature); // feature: e.g. "gc"

// wtfix.ini [const]: values the consts fix writes over data.cdb's constant sheet once the game loads it.
typedef struct {
    char name[64];
    double value;
} WtConst;
#define WT_MAX_CONSTS 64
extern WtConst wt_consts[WT_MAX_CONSTS];
extern int wt_nconsts;
int wt_read_consts(const wchar_t *ini_path);
// Allows an inline ; comment; the fallback when missing or not a number.
double wt_ini_number(const wchar_t *ini_path, const char *section, const char *key, double fallback);
// Unset, every number is its fallback.
extern double (*wt_number)(const char *section, const char *key, double fallback);

typedef int (*WtEnabled)(const char *fix);
// *out is malloc'd, or NULL when nothing changed or the result failed to read back.
int wt_patch(const unsigned char *in, size_t size, WtEnabled enabled, unsigned char **out, size_t *out_size);
// The wtfix folder in the game folder: wtfix.ini, logs\, dll\.
const wchar_t *wt_root(void);
// 1 for the game's OpenGL build, which wtfix refuses to start (DirectX 11 only).
int wt_boot_opengl(const unsigned char *boot, size_t size);

HlOp wt_op(int op, int n, ...);
int wt_expect(HlCode *c, const HlFun *f, int at, int op, const char *callee); // the op, and its callee if given
int wt_findex(HlCode *c, int function);
// OString yields raw characters, not a String: pass names as the global holding the literal.
int wt_string_global(HlCode *c, const char *s);
int wt_fun_type(HlCode *c, int nargs, int arg0, int arg1, int ret); // nargs <= 2
// Checks the printed type ("(battle.Unit)->bool"); logs a mismatch.
int wt_signature(HlCode *c, int function, const char *expected);
