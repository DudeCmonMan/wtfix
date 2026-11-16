// HL object field offsets from the bytecode's types, for natives that read objects in place.
#pragma once
#include "hlcode.h"

// field as hl_type_field numbers it; -1 for a non-object or a packed struct field.
int wt_field_offset(HlCode *c, int type, int field);

// hl_alloc_array's header: type, element type, size.
#define WT_VARRAY_DATA 24
