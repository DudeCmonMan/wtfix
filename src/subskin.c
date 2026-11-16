// subskin's natives: a count of repeat computes in a frame tells a sub-skin its base recomputed. Main thread only.
#include <stdbool.h>
#include "live.h"

static int repeats;

static void joints_repeat(void) { repeats++; }
static int joints_gen(void) { return repeats; }
static bool joints_fresh(int seen) { return wt_live[LIVE_SUBSKIN_ONCE].value >= 0.5 && seen == repeats; }

#define HL_PRIM(name) \
    __declspec(dllexport) void *hlp_##name(const char **sign) { *sign = NULL; return (void *)name; }
HL_PRIM(joints_repeat)
HL_PRIM(joints_gen)
HL_PRIM(joints_fresh)
