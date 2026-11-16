#include "hlcode.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const signed char OP_ARGS[OLast] = {
    2, 2, 2, 2, 2, 2, 1,                         // Mov Int Float Bool Bytes String Null
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,       // Add .. Xor
    2, 2, 1, 1,                                  // Neg Not Incr Decr
    2, 3, 4, 5, 6, -1, -1, -1, -1,               // Call0..4 CallN CallMethod CallThis CallClosure
    2, 3, 3,                                     // StaticClosure InstanceClosure VirtualClosure
    2, 2, 3, 3, 2, 2, 3, 3,                      // GetGlobal SetGlobal Field SetField GetThis SetThis DynGet DynSet
    2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 1, // JTrue .. JAlways
    2, 2, 2, 2, 2, 2, 2,                         // ToDyn .. ToVirtual
    0, 1, 1, 1, -1, 1, 2, 1,                     // Label Ret Throw Rethrow Switch NullCheck Trap EndTrap
    3, 3, 3, 3, 3, 3, 3, 3,                      // GetI8 .. SetArray
    1, 2, 2, 2, 2,                               // New ArraySize Type GetType GetTID
    2, 2, 2,                                     // Ref Unref Setref
    -1, 2, 2, 4, 3,                              // MakeEnum EnumAlloc EnumIndex EnumField SetEnumField
    0, 2, 3, 0, 3, 3, 1,                         // Assert RefData RefOffset Nop Prefetch Asm Catch
};

static const char *OP_NAMES[OLast] = {
    "Mov", "Int", "Float", "Bool", "Bytes", "String", "Null", "Add", "Sub", "Mul", "SDiv", "UDiv", "SMod", "UMod",
    "Shl", "SShr", "UShr", "And", "Or", "Xor", "Neg", "Not", "Incr", "Decr", "Call0", "Call1", "Call2", "Call3",
    "Call4", "CallN", "CallMethod", "CallThis", "CallClosure", "StaticClosure", "InstanceClosure", "VirtualClosure",
    "GetGlobal", "SetGlobal", "Field", "SetField", "GetThis", "SetThis", "DynGet", "DynSet", "JTrue", "JFalse",
    "JNull", "JNotNull", "JSLt", "JSGte", "JSGt", "JSLte", "JULt", "JUGte", "JNotLt", "JNotGte", "JEq", "JNotEq",
    "JAlways", "ToDyn", "ToSFloat", "ToUFloat", "ToInt", "SafeCast", "UnsafeCast", "ToVirtual", "Label", "Ret",
    "Throw", "Rethrow", "Switch", "NullCheck", "Trap", "EndTrap", "GetI8", "GetI16", "GetMem", "GetArray", "SetI8",
    "SetI16", "SetMem", "SetArray", "New", "ArraySize", "Type", "GetType", "GetTID", "Ref", "Unref", "Setref",
    "MakeEnum", "EnumAlloc", "EnumIndex", "EnumField", "SetEnumField", "Assert", "RefData", "RefOffset", "Nop",
    "Prefetch", "Asm", "Catch",
};

static const char *KIND_NAMES[HLAST] = {
    "void", "u8", "u16", "i32", "i64", "f32", "f64", "bool", "bytes", "dynamic", "fun", "obj", "array", "type", "ref",
    "virtual", "dynobj", "abstract", "enum", "null", "method", "struct", "packed", "guid",
};

// ---------------------------------------------------------------- reading

typedef struct {
    const unsigned char *d;
    size_t pos, size;
    int err;
} Rd;

static int rbyte(Rd *r) {
    if (r->pos >= r->size) {
        r->err = 1;
        return 0;
    }
    return r->d[r->pos++];
}

static int rd_index(Rd *r) {
    int b = rbyte(r);
    if (!(b & 0x80)) return b & 0x7F;
    if (!(b & 0x40)) {
        int v = rbyte(r) | ((b & 31) << 8);
        return (b & 0x20) ? -v : v;
    }
    int c = rbyte(r), d = rbyte(r), e = rbyte(r);
    int v = ((b & 31) << 24) | (c << 16) | (d << 8) | e;
    return (b & 0x20) ? -v : v;
}

static int rd_uindex(Rd *r) {
    int v = rd_index(r);
    if (v < 0) r->err = 1;
    return v;
}

static int ri32(Rd *r) {
    if (r->pos + 4 > r->size) {
        r->err = 1;
        return 0;
    }
    int v;
    memcpy(&v, r->d + r->pos, 4);
    r->pos += 4;
    return v;
}

static int fail(HlCode *c, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(c->error, sizeof c->error, format, args);
    va_end(args);
    return -1;
}

static int *ints_alloc(int n) { return (int *)calloc(n > 0 ? n : 1, sizeof(int)); }

static int read_strings(Rd *r, HlCode *c, int count, HlStr **out, size_t *data_at, size_t *lens_at) {
    int size = ri32(r);
    if (r->err || size < 0 || r->pos + (size_t)size > r->size) return fail(c, "bad string block");
    const char *data = (const char *)r->d + r->pos;
    if (data_at) *data_at = r->pos;
    r->pos += size;
    if (lens_at) *lens_at = r->pos;
    HlStr *s = (HlStr *)calloc(count > 0 ? count : 1, sizeof(HlStr));
    int at = 0;
    for (int i = 0; i < count; i++) {
        int n = rd_uindex(r);
        if (r->err || at + n >= size || data[at + n] != 0) return fail(c, "bad string %d", i);
        s[i].s = data + at;
        s[i].len = n;
        at += n + 1;
    }
    *out = s;
    return 0;
}

static int read_type(Rd *r, HlCode *c, HlType *t) {
    memset(t, 0, sizeof *t);
    t->name = t->super = -1;
    t->kind = rbyte(r);
    switch (t->kind) {
    case HFUN:
    case HMETHOD:
        t->nargs = rbyte(r);
        t->args = ints_alloc(t->nargs);
        for (int i = 0; i < t->nargs; i++) t->args[i] = rd_uindex(r);
        t->ret = rd_uindex(r);
        break;
    case HOBJ:
    case HSTRUCT:
        t->name = rd_index(r);
        t->super = rd_index(r);
        t->global = rd_uindex(r);
        t->nfields = rd_uindex(r);
        t->nproto = rd_uindex(r);
        t->nbindings = rd_uindex(r);
        if (r->err) break;
        t->fields = ints_alloc(2 * t->nfields);
        t->proto = ints_alloc(3 * t->nproto);
        t->bindings = ints_alloc(2 * t->nbindings);
        for (int i = 0; i < t->nfields; i++) t->fields[2 * i] = rd_index(r), t->fields[2 * i + 1] = rd_uindex(r);
        for (int i = 0; i < t->nproto; i++)
            t->proto[3 * i] = rd_index(r), t->proto[3 * i + 1] = rd_uindex(r), t->proto[3 * i + 2] = rd_index(r);
        for (int i = 0; i < t->nbindings; i++) t->bindings[2 * i] = rd_uindex(r), t->bindings[2 * i + 1] = rd_uindex(r);
        break;
    case HREF:
    case HNULL:
    case HPACKED:
        t->param = rd_uindex(r);
        break;
    case HVIRTUAL:
        t->nfields = rd_uindex(r);
        if (r->err) break;
        t->fields = ints_alloc(2 * t->nfields);
        for (int i = 0; i < t->nfields; i++) t->fields[2 * i] = rd_index(r), t->fields[2 * i + 1] = rd_uindex(r);
        break;
    case HABSTRACT:
        t->name = rd_index(r);
        break;
    case HENUM: {
        t->name = rd_index(r);
        t->global = rd_uindex(r);
        int n = rd_uindex(r);
        t->nconstructs = r->err ? 0 : n;
        t->constructs = ints_alloc(t->nconstructs);
        for (int i = 0; i < n && !r->err; i++) {
            t->constructs[i] = rd_index(r);
            int k = rd_uindex(r);
            for (int j = 0; j < k && !r->err; j++) rd_uindex(r);
        }
        break;
    }
    default:
        if (t->kind >= HLAST) return fail(c, "bad type kind %d", t->kind);
    }
    if (r->err) return fail(c, "truncated type");
    if (t->name >= c->nstrings) return fail(c, "bad type name");
    return 0;
}

static int decode_op(Rd *r, HlOp *o) {
    o->op = rd_uindex(r);
    if (r->err || o->op >= OLast) return -1;
    int n = OP_ARGS[o->op];
    if (n >= 0) {
        o->n = n;
        for (int i = 0; i < n; i++) o->a[i] = rd_index(r);
    } else if (o->op == OSwitch) {
        o->a[0] = rd_uindex(r);
        int count = rd_uindex(r);
        if (r->err || count < 0 || count + 3 > HL_OP_MAX) return -1;
        o->a[1] = count;
        for (int i = 0; i < count; i++) o->a[2 + i] = rd_uindex(r);
        o->a[2 + count] = rd_uindex(r);
        o->n = count + 3;
    } else {
        o->a[0] = rd_index(r);
        o->a[1] = rd_index(r);
        int count = rbyte(r);
        if (count + 3 > HL_OP_MAX) return -1;
        o->a[2] = count;
        for (int i = 0; i < count; i++) o->a[3 + i] = rd_index(r);
        o->n = count + 3;
    }
    return r->err ? -1 : 0;
}

int hl_decode_op(const HlCode *c, size_t *at, HlOp *op) {
    Rd r = {c->data, *at, c->size, 0};
    int e = decode_op(&r, op);
    *at = r.pos;
    return e;
}

static void read_debug(Rd *r, int nops, int *files, int *lines) {
    int curfile = -1, curline = 0, i = 0;
    while (i < nops && !r->err) {
        int c = rbyte(r);
        if (c & 1) {
            curfile = ((c >> 1) << 8) | rbyte(r);
        } else if (c & 2) {
            int delta = c >> 6, count = (c >> 2) & 15;
            while (count-- && i < nops) {
                if (files) files[i] = curfile, lines[i] = curline;
                i++;
            }
            curline += delta;
        } else if (c & 4) {
            curline += c >> 3;
            if (files) files[i] = curfile, lines[i] = curline;
            i++;
        } else {
            int b2 = rbyte(r), b3 = rbyte(r);
            curline = (c >> 3) | (b2 << 5) | (b3 << 13);
            if (files) files[i] = curfile, lines[i] = curline;
            i++;
        }
    }
}

static int read_function(Rd *r, HlCode *c, HlFunction *f, HlOp *scratch) {
    f->record = r->pos;
    f->type = rd_uindex(r);
    f->findex = rd_uindex(r);
    f->nregs = rd_uindex(r);
    f->nops = rd_uindex(r);
    if (r->err) return fail(c, "truncated function");
    f->regs = ints_alloc(f->nregs);
    for (int i = 0; i < f->nregs; i++) f->regs[i] = rd_uindex(r);
    f->ops_at = r->pos;
    for (int i = 0; i < f->nops; i++)
        if (decode_op(r, scratch)) return fail(c, "bad op %d in function %d at 0x%zx", i, f->findex, r->pos);
    if (c->hasdebug) {
        read_debug(r, f->nops, NULL, NULL);
        if (c->version >= 3) {
            int n = rd_uindex(r);
            for (int i = 0; i < n && !r->err; i++) rd_uindex(r), rd_index(r);
        }
    }
    f->end = r->pos;
    f->obj = f->field = f->parent = -1;
    f->ref = 0;
    return r->err ? fail(c, "truncated function %d", f->findex) : 0;
}

static int field_at(const HlCode *c, int type, int fid, int *name, int *ftype) {
    int chain[256], n = 0;
    for (int t = type; t >= 0 && n < 256; t = c->types[t].super) chain[n++] = t;
    for (int k = n - 1; k >= 0; k--) {
        const HlType *t = &c->types[chain[k]];
        if (fid < t->nfields) {
            *name = t->fields[2 * fid];
            *ftype = t->fields[2 * fid + 1];
            return 0;
        }
        fid -= t->nfields;
    }
    return -1;
}

static void resolve_names(HlCode *c, HlOp *op) {
    for (int i = 0; i < c->ntypes; i++) {
        HlType *t = &c->types[i];
        if (t->kind != HOBJ && t->kind != HSTRUCT) continue;
        for (int j = 0; j < t->nproto; j++) {
            int fi = t->proto[3 * j + 1];
            if (fi < c->nfindex && c->by_findex[fi] >= 0) {
                HlFunction *f = &c->functions[c->by_findex[fi]];
                f->obj = i;
                f->field = t->proto[3 * j];
            }
        }
        for (int j = 0; j < t->nbindings; j++) {
            int name, ftype, fi = t->bindings[2 * j + 1];
            if (field_at(c, i, t->bindings[2 * j], &name, &ftype)) continue;
            int k = c->types[ftype].kind;
            if (k != HFUN && k != HDYN) continue;
            if (fi < c->nfindex && c->by_findex[fi] >= 0) {
                HlFunction *f = &c->functions[c->by_findex[fi]];
                f->obj = i;
                f->field = name;
            }
        }
    }
    for (int i = 0; i < c->nfunctions; i++) {
        int root = i;
        while (root >= 0 && c->functions[root].obj < 0) root = c->functions[root].parent;
        if (root < 0) continue;
        size_t at = c->functions[i].ops_at;
        for (int k = 0; k < c->functions[i].nops; k++) {
            hl_decode_op(c, &at, op);
            if ((op->op >= OCall0 && op->op <= OCallN) || op->op == OStaticClosure || op->op == OInstanceClosure) {
                int fi = op->a[1];
                if (fi < 0 || fi >= c->nfindex || c->by_findex[fi] < 0) continue;
                HlFunction *g = &c->functions[c->by_findex[fi]];
                if (g->obj >= 0) continue;
                g->parent = root;
                g->ref = c->functions[root].ref++;
            }
        }
    }
}

int hl_read(HlCode *c, const unsigned char *data, size_t size) {
    memset(c, 0, sizeof *c);
    c->data = data;
    c->size = size;
    Rd r = {data, 0, size, 0};
    if (size < 4 || memcmp(data, "HLB", 3)) return fail(c, "not HashLink bytecode");
    r.pos = 3;
    c->version = rbyte(&r);
    if (c->version < 4 || c->version > 5) return fail(c, "unsupported bytecode version %d", c->version);
    c->flags = rd_uindex(&r);
    c->hasdebug = c->flags & 1;
    c->nints = rd_uindex(&r);
    c->nfloats = rd_uindex(&r);
    c->nstrings = rd_uindex(&r);
    c->nbytes = c->version >= 5 ? rd_uindex(&r) : 0;
    c->ntypes = rd_uindex(&r);
    c->nglobals = rd_uindex(&r);
    c->nnatives = rd_uindex(&r);
    c->nfunctions = rd_uindex(&r);
    c->nconstants = c->version >= 4 ? rd_uindex(&r) : 0;
    c->entrypoint = rd_uindex(&r);
    if (r.err) return fail(c, "truncated header");
    c->at[HL_AT_INTS] = r.pos;
    r.pos += 4 * (size_t)c->nints;
    c->at[HL_AT_FLOATS] = r.pos;
    r.pos += 8 * (size_t)c->nfloats;
    c->at[HL_AT_STRINGS] = r.pos;
    if (r.pos > size) return fail(c, "truncated constants");
    if (read_strings(&r, c, c->nstrings, &c->strings, &c->at[HL_AT_STRING_DATA], &c->at[HL_AT_STRING_LENS])) return -1;
    c->at[HL_AT_STRINGS_END] = r.pos;
    if (c->nbytes) {
        int n = ri32(&r);
        r.pos += n;
        for (int i = 0; i < c->nbytes; i++) rd_uindex(&r);
    }
    c->at[HL_AT_DEBUGFILES] = r.pos;
    if (c->hasdebug) {
        HlStr *files;
        c->ndebugfiles = rd_uindex(&r);
        if (read_strings(&r, c, c->ndebugfiles, &files, NULL, NULL)) return -1;
        free(files);
    }
    c->at[HL_AT_TYPES] = r.pos;
    c->types = (HlType *)calloc(c->ntypes, sizeof(HlType));
    for (int i = 0; i < c->ntypes; i++)
        if (read_type(&r, c, &c->types[i])) return -1;
    c->at[HL_AT_GLOBALS] = r.pos;
    c->globals = ints_alloc(c->nglobals);
    for (int i = 0; i < c->nglobals; i++) c->globals[i] = rd_uindex(&r);
    c->at[HL_AT_NATIVES] = r.pos;
    c->natives = (HlNative *)calloc(c->nnatives > 0 ? c->nnatives : 1, sizeof(HlNative));
    for (int i = 0; i < c->nnatives; i++) {
        HlNative *n = &c->natives[i];
        n->lib = rd_index(&r);
        n->name = rd_index(&r);
        n->type = rd_uindex(&r);
        n->findex = rd_uindex(&r);
    }
    if (r.err) return fail(c, "truncated natives");
    c->at[HL_AT_FUNCTIONS] = r.pos;
    HlOp *scratch = (HlOp *)malloc(sizeof(HlOp));
    c->functions = (HlFunction *)calloc(c->nfunctions, sizeof(HlFunction));
    for (int i = 0; i < c->nfunctions; i++)
        if (read_function(&r, c, &c->functions[i], scratch)) return free(scratch), -1;
    c->at[HL_AT_CONSTANTS] = r.pos;
    c->const_global = ints_alloc(c->nconstants);
    c->const_nfields = ints_alloc(c->nconstants);
    c->const_fields = (int **)calloc(c->nconstants > 0 ? c->nconstants : 1, sizeof(int *));
    for (int i = 0; i < c->nconstants; i++) {
        c->const_global[i] = rd_uindex(&r);
        int n = c->const_nfields[i] = rd_uindex(&r);
        if (r.err) break;
        c->const_fields[i] = ints_alloc(n);
        for (int j = 0; j < n; j++) c->const_fields[i][j] = rd_uindex(&r);
    }
    if (r.err) return free(scratch), fail(c, "truncated constants");
    if (r.pos != size) return free(scratch), fail(c, "%zu trailing bytes", size - r.pos);

    c->nfindex = c->nfunctions + c->nnatives;
    c->by_findex = ints_alloc(c->nfindex);
    for (int i = 0; i < c->nfindex; i++) c->by_findex[i] = -1;
    for (int i = 0; i < c->nfunctions; i++) {
        int fi = c->functions[i].findex;
        if (fi >= c->nfindex) return free(scratch), fail(c, "findex %d out of range", fi);
        c->by_findex[fi] = i;
    }
    for (int i = 0; i < c->nnatives; i++) {
        int fi = c->natives[i].findex;
        if (fi >= c->nfindex) return free(scratch), fail(c, "native findex %d out of range", fi);
        c->by_findex[fi] = -i - 2;
    }
    c->global_type = ints_alloc(c->nglobals);
    c->global_string = ints_alloc(c->nglobals);
    for (int i = 0; i < c->nglobals; i++) c->global_type[i] = c->global_string[i] = -1;
    for (int i = 0; i < c->ntypes; i++) {
        HlType *t = &c->types[i];
        if ((t->kind == HOBJ || t->kind == HSTRUCT || t->kind == HENUM) && t->global > 0 && t->global <= c->nglobals)
            c->global_type[t->global - 1] = i;
    }
    for (int i = 0; i < c->nconstants; i++) {
        int g = c->const_global[i];
        if (g < 0 || g >= c->nglobals) continue;
        HlType *t = &c->types[c->globals[g]];
        if (t->kind == HOBJ && t->name >= 0 && c->strings[t->name].len == 6 && !memcmp(c->strings[t->name].s, "String", 6) &&
            c->const_nfields[i] > 0)
            c->global_string[g] = c->const_fields[i][0];
    }
    resolve_names(c, scratch);
    free(scratch);
    return 0;
}

void hl_free(HlCode *c) {
    for (int i = 0; i < c->ntypes; i++) {
        HlType *t = &c->types[i];
        free(t->fields), free(t->proto), free(t->bindings), free(t->args), free(t->text), free(t->constructs);
    }
    for (int i = 0; i < c->nfunctions; i++) free(c->functions[i].regs);
    for (int i = 0; i < c->nconstants; i++) free(c->const_fields[i]);
    free(c->types), free(c->strings), free(c->globals), free(c->natives), free(c->functions);
    free(c->const_global), free(c->const_nfields), free(c->const_fields);
    free(c->by_findex), free(c->global_type), free(c->global_string);
    memset(c, 0, sizeof *c);
}

// ---------------------------------------------------------------- names and shapes

typedef struct {
    char *p;
    size_t len, cap;
} Sb;

static void sb_add(Sb *b, const void *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        b->cap = (b->len + n + 1) * 2;
        b->p = (char *)realloc(b->p, b->cap);
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void sb_str(Sb *b, const char *s) { sb_add(b, s, strlen(s)); }

static void sb_fmt(Sb *b, const char *format, ...) {
    char tmp[64];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(tmp, sizeof tmp, format, args);
    va_end(args);
    sb_add(b, tmp, n);
}

static void sb_hlstr(Sb *b, const HlCode *c, int s) {
    if (s >= 0 && s < c->nstrings && c->strings[s].len > 0) sb_add(b, c->strings[s].s, (unsigned)c->strings[s].len);
}

const char *hl_type_name(HlCode *c, int ti) {
    HlType *t = &c->types[ti];
    if (t->text) return t->text;
    Sb b = {0};
    switch (t->kind) {
    case HOBJ:
    case HSTRUCT:
    case HENUM:
    case HABSTRACT:
        sb_hlstr(&b, c, t->name);
        break;
    case HREF:
    case HNULL:
    case HPACKED:
        sb_str(&b, KIND_NAMES[t->kind]);
        sb_str(&b, "<");
        sb_str(&b, hl_type_name(c, t->param));
        sb_str(&b, ">");
        break;
    case HFUN:
    case HMETHOD:
        sb_str(&b, "(");
        for (int i = 0; i < t->nargs; i++) {
            if (i) sb_str(&b, ",");
            sb_str(&b, hl_type_name(c, t->args[i]));
        }
        sb_str(&b, ")->");
        sb_str(&b, hl_type_name(c, t->ret));
        break;
    case HVIRTUAL:
        sb_str(&b, "virtual<");
        for (int i = 0; i < t->nfields; i++) {
            if (i) sb_str(&b, ",");
            sb_hlstr(&b, c, t->fields[2 * i]);
        }
        sb_str(&b, ">");
        break;
    default:
        sb_str(&b, KIND_NAMES[t->kind]);
    }
    if (!b.p) sb_str(&b, "");
    t->text = b.p;
    return t->text;
}

static void name_into(const HlCode *c, int findex, int stable, Sb *b) {
    int i = findex >= 0 && findex < c->nfindex ? c->by_findex[findex] : -1;
    if (i <= -2) {
        const HlNative *n = &c->natives[-i - 2];
        const HlStr *lib = &c->strings[n->lib];
        int skip = lib->len && lib->s[0] == '?';
        sb_add(b, lib->s + skip, lib->len - skip);
        sb_str(b, "@");
        sb_hlstr(b, c, n->name);
        return;
    }
    if (i < 0) {
        sb_fmt(b, "?%d", findex);
        return;
    }
    const HlFunction *f = &c->functions[i];
    if (f->obj >= 0) {
        sb_hlstr(b, c, c->types[f->obj].name);
        sb_str(b, ".");
        sb_hlstr(b, c, f->field);
    } else if (f->parent >= 0) {
        const HlFunction *p = &c->functions[f->parent];
        sb_hlstr(b, c, c->types[p->obj].name);
        sb_str(b, ".~");
        sb_hlstr(b, c, p->field);
        sb_fmt(b, ".%d", f->ref);
    } else if (stable) {
        sb_str(b, "fun$");
    } else {
        sb_fmt(b, "fun$%d", f->findex);
    }
}

char *hl_function_name(const HlCode *c, int findex, int stable, char *buf, size_t size) {
    Sb b = {0};
    name_into(c, findex, stable, &b);
    snprintf(buf, size, "%s", b.p ? b.p : "");
    free(b.p);
    return buf;
}

int hl_find_function(const HlCode *c, const char *name) {
    char buf[512];
    for (int i = 0; i < c->nfunctions; i++)
        if (!strcmp(hl_function_name(c, c->functions[i].findex, 0, buf, sizeof buf), name)) return i;
    return -1;
}

static int str_eq(const HlCode *c, int s, const char *v) {
    return s >= 0 && s < c->nstrings && c->strings[s].len == (int)strlen(v) && !memcmp(c->strings[s].s, v, c->strings[s].len);
}

int hl_find_native(const HlCode *c, const char *lib, const char *name) {
    for (int i = 0; i < c->nnatives; i++) {
        const HlStr *l = &c->strings[c->natives[i].lib];
        int skip = l->len && l->s[0] == '?';
        if (l->len - skip == (int)strlen(lib) && !memcmp(l->s + skip, lib, l->len - skip) && str_eq(c, c->natives[i].name, name))
            return i;
    }
    return -1;
}

int hl_find_type(const HlCode *c, int kind, const char *name) {
    for (int i = 0; i < c->ntypes; i++)
        if (c->types[i].kind == kind && (!name || str_eq(c, c->types[i].name, name))) return i;
    return -1;
}

int hl_type_field(const HlCode *c, int type, const char *name) {
    int fname, ftype;
    for (int fid = 0; !field_at(c, type, fid, &fname, &ftype); fid++)
        if (str_eq(c, fname, name)) return fid;
    return -1;
}

int hl_field_type(const HlCode *c, int type, int field) {
    int fname, ftype;
    return field >= 0 && !field_at(c, type, field, &fname, &ftype) ? ftype : -1;
}

int hl_find_int(const HlCode *c, int value) {
    for (int i = 0; i < c->nints; i++) {
        int v;
        memcpy(&v, c->data + c->at[HL_AT_INTS] + 4 * (size_t)i, 4);
        if (v == value) return i;
    }
    return -1;
}

static void string_text(Sb *b, const HlCode *c, int s) {
    sb_fmt(b, "s%d:", c->strings[s].len);
    sb_add(b, c->strings[s].s, c->strings[s].len);
}

static void global_text(Sb *b, HlCode *c, int g) {
    if (c->global_string[g] >= 0) {
        string_text(b, c, c->global_string[g]);
    } else if (c->global_type[g] >= 0) {
        const HlStr *n = &c->strings[c->types[c->global_type[g]].name];
        int skip = n->len && n->s[0] == '$';
        sb_str(b, "g");
        sb_add(b, n->s + skip, n->len - skip);
    } else {
        sb_str(b, "g?");
        sb_str(b, hl_type_name(c, c->globals[g]));
    }
}

static void op_text(Sb *b, HlCode *c, const HlOp *o) {
    const int *a = o->a;
    sb_str(b, OP_NAMES[o->op]);
#define NUM(x) sb_fmt(b, " %d", (x))
    switch (o->op) {
    case OCall0:
    case OCall1:
    case OCall2:
    case OCall3:
    case OCall4:
    case OCallN:
    case OStaticClosure:
    case OInstanceClosure:
        NUM(a[0]);
        sb_str(b, " @");
        name_into(c, a[1], 1, b);
        for (int i = 2; i < o->n; i++) NUM(a[i]);
        break;
    case OInt: {
        int v;
        memcpy(&v, c->data + c->at[HL_AT_INTS] + 4 * (size_t)a[1], 4);
        NUM(a[0]);
        sb_fmt(b, " i%d", v);
        break;
    }
    case OFloat: {
        uint64_t v;
        memcpy(&v, c->data + c->at[HL_AT_FLOATS] + 8 * (size_t)a[1], 8);
        NUM(a[0]);
        sb_fmt(b, " f%016llx", (unsigned long long)v);
        break;
    }
    case OString:
    case OBytes:
        NUM(a[0]);
        if (c->version < 5 || o->op == OString) {
            sb_str(b, " ");
            string_text(b, c, a[1]);
        } else {
            NUM(a[1]);
        }
        break;
    case ODynGet:
        NUM(a[0]), NUM(a[1]);
        sb_str(b, " ");
        string_text(b, c, a[2]);
        break;
    case ODynSet:
        NUM(a[0]);
        sb_str(b, " ");
        string_text(b, c, a[1]);
        NUM(a[2]);
        break;
    case OGetGlobal:
        NUM(a[0]);
        sb_str(b, " ");
        global_text(b, c, a[1]);
        break;
    case OSetGlobal:
        sb_str(b, " ");
        global_text(b, c, a[0]);
        NUM(a[1]);
        break;
    case OType:
        NUM(a[0]);
        sb_str(b, " t");
        sb_str(b, hl_type_name(c, a[1]));
        break;
    default:
        for (int i = 0; i < o->n; i++) NUM(a[i]);
    }
#undef NUM
    sb_str(b, ";");
}

uint64_t hl_shape(HlCode *c, int function) {
    const HlFunction *f = &c->functions[function];
    Sb b = {0};
    for (int i = 0; i < f->nregs; i++) {
        sb_str(&b, "R");
        sb_str(&b, hl_type_name(c, f->regs[i]));
        sb_str(&b, ";");
    }
    HlOp *op = (HlOp *)malloc(sizeof(HlOp));
    size_t at = f->ops_at;
    for (int i = 0; i < f->nops; i++) {
        hl_decode_op(c, &at, op);
        op_text(&b, c, op);
    }
    free(op);
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < b.len; i++) h = (h ^ (unsigned char)b.p[i]) * 0x100000001B3ull;
    free(b.p);
    return h;
}

// ---------------------------------------------------------------- functions in editable form

int hl_is_jump(int op) {
    return (op >= OJTrue && op <= OJAlways) || op == OSwitch || op == OTrap;
}

int *hl_jump_target(HlOp *o, int k) {
    switch (o->op) {
    case OJTrue:
    case OJFalse:
    case OJNull:
    case OJNotNull:
    case OTrap:
        return &o->a[1];
    case OJAlways:
        return &o->a[0];
    case OSwitch:
        return k < 0 ? &o->a[2 + o->a[1]] : &o->a[2 + k];
    default:
        if (o->op >= OJSLt && o->op <= OJNotEq) return &o->a[2];
        return NULL;
    }
}

static void each_target(HlOp *o, void (*fn)(int *t, void *ctx), void *ctx) {
    if (!hl_is_jump(o->op)) return;
    if (o->op == OSwitch) {
        for (int k = 0; k < o->a[1]; k++) fn(hl_jump_target(o, k), ctx);
        fn(hl_jump_target(o, -1), ctx);
    } else {
        fn(hl_jump_target(o, 0), ctx);
    }
}

static void fun_grow(HlFun *f, int n) {
    if (n <= f->cap) return;
    f->cap = n * 2;
    f->ops = (HlOp *)realloc(f->ops, f->cap * sizeof(HlOp));
    f->files = (int *)realloc(f->files, f->cap * sizeof(int));
    f->lines = (int *)realloc(f->lines, f->cap * sizeof(int));
}

typedef struct {
    int base, delta, at;
} Shift;

static void to_absolute(int *t, void *ctx) { *t += *(int *)ctx + 1; }
static void to_relative(int *t, void *ctx) { *t -= *(int *)ctx + 1; }

HlFun *hl_fun_decode(HlCode *c, int function) {
    const HlFunction *src = &c->functions[function];
    if (src->nregs > 1024) return NULL;
    HlFun *f = (HlFun *)calloc(1, sizeof(HlFun));
    f->type = src->type;
    f->nregs = src->nregs;
    memcpy(f->regs, src->regs, src->nregs * sizeof(int));
    fun_grow(f, src->nops + 16);
    f->nops = src->nops;
    Rd r = {c->data, src->ops_at, c->size, 0};
    for (int i = 0; i < src->nops; i++) {
        if (decode_op(&r, &f->ops[i]) || f->ops[i].op == OCatch) return hl_fun_free(f), NULL;
        each_target(&f->ops[i], to_absolute, &i);
    }
    if (c->hasdebug) {
        read_debug(&r, src->nops, f->files, f->lines);
    } else {
        memset(f->files, 0, src->nops * sizeof(int));
        memset(f->lines, 0, src->nops * sizeof(int));
    }
    return f;
}

HlFun *hl_fun_copy(const HlFun *src) {
    HlFun *f = (HlFun *)calloc(1, sizeof(HlFun));
    *f = *src;
    f->ops = NULL, f->files = f->lines = NULL, f->cap = 0;
    fun_grow(f, src->nops + 16);
    memcpy(f->ops, src->ops, src->nops * sizeof(HlOp));
    memcpy(f->files, src->files, src->nops * sizeof(int));
    memcpy(f->lines, src->lines, src->nops * sizeof(int));
    return f;
}

HlFun *hl_fun_new(int type, int nregs, const int *regs, const HlOp *ops, int nops, int file, int line) {
    if (nregs > 1024) return NULL;
    HlFun *f = (HlFun *)calloc(1, sizeof(HlFun));
    f->type = type;
    f->nregs = nregs;
    memcpy(f->regs, regs, nregs * sizeof(int));
    fun_grow(f, nops + 16);
    f->nops = nops;
    memcpy(f->ops, ops, nops * sizeof(HlOp));
    for (int i = 0; i < nops; i++) f->files[i] = file, f->lines[i] = line;
    return f;
}

void hl_fun_free(HlFun *f) {
    if (!f) return;
    free(f->ops), free(f->files), free(f->lines), free(f);
}

static void shift_target(int *t, void *ctx) {
    Shift *s = (Shift *)ctx;
    if (*t >= s->at) *t += s->delta;
}

void hl_fun_insert(HlFun *f, int at, const HlOp *ops, int n) {
    fun_grow(f, f->nops + n);
    memmove(&f->ops[at + n], &f->ops[at], (f->nops - at) * sizeof(HlOp));
    memmove(&f->files[at + n], &f->files[at], (f->nops - at) * sizeof(int));
    memmove(&f->lines[at + n], &f->lines[at], (f->nops - at) * sizeof(int));
    int near = at > 0 ? at - 1 : at + n;
    for (int i = 0; i < n; i++) {
        f->ops[at + i] = ops[i];
        f->files[at + i] = f->files[near < f->nops + n ? near : 0];
        f->lines[at + i] = f->lines[near < f->nops + n ? near : 0];
    }
    f->nops += n;
    Shift s = {0, n, at};
    for (int i = 0; i < f->nops; i++) each_target(&f->ops[i], shift_target, &s);
}

typedef struct {
    const int *map;
    int bad;
} Remap;

static void remap_target(int *t, void *ctx) {
    Remap *m = (Remap *)ctx;
    if (*t < 0 || m->map[*t] < 0) m->bad = 1;
    else *t = m->map[*t];
}

int hl_fun_reorder(HlFun *f, const int *order, int n) {
    int *map = ints_alloc(f->nops);
    for (int i = 0; i < f->nops; i++) map[i] = -1;
    for (int i = 0; i < n; i++) {
        if (order[i] < 0 || order[i] >= f->nops || map[order[i]] >= 0) return free(map), -1;
        map[order[i]] = i;
    }
    HlOp *ops = (HlOp *)malloc((n + 16) * sizeof(HlOp));
    int *files = ints_alloc(n + 16), *lines = ints_alloc(n + 16);
    for (int i = 0; i < n; i++) {
        ops[i] = f->ops[order[i]];
        files[i] = f->files[order[i]];
        lines[i] = f->lines[order[i]];
    }
    Remap m = {map, 0};
    for (int i = 0; i < n; i++) each_target(&ops[i], remap_target, &m);
    free(map);
    if (m.bad) return free(ops), free(files), free(lines), -1;
    free(f->ops), free(f->files), free(f->lines);
    f->ops = ops, f->files = files, f->lines = lines, f->nops = n, f->cap = n + 16;
    return 0;
}

// ---------------------------------------------------------------- writing

typedef struct {
    unsigned char *p;
    size_t len, cap;
} Buf;

static void put(Buf *b, const void *s, size_t n) {
    if (b->len + n > b->cap) {
        b->cap = (b->len + n) * 2 + 64;
        b->p = (unsigned char *)realloc(b->p, b->cap);
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
}

static void put_byte(Buf *b, int v) {
    unsigned char c = (unsigned char)v;
    put(b, &c, 1);
}

static void put_index(Buf *b, int v) {
    unsigned int a = v < 0 ? (unsigned int)-v : (unsigned int)v;
    int neg = v < 0 ? 0x20 : 0;
    if (v >= 0 && v < 0x80) {
        put_byte(b, v);
    } else if (a < 0x2000) {
        put_byte(b, 0x80 | neg | (a >> 8));
        put_byte(b, a & 0xFF);
    } else {
        put_byte(b, 0xC0 | neg | (a >> 24));
        put_byte(b, (a >> 16) & 0xFF);
        put_byte(b, (a >> 8) & 0xFF);
        put_byte(b, a & 0xFF);
    }
}

static void put_i32(Buf *b, int v) { put(b, &v, 4); }

static int encode_function(const HlCode *c, int findex, const HlFun *src, Buf *b) {
    HlFun *f = hl_fun_copy(src);
    for (int i = 0; i < f->nops; i++) {
        HlOp *o = &f->ops[i];
        if (o->op == OSwitch) {
            for (int k = -1; k < o->a[1]; k++)
                if (*hl_jump_target(o, k) <= i) return hl_fun_free(f), -1; // Switch offsets are unsigned
        }
        each_target(o, to_relative, &i);
    }
    put_index(b, f->type);
    put_index(b, findex);
    put_index(b, f->nregs);
    put_index(b, f->nops);
    for (int i = 0; i < f->nregs; i++) put_index(b, f->regs[i]);
    for (int i = 0; i < f->nops; i++) {
        const HlOp *o = &f->ops[i];
        put_index(b, o->op);
        int n = OP_ARGS[o->op];
        if (n >= 0) {
            for (int k = 0; k < n; k++) put_index(b, o->a[k]);
        } else if (o->op == OSwitch) {
            put_index(b, o->a[0]);
            put_index(b, o->a[1]);
            for (int k = 0; k <= o->a[1]; k++) put_index(b, o->a[2 + k]);
        } else {
            put_index(b, o->a[0]);
            put_index(b, o->a[1]);
            put_byte(b, o->a[2]);
            for (int k = 0; k < o->a[2]; k++) put_index(b, o->a[3 + k]);
        }
    }
    if (c->hasdebug) {
        int file = -1;
        for (int i = 0; i < f->nops; i++) {
            if (f->files[i] != file) {
                file = f->files[i] < 0 ? 0 : f->files[i];
                put_byte(b, ((file >> 8) << 1) | 1);
                put_byte(b, file & 0xFF);
            }
            int line = f->lines[i];
            put_byte(b, (line & 31) << 3);
            put_byte(b, (line >> 5) & 0xFF);
            put_byte(b, (line >> 13) & 0xFF);
        }
        if (c->version >= 3) put_index(b, 0);
    }
    hl_fun_free(f);
    return 0;
}

typedef struct {
    int function; // function index for a replacement, -1 for an added function
    int findex;
    Buf record;
} Record;

struct HlEdit {
    HlCode *c;
    Buf string_data, string_lens, floats, ints;
    char **new_strings;
    int nnew_strings, nnew_natives, nnew_floats, nnew_ints, nnew_globals;
    Buf natives, globals;
    Record *records;
    int nrecords, nadded;
};

HlEdit *hl_edit_begin(HlCode *c) {
    HlEdit *e = (HlEdit *)calloc(1, sizeof(HlEdit));
    e->c = c;
    return e;
}

// Marks pack the counts of everything added so far, so a rollback can drop what came after.
typedef struct {
    size_t string_data, string_lens, natives, floats, ints, globals;
    int nnew_strings, nnew_natives, nnew_floats, nnew_ints, nnew_globals, nrecords, nadded;
} Mark;

static Mark marks[64];
static int nmarks;

int hl_edit_mark(HlEdit *e) {
    if (nmarks == 64) nmarks = 0;
    marks[nmarks] = (Mark){e->string_data.len, e->string_lens.len, e->natives.len, e->floats.len, e->ints.len,
                           e->globals.len, e->nnew_strings, e->nnew_natives, e->nnew_floats, e->nnew_ints,
                           e->nnew_globals, e->nrecords, e->nadded};
    return nmarks++;
}

void hl_edit_rollback(HlEdit *e, int mark) {
    Mark m = marks[mark];
    for (int i = m.nnew_strings; i < e->nnew_strings; i++) free(e->new_strings[i]);
    for (int i = m.nrecords; i < e->nrecords; i++) free(e->records[i].record.p);
    e->string_data.len = m.string_data, e->string_lens.len = m.string_lens, e->natives.len = m.natives;
    e->floats.len = m.floats, e->nnew_floats = m.nnew_floats, e->ints.len = m.ints, e->nnew_ints = m.nnew_ints;
    e->globals.len = m.globals, e->nnew_globals = m.nnew_globals;
    e->nnew_strings = m.nnew_strings, e->nnew_natives = m.nnew_natives, e->nrecords = m.nrecords, e->nadded = m.nadded;
}

int hl_edit_int(HlEdit *e, int v) {
    int i = hl_find_int(e->c, v);
    if (i >= 0) return i;
    for (i = 0; i < e->nnew_ints; i++)
        if (!memcmp(e->ints.p + 4 * (size_t)i, &v, 4)) return e->c->nints + i;
    put(&e->ints, &v, 4);
    return e->c->nints + e->nnew_ints++;
}

int hl_edit_float(HlEdit *e, double v) {
    const unsigned char *table = e->c->data + e->c->at[HL_AT_FLOATS];
    for (int i = 0; i < e->c->nfloats; i++)
        if (!memcmp(table + 8 * (size_t)i, &v, 8)) return i;
    for (int i = 0; i < e->nnew_floats; i++)
        if (!memcmp(e->floats.p + 8 * (size_t)i, &v, 8)) return e->c->nfloats + i;
    put(&e->floats, &v, 8);
    return e->c->nfloats + e->nnew_floats++;
}

int hl_edit_string(HlEdit *e, const char *s) {
    for (int i = 0; i < e->c->nstrings; i++)
        if (str_eq(e->c, i, s)) return i;
    for (int i = 0; i < e->nnew_strings; i++)
        if (!strcmp(e->new_strings[i], s)) return e->c->nstrings + i;
    e->new_strings = (char **)realloc(e->new_strings, (e->nnew_strings + 1) * sizeof(char *));
    e->new_strings[e->nnew_strings] = strdup(s);
    put(&e->string_data, s, strlen(s) + 1);
    put_index(&e->string_lens, (int)strlen(s));
    return e->c->nstrings + e->nnew_strings++;
}

int hl_edit_global(HlEdit *e, int type) {
    put_index(&e->globals, type);
    return e->c->nglobals + e->nnew_globals++;
}

static int next_findex(HlEdit *e) { return e->c->nfindex + e->nnew_natives + e->nadded; }

int hl_edit_native(HlEdit *e, const char *lib, const char *name, int type) {
    int l = hl_edit_string(e, lib), n = hl_edit_string(e, name), findex = next_findex(e);
    put_index(&e->natives, l);
    put_index(&e->natives, n);
    put_index(&e->natives, type);
    put_index(&e->natives, findex);
    e->nnew_natives++;
    return findex;
}

static Record *new_record(HlEdit *e) {
    e->records = (Record *)realloc(e->records, (e->nrecords + 1) * sizeof(Record));
    Record *r = &e->records[e->nrecords++];
    memset(r, 0, sizeof *r);
    return r;
}

int hl_edit_replace(HlEdit *e, int function, const HlFun *f) {
    for (int i = e->nrecords - 1; i >= 0; i--)
        if (e->records[i].function == function) return -1; // one replacement per function per pass
    Record *r = new_record(e);
    r->function = function;
    r->findex = e->c->functions[function].findex;
    if (encode_function(e->c, r->findex, f, &r->record)) return free(r->record.p), e->nrecords--, -1;
    return 0;
}

int hl_edit_add(HlEdit *e, const HlFun *f) {
    int findex = next_findex(e);
    Record *r = new_record(e);
    r->function = -1;
    r->findex = findex;
    if (encode_function(e->c, findex, f, &r->record)) return free(r->record.p), e->nrecords--, -1;
    e->nadded++;
    return findex;
}

unsigned char *hl_edit_write(HlEdit *e, size_t *size) {
    const HlCode *c = e->c;
    const unsigned char *d = c->data;
    Buf b = {0};
    put(&b, "HLB", 3);
    put_byte(&b, c->version);
    put_index(&b, c->flags);
    put_index(&b, c->nints + e->nnew_ints);
    put_index(&b, c->nfloats + e->nnew_floats);
    put_index(&b, c->nstrings + e->nnew_strings);
    if (c->version >= 5) put_index(&b, c->nbytes);
    put_index(&b, c->ntypes);
    put_index(&b, c->nglobals + e->nnew_globals);
    put_index(&b, c->nnatives + e->nnew_natives);
    put_index(&b, c->nfunctions + e->nadded);
    if (c->version >= 4) put_index(&b, c->nconstants);
    put_index(&b, c->entrypoint);
    put(&b, d + c->at[HL_AT_INTS], c->at[HL_AT_FLOATS] - c->at[HL_AT_INTS]);
    put(&b, e->ints.p, e->ints.len);
    put(&b, d + c->at[HL_AT_FLOATS], c->at[HL_AT_STRINGS] - c->at[HL_AT_FLOATS]);
    put(&b, e->floats.p, e->floats.len);
    put_i32(&b, (int)(c->at[HL_AT_STRING_LENS] - c->at[HL_AT_STRING_DATA] + e->string_data.len));
    put(&b, d + c->at[HL_AT_STRING_DATA], c->at[HL_AT_STRING_LENS] - c->at[HL_AT_STRING_DATA]);
    put(&b, e->string_data.p, e->string_data.len);
    put(&b, d + c->at[HL_AT_STRING_LENS], c->at[HL_AT_STRINGS_END] - c->at[HL_AT_STRING_LENS]);
    put(&b, e->string_lens.p, e->string_lens.len);
    put(&b, d + c->at[HL_AT_STRINGS_END], c->at[HL_AT_NATIVES] - c->at[HL_AT_STRINGS_END]);
    put(&b, e->globals.p, e->globals.len);
    put(&b, d + c->at[HL_AT_NATIVES], c->at[HL_AT_FUNCTIONS] - c->at[HL_AT_NATIVES]);
    put(&b, e->natives.p, e->natives.len);
    int *replaced = ints_alloc(c->nfunctions);
    for (int i = 0; i < c->nfunctions; i++) replaced[i] = -1;
    for (int k = 0; k < e->nrecords; k++)
        if (e->records[k].function >= 0) replaced[e->records[k].function] = k;
    for (int i = 0; i < c->nfunctions; i++) {
        if (replaced[i] >= 0) put(&b, e->records[replaced[i]].record.p, e->records[replaced[i]].record.len);
        else put(&b, d + c->functions[i].record, c->functions[i].end - c->functions[i].record);
    }
    free(replaced);
    for (int k = 0; k < e->nrecords; k++)
        if (e->records[k].function < 0) put(&b, e->records[k].record.p, e->records[k].record.len);
    put(&b, d + c->at[HL_AT_CONSTANTS], c->size - c->at[HL_AT_CONSTANTS]);
    *size = b.len;
    return b.p;
}

void hl_edit_free(HlEdit *e) {
    for (int i = 0; i < e->nnew_strings; i++) free(e->new_strings[i]);
    for (int i = 0; i < e->nrecords; i++) free(e->records[i].record.p);
    free(e->new_strings), free(e->records), free(e->string_data.p), free(e->string_lens.p), free(e->natives.p);
    free(e->floats.p), free(e->ints.p), free(e->globals.p);
    free(e);
}
