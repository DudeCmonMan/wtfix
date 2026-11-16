// Label jumps for hl_fun_new bodies. A backward jump must target an OLabel: the JIT drops cached registers only there.
#pragma once
#include "fix.h"
#include <stdlib.h>

typedef struct {
    HlOp *ops;
    int n, label[48], at[256], to[256], njumps;
} Asm;

static inline void asm_begin(Asm *a, int cap) {
    *a = (Asm){0};
    a->ops = (HlOp *)calloc(cap, sizeof(HlOp));
}
static inline void op(Asm *a, HlOp o) { a->ops[a->n++] = o; }
static inline void jump(Asm *a, HlOp o, int label) {
    a->at[a->njumps] = a->n, a->to[a->njumps++] = label;
    op(a, o);
}
static inline void mark(Asm *a, int label) { a->label[label] = a->n; }
static inline void resolve(Asm *a) {
    for (int j = 0; j < a->njumps; j++) *hl_jump_target(&a->ops[a->at[j]], 0) = a->label[a->to[j]];
}
