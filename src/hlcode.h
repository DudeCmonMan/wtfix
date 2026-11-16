// HashLink bytecode reader, function names, shape signatures and editor; follows the VM's src/code.c.
#pragma once
#include <stddef.h>
#include <stdint.h>

enum {
    HVOID, HUI8, HUI16, HI32, HI64, HF32, HF64, HBOOL, HBYTES, HDYN, HFUN, HOBJ, HARRAY, HTYPE, HREF, HVIRTUAL,
    HDYNOBJ, HABSTRACT, HENUM, HNULL, HMETHOD, HSTRUCT, HPACKED, HGUID, HLAST
};

enum {
    OMov, OInt, OFloat, OBool, OBytes, OString, ONull, OAdd, OSub, OMul, OSDiv, OUDiv, OSMod, OUMod, OShl, OSShr,
    OUShr, OAnd, OOr, OXor, ONeg, ONot, OIncr, ODecr, OCall0, OCall1, OCall2, OCall3, OCall4, OCallN, OCallMethod,
    OCallThis, OCallClosure, OStaticClosure, OInstanceClosure, OVirtualClosure, OGetGlobal, OSetGlobal, OField,
    OSetField, OGetThis, OSetThis, ODynGet, ODynSet, OJTrue, OJFalse, OJNull, OJNotNull, OJSLt, OJSGte, OJSGt, OJSLte,
    OJULt, OJUGte, OJNotLt, OJNotGte, OJEq, OJNotEq, OJAlways, OToDyn, OToSFloat, OToUFloat, OToInt, OSafeCast,
    OUnsafeCast, OToVirtual, OLabel, ORet, OThrow, ORethrow, OSwitch, ONullCheck, OTrap, OEndTrap, OGetI8, OGetI16,
    OGetMem, OGetArray, OSetI8, OSetI16, OSetMem, OSetArray, ONew, OArraySize, OType, OGetType, OGetTID, ORef, OUnref,
    OSetref, OMakeEnum, OEnumAlloc, OEnumIndex, OEnumField, OSetEnumField, OAssert, ORefData, ORefOffset, ONop,
    OPrefetch, OAsm, OCatch, OLast
};

typedef struct {
    const char *s;
    int len;
} HlStr;

typedef struct {
    int kind;
    int name;   // string, or -1
    int super;  // type, or -1
    int global; // global + 1, or 0
    int nfields, nproto, nbindings, nargs;
    int *fields;   // (name string, type) pairs
    int *proto;    // (name string, findex, pindex) triples
    int *bindings; // (field, findex) pairs
    int *args;
    int ret;
    int param;
    int nconstructs;
    int *constructs; // name strings
    char *text; // canonical name, lazy
} HlType;

typedef struct {
    int type, findex, nregs, nops;
    int *regs;
    size_t record, ops_at, end; // file offsets: the record, its first op, one past its debug info
    int obj;    // owning type, or -1
    int field;  // field name string, or -1
    int parent; // function index of the named root, or -1
    int ref;
} HlFunction;

typedef struct {
    int lib, name, type, findex;
} HlNative;

enum {
    HL_AT_INTS, HL_AT_FLOATS, HL_AT_STRINGS, HL_AT_STRING_DATA, HL_AT_STRING_LENS, HL_AT_STRINGS_END,
    HL_AT_DEBUGFILES, HL_AT_TYPES, HL_AT_GLOBALS, HL_AT_NATIVES, HL_AT_FUNCTIONS, HL_AT_CONSTANTS, HL_AT_COUNT
};

typedef struct {
    const unsigned char *data;
    size_t size;
    int version, flags, hasdebug;
    int nints, nfloats, nstrings, nbytes, ntypes, nglobals, nnatives, nfunctions, nconstants, entrypoint, ndebugfiles;
    HlStr *strings;
    HlType *types;
    int *globals;
    HlNative *natives;
    HlFunction *functions;
    int *const_global, *const_nfields, **const_fields;
    int nfindex;        // functions + natives
    int *by_findex;     // findex -> function index, or -(native index) - 2, or -1
    int *global_type;   // global -> type whose class or enum object it holds, or -1
    int *global_string; // global -> string literal it holds, or -1
    size_t at[HL_AT_COUNT];
    char error[256];
} HlCode;

#define HL_OP_MAX 300

// a[]: CallN/Method/This/Closure and MakeEnum as p1, p2, count, args...; Switch as p1, count, offsets..., default
typedef struct {
    int op, n;
    int a[HL_OP_MAX];
} HlOp;

int hl_read(HlCode *c, const unsigned char *data, size_t size); // 0 on success, else c->error
void hl_free(HlCode *c);
int hl_decode_op(const HlCode *c, size_t *at, HlOp *op); // advances *at; 0 on success

// "Type.field", "Root.~field.N", "fun$N" or "lib@name"; stable drops fun$ indices, which differ between builds.
char *hl_function_name(const HlCode *c, int findex, int stable, char *buf, size_t size);
int hl_find_function(const HlCode *c, const char *name); // function index (not findex), or -1
int hl_find_native(const HlCode *c, const char *lib, const char *name); // native index, or -1
int hl_find_type(const HlCode *c, int kind, const char *name);
int hl_type_field(const HlCode *c, int type, const char *name); // supers' fields first
int hl_field_type(const HlCode *c, int type, int field);
int hl_find_int(const HlCode *c, int value);
const char *hl_type_name(HlCode *c, int type);
uint64_t hl_shape(HlCode *c, int function); // FNV-1a 64 of the canonical text

// Jump targets are absolute op indices in this form.
typedef struct {
    int type, nregs, nops, cap;
    int regs[1024];
    HlOp *ops;
    int *files, *lines;
} HlFun;

HlFun *hl_fun_decode(HlCode *c, int function); // NULL if too many regs or an op it can't relocate
HlFun *hl_fun_copy(const HlFun *f);
// regs start with the arguments, as the type has them; ops use absolute targets; all ops get one source line.
HlFun *hl_fun_new(int type, int nregs, const int *regs, const HlOp *ops, int nops, int file, int line);
void hl_fun_free(HlFun *f);
int hl_is_jump(int op);
// Targets >= at move with their op, so a target `at` in the inserted ops names the original op, not the block.
void hl_fun_insert(HlFun *f, int at, const HlOp *ops, int n);
// New op i is old op order[i]; every jump target must be kept.
int hl_fun_reorder(HlFun *f, const int *order, int n);
// For Switch, case k, or the default when k < 0.
int *hl_jump_target(HlOp *op, int k);

typedef struct HlEdit HlEdit;
HlEdit *hl_edit_begin(HlCode *c);
int hl_edit_mark(HlEdit *e);
void hl_edit_rollback(HlEdit *e, int mark);
int hl_edit_string(HlEdit *e, const char *s);                       // existing or added
int hl_edit_float(HlEdit *e, double v);                             // existing or added
int hl_edit_int(HlEdit *e, int v);                                  // existing or added
int hl_edit_native(HlEdit *e, const char *lib, const char *name, int type); // returns a findex
int hl_edit_global(HlEdit *e, int type);                            // starts zero
int hl_edit_replace(HlEdit *e, int function, const HlFun *f);
int hl_edit_add(HlEdit *e, const HlFun *f);                         // returns its findex
unsigned char *hl_edit_write(HlEdit *e, size_t *size);              // malloc'd
void hl_edit_free(HlEdit *e);
