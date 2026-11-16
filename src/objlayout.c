// As libhl's hl_get_obj_rt: root class first, fields self-aligned, a subclass resumes after its parent's last field.
#include "objlayout.h"

static int kind_size(int kind) {
    switch (kind) {
    case HVOID: return 0;
    case HUI8: case HBOOL: return 1;
    case HUI16: return 2;
    case HI32: case HF32: return 4;
    case HPACKED: return -1;
    default: return 8;
    }
}

int wt_field_offset(HlCode *c, int type, int field) {
    int chain[64], n = 0;
    for (int t = type; t >= 0; t = c->types[t].super) {
        if (c->types[t].kind != HOBJ || n == 64) return -1;
        chain[n++] = t;
    }
    int at = 8, index = 0;
    for (int k = n - 1; k >= 0; k--) {
        const HlType *t = &c->types[chain[k]];
        for (int i = 0; i < t->nfields; i++, index++) {
            int size = kind_size(c->types[t->fields[2 * i + 1]].kind);
            if (size < 0) return -1;
            if (size > 1) at = (at + size - 1) & -size;
            if (index == field) return at;
            at += size;
        }
    }
    return -1;
}
