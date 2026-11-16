// The field offsets the skin joint kernel reads, worked out from the bytecode's types by the subskin fix.
#pragma once
#include "hlcode.h"

enum {
    SK_SKINDATA, SK_JOINTSDATA, SK_PALETTE, SK_ABSPOS, SK_RETARGET,
    SK_BASE,
    AS_ALLJOINTS,
    AR_LENGTH, AR_ARRAY,
    J_INDEX, J_BIND, J_PARENT, J_FOLLOW, J_DEFMAT, J_TRANSPOS, J_RETARGET,
    JD_REL, JD_ABS, JD_ADD, JD_TARGET, JD_ORIGIN,
    M_11, // _11.._44 follow it
    WT_SKIN_FIELDS
};
// The class each field belongs to, for the run-time check against libhl.
enum { ROLE_SKIN, ROLE_SUBSKIN, ROLE_ANIM, ROLE_ARRAY, ROLE_JOINT, ROLE_JOINTDATA, ROLE_MATRIX, WT_SKIN_ROLES };

typedef struct {
    int ready;
    int off[WT_SKIN_FIELDS];
    int fid[WT_SKIN_FIELDS];
} WtSkinLayout;

extern WtSkinLayout wt_skin_layout;
extern const int wt_skin_role[WT_SKIN_FIELDS];
// Checks each field's type; 0, or -1 with the reason logged.
int wt_skin_layout_build(HlCode *c, WtSkinLayout *out);
