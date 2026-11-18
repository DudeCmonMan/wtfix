// DLSS or FSR in place of the game's TAA: TAA, scene size, sampler LOD bias and AA toggle patches (natives in dlss.c).
#include "../dlss.h"
#include "../fix.h"
#include "../asm.h"
#include "../objlayout.h"
#include <stdio.h>
#include <string.h>

WtDlssLayout wt_dlss_layout;

static const WtTarget targets[] = {
    {"gfx.$GraphicsControl.setAntialiasing", 0xb9138afd56816e0aull},
    {"gfx.$GraphicsControl.initMode", 0xb60440af634b2fabull},
    {"hrt.prefab.rfx.TemporalFiltering.start", 0xff1ec843f801d756ull},
    {"hrt.prefab.rfx.TemporalFiltering.begin", 0xe565c57a03860ed7ull},
    {"hrt.prefab.rfx.TemporalFiltering.end", 0x8336eb4565abaeb3ull},
    {"h3d.scene.RenderContext.start", 0xd788a7065677b97cull},
    {"h3d.mat.Pass.addShader", 0x557ae7485d3828a9ull},
    {"h3d.mat.Pass.addSelfShader", 0x2a82142049bfd392ull},
    {"h3d.mat.Pass.addShaderAtIndex", 0x2f6eae319f1b9b0dull},
    {"h3d.mat.Pass.setColorMaski", 0x6757f64da594b46bull},
    {"hl.BaseType.check", 0},
    {"h3d.mat.Pass.setColorMask", 0x53d952fe60bc83ecull},
    {"h3d.scene.Renderer.process", 0x001ab46b70cbc391ull},
    {"h3d.impl.DirectXDriver.uploadBuffers", 0x220a0ffcfd85b1ccull},
    {"h3d.scene.pbr.Renderer.setRenderResolution", 0x87a25092889bc30eull},
    {"h3d.impl.TextureCache.allocTarget", 0},
    {"h3d.scene.pbr.Renderer.initGlobals", 0x3bdeb87be86506c0ull},
    {"h3d.mat.Texture.set_filter", 0},
    {"h3d.scene.Renderer.setTarget", 0},
};
enum { T_PROCESS = 12, T_UPLOAD, T_SET_RES, T_ALLOC, T_GLOBALS, T_FILTER, T_SET_TARGET, T_COUNT };

// These displace vertices but not their previous position, so their velocity is wrong: their passes stop writing
// target 5 and the native uses camera motion there. A 0 mask nibble means target 0's and setColorMaski can only add
// bits, so target 5 keeps alpha only (RG16F has none). Masking a static pass is harmless, so doubtful ones are in.
static const char *const displacing[] = {
    "hrt.shader.$Terrain",          "prefab.terrain.$BakedTerrain",
    "prefab.terrain.$TerrainBlend", "prefab.$TerrainPathShader",
    "prefab.$GrassWindShader",      "prefab.$GrassCharacterMovementShader",
    "shaders.$CampGrass",           "prefab.$TreeWindShader",
    "prefab.$PreciseWindShader",    "prefab.$WindPolygonShader",
    "hrt.shader.$MeshWave",         "shaders.$GravityRope",
    "prefab.$FurShader",            "prefab.$FurStep",
    "shaders.$MonsterDeformShader", "shaders.$VertexDisplacement",
    "prefab.$OceanShader",          "prefab.$OceanDepthShader",
    "shaders.$GerstnerWave",        "shaders.$OceanTrail",
    "prefab.$BattleSnowShader",     "prefab.$SplineMeshShader",
    "prefab.$LookAtVertexShader",   "hrt.prefab.l3d.$BillboardShader",
    "hrt.shader.$BaseEmitter",      "h3d.shader.$GpuParticle",
    "hrt.prefab.fx.gpuemitter.$ParticleShader", "prefab.$MeteoParticle",
    "hrt.shader.$BaseTrails",
};
#define NDISPLACING (int)(sizeof displacing / sizeof displacing[0])
#define VELOCITY_TARGET 5

static int class_global(HlCode *c, const char *name, int *type) {
    *type = hl_find_type(c, HOBJ, name);
    for (int g = 0; *type >= 0 && g < c->nglobals; g++)
        if (c->globals[g] == *type) return g;
    return -1;
}

// (pass, s) -> s, called first by addShader and siblings; dlss_seen/learn cache the verdict (1 displacing, 2 not).
static int mask_helper(HlCode *c, HlEdit *e, int type, int check, int mask, int f_seen, int f_learn) {
    int tpass = c->types[type].args[0], tshader = c->types[type].args[1];
    int tbool = hl_find_type(c, HBOOL, NULL), tdyn = hl_find_type(c, HDYN, NULL), ti32 = hl_find_type(c, HI32, NULL);
    int tvoid = hl_find_type(c, HVOID, NULL);
    enum { RPASS, RSHADER, RHIT, RDYN, RFALSE, RTARGET, RVOID, RTRUE, RVERDICT, RCONST, RCLASS };
    int regs[RCLASS + NDISPLACING] = {tpass, tshader, tbool, tdyn, tbool, ti32, tvoid, tbool, ti32, ti32};
    int globals[NDISPLACING];
    for (int k = 0; k < NDISPLACING; k++) {
        globals[k] = class_global(c, displacing[k], &regs[RCLASS + k]);
        if (globals[k] < 0) {
            wt_log("  no class %s", displacing[k]);
            return -1;
        }
    }
    int yes = hl_edit_int(e, 1), no = hl_edit_int(e, 2);
    enum { L_RET, L_MASK, L_YES };
    Asm a;
    asm_begin(&a, 24 + 3 * NDISPLACING);
    jump(&a, wt_op(OJNull, 2, RSHADER, 0), L_RET);
    op(&a, wt_op(OMov, 2, RDYN, RSHADER));
    op(&a, wt_op(OCall1, 3, RVERDICT, f_seen, RDYN));
    op(&a, wt_op(OInt, 2, RCONST, yes));
    jump(&a, wt_op(OJEq, 3, RVERDICT, RCONST, 0), L_MASK);
    op(&a, wt_op(OInt, 2, RCONST, no));
    jump(&a, wt_op(OJEq, 3, RVERDICT, RCONST, 0), L_RET);
    for (int k = 0; k < NDISPLACING; k++) {
        op(&a, wt_op(OGetGlobal, 2, RCLASS + k, globals[k]));
        op(&a, wt_op(OCall2, 4, RHIT, check, RCLASS + k, RDYN));
        jump(&a, wt_op(OJTrue, 2, RHIT, 0), L_YES);
    }
    op(&a, wt_op(OInt, 2, RCONST, no));
    op(&a, wt_op(OCall2, 4, RHIT, f_learn, RDYN, RCONST));
    jump(&a, wt_op(OJAlways, 1, 0), L_RET);
    mark(&a, L_YES);
    op(&a, wt_op(OInt, 2, RCONST, yes));
    op(&a, wt_op(OCall2, 4, RHIT, f_learn, RDYN, RCONST));
    mark(&a, L_MASK);
    op(&a, wt_op(OBool, 2, RFALSE, 0));
    op(&a, wt_op(OBool, 2, RTRUE, 1));
    op(&a, wt_op(OInt, 2, RTARGET, hl_edit_int(e, VELOCITY_TARGET)));
    op(&a, wt_op(OCallN, 9, RVOID, mask, 6, RPASS, RFALSE, RFALSE, RFALSE, RTRUE, RTARGET));
    mark(&a, L_RET);
    op(&a, wt_op(ORet, 1, RSHADER));
    resolve(&a);
    HlFun *f = hl_fun_new(type, RCLASS + NDISPLACING, regs, a.ops, a.n, 0, 0);
    free(a.ops);
    int findex = f ? hl_edit_add(e, f) : -1;
    hl_fun_free(f);
    return findex;
}

// setColorMask: "this.colorMask = r5" (op 19) keeps target 5's alpha bit from before.
static int keep_mask(HlCode *c, HlEdit *e, int function) {
    HlFun *f = hl_fun_decode(c, function);
    if (!f || f->nops != 21 || f->ops[19].op != OSetThis || f->ops[19].a[1] != 5) {
        wt_log("  Pass.setColorMask: op 19 is not \"this.colorMask = r5\"");
        hl_fun_free(f);
        return -1;
    }
    int ti32 = hl_find_type(c, HI32, NULL), rold = f->nregs, rbit = f->nregs + 1;
    f->regs[f->nregs++] = ti32;
    f->regs[f->nregs++] = ti32;
    HlOp keep[] = {
        wt_op(OGetThis, 2, rold, f->ops[19].a[0]),
        wt_op(OInt, 2, rbit, hl_edit_int(e, 8 << (4 * VELOCITY_TARGET))),
        wt_op(OAnd, 3, rold, rold, rbit),
        wt_op(OOr, 3, 5, 5, rold),
    };
    hl_fun_insert(f, 19, keep, 4);
    int r = hl_edit_replace(e, function, f);
    hl_fun_free(f);
    return r;
}

static int masked(HlCode *c, HlEdit *e, int function, int helper) {
    HlFun *f = hl_fun_decode(c, function);
    if (!f) return -1;
    HlOp call = wt_op(OCall2, 4, 1, helper, 0, 1);
    hl_fun_insert(f, 0, &call, 1);
    int r = hl_edit_replace(e, function, f);
    hl_fun_free(f);
    return r;
}

int wt_dlss_layout_build(HlCode *c) {
    static const char *const names[16] = {"_11", "_12", "_13", "_14", "_21", "_22", "_23", "_24",
                                          "_31", "_32", "_33", "_34", "_41", "_42", "_43", "_44"};
    int t = hl_find_type(c, HOBJ, "h3d.MatrixImpl"), tf64 = hl_find_type(c, HF64, NULL);
    for (int i = 0; i < 16; i++) {
        int fid = t >= 0 ? hl_type_field(c, t, names[i]) : -1;
        int at = fid >= 0 && hl_field_type(c, t, fid) == tf64 ? wt_field_offset(c, t, fid) : -1;
        if (at < 0) return -1;
        wt_dlss_layout.matrix[i] = at;
    }
    return 0;
}

static int add_reg(HlFun *f, int type) {
    f->regs[f->nregs] = type;
    return f->nregs++;
}

static int field_of(HlCode *c, int type, const char *name, int *ftype) {
    int fid = type >= 0 ? hl_type_field(c, type, name) : -1;
    *ftype = fid >= 0 ? hl_field_type(c, type, fid) : -1;
    if (fid < 0) wt_log("  %s has no field %s", type >= 0 ? hl_type_name(c, type) : "?", name);
    return fid;
}

typedef struct {
    int f_on, f_set, f_num, f_taa, f_changed, f_taa_effect, f_sharpen_effect, f_bloom_effect;
    int f_frame, f_render_size, f_bias_key, f_upscaled, f_upscale;
    int g_bias;
    int set_res, alloc, globals, filter, set_target;
    int t_alloc, t_set_target;
} Natives;

// Ops 0-10, 11-21, 22-32: "global <effect class>; ...; toggle(..., bool)"; each bool goes through a native first.
static int effects(HlCode *c, HlEdit *e, int function, const Natives *n) {
    HlFun *f = hl_fun_decode(c, function);
    static const int loads[3] = {0, 11, 22}, calls[3] = {10, 21, 32};
    static const char *const classes[3] = {"hrt.prefab.rfx.$TemporalBloom", "hrt.prefab.rfx.$TemporalFiltering",
                                           "hrt.prefab.rfx.$Sharpen"};
    const int natives[3] = {n->f_bloom_effect, n->f_taa_effect, n->f_sharpen_effect};
    for (int k = 0; f && k < 3; k++) {
        const HlOp *load = &f->ops[loads[k]], *call = &f->ops[calls[k]];
        if (load->op != OGetGlobal || strcmp(hl_type_name(c, f->regs[load->a[0]]), classes[k]) ||
            call->op < OCall2 || call->op > OCall3 ||
            f->regs[call->a[call->op == OCall2 ? 3 : 4]] != hl_find_type(c, HBOOL, NULL)) {
            wt_log("  %s: op %d or %d is not the %s toggle", hl_type_name(c, f->regs[0]), loads[k], calls[k], classes[k]);
            hl_fun_free(f);
            return -1;
        }
    }
    if (!f) return -1;
    for (int k = 2; k >= 0; k--) {
        const HlOp *call = &f->ops[calls[k]];
        int on = call->a[call->op == OCall2 ? 3 : 4];
        HlOp swap = wt_op(OCall1, 3, on, natives[k], on);
        // The option's "on" branch jumps to the toggle: the native must be on both paths.
        if (hl_fun_insert_entry(f, calls[k], &swap, 1)) {
            wt_log("  %s: a loop jumps back to the %s toggle", hl_type_name(c, f->regs[0]), classes[k]);
            hl_fun_free(f);
            return -1;
        }
    }
    int r = hl_edit_replace(e, function, f);
    hl_fun_free(f);
    return r;
}

static int start(HlCode *c, HlEdit *e, int function, const Natives *n) {
    HlFun *f = hl_fun_decode(c, function);
    if (!f || f->nops != 6 || f->ops[3].op != OGetThis || f->ops[4].op != OSetField || f->ops[4].a[2] != f->ops[3].a[0]) {
        wt_log("  TemporalFiltering.start: ops 3-4 are not \"ctx.computeVelocity = this.velocity\"");
        hl_fun_free(f);
        return -1;
    }
    int tbool = hl_find_type(c, HBOOL, NULL), ron = add_reg(f, tbool), rv = f->ops[3].a[0];
    HlOp on[] = {
        wt_op(OCall0, 2, ron, n->f_on),
        wt_op(OJFalse, 2, ron, 4),
        wt_op(OBool, 2, rv, 1),
    };
    hl_fun_insert(f, 4, on, 3);
    int r = hl_edit_replace(e, function, f);
    hl_fun_free(f);
    return r;
}

static int begin(HlCode *c, HlEdit *e, int function, const Natives *n) {
    HlFun *f = hl_fun_decode(c, function);
    int pattern = wt_string_global(c, "Halton_2_3_x256");
    if (!f || pattern < 0 || f->ops[10].op != OGetThis || f->ops[11].op != OSetField || f->ops[14].op != OGetThis ||
        f->ops[15].op != OSetField || f->regs[f->ops[14].a[0]] != hl_find_type(c, HF64, NULL)) {
        wt_log("  TemporalFiltering.begin: ops 10-15 are not the jitter pattern and scale, or no Halton_2_3_x256");
        hl_fun_free(f);
        return -1;
    }
    int ron = add_reg(f, hl_find_type(c, HBOOL, NULL)), rpattern = f->ops[10].a[0], rscale = f->ops[14].a[0];
    HlOp scale[] = {
        wt_op(OCall0, 2, ron, n->f_on),
        wt_op(OJFalse, 2, ron, 15),
        wt_op(OFloat, 2, rscale, hl_edit_float(e, 2.0)),
    };
    hl_fun_insert(f, 15, scale, 3);
    HlOp halton[] = {
        wt_op(OCall0, 2, ron, n->f_on),
        wt_op(OJFalse, 2, ron, 11),
        wt_op(OGetGlobal, 2, rpattern, pattern),
    };
    hl_fun_insert(f, 11, halton, 3);
    int r = hl_edit_replace(e, function, f);
    hl_fun_free(f);
    return r;
}

static int pass_texture(HlOp *ops, int n, int rtex, int slot_int, const int *r, int f_t, int f_res, int f_set, int fail) {
    ops[n++] = wt_op(OJNull, 2, rtex, fail);
    ops[n++] = wt_op(OField, 3, r[0], rtex, f_t);
    ops[n++] = wt_op(OJNull, 2, r[0], fail);
    ops[n++] = wt_op(OField, 3, r[1], r[0], f_res);
    ops[n++] = wt_op(OJNull, 2, r[1], fail);
    ops[n++] = wt_op(OToDyn, 2, r[2], r[1]);
    ops[n++] = wt_op(OInt, 2, r[3], slot_int);
    ops[n++] = wt_op(OCall2, 4, r[4], f_set, r[3], r[2]);
    return n;
}

typedef struct {
    int f_t, f_res, f_textures, f_depth, f_velocity, f_hdr, f_ldr, f_depthbuf, f_format, f_filter;
    int f_inv, f_prev, f_cam, f_cv, f_jx, f_jy, f_fov, f_near, f_far, f_engine, f_width, f_height, f_cache, f_tonemap,
        f_shader, f_hdrtex;
    int tvirt, tres, ttextures, tmat, tcam, tengine, tcache, tfx, tscreen, ttonemap, tfmt, tfilter, tref, tflags;
    int tstring, tstep;
} EndLayout;

static int end_layout(HlCode *c, const HlFun *f, const Natives *n, EndLayout *l) {
    int ttex = f->regs[8], tshader = f->regs[35], tpbr = f->regs[41], tctx = f->regs[3], tx, tf64, tbool, tmat2, ti32;
    l->tstep = f->regs[5];
    l->f_t = field_of(c, ttex, "t", &l->tvirt), l->f_res = l->f_t >= 0 ? field_of(c, l->tvirt, "res", &l->tres) : -1;
    l->f_depthbuf = field_of(c, ttex, "depthBuffer", &tx), l->f_format = field_of(c, ttex, "format", &l->tfmt);
    l->f_filter = field_of(c, ttex, "filter", &l->tfilter);
    l->f_textures = field_of(c, tpbr, "textures", &l->ttextures);
    l->f_depth = l->f_textures >= 0 ? field_of(c, l->ttextures, "depth", &tx) : -1;
    l->f_velocity = l->f_textures >= 0 ? field_of(c, l->ttextures, "velocity", &tx) : -1;
    l->f_hdr = l->f_textures >= 0 ? field_of(c, l->ttextures, "hdr", &tx) : -1;
    l->f_ldr = l->f_textures >= 0 ? field_of(c, l->ttextures, "ldr", &tx) : -1;
    l->f_tonemap = field_of(c, tpbr, "tonemap", &l->tfx);
    l->f_shader = l->f_tonemap >= 0 ? field_of(c, l->tfx, "shader", &l->tscreen) : -1;
    l->ttonemap = hl_find_type(c, HOBJ, "h3d.shader.pbr.ToneMapping");
    l->f_hdrtex = field_of(c, l->ttonemap, "hdrTexture__", &tx);
    l->f_inv = field_of(c, tshader, "cameraInverseViewProj__", &l->tmat);
    l->f_prev = field_of(c, tshader, "prevCamMat__", &tmat2);
    l->f_cam = field_of(c, tctx, "camera", &l->tcam), l->f_cv = field_of(c, tctx, "computeVelocity", &tbool);
    l->f_engine = field_of(c, tctx, "engine", &l->tengine), l->f_cache = field_of(c, tctx, "textures", &l->tcache);
    l->f_width = l->f_engine >= 0 ? field_of(c, l->tengine, "width", &ti32) : -1;
    l->f_height = l->f_engine >= 0 ? field_of(c, l->tengine, "height", &tx) : -1;
    l->f_jx = l->f_cam >= 0 ? field_of(c, l->tcam, "jitterOffsetX", &tf64) : -1;
    l->f_jy = l->f_cam >= 0 ? field_of(c, l->tcam, "jitterOffsetY", &tx) : -1;
    int tfov, tnear, tfar;
    l->f_fov = l->f_cam >= 0 ? field_of(c, l->tcam, "fovY", &tfov) : -1;
    l->f_near = l->f_cam >= 0 ? field_of(c, l->tcam, "zNear", &tnear) : -1;
    l->f_far = l->f_cam >= 0 ? field_of(c, l->tcam, "zFar", &tfar) : -1;
    const HlType *at = &c->types[n->t_alloc];
    l->tref = at->args[4], l->tflags = at->args[6], l->tstring = at->args[1];
    int fields[] = {l->f_t, l->f_res, l->f_depthbuf, l->f_format, l->f_filter, l->f_textures, l->f_depth, l->f_velocity,
                    l->f_hdr, l->f_ldr, l->f_tonemap, l->f_shader, l->f_hdrtex, l->f_inv, l->f_prev, l->f_cam, l->f_cv,
                    l->f_engine, l->f_cache, l->f_width, l->f_height, l->f_jx, l->f_jy, l->f_fov, l->f_near, l->f_far};
    for (unsigned i = 0; i < sizeof fields / sizeof fields[0]; i++)
        if (fields[i] < 0) return -1;
    return c->types[l->tres].kind == HABSTRACT && l->tmat == tmat2 && tf64 == hl_find_type(c, HF64, NULL) &&
           tfov == tf64 && tnear == tf64 && tfar == tf64 &&
                   tbool == hl_find_type(c, HBOOL, NULL) && ti32 == hl_find_type(c, HI32, NULL) && at->nargs == 7 &&
                   at->args[0] == l->tcache && at->args[2] == ti32 && at->args[3] == ti32 && at->args[5] == l->tfmt &&
                   at->ret == ttex
               ? 0
               : -1;
}

static int end(HlCode *c, HlEdit *e, int function, const Natives *n) {
    HlFun *f = hl_fun_decode(c, function);
    if (!f) return -1;
    // Ops 7-11: pre-tonemap TAA test; 190-194: setTarget(hdr r8), blend; 200: history copy; 201: prevCamMat.load;
    // 225-226: camera.update(); return.
    int ttex = f->regs[8], tshader = f->regs[35], tpbr = f->regs[41], tctx = f->regs[3];
    int ok = wt_expect(c, f, 191, OCall3, "h3d.scene.Renderer.setTarget") && f->ops[191].a[3] == 8 &&
             f->ops[191].a[4] == 43 && wt_expect(c, f, 194, OCall1, "h3d.pass.ScreenFx.render") &&
             wt_expect(c, f, 200, OCallN, "h3d.pass.$Copy.run") && wt_expect(c, f, 204, OCall2, "h3d.MatrixImpl.load") &&
             wt_expect(c, f, 225, OCall1, "h3d.Camera.update") && wt_expect(c, f, 226, ORet, NULL) &&
             wt_expect(c, f, 7, OGetGlobal, NULL) && f->ops[7].a[0] == 5 && wt_expect(c, f, 8, OJNotEq, NULL) &&
             f->ops[8].a[0] == 2 && f->ops[8].a[1] == 5 && wt_expect(c, f, 10, OGetGlobal, NULL) &&
             f->ops[10].a[1] == wt_string_global(c, "BeforeTonemapping") && f->ops[201].op == OField &&
             f->ops[201].a[1] == 35 && f->regs[24] == ttex && f->regs[1] == c->types[n->t_set_target].args[0] &&
             f->regs[43] == c->types[n->t_set_target].args[2] &&
             !strcmp(hl_type_name(c, ttex), "h3d.mat.Texture") &&
             !strcmp(hl_type_name(c, tshader), "hrt.prefab.rfx.TemporalFilteringShader") &&
             !strcmp(hl_type_name(c, tpbr), "h3d.scene.pbr.Renderer") &&
             !strcmp(hl_type_name(c, tctx), "h3d.scene.RenderContext");
    EndLayout l;
    if (!ok || end_layout(c, f, n, &l)) {
        wt_log("  TemporalFiltering.end: ops 7-11, 190-226 or the textures, shader and camera fields are not as expected");
        hl_fun_free(f);
        return -1;
    }
    int g_before = f->ops[7].a[1], g_hdr = wt_string_global(c, "hdrOutput"), g_ldr = wt_string_global(c, "ldrOutput");
    int g_depth = wt_string_global(c, "depth");
    if (g_hdr < 0 || g_ldr < 0 || g_depth < 0) {
        wt_log("  no hdrOutput, ldrOutput or depth string");
        hl_fun_free(f);
        return -1;
    }
    int tdyn = hl_find_type(c, HDYN, NULL), ti32 = hl_find_type(c, HI32, NULL), tvoid = hl_find_type(c, HVOID, NULL);
    int tbool = hl_find_type(c, HBOOL, NULL), tf64 = hl_find_type(c, HF64, NULL);
    int r[5] = {add_reg(f, l.tvirt), add_reg(f, l.tres), add_reg(f, tdyn), add_reg(f, ti32), add_reg(f, tvoid)};
    int ron = add_reg(f, tbool), rtextures = add_reg(f, l.ttextures), rtex = add_reg(f, ttex), rmat = add_reg(f, l.tmat);
    int rcam = add_reg(f, l.tcam), rnum = add_reg(f, tf64), rstep = add_reg(f, l.tstep), reng = add_reg(f, l.tengine);
    int rw = add_reg(f, ti32), rh = add_reg(f, ti32), rcache = add_reg(f, l.tcache), rtrue = add_reg(f, tbool);
    int rref = add_reg(f, l.tref), rflags = add_reg(f, l.tflags), rfmt = add_reg(f, l.tfmt);
    int rfilter = add_reg(f, l.tfilter), rname = add_reg(f, l.tstring), rhdr = add_reg(f, ttex), rldr = add_reg(f, ttex);
    int rdepth = add_reg(f, ttex), rfx = add_reg(f, l.tfx), rscreen = add_reg(f, l.tscreen);
    int rtonemap = add_reg(f, l.ttonemap);
    int slot[DLSS_SLOTS], num[DLSS_NUMBERS];
    for (int i = 0; i < DLSS_SLOTS; i++) slot[i] = hl_edit_int(e, i);
    for (int i = 0; i < DLSS_NUMBERS; i++) num[i] = hl_edit_int(e, i);

    // After an upscaled evaluate the rest of the frame runs at output size; inserted first so 226 is still the return.
    HlOp s[128];
    int k = 0;
    s[k++] = wt_op(OCall0, 2, ron, n->f_upscaled);
    s[k++] = wt_op(OJFalse, 2, ron, 226);
    s[k++] = wt_op(OField, 3, reng, 3, l.f_engine);
    s[k++] = wt_op(OJNull, 2, reng, 226);
    s[k++] = wt_op(OField, 3, rw, reng, l.f_width);
    s[k++] = wt_op(OField, 3, rh, reng, l.f_height);
    s[k++] = wt_op(OCall3, 5, r[4], n->set_res, 41, rw, rh);
    s[k++] = wt_op(OField, 3, rcache, 3, l.f_cache);
    s[k++] = wt_op(OJNull, 2, rcache, 226);
    s[k++] = wt_op(OField, 3, rtextures, 41, l.f_textures);
    s[k++] = wt_op(OJNull, 2, rtextures, 226);
    s[k++] = wt_op(OBool, 2, rtrue, 1);
    s[k++] = wt_op(ORef, 2, rref, rtrue);
    s[k++] = wt_op(ONull, 1, rflags);
    // depth=true: the new targets take the default depth buffer.
    const int olds[3] = {l.f_hdr, l.f_ldr, l.f_depth}, names[3] = {g_hdr, g_ldr, g_depth}, news[3] = {rhdr, rldr, rdepth};
    for (int t = 0; t < 3; t++) {
        s[k++] = wt_op(OField, 3, rtex, rtextures, olds[t]);
        s[k++] = wt_op(OJNull, 2, rtex, 226);
        s[k++] = wt_op(OField, 3, rfmt, rtex, l.f_format);
        if (t == 2) s[k++] = wt_op(OField, 3, rfilter, rtex, l.f_filter);
        s[k++] = wt_op(OGetGlobal, 2, rname, names[t]);
        s[k++] = wt_op(OCallN, 10, news[t], n->alloc, 7, rcache, rname, rw, rh, rref, rfmt, rflags);
    }
    s[k++] = wt_op(OCall2, 4, rfilter, n->filter, rdepth, rfilter);
    k = pass_texture(s, k, rhdr, slot[DLSS_UP_COLOR], r, l.f_t, l.f_res, n->f_set, 226);
    k = pass_texture(s, k, rdepth, slot[DLSS_UP_DEPTH], r, l.f_t, l.f_res, n->f_set, 226);
    s[k++] = wt_op(OField, 3, rtex, rhdr, l.f_depthbuf);
    k = pass_texture(s, k, rtex, slot[DLSS_UP_DS], r, l.f_t, l.f_res, n->f_set, 226);
    s[k++] = wt_op(OCall0, 2, ron, n->f_upscale);
    s[k++] = wt_op(OJFalse, 2, ron, 226);
    for (int t = 0; t < 3; t++) s[k++] = wt_op(OSetField, 3, rtextures, olds[t], news[t]);
    s[k++] = wt_op(OCall1, 3, r[4], n->globals, 41);
    s[k++] = wt_op(OCall3, 5, r[4], n->set_target, 1, rhdr, 43);
    s[k++] = wt_op(OField, 3, rfx, 41, l.f_tonemap);
    s[k++] = wt_op(OJNull, 2, rfx, 226);
    s[k++] = wt_op(OField, 3, rscreen, rfx, l.f_shader);
    s[k++] = wt_op(OJNull, 2, rscreen, 226);
    s[k++] = wt_op(OUnsafeCast, 2, rtonemap, rscreen);
    s[k++] = wt_op(OSetField, 3, rtonemap, l.f_hdrtex, rhdr);
    hl_fun_insert(f, 226, s, k);

    // Inserted at 192 before the velocity part: here 192 is the game's blend (fallback), 201 its bookkeeping (done).
    HlOp a[128];
    k = 0;
    a[k++] = wt_op(OCall0, 2, ron, n->f_on);
    a[k++] = wt_op(OJFalse, 2, ron, 192);
    a[k++] = wt_op(OGetGlobal, 2, rstep, g_before);
    a[k++] = wt_op(OJNotEq, 3, 2, rstep, 192);
    k = pass_texture(a, k, 24, slot[DLSS_COLOR], r, l.f_t, l.f_res, n->f_set, 192);
    k = pass_texture(a, k, 8, slot[DLSS_OUTPUT], r, l.f_t, l.f_res, n->f_set, 192);
    a[k++] = wt_op(OField, 3, rtex, 8, l.f_depthbuf);
    k = pass_texture(a, k, rtex, slot[DLSS_SCENE_DS], r, l.f_t, l.f_res, n->f_set, 192);
    a[k++] = wt_op(OJNull, 2, 41, 192);
    a[k++] = wt_op(OMov, 2, r[2], 41);
    a[k++] = wt_op(OInt, 2, r[3], slot[DLSS_RENDERER]);
    a[k++] = wt_op(OCall2, 4, r[4], n->f_set, r[3], r[2]);
    a[k++] = wt_op(OField, 3, rtextures, 41, l.f_textures);
    a[k++] = wt_op(OJNull, 2, rtextures, 192);
    a[k++] = wt_op(OField, 3, rtex, rtextures, l.f_depth);
    k = pass_texture(a, k, rtex, slot[DLSS_DEPTH], r, l.f_t, l.f_res, n->f_set, 192);
    a[k++] = wt_op(OJNull, 2, 35, 192);
    const int mats[2] = {l.f_inv, l.f_prev}, mat_slots[2] = {slot[DLSS_CUR_INV_VP], slot[DLSS_PREV_VP]};
    for (int m = 0; m < 2; m++) {
        a[k++] = wt_op(OField, 3, rmat, 35, mats[m]);
        a[k++] = wt_op(OJNull, 2, rmat, 192);
        a[k++] = wt_op(OMov, 2, r[2], rmat);
        a[k++] = wt_op(OInt, 2, r[3], mat_slots[m]);
        a[k++] = wt_op(OCall2, 4, r[4], n->f_set, r[3], r[2]);
    }
    a[k++] = wt_op(OField, 3, rcam, 3, l.f_cam);
    a[k++] = wt_op(OJNull, 2, rcam, 192);
    const int cam_fields[DLSS_NUMBERS] = {l.f_jx, l.f_jy, l.f_fov, l.f_near, l.f_far};
    for (int m = 0; m < DLSS_NUMBERS; m++) {
        a[k++] = wt_op(OField, 3, rnum, rcam, cam_fields[m]);
        a[k++] = wt_op(OInt, 2, r[3], num[m]);
        a[k++] = wt_op(OCall2, 4, r[4], n->f_num, r[3], rnum);
    }
    a[k++] = wt_op(OCall0, 2, ron, n->f_taa);
    a[k++] = wt_op(OJFalse, 2, ron, 192);
    // Later passes sample without the scene's LOD bias.
    a[k++] = wt_op(OInt, 2, r[3], hl_edit_int(e, 0));
    a[k++] = wt_op(OSetGlobal, 2, n->g_bias, r[3]);
    a[k++] = wt_op(OJAlways, 1, 201);
    hl_fun_insert(f, 192, a, k);

    // Velocity only while ctx.computeVelocity, else null; here 192 is the main part.
    HlOp v[24];
    k = 0;
    v[k++] = wt_op(OCall0, 2, ron, n->f_on);
    v[k++] = wt_op(OJFalse, 2, ron, 192);
    v[k++] = wt_op(ONull, 1, r[2]);
    v[k++] = wt_op(OInt, 2, r[3], slot[DLSS_VELOCITY]);
    v[k++] = wt_op(OCall2, 4, r[4], n->f_set, r[3], r[2]);
    v[k++] = wt_op(OField, 3, ron, 3, l.f_cv);
    v[k++] = wt_op(OJFalse, 2, ron, 192);
    v[k++] = wt_op(OJNull, 2, 41, 192);
    v[k++] = wt_op(OField, 3, rtextures, 41, l.f_textures);
    v[k++] = wt_op(OJNull, 2, rtextures, 192);
    v[k++] = wt_op(OField, 3, rtex, rtextures, l.f_velocity);
    k = pass_texture(v, k, rtex, slot[DLSS_VELOCITY], r, l.f_t, l.f_res, n->f_set, 192);
    hl_fun_insert(f, 192, v, k);
    int result = hl_edit_replace(e, function, f);
    hl_fun_free(f);
    return result;
}

// Ops 46-47: skip render() when computing statics; op 51: resetTarget. Size and bias set after 47, bias cleared at 51.
static int process(HlCode *c, HlEdit *e, int function, const Natives *n) {
    HlFun *f = hl_fun_decode(c, function);
    int tctx, tengine, tx, ti32 = hl_find_type(c, HI32, NULL), tpbr = hl_find_type(c, HOBJ, "h3d.scene.pbr.Renderer");
    int ok = f && wt_expect(c, f, 46, OField, NULL) && f->ops[46].a[0] == 2 && wt_expect(c, f, 47, OJFalse, NULL) &&
             f->ops[47].a[0] == 2 && wt_expect(c, f, 51, OCall1, "h3d.scene.Renderer.resetTarget") && tpbr >= 0;
    int f_ctx = ok ? field_of(c, f->regs[0], "ctx", &tctx) : -1;
    int f_engine = f_ctx >= 0 ? field_of(c, tctx, "engine", &tengine) : -1;
    int f_static = f_ctx >= 0 ? field_of(c, tctx, "computingStatic", &tx) : -1;
    int f_width = f_engine >= 0 ? field_of(c, tengine, "width", &tx) : -1;
    int f_height = f_engine >= 0 ? field_of(c, tengine, "height", &tx) : -1;
    if (!ok || f_height < 0 || f_width < 0 || f->ops[46].a[2] != f_static || tx != ti32) {
        wt_log("  Renderer.process: ops 46-51 are not \"if (ctx.computingStatic) ... else render(); resetTarget()\"");
        hl_fun_free(f);
        return -1;
    }
    int tbool = hl_find_type(c, HBOOL, NULL), tdyn = hl_find_type(c, HDYN, NULL);
    int tvoid = hl_find_type(c, HVOID, NULL);
    int rctx = add_reg(f, tctx), reng = add_reg(f, tengine), rw = add_reg(f, ti32), rh = add_reg(f, ti32);
    int rsixteen = add_reg(f, ti32), rpacked = add_reg(f, ti32), rdyn = add_reg(f, tdyn), rok = add_reg(f, tbool);
    int rmask = add_reg(f, ti32), rpbr = add_reg(f, tpbr), rvoid = add_reg(f, tvoid);
    HlOp clear[] = {
        wt_op(OInt, 2, rmask, hl_edit_int(e, 0)),
        wt_op(OSetGlobal, 2, n->g_bias, rmask),
    };
    hl_fun_insert(f, 51, clear, 2);
    HlOp size[] = {
        wt_op(OJTrue, 2, 2, 47),
        wt_op(OField, 3, rctx, 0, f_ctx),
        wt_op(OJNull, 2, rctx, 47),
        wt_op(OField, 3, reng, rctx, f_engine),
        wt_op(OJNull, 2, reng, 47),
        wt_op(OField, 3, rw, reng, f_width),
        wt_op(OField, 3, rh, reng, f_height),
        wt_op(OInt, 2, rsixteen, hl_edit_int(e, 16)),
        wt_op(OShl, 3, rpacked, rw, rsixteen),
        wt_op(OOr, 3, rpacked, rpacked, rh),
        wt_op(OMov, 2, rdyn, 0),
        wt_op(OCall2, 4, rok, n->f_frame, rdyn, rpacked),
        wt_op(OJFalse, 2, rok, 47),
        wt_op(OCall1, 3, rpacked, n->f_render_size, rdyn),
        wt_op(OUShr, 3, rw, rpacked, rsixteen),
        wt_op(OInt, 2, rmask, hl_edit_int(e, 0xffff)),
        wt_op(OAnd, 3, rh, rpacked, rmask),
        wt_op(OUnsafeCast, 2, rpbr, 0),
        wt_op(OCall3, 5, rvoid, n->set_res, rpbr, rw, rh),
        wt_op(OCall1, 3, rpacked, n->f_bias_key, rdyn),
        wt_op(OSetGlobal, 2, n->g_bias, rpacked),
    };
    hl_fun_insert(f, 47, size, sizeof size / sizeof size[0]);
    int r = hl_edit_replace(e, function, f);
    hl_fun_free(f);
    return r;
}

// Ops 247-249: sampler key = texture bits without 17-20; ORing the bias bits in caches biased samplers apart.
// Op 317: a new sampler's mipLodBias, which gets (1 - code) / 4 added.
static int lod_bias(HlCode *c, HlEdit *e, int function, const Natives *n) {
    HlFun *f = hl_fun_decode(c, function);
    int ok = f && wt_expect(c, f, 247, OField, NULL) && f->ops[247].a[0] == 25 && wt_expect(c, f, 248, OInt, NULL) &&
             f->ops[248].a[1] == hl_find_int(c, (int)0xFFE1FFFF) && wt_expect(c, f, 249, OAnd, NULL) &&
             f->ops[249].a[0] == 25 && f->ops[249].a[1] == 25 && f->ops[249].a[2] == f->ops[248].a[0] &&
             wt_expect(c, f, 317, OCall1, "h3d.mat.Texture.get_lodBias") && f->ops[317].a[0] == 41 &&
             wt_expect(c, f, 318, OToSFloat, NULL) && f->ops[318].a[1] == 41 && f->regs[41] == hl_find_type(c, HF64, NULL);
    if (!ok) {
        wt_log("  DirectXDriver.uploadBuffers: ops 247-249 or 317-318 are not the sampler key and LOD bias");
        hl_fun_free(f);
        return -1;
    }
    int ti32 = hl_find_type(c, HI32, NULL), tf64 = hl_find_type(c, HF64, NULL);
    int rcode = add_reg(f, ti32), rconst = add_reg(f, ti32), rbias = add_reg(f, tf64), rquarter = add_reg(f, tf64);
    HlOp bias[] = {
        wt_op(OInt, 2, rconst, hl_edit_int(e, 17)),
        wt_op(OUShr, 3, rcode, 25, rconst),
        wt_op(OInt, 2, rconst, hl_edit_int(e, 15)),
        wt_op(OAnd, 3, rcode, rcode, rconst),
        wt_op(OInt, 2, rconst, hl_edit_int(e, 0)),
        wt_op(OJEq, 3, rcode, rconst, 318),
        wt_op(OInt, 2, rconst, hl_edit_int(e, 1)),
        wt_op(OSub, 3, rcode, rconst, rcode),
        wt_op(OToSFloat, 2, rbias, rcode),
        wt_op(OFloat, 2, rquarter, hl_edit_float(e, 0.25)),
        wt_op(OMul, 3, rbias, rbias, rquarter),
        wt_op(OAdd, 3, 41, 41, rbias),
    };
    hl_fun_insert(f, 318, bias, sizeof bias / sizeof bias[0]);
    HlOp key[] = {
        wt_op(OGetGlobal, 2, rcode, n->g_bias),
        wt_op(OOr, 3, 25, 25, rcode),
    };
    hl_fun_insert(f, 250, key, 2);
    int r = hl_edit_replace(e, function, f);
    hl_fun_free(f);
    return r;
}

// When a DLSS switch changed, setAntialiasing again, in a trap.
static int live(HlCode *c, HlEdit *e, int function, int setaa, const Natives *n) {
    HlFun *f = hl_fun_decode(c, function);
    if (!f) return -1;
    int rchanged = add_reg(f, hl_find_type(c, HBOOL, NULL)), rexc = add_reg(f, hl_find_type(c, HDYN, NULL));
    int rvoid = add_reg(f, hl_find_type(c, HVOID, NULL));
    HlOp pop = wt_op(OEndTrap, 1, 1);
    hl_fun_insert(f, 0, &pop, 1);
    HlOp apply[] = {
        wt_op(OCall0, 2, rchanged, n->f_changed),
        wt_op(OJFalse, 2, rchanged, 1),
        wt_op(OTrap, 2, rexc, 1),
        wt_op(OCall0, 2, rvoid, setaa),
    };
    hl_fun_insert(f, 0, apply, 4);
    int r = hl_edit_replace(e, function, f);
    hl_fun_free(f);
    return r;
}

static int apply(HlCode *c, HlEdit *e, const int *fn) {
    int tbool = hl_find_type(c, HBOOL, NULL), ti32 = hl_find_type(c, HI32, NULL), tf64 = hl_find_type(c, HF64, NULL);
    int tdyn = hl_find_type(c, HDYN, NULL), tvoid = hl_find_type(c, HVOID, NULL);
    int t_flag = wt_fun_type(c, 0, -1, -1, tbool), t_set = wt_fun_type(c, 2, ti32, tdyn, tvoid);
    int t_num = wt_fun_type(c, 2, ti32, tf64, tvoid), t_effect = wt_fun_type(c, 1, tbool, -1, tbool);
    int t_seen = wt_fun_type(c, 1, tdyn, -1, ti32), t_learn = wt_fun_type(c, 2, tdyn, ti32, tbool);
    if (t_flag < 0 || t_set < 0 || t_num < 0 || t_effect < 0 || t_seen < 0 || t_learn < 0 || wt_dlss_layout_build(c)) {
        wt_log("  no function types for the natives, or h3d.MatrixImpl is not laid out as expected");
        return -1;
    }
    const HlType *set_res = &c->types[c->functions[fn[T_SET_RES]].type];
    const HlType *globals = &c->types[c->functions[fn[T_GLOBALS]].type];
    const HlType *filter = &c->types[c->functions[fn[T_FILTER]].type];
    const HlType *target = &c->types[c->functions[fn[T_SET_TARGET]].type];
    int tpbr = hl_find_type(c, HOBJ, "h3d.scene.pbr.Renderer"), ttex = hl_find_type(c, HOBJ, "h3d.mat.Texture");
    if (set_res->nargs != 3 || set_res->args[0] != tpbr || set_res->args[1] != ti32 || set_res->args[2] != ti32 ||
        globals->nargs != 1 || globals->args[0] != tpbr || filter->nargs != 2 || filter->args[0] != ttex ||
        filter->ret != filter->args[1] || target->nargs != 3 || target->args[1] != ttex) {
        wt_log("  setRenderResolution, initGlobals, Texture.set_filter or Renderer.setTarget have other types");
        return -1;
    }
    Natives n = {
        hl_edit_native(e, "wtfix", "dlss_on", t_flag),
        hl_edit_native(e, "wtfix", "dlss_set", t_set),
        hl_edit_native(e, "wtfix", "dlss_num", t_num),
        hl_edit_native(e, "wtfix", "dlss_taa", t_flag),
        hl_edit_native(e, "wtfix", "dlss_changed", t_flag),
        hl_edit_native(e, "wtfix", "dlss_taa_effect", t_effect),
        hl_edit_native(e, "wtfix", "dlss_sharpen_effect", t_effect),
        hl_edit_native(e, "wtfix", "dlss_bloom_effect", t_effect),
        hl_edit_native(e, "wtfix", "dlss_frame", t_learn),
        hl_edit_native(e, "wtfix", "dlss_render_size", t_seen),
        hl_edit_native(e, "wtfix", "dlss_bias_key", t_seen),
        hl_edit_native(e, "wtfix", "dlss_upscaled", t_flag),
        hl_edit_native(e, "wtfix", "dlss_upscale", t_flag),
        hl_edit_global(e, ti32),
        wt_findex(c, fn[T_SET_RES]),
        wt_findex(c, fn[T_ALLOC]),
        wt_findex(c, fn[T_GLOBALS]),
        wt_findex(c, fn[T_FILTER]),
        wt_findex(c, fn[T_SET_TARGET]),
        c->functions[fn[T_ALLOC]].type,
        c->functions[fn[T_SET_TARGET]].type,
    };
    if (effects(c, e, fn[0], &n) || effects(c, e, fn[1], &n) || start(c, e, fn[2], &n) || begin(c, e, fn[3], &n) ||
        end(c, e, fn[4], &n) || live(c, e, fn[5], wt_findex(c, fn[0]), &n) || process(c, e, fn[T_PROCESS], &n) ||
        lod_bias(c, e, fn[T_UPLOAD], &n))
        return -1;
    int add_type = c->functions[fn[6]].type;
    if (c->functions[fn[7]].type != add_type || c->types[c->functions[fn[8]].type].nargs != 3 ||
        c->types[c->functions[fn[8]].type].args[1] != c->types[add_type].args[1]) {
        wt_log("  Pass.addShader, addSelfShader and addShaderAtIndex do not take (pass, shader)");
        return -1;
    }
    int helper = mask_helper(c, e, add_type, wt_findex(c, fn[10]), wt_findex(c, fn[9]),
                             hl_edit_native(e, "wtfix", "dlss_seen", t_seen), hl_edit_native(e, "wtfix", "dlss_learn", t_learn));
    if (helper < 0 || masked(c, e, fn[6], helper) || masked(c, e, fn[7], helper) || masked(c, e, fn[8], helper) ||
        keep_mask(c, e, fn[11]))
        return -1;
    return 0;
}

const WtFix fix_dlss = {"dlss",
                        "NVIDIA DLSS or AMD FSR 3.1 (anti-aliasing or upscaling) in place of the game's TAA, "
                        "with motion vectors (live)",
                        targets, T_COUNT, apply};
