// Pass lists read in place, with field offsets from the bytecode.
#pragma once
#include "hlcode.h"

int wt_pass_layout(HlCode *c);                             // 0, or -1 when a field moved (names and counts then fail)
void wt_pass_name(void *passes, char *out, int size);      // "" for an empty list
int wt_pass_count(void *passes, int limit);                // pass objects in the list, up to limit
// Up to `parts` even ranges that never part one scene object's pass objects; returns how many.
int wt_pass_ranges(void *passes, int off_obj, int parts, void **firsts, int *counts);
