// What a worker's copy of the draw state is made of; the patch fills the offsets.
#pragma once

// The fields Heaps' draw loop writes, copied per worker; the rest of each object is shared.
enum { COPY_OBJ, COPY_BYTES, COPY_ARRAY, COPY_MAP };
enum {
    C_OUTPUT, C_CONTEXT, C_ENGINE, C_DRIVER, C_PIPELINE, C_GLOBALS, C_INTMAP, C_BUFFERS, C_SHADERBUFFERS, C_ARRAYINT,
    C_ARRAYF32, C_ARRAYOBJ, C_BOX, C_COUNT
};
typedef struct {
    int cls;
    const char *field;
    int kind, child, offset;
} WtOwned;
extern const char *const wt_defer_classes[C_COUNT];
extern WtOwned wt_defer_owned[];
extern const int wt_defer_nowned;
// PassObject.obj (ranges keep one object's pass objects together) and h3d.Engine's counters (summed back).
extern int wt_defer_off_obj, wt_defer_off_calls, wt_defer_off_tris, wt_defer_off_switches, wt_defer_off_dispatches;
// The driver's compiled shaders and each stage's copy of its last parameter upload (spoiled after a split).
extern int wt_defer_off_shaders, wt_defer_off_vertex, wt_defer_off_fragment, wt_defer_off_params_content;

int wt_defer_stats(char *out, int size); // for the settings panel
