// The main pass's loop split for worker threads; a late fix, as it edits driver functions other fixes edit.
#include <stdlib.h>
#include <string.h>
#include "../deferdraw.h"
#include "../fix.h"
#include "../objlayout.h"
#include "../passes.h"

// What a worker's copy is made of (offsets found here).
const char *const wt_defer_classes[C_COUNT] = {
    "h3d.pass.Output", "h3d.scene.RenderContext", "h3d.Engine", "h3d.impl.DirectXDriver", "h3d.impl.PipelineState",
    "hxsl.Globals", "haxe.ds.IntMap", "h3d.shader.Buffers", "h3d.shader.ShaderBuffers", "hl.types.ArrayBytes_Int",
    "hl.types.ArrayBytes_hl_F32", "hl.types.ArrayObj", "dx.ResourceBox"};
WtOwned wt_defer_owned[] = {
    {C_OUTPUT, "ctx", COPY_OBJ, C_CONTEXT, -1},
    {C_CONTEXT, "engine", COPY_OBJ, C_ENGINE, -1},
    {C_CONTEXT, "globals", COPY_OBJ, C_GLOBALS, -1},
    {C_CONTEXT, "shaderBuffers", COPY_OBJ, C_BUFFERS, -1},
    {C_ENGINE, "driver", COPY_OBJ, C_DRIVER, -1},
    {C_DRIVER, "viewport", COPY_BYTES, 0, -1},
    {C_DRIVER, "rects", COPY_BYTES, 0, -1},
    {C_DRIVER, "blendFactors", COPY_BYTES, 0, -1},
    {C_DRIVER, "box", COPY_OBJ, C_BOX, -1},
    {C_DRIVER, "strides", COPY_OBJ, C_ARRAYINT, -1},
    {C_DRIVER, "offsets", COPY_OBJ, C_ARRAYINT, -1},
    {C_DRIVER, "currentTargets", COPY_ARRAY, 0, -1},
    {C_DRIVER, "currentTargetResources", COPY_ARRAY, 0, -1},
    {C_DRIVER, "currentVBuffers", COPY_ARRAY, 0, -1},
    {C_DRIVER, "vertexShader", COPY_OBJ, C_PIPELINE, -1},
    {C_DRIVER, "pixelShader", COPY_OBJ, C_PIPELINE, -1},
    {C_DRIVER, "tmpTextures", COPY_OBJ, C_ARRAYOBJ, -1},
    {C_PIPELINE, "samplers", COPY_ARRAY, 0, -1},
    {C_PIPELINE, "samplerBits", COPY_OBJ, C_ARRAYINT, -1},
    {C_PIPELINE, "resources", COPY_ARRAY, 0, -1},
    {C_PIPELINE, "buffers", COPY_ARRAY, 0, -1},
    {C_GLOBALS, "map", COPY_OBJ, C_INTMAP, -1},
    {C_INTMAP, "h", COPY_MAP, 0, -1},
    {C_BUFFERS, "vertex", COPY_OBJ, C_SHADERBUFFERS, -1},
    {C_BUFFERS, "fragment", COPY_OBJ, C_SHADERBUFFERS, -1},
    {C_SHADERBUFFERS, "globals", COPY_OBJ, C_ARRAYF32, -1},
    {C_SHADERBUFFERS, "params", COPY_OBJ, C_ARRAYF32, -1},
    {C_SHADERBUFFERS, "tex", COPY_OBJ, C_ARRAYOBJ, -1},
    {C_SHADERBUFFERS, "buffers", COPY_OBJ, C_ARRAYOBJ, -1},
    {C_SHADERBUFFERS, "texHandles", COPY_OBJ, C_ARRAYOBJ, -1},
    {C_SHADERBUFFERS, "bufHandles", COPY_OBJ, C_ARRAYOBJ, -1},
    {C_ARRAYINT, "bytes", COPY_BYTES, 0, -1},
    {C_ARRAYF32, "bytes", COPY_BYTES, 0, -1},
    {C_ARRAYOBJ, "array", COPY_ARRAY, 0, -1},
};
const int wt_defer_nowned = sizeof wt_defer_owned / sizeof wt_defer_owned[0];
int wt_defer_off_obj = -1, wt_defer_off_calls = -1, wt_defer_off_tris = -1, wt_defer_off_switches = -1,
    wt_defer_off_dispatches = -1, wt_defer_off_shaders = -1, wt_defer_off_vertex = -1, wt_defer_off_fragment = -1,
    wt_defer_off_params_content = -1;

// Registers an edit asks for, added to the function when it is applied.
enum { R_VOID = -1, R_BOOL = -2, R_DYN = -3, R_DYN2 = -4, R_CLOSURE = -5, R_COUNT = 5 };
enum { E_ENTRY, E_PLAIN, E_REPLACE };

typedef struct {
    int plan, at, how, n;
    HlOp ops[6];
} Edit;

typedef struct {
    int function, changed;
    HlFun *f;
} Plan;

#define MAX_PLANS 160
#define MAX_EDITS 400
static Plan *plans;
static Edit *edits;
static int nplans, nedits;
static int tvoid, tbool, tdyn, tclosure;

static Plan *plan_for(HlCode *c, int function) {
    for (int i = 0; i < nplans; i++)
        if (plans[i].function == function) return &plans[i];
    if (nplans >= MAX_PLANS) return NULL;
    Plan *p = &plans[nplans];
    p->function = function, p->changed = 0, p->f = hl_fun_decode(c, function);
    if (!p->f) return NULL;
    nplans++;
    return p;
}

static int add_edit(Plan *p, int at, int how, const HlOp *ops, int n) {
    if (!p || nedits >= MAX_EDITS) return -1;
    Edit *e = &edits[nedits++];
    e->plan = (int)(p - plans), e->at = at, e->how = how, e->n = n;
    memcpy(e->ops, ops, n * sizeof(HlOp));
    p->changed = 1;
    return 0;
}

static int by_plan_at_desc(const void *a, const void *b) {
    const Edit *x = (const Edit *)a, *y = (const Edit *)b;
    return x->plan != y->plan ? x->plan - y->plan : y->at - x->at;
}

// A plan's edits, last op first so each one's position still holds, with the registers they asked for.
static int apply_plan(Plan *p, Edit *first, int n) {
    HlFun *f = p->f;
    int regs[R_COUNT] = {-1, -1, -1, -1, -1}, types[R_COUNT] = {tvoid, tbool, tdyn, tdyn, tclosure};
    for (int k = 0; k < n; k++) {
        Edit *e = &first[k];
        HlOp block[7];
        for (int i = 0; i < e->n; i++) {
            block[i + 1] = e->ops[i];
            for (int j = 0; j < block[i + 1].n; j++) {
                int v = block[i + 1].a[j];
                if (v >= 0 || v < -R_COUNT) continue;
                if (regs[-v - 1] < 0) {
                    if (f->nregs >= 1024) return -1;
                    regs[-v - 1] = f->nregs, f->regs[f->nregs++] = types[-v - 1];
                }
                block[i + 1].a[j] = regs[-v - 1];
            }
        }
        if (e->how == E_REPLACE) f->ops[e->at] = block[1];
        else if (e->how == E_PLAIN) hl_fun_insert(f, e->at, block + 1, e->n);
        else if (hl_fun_insert_entry(f, e->at, block + 1, e->n)) {
            block[0] = wt_op(OLabel, 0); // a loop jumps back to the spot: the JIT drops its register cache at labels
            if (hl_fun_insert_entry(f, e->at, block, e->n + 1)) return -1;
        }
    }
    return 0;
}

static int jumps_into(const HlFun *f, int target) {
    for (int i = 0; i < f->nops; i++) {
        HlOp *o = &f->ops[i];
        if (!hl_is_jump(o->op)) continue;
        for (int k = o->op == OSwitch ? -1 : 0; k < (o->op == OSwitch ? o->a[1] : 1); k++)
            if (*hl_jump_target(o, k) == target) return 1;
    }
    return 0;
}

// The loop over the sorted list: "r = passes.current; label; if r == null goto exit; ...; goto label".
static int find_loop(HlCode *c, const HlFun *f, int *load, int *body, int *exit) {
    int tlist = hl_find_type(c, HOBJ, "h3d.pass.PassList"), current = tlist < 0 ? -1 : hl_type_field(c, tlist, "current");
    int found = 0;
    for (int i = 0; i + 2 < f->nops; i++) {
        const HlOp *a = &f->ops[i], *label = &f->ops[i + 1], *test = &f->ops[i + 2];
        if (a->op != OField || a->a[1] != 1 || a->a[2] != current || label->op != OLabel || test->op != OJNull ||
            test->a[0] != a->a[0])
            continue;
        *load = i, *body = i + 3, *exit = *hl_jump_target((HlOp *)test, 0), found++;
    }
    if (found != 1) return wt_log("  Output.draw: %d loops over passes.current (want 1)", found), -1;
    if (*exit <= *body || f->ops[*exit - 1].op != OJAlways || *hl_jump_target(&f->ops[*exit - 1], 0) != *load + 1)
        return wt_log("  Output.draw: the loop does not end with a jump back to its label"), -1;
    if (jumps_into(f, *body)) return wt_log("  Output.draw: a jump lands inside the loop's head"), -1;
    return 0;
}

static int calls(HlCode *c, const HlOp *o, const char *name) {
    if (o->op < OCall0 || o->op > OCallN) return 0;
    char buf[256];
    hl_function_name(c, o->a[1], 0, buf, sizeof buf);
    return !strcmp(buf, name) || (name[strlen(name) - 1] == '*' && !strncmp(buf, name, strlen(name) - 1));
}

static int offset_of(HlCode *c, const char *cls, const char *name) {
    int t = hl_find_type(c, HOBJ, cls), f = t < 0 ? -1 : hl_type_field(c, t, name);
    int off = f < 0 ? -1 : wt_field_offset(c, t, f);
    if (off < 0) wt_log("  no field %s.%s", cls, name);
    return off;
}

typedef struct {
    int begin, closure, object, end, frame, first, more, mset, mget, all, check, i2f;
} Natives;

// The workers' loop: Output.draw from the sort's merge point, the list from range_first, stopping on range_more.
static int add_range_loop(HlCode *c, HlEdit *e, int draw, const Natives *nt) {
    HlFun *f = hl_fun_decode(c, draw);
    int load, body, exit, merge = -1;
    if (!f || find_loop(c, f, &load, &body, &exit)) return hl_fun_free(f), -1;
    for (int i = 0; i < load; i++)
        if (f->ops[i].op == OJAlways && *hl_jump_target(&f->ops[i], 0) > i && *hl_jump_target(&f->ops[i], 0) <= load)
            merge = *hl_jump_target(&f->ops[i], 0);
    if (merge < 0) return wt_log("  Output.draw: no merge point after the sort"), hl_fun_free(f), -1;
    // No try in the copy (Output.onShaderError is never set): a redraw's exception is thrown after the join.
    for (int i = 0; i < f->nops; i++)
        if (f->ops[i].op == OTrap || f->ops[i].op == OEndTrap) f->ops[i] = wt_op(ONop, 0);
    int item = f->ops[load].a[0], rbool = f->nregs, rdyn = f->nregs + 1;
    if (f->nregs + 2 > 1024) return hl_fun_free(f), -1;
    f->regs[f->nregs++] = tbool, f->regs[f->nregs++] = tdyn;
    HlOp more[] = {wt_op(OCall0, 2, rbool, nt->more), wt_op(OJFalse, 2, rbool, exit)};
    hl_fun_insert(f, body, more, 2);
    f->ops[load] = wt_op(OCall0, 2, rdyn, nt->first);
    HlOp cast[] = {wt_op(OUnsafeCast, 2, item, rdyn)};
    hl_fun_insert(f, load + 1, cast, 1); // the loop's jump back still lands on its label
    HlOp skip[] = {wt_op(OJAlways, 1, merge)};
    hl_fun_insert(f, 0, skip, 1);
    int findex = hl_edit_add(e, f);
    hl_fun_free(f);
    return findex;
}

static int apply_fix(HlCode *c, HlEdit *e, const int *fn) {
    tvoid = hl_find_type(c, HVOID, NULL), tbool = hl_find_type(c, HBOOL, NULL), tdyn = hl_find_type(c, HDYN, NULL);
    int ti32 = hl_find_type(c, HI32, NULL), i2f = hl_find_function(c, "haxe.io.$FPHelper.i32ToFloat");
    tclosure = c->functions[fn[0]].type;
    int t_dd = wt_fun_type(c, 2, tdyn, tdyn, tvoid), t_d = wt_fun_type(c, 1, tdyn, -1, tvoid);
    int t_b = wt_fun_type(c, 0, -1, -1, tbool), t_v = wt_fun_type(c, 0, -1, -1, tvoid);
    int t_dret = wt_fun_type(c, 0, -1, -1, tdyn), t_i = wt_fun_type(c, 0, -1, -1, ti32);
    int t_iv = wt_fun_type(c, 1, ti32, -1, tvoid), t_if = i2f < 0 ? -1 : c->functions[i2f].type;
    if (t_dd < 0 || t_d < 0 || t_b < 0 || t_v < 0 || t_dret < 0 || t_i < 0 || t_iv < 0 || t_if < 0)
        return wt_log("  a native's function type is missing"), -1;
    if (wt_pass_layout(c)) return wt_log("  a pass list, pass object, pass or String field moved"), -1;
    wt_defer_off_obj = offset_of(c, "h3d.pass.PassObject", "obj");
    wt_defer_off_calls = offset_of(c, "h3d.Engine", "drawCalls");
    wt_defer_off_tris = offset_of(c, "h3d.Engine", "drawTriangles");
    wt_defer_off_switches = offset_of(c, "h3d.Engine", "shaderSwitches");
    wt_defer_off_dispatches = offset_of(c, "h3d.Engine", "dispatches");
    wt_defer_off_shaders = offset_of(c, "h3d.impl.DirectXDriver", "shaders");
    wt_defer_off_vertex = offset_of(c, "h3d.impl._DirectXDriver.CompiledShader", "vertex");
    wt_defer_off_fragment = offset_of(c, "h3d.impl._DirectXDriver.CompiledShader", "fragment");
    wt_defer_off_params_content = offset_of(c, "h3d.impl._DirectXDriver.ShaderContext", "paramsContent");
    int bad = wt_defer_off_obj < 0 || wt_defer_off_calls < 0 || wt_defer_off_tris < 0 || wt_defer_off_switches < 0 ||
              wt_defer_off_dispatches < 0 || wt_defer_off_shaders < 0 || wt_defer_off_vertex < 0 ||
              wt_defer_off_fragment < 0 || wt_defer_off_params_content < 0;
    for (int i = 0; i < wt_defer_nowned; i++) {
        WtOwned *o = &wt_defer_owned[i];
        o->offset = offset_of(c, wt_defer_classes[o->cls], o->field);
        bad |= o->offset < 0;
    }
    if (bad) return -1;
    Natives nt = {
        hl_edit_native(e, "wtfix", "defer_begin", t_dd), hl_edit_native(e, "wtfix", "defer_closure", t_d),
        hl_edit_native(e, "wtfix", "defer_object", t_b), hl_edit_native(e, "wtfix", "defer_end", t_v),
        hl_edit_native(e, "wtfix", "defer_frame", t_v), hl_edit_native(e, "wtfix", "range_first", t_dret),
        hl_edit_native(e, "wtfix", "range_more", t_b), hl_edit_native(e, "wtfix", "material_set", t_iv),
        hl_edit_native(e, "wtfix", "material_get", t_i), hl_edit_native(e, "wtfix", "params_upload_all", t_b),
        hl_edit_native(e, "wtfix", "worker_check", t_v), hl_edit_native(e, "wtfix", "i32_to_f32", t_if),
    };
    int range = add_range_loop(c, e, fn[0], &nt);
    if (range < 0) return -1;
    plans = (Plan *)calloc(MAX_PLANS, sizeof(Plan)), edits = (Edit *)calloc(MAX_EDITS, sizeof(Edit));
    nplans = nedits = 0;
    int failed = !plans || !edits;

    // Output.draw: the split, the main thread's turns, the join.
    Plan *draw = plan_for(c, fn[0]);
    int load, body, exit;
    if (!draw || find_loop(c, draw->f, &load, &body, &exit)) failed = 1;
    else {
        HlOp end[] = {wt_op(OCall0, 2, R_VOID, nt.end)};
        HlOp turn[] = {wt_op(OCall0, 2, R_BOOL, nt.object), wt_op(OJFalse, 2, R_BOOL, exit)};
        HlOp begin[] = {wt_op(OStaticClosure, 2, R_CLOSURE, range), wt_op(OMov, 2, R_DYN, R_CLOSURE),
                        wt_op(OCall1, 3, R_VOID, nt.closure, R_DYN), wt_op(OMov, 2, R_DYN, 1),
                        wt_op(OMov, 2, R_DYN2, 0), wt_op(OCall2, 4, R_VOID, nt.begin, R_DYN, R_DYN2)};
        failed |= add_edit(draw, exit, E_ENTRY, end, 1) || add_edit(draw, body, E_PLAIN, turn, 2) ||
                  add_edit(draw, load, E_ENTRY, begin, 6);
    }
    HlOp frame[] = {wt_op(OCall0, 2, R_VOID, nt.frame)};
    failed |= add_edit(plan_for(c, fn[1]), 0, E_ENTRY, frame, 1);

    // The driver's parameter upload: workers skip the compare with the shared copy ("if (prevContent != null) ...").
    int upload = hl_find_function(c, "h3d.impl.DirectXDriver.uploadShaderBuffer");
    Plan *up = upload < 0 ? NULL : plan_for(c, upload);
    int skip = -1;
    for (int i = 0; up && i < up->f->nops; i++)
        if (up->f->ops[i].op == OJNull && up->f->ops[i].a[0] == 4 && skip < 0) skip = i;
    if (skip < 0) failed = 1, wt_log("  uploadShaderBuffer: no test of its previous content");
    else {
        HlOp all[] = {wt_op(OCall0, 2, R_BOOL, nt.all), wt_op(OJTrue, 2, R_BOOL, *hl_jump_target(&up->f->ops[skip], 0))};
        failed |= add_edit(up, skip, E_PLAIN, all, 2);
    }

    // HMDModel.curMaterial per thread: every read and write (selectMaterial, render, the model libraries' -1).
    int hmd = hl_find_type(c, HOBJ, "h3d.prim.HMDModel"), cur = hmd < 0 ? -1 : hl_type_field(c, hmd, "curMaterial");
    if (cur < 0) failed = 1, wt_log("  no HMDModel.curMaterial");
    int touched = 0;
    for (int fi = 0; fi < c->nfunctions && cur >= 0; fi++) {
        char name[256];
        hl_function_name(c, c->functions[fi].findex, 0, name, sizeof name);
        if (strncmp(name, "h3d.prim.HMDModel.", 18) && !strstr(name, "ModelLibrary.__makeInternal")) continue;
        Plan *p = plan_for(c, fi);
        for (int i = 0; p && i < p->f->nops; i++) {
            HlOp *o = &p->f->ops[i], op;
            int this_hmd = p->f->regs[0] == hmd;
            if (o->op == OGetThis && this_hmd && o->a[1] == cur) op = wt_op(OCall0, 2, o->a[0], nt.mget);
            else if (o->op == OSetThis && this_hmd && o->a[0] == cur) op = wt_op(OCall1, 3, R_VOID, nt.mset, o->a[1]);
            else if (o->op == OSetField && p->f->regs[o->a[0]] == hmd && o->a[1] == cur)
                op = wt_op(OCall1, 3, R_VOID, nt.mset, o->a[2]);
            else if (o->op == OField && p->f->regs[o->a[1]] == hmd && o->a[2] == cur)
                op = wt_op(OCall0, 2, o->a[0], nt.mget);
            else continue;
            failed |= add_edit(p, i, E_REPLACE, &op, 1), touched++;
        }
    }
    if (touched < 3) failed = 1, wt_log("  HMDModel.curMaterial: %d uses found", touched);

    // FPHelper.i32ToFloat: a native instead of the static scratch buffer.
    Plan *fp = plan_for(c, i2f);
    int rf = -1;
    for (int r = 0; fp && r < fp->f->nregs && rf < 0; r++)
        if (c->types[fp->f->regs[r]].kind == HF32) rf = r;
    if (rf < 0 || fp->f->nops < 2) failed = 1, wt_log("  FPHelper.i32ToFloat: no f32 register");
    else fp->f->ops[0] = wt_op(OCall1, 3, rf, nt.i2f, 0), fp->f->ops[1] = wt_op(ORet, 1, rf), fp->f->nops = 2, fp->changed = 1;

    // Worker checks before anything that allocates, compiles or fills a cache the driver or the engine keeps.
    static const char *const entries[] = {"h3d.impl.DirectXDriver.compileShader", "h3d.impl.DirectXDriver.makeLayout",
                                          "h3d.mat.Texture.alloc", "h3d.impl.MemoryManager.allocBuffer",
                                          "h3d.impl.MemoryManager.allocTexture"};
    HlOp check[] = {wt_op(OCall0, 2, R_VOID, nt.check)};
    int guards = 0;
    for (size_t k = 0; k < sizeof entries / sizeof entries[0]; k++) {
        int fi = hl_find_function(c, entries[k]);
        if (fi < 0) failed = 1, wt_log("  no %s", entries[k]);
        else failed |= add_edit(plan_for(c, fi), 0, E_ENTRY, check, 1), guards++;
    }
    for (int fi = 0; fi < c->nfunctions; fi++) {
        char name[256];
        hl_function_name(c, c->functions[fi].findex, 0, name, sizeof name);
        if (strncmp(name, "h3d.impl.DirectXDriver.", 23) && strncmp(name, "h3d.Engine.", 11)) continue;
        int entry = 0;
        for (size_t k = 0; k < sizeof entries / sizeof entries[0]; k++) entry |= !strcmp(name, entries[k]);
        if (entry) continue;
        HlFun *f = hl_fun_decode(c, fi);
        int any = 0;
        for (int i = 0; f && i < f->nops; i++)
            any |= calls(c, &f->ops[i], "directx@create_*") || calls(c, &f->ops[i], "std@hiset") ||
                   calls(c, &f->ops[i], "haxe.ds.IntMap.set") || calls(c, &f->ops[i], "haxe.ds.ObjectMap.set");
        hl_fun_free(f);
        if (!any) continue;
        Plan *p = plan_for(c, fi);
        for (int i = 0; p && i < p->f->nops; i++)
            if (calls(c, &p->f->ops[i], "directx@create_*") || calls(c, &p->f->ops[i], "std@hiset") ||
                calls(c, &p->f->ops[i], "haxe.ds.IntMap.set") || calls(c, &p->f->ops[i], "haxe.ds.ObjectMap.set"))
                failed |= add_edit(p, i, E_ENTRY, check, 1), guards++;
    }

    qsort(edits, nedits, sizeof(Edit), by_plan_at_desc);
    int edited = 0;
    for (int i = 0, k = 0; i < nplans && !failed; i++) {
        int n = 0;
        while (k + n < nedits && edits[k + n].plan == i) n++;
        if (plans[i].changed)
            failed |= apply_plan(&plans[i], edits + k, n) || hl_edit_replace(e, plans[i].function, plans[i].f), edited++;
        k += n;
    }
    for (int i = 0; i < nplans; i++) hl_fun_free(plans[i].f);
    free(plans), free(edits), plans = NULL, edits = NULL;
    if (failed) return wt_log("  the threaded draw's edits did not all go in"), -1;
    wt_log("  %d functions edited, %d worker checks, %d curMaterial uses per thread", edited, guards, touched);
    return 0;
}

static const WtTarget targets[] = {
    {"h3d.pass.Output.draw", 0},
    {"hxd.App.mainLoop", 0},
};

const WtFix fix_deferdraw = {"deferdraw", "the main pass drawn by the main thread and worker threads ([perf] draw_threads)",
                             targets, 2, apply_fix};
