// DLSS natives for the patched TemporalFiltering; on any failure the game's TAA runs as before.
// wtdlss.dll is loaded on first use from wtfix.hdll's folder, NVIDIA's nvngx_dlss.dll next to it.
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../dlss/wtdlss.h"
#include "dlss.h"
#include "dlssup.h"
#include "fix.h"
#include "live.h"

#define HL_PRIM(name) \
    __declspec(dllexport) void *hlp_##name(const char **sign) { *sign = NULL; return (void *)name; }

typedef struct { // a boxed HL abstract: the pointer sits after the type
    void *type;
    void *ptr;
} HlBoxed;

static struct {
    WtDlssInit init;
    WtDlssOptimal optimal;
    WtDlssCreate create;
    WtDlssEvaluate evaluate;
    WtDlssRelease release;
} api;

static const void *slot[DLSS_SLOTS];
static double number[DLSS_NUMBERS];
static bool loaded, failed, ready;
static int failures;
static char status[160] = "off";
static ID3D11Device *device;
static ID3D11DeviceContext *context;
static ID3D11ComputeShader *mv_shader;
static ID3D11Buffer *mv_constants;
static ID3D11Texture2D *mv_tex, *out_tex;
static ID3D11UnorderedAccessView *mv_uav, *out_uav;
static int mv_w, mv_h, out_w, out_h, out_format;
static int feature_w, feature_h, feature_out_w, feature_out_h, feature_quality = -1, feature_mode;
static bool evaluated;
#define FEATURE_FLAGS (WTDLSS_HDR | WTDLSS_MV_LOW_RES | WTDLSS_AUTO_EXPOSURE)
static LARGE_INTEGER last_eval;

// frame_mode -1: the frame was not sized by dlss_frame; misses: owner frames since DLSS last ran.
static const void *owner;
static LARGE_INTEGER owner_seen;
static int engine_w, engine_h, render_w, render_h, frame_mode = -1, misses, bias_key;
static int sized_mode = -1, sized_w, sized_h, sized_rw, sized_rh, fallbacks, fallback_mode = -1;
static struct {
    bool pending;
    WtDlssUp up;
} upscaled;

static const int qualities[5] = {WTDLSS_DLAA, WTDLSS_QUALITY, WTDLSS_BALANCED, WTDLSS_PERF, WTDLSS_ULTRA_PERF};
static const char *const mode_names[5] = {"DLAA", "Quality", "Balanced", "Performance", "Ultra performance"};

// The engine's velocity where written, else camera motion from depth (unwritten pixels hold the clear: 0, or 1 on the
// world map). Units: UV, current to previous, +y down. Heaps matrices take row vectors.
static const char mv_source[] =
    "cbuffer C : register(b0) { row_major float4x4 inv; row_major float4x4 prev; float2 size; uint use_velocity; uint pad; };\n"
    "Texture2D<float> depth : register(t0);\n"
    "Texture2D<float2> velocity : register(t1);\n"
    "RWTexture2D<float2> mv : register(u0);\n"
    "[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {\n"
    "  if (id.x < (uint)size.x && id.y < (uint)size.y) {\n"
    "    float d = depth[id.xy];\n"
    "    float2 uv = (float2(id.xy) + 0.5) / size;\n"
    "    float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2);\n"
    "    float4 q = mul(mul(float4(ndc, d, 1), inv), prev);\n"
    "    float2 camera = q.w > 0 ? (q.xy / q.w - ndc) * float2(0.5, -0.5) : float2(0, 0);\n"
    "    float2 v = use_velocity != 0 ? velocity[id.xy] : float2(0, 0);\n"
    "    bool engine = d < 1.0 && any(v != 0) && all(abs(v) < 0.5);\n"
    "    mv[id.xy] = engine ? v : camera;\n"
    "  }\n"
    "}\n";

typedef struct {
    float inv[16], prev[16];
    float size[2];
    unsigned use_velocity, pad;
} MvConstants;

static void folder_of_wtfix(wchar_t *out, size_t n) {
    HMODULE self = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)(void *)folder_of_wtfix, &self);
    GetModuleFileNameW(self, out, (DWORD)n);
    wchar_t *slash = wcsrchr(out, L'\\');
    if (slash) *slash = 0;
}

static void ngx_line(const char *line) { wt_log("dlss: %s", line); }

static void stop(const char *why) {
    failed = true;
    snprintf(status, sizeof status, "off: %s", why);
    wt_log("dlss: OFF for this session: %s; the game's TAA runs", why);
}

static bool load(void) {
    if (loaded) return true;
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    folder_of_wtfix(dir, MAX_PATH);
    swprintf(path, MAX_PATH, L"%ls\\wtdlss.dll", dir);
    HMODULE lib = LoadLibraryW(path);
    if (!lib) {
        stop("wtdlss.dll is missing");
        return false;
    }
    api.init = (WtDlssInit)(void *)GetProcAddress(lib, "wtdlss_init");
    api.optimal = (WtDlssOptimal)(void *)GetProcAddress(lib, "wtdlss_optimal");
    api.create = (WtDlssCreate)(void *)GetProcAddress(lib, "wtdlss_create");
    api.evaluate = (WtDlssEvaluate)(void *)GetProcAddress(lib, "wtdlss_evaluate");
    api.release = (WtDlssRelease)(void *)GetProcAddress(lib, "wtdlss_release");
    if (!api.init || !api.optimal || !api.create || !api.evaluate || !api.release) {
        stop("wtdlss.dll is not this build's");
        return false;
    }
    loaded = true;
    return true;
}

static bool start_on(ID3D11Device *game_device) {
    device = game_device;
    ID3D11Device_AddRef(device);
    ID3D11Device_GetImmediateContext(device, &context);
    wchar_t dir[MAX_PATH], data[MAX_PATH];
    folder_of_wtfix(dir, MAX_PATH);
    swprintf(data, MAX_PATH, L"%ls\\logs", wt_root());
    int r = api.init(device, data, dir, ngx_line);
    if (r) {
        char why[64];
        snprintf(why, sizeof why, "NVIDIA DLSS did not start (0x%08x)", r);
        stop(why);
        return false;
    }
    ID3DBlob *code = NULL, *errors = NULL;
    HRESULT hr = D3DCompile(mv_source, sizeof mv_source - 1, "dlss_mv", NULL, NULL, "main", "cs_5_0",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
        wt_log("dlss: motion vector shader: %s", errors ? (const char *)ID3D10Blob_GetBufferPointer(errors) : "?");
        if (errors) ID3D10Blob_Release(errors);
        stop("motion vector shader did not compile");
        return false;
    }
    hr = ID3D11Device_CreateComputeShader(device, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code),
                                          NULL, &mv_shader);
    ID3D10Blob_Release(code);
    D3D11_BUFFER_DESC bd = {sizeof(MvConstants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER,
                            D3D11_CPU_ACCESS_WRITE, 0, 0};
    if (FAILED(hr) || FAILED(ID3D11Device_CreateBuffer(device, &bd, NULL, &mv_constants))) {
        stop("motion vector shader");
        return false;
    }
    char why[256];
    if (!wt_dlss_up_start(device, why, sizeof why)) {
        wt_log("dlss: upscaling's depth pass: %s", why);
        stop("upscaling's depth pass did not start");
        return false;
    }
    ready = true;
    wt_log("dlss: NVIDIA DLSS started");
    return true;
}

// Fallback when not started ahead: the device comes from the first TAA frame's texture.
static bool start(ID3D11Resource *any) {
    ID3D11Device *game_device;
    ID3D11Resource_GetDevice(any, &game_device);
    bool ok = start_on(game_device);
    ID3D11Device_Release(game_device);
    return ok;
}

static void texture_size(ID3D11Resource *res, int *w, int *h, int *format) {
    ID3D11Texture2D *tex = (ID3D11Texture2D *)res;
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc(tex, &d);
    *w = (int)d.Width, *h = (int)d.Height, *format = (int)d.Format;
}

static void release_texture(ID3D11Texture2D **tex, ID3D11UnorderedAccessView **uav) {
    if (*uav) ID3D11UnorderedAccessView_Release(*uav);
    if (*tex) ID3D11Texture2D_Release(*tex);
    *uav = NULL, *tex = NULL;
}

static bool own_texture(ID3D11Texture2D **tex, ID3D11UnorderedAccessView **uav, int w, int h, int format) {
    release_texture(tex, uav);
    D3D11_TEXTURE2D_DESC d = {0};
    d.Width = w, d.Height = h, d.MipLevels = 1, d.ArraySize = 1, d.Format = (DXGI_FORMAT)format;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    return SUCCEEDED(ID3D11Device_CreateTexture2D(device, &d, NULL, tex)) &&
           SUCCEEDED(ID3D11Device_CreateUnorderedAccessView(device, (ID3D11Resource *)*tex, NULL, uav));
}

static void read_matrix(const void *obj, float *out) {
    for (int i = 0; i < 16; i++) out[i] = (float)*(const double *)((const char *)obj + wt_dlss_layout.matrix[i]);
}

// A cut moves the screen's centre by more than a quarter of the screen between frames.
static bool camera_cut(const float *inv, const float *prev) {
    float p[4], q[4], v[4] = {0, 0, 0.5f, 1};
    for (int c = 0; c < 4; c++) p[c] = v[0] * inv[c] + v[1] * inv[4 + c] + v[2] * inv[8 + c] + v[3] * inv[12 + c];
    for (int c = 0; c < 4; c++) q[c] = p[0] * prev[c] + p[1] * prev[4 + c] + p[2] * prev[8 + c] + p[3] * prev[12 + c];
    return q[3] <= 0 || fabsf(q[0] / q[3]) > 0.5f || fabsf(q[1] / q[3]) > 0.5f;
}

static bool motion_vectors(ID3D11Resource *depth, ID3D11Resource *velocity, const MvConstants *k, int w, int h) {
    if ((w != mv_w || h != mv_h || !mv_tex) && !own_texture(&mv_tex, &mv_uav, w, h, DXGI_FORMAT_R16G16_FLOAT)) return false;
    mv_w = w, mv_h = h;
    ID3D11ShaderResourceView *views[2] = {NULL, NULL};
    if (FAILED(ID3D11Device_CreateShaderResourceView(device, depth, NULL, &views[0]))) return false;
    if (velocity && FAILED(ID3D11Device_CreateShaderResourceView(device, velocity, NULL, &views[1]))) views[1] = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(context, (ID3D11Resource *)mv_constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        MvConstants c = *k;
        c.use_velocity = views[1] != NULL;
        memcpy(m.pData, &c, sizeof c);
        ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)mv_constants, 0);
    }
    // Heaps never uses the compute stage, so its bindings are ours; they are cleared after.
    ID3D11DeviceContext_CSSetShader(context, mv_shader, NULL, 0);
    ID3D11DeviceContext_CSSetConstantBuffers(context, 0, 1, &mv_constants);
    ID3D11DeviceContext_CSSetShaderResources(context, 0, 2, views);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(context, 0, 1, &mv_uav, NULL);
    ID3D11DeviceContext_Dispatch(context, (w + 7) / 8, (h + 7) / 8, 1);
    ID3D11ShaderResourceView *no_views[2] = {NULL, NULL};
    ID3D11UnorderedAccessView *no_uav = NULL;
    ID3D11Buffer *no_buffer = NULL;
    ID3D11DeviceContext_CSSetShaderResources(context, 0, 2, no_views);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(context, 0, 1, &no_uav, NULL);
    ID3D11DeviceContext_CSSetConstantBuffers(context, 0, 1, &no_buffer);
    ID3D11DeviceContext_CSSetShader(context, NULL, NULL, 0);
    for (int i = 0; i < 2; i++)
        if (views[i]) ID3D11ShaderResourceView_Release(views[i]);
    return true;
}

static int mode(void) {
    int m = (int)(wt_live[LIVE_DLSS_MODE].value + 0.5);
    return m < 0 ? 0 : m > 4 ? 4 : m;
}

static bool dlss_on(void) {
    return wt_live[LIVE_DLSS].value >= 0.5 && !failed && load();
}

static void dlss_set(int i, const void *v) {
    if (i >= 0 && i < DLSS_SLOTS) slot[i] = v;
}

static void dlss_num(int i, double v) {
    if (i >= 0 && i < DLSS_NUMBERS) number[i] = v;
}

static void *resource(int i) {
    const HlBoxed *b = slot[i];
    return b ? b->ptr : NULL;
}

static double seconds_since(LARGE_INTEGER then) {
    LARGE_INTEGER now, hz;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&hz);
    return (double)(now.QuadPart - then.QuadPart) / hz.QuadPart;
}

static bool scaled_size(int m) {
    if (m != sized_mode || engine_w != sized_w || engine_h != sized_h) {
        int w = 0, h = 0, x;
        int r = api.optimal(qualities[m], engine_w, engine_h, &w, &h, &x, &x, &x, &x);
        sized_mode = m, sized_w = engine_w, sized_h = engine_h;
        sized_rw = !r && w > 0 && h > 0 && w <= engine_w && h <= engine_h ? w : 0, sized_rh = h;
        if (sized_rw) wt_log("dlss: %s renders the scene at %dx%d for %dx%d", mode_names[m], w, h, engine_w, engine_h);
        else wt_log("dlss: %s has no scene size for %dx%d (0x%08x); DLAA instead", mode_names[m], engine_w, engine_h, r);
    }
    render_w = sized_rw ? sized_rw : engine_w, render_h = sized_rw ? sized_rh : engine_h;
    return sized_rw > 0;
}

// NVIDIA's LOD bias log2(render / output width) - 1, in quarter steps as a 4-bit code in the sampler key's free bits
// 17-20; the patched uploadBuffers turns it back into a bias.
static int lod_key(int w, int ow) {
    if (wt_live[LIVE_DLSS_MIP_BIAS].value < 0.5) return 0;
    long code = 1 + lround(-(log2((double)w / ow) - 1.0) * 4);
    return (int)(code < 1 ? 1 : code > 15 ? 15 : code) << 17;
}

static void prepare(IDXGISwapChain *swapchain) {
    DXGI_SWAP_CHAIN_DESC sd;
    if (FAILED(IDXGISwapChain_GetDesc(swapchain, &sd)) || !sd.BufferDesc.Width || !sd.BufferDesc.Height) return;
    engine_w = (int)sd.BufferDesc.Width, engine_h = (int)sd.BufferDesc.Height;
    int m = mode();
    if (m > 0 && !scaled_size(m)) m = 0;
    int w = m > 0 ? render_w : engine_w, h = m > 0 ? render_h : engine_h;
    int quality = m > 0 ? qualities[m] : WTDLSS_DLAA, r = api.create(context, quality, w, h, engine_w, engine_h,
                                                                     FEATURE_FLAGS, 0);
    if (r) {
        wt_log("dlss: %s for %dx%d not made ahead (0x%08x); the first scene makes it", mode_names[m], engine_w, engine_h,
               r);
        return;
    }
    feature_w = w, feature_h = h, feature_out_w = engine_w, feature_out_h = engine_h, feature_quality = quality;
    feature_mode = m;
    if (own_texture(&out_tex, &out_uav, engine_w, engine_h, DXGI_FORMAT_R16G16B16A16_FLOAT))
        out_w = engine_w, out_h = engine_h, out_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (own_texture(&mv_tex, &mv_uav, w, h, DXGI_FORMAT_R16G16_FLOAT)) mv_w = w, mv_h = h;
    if (m > 0) wt_dlss_up_prepare(engine_w, engine_h);
    wt_log("dlss: %s made ahead: %dx%d -> %dx%d", mode_names[m], w, h, engine_w, engine_h);
}

// From the overlay's first frame or when DLSS is switched on, so NGX's slow start doesn't stall the first TAA scene.
void wt_dlss_startup(ID3D11Device *game_device, IDXGISwapChain *swapchain) {
    if (ready || failed || wt_live[LIVE_DLSS].value < 0.5 || !load()) return;
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    ID3D11DeviceContext *c;
    ID3D11Device_GetImmediateContext(game_device, &c);
    WtD3dState *saved = wt_d3d_save(c);
    if (start_on(game_device)) prepare(swapchain);
    wt_d3d_restore(c, saved);
    ID3D11DeviceContext_Release(c);
    if (ready) wt_log("dlss: started ahead in %.0f ms", seconds_since(t0) * 1000);
}

static bool evaluate(void) {
    ID3D11Resource *color = resource(DLSS_COLOR), *output = resource(DLSS_OUTPUT), *depth = resource(DLSS_DEPTH);
    ID3D11Resource *velocity = resource(DLSS_VELOCITY), *scene_ds = resource(DLSS_SCENE_DS);
    const void *renderer = slot[DLSS_RENDERER];
    if (!color || !output || !depth || !slot[DLSS_CUR_INV_VP] || !slot[DLSS_PREV_VP] || !renderer) {
        static bool said;
        if (!said)
            wt_log("dlss: the TAA's inputs are incomplete (colour %d, output %d, depth %d, matrices %d %d, renderer %d)",
                   !!color, !!output, !!depth, !!slot[DLSS_CUR_INV_VP], !!slot[DLSS_PREV_VP], !!renderer);
        said = true;
        return false;
    }
    if (!ready && !start(color)) return false;
    // One scene at a time: another one's TAA stays the game's until the owner has not rendered for half a second.
    if (renderer != owner) {
        if (owner && misses < 3 && seconds_since(owner_seen) < 0.5) return false;
        owner = renderer, misses = 0, frame_mode = -1;
    }
    int w, h, f, ow, oh, of;
    texture_size(color, &w, &h, &f);
    texture_size(output, &ow, &oh, &of);
    // A frame not sized for the mode (the first of a scene, the TAA's history being remade) keeps the game's TAA.
    bool upscale = frame_mode > 0;
    if (frame_mode < 0 || (upscale ? w != render_w || h != render_h || !scene_ds : w != ow || h != oh)) {
        static int said;
        if (frame_mode >= 0 && said++ < 5)
            wt_log("dlss: skipped a frame: the scene is %dx%d, sized for %dx%d (%s), output %dx%d", w, h, render_w,
                   render_h, mode_names[frame_mode], ow, oh);
        return false;
    }
    if (upscale) ow = engine_w, oh = engine_h, of = f;
    int quality = upscale ? qualities[frame_mode] : WTDLSS_DLAA;
    bool reset = false;
    if (w != feature_w || h != feature_h || ow != feature_out_w || oh != feature_out_h || quality != feature_quality) {
        int r = api.create(context, quality, w, h, ow, oh, FEATURE_FLAGS, 0);
        if (r) {
            char why[96];
            snprintf(why, sizeof why, "DLSS could not be set up for %dx%d -> %dx%d (0x%08x)", w, h, ow, oh, r);
            stop(why);
            return false;
        }
        feature_w = w, feature_h = h, feature_out_w = ow, feature_out_h = oh, feature_quality = quality;
        feature_mode = frame_mode;
        reset = true;
        snprintf(status, sizeof status, "on: %s, %dx%d -> %dx%d", mode_names[frame_mode], w, h, ow, oh);
        wt_log("dlss: %s", status);
    }
    if ((ow != out_w || oh != out_h || of != out_format || !out_tex) && !own_texture(&out_tex, &out_uav, ow, oh, of)) {
        stop("no output texture");
        return false;
    }
    out_w = ow, out_h = oh, out_format = of;
    MvConstants k = {0};
    read_matrix(slot[DLSS_CUR_INV_VP], k.inv);
    read_matrix(slot[DLSS_PREV_VP], k.prev);
    k.size[0] = (float)w, k.size[1] = (float)h;
    LARGE_INTEGER now, hz;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&hz);
    double ms = last_eval.QuadPart ? (now.QuadPart - last_eval.QuadPart) * 1000.0 / hz.QuadPart : 0;
    last_eval = now;
    if (ms <= 0 || ms > 250 || camera_cut(k.inv, k.prev)) reset = true;
    if (!motion_vectors(depth, velocity, &k, w, h)) {
        stop("motion vectors");
        return false;
    }
    WtDlssFrame frame = {0};
    frame.color = color, frame.depth = depth, frame.motion = mv_tex, frame.output = out_tex;
    frame.jitter_x = (float)(number[DLSS_JITTER_X] * w * 0.5);
    frame.jitter_y = (float)(-number[DLSS_JITTER_Y] * h * 0.5);
    frame.mv_scale_x = (float)w, frame.mv_scale_y = (float)h;
    frame.render_width = w, frame.render_height = h;
    frame.reset = reset;
    frame.frame_ms = (float)(ms > 0 && ms < 250 ? ms : 16.7);
    int r = api.evaluate(context, &frame);
    if (r) {
        wt_log("dlss: evaluate failed (0x%08x)", r);
        if (++failures >= 3) stop("DLSS failed three frames running");
        return false;
    }
    failures = 0, misses = 0, evaluated = true;
    if (upscale) fallbacks = 0;
    if (!upscale) {
        ID3D11DeviceContext_CopyResource(context, output, (ID3D11Resource *)out_tex);
        return true;
    }
    upscaled.pending = true;
    upscaled.up = (WtDlssUp){depth,          scene_ds,       NULL, NULL, w, h, ow, oh, frame.jitter_x, frame.jitter_y,
                             (ID3D11Resource *)mv_tex, frame.reset != 0};
    return true;
}

static bool dlss_taa(void) {
    upscaled.pending = false;
    bool done = dlss_on() && evaluate();
    memset(slot, 0, sizeof slot);
    return done;
}

// setAntialiasing's three effects: the game's choice (AA on) in, the one under DLSS out.
static bool dlss_taa_effect(bool game) { return game || dlss_on(); }
static bool dlss_sharpen_effect(bool game) { return dlss_on() ? wt_live[LIVE_DLSS_SHARPEN].value >= 0.5 : game; }
static bool dlss_bloom_effect(bool game) { return dlss_on() ? game || wt_live[LIVE_DLSS_BLOOM].value >= 0.5 : game; }
static bool dlss_changed(void) { return wt_live_take(LIVE_TAKE_DLSS); }

// Each scene render (packed: back buffer w << 16 | h): true for DLSS's scene, which then uses render_size and bias_key.
static bool dlss_frame(const void *renderer, int packed) {
    if (!renderer || renderer != owner) return false;
    QueryPerformanceCounter(&owner_seen);
    engine_w = packed >> 16 & 0x7fff, engine_h = packed & 0xffff;
    // Three frames without DLSS fall back to full size and DLAA; a mode failing so thrice stays DLAA until changed.
    if (mode() != fallback_mode) fallbacks = 0, fallback_mode = mode();
    if (misses == 3 && mode() > 0 && dlss_on() && ready && ++fallbacks == 3)
        wt_log("dlss: %s's scaled frames never reached DLSS; DLAA until the mode changes", mode_names[mode()]);
    int m = !dlss_on() || !ready ? -1 : misses >= 3 || fallbacks >= 3 ? 0 : mode();
    misses++;
    if (m > 0 && !scaled_size(m)) m = 0;
    if (m <= 0) render_w = engine_w, render_h = engine_h;
    frame_mode = m;
    bias_key = m >= 0 ? lod_key(render_w, engine_w) : 0;
    return true;
}

static int dlss_render_size(const void *renderer) { return renderer == owner ? render_w << 16 | render_h : 0; }
static int dlss_bias_key(const void *renderer) { return renderer == owner ? bias_key : 0; }
static bool dlss_upscaled(void) { return upscaled.pending; }

// False leaves the frame on the scene-size textures and turns DLSS off.
static bool dlss_upscale(void) {
    ID3D11Resource *color = resource(DLSS_UP_COLOR);
    WtDlssUp up = upscaled.up;
    up.out_depth = resource(DLSS_UP_DEPTH), up.out_depth_stencil = resource(DLSS_UP_DS);
    bool pending = upscaled.pending;
    upscaled.pending = false;
    memset(slot, 0, sizeof slot);
    if (!pending) return false;
    char why[256] = "the output textures are missing";
    int w = 0, h = 0, f = 0;
    if (color) texture_size(color, &w, &h, &f);
    if (color && (w != out_w || h != out_h || f != out_format))
        snprintf(why, sizeof why, "the output colour is %dx%d format %d, DLSS's %dx%d format %d", w, h, f, out_w, out_h,
                 out_format);
    else if (color && up.out_depth && up.out_depth_stencil && wt_dlss_up_run(context, &up, why, sizeof why)) {
        ID3D11DeviceContext_CopyResource(context, color, (ID3D11Resource *)out_tex);
        return true;
    }
    wt_log("dlss: upscaling: %s", why);
    stop("upscaling's output textures are not as expected");
    return false;
}

// Mask helper verdicts by shader class (the object's first word): shaders are added per frame too, so check once.
#define SEEN_SLOTS 1024
static struct {
    const void *type;
    int verdict;
} seen[SEEN_SLOTS];

static int slot_of(const void *type) {
    unsigned h = (unsigned)(((uintptr_t)type >> 4) * 2654435761u) % SEEN_SLOTS;
    for (int n = 0; n < SEEN_SLOTS; n++, h = (h + 1) % SEEN_SLOTS)
        if (!seen[h].type || seen[h].type == type) return (int)h;
    return -1;
}

static int dlss_seen(const void *shader) {
    int h = shader ? slot_of(*(const void *const *)shader) : -1;
    return h >= 0 ? seen[h].verdict : 0;
}

static bool dlss_learn(const void *shader, int verdict) {
    int h = shader ? slot_of(*(const void *const *)shader) : -1;
    if (h >= 0) seen[h].type = *(const void *const *)shader, seen[h].verdict = verdict;
    return h >= 0;
}

HL_PRIM(dlss_seen)
HL_PRIM(dlss_learn)
HL_PRIM(dlss_on)
HL_PRIM(dlss_set)
HL_PRIM(dlss_num)
HL_PRIM(dlss_taa)
HL_PRIM(dlss_taa_effect)
HL_PRIM(dlss_sharpen_effect)
HL_PRIM(dlss_bloom_effect)
HL_PRIM(dlss_changed)
HL_PRIM(dlss_frame)
HL_PRIM(dlss_render_size)
HL_PRIM(dlss_bias_key)
HL_PRIM(dlss_upscaled)
HL_PRIM(dlss_upscale)

int wt_dlss_stats(char *out, int size) {
    if (wt_live[LIVE_DLSS].value < 0.5 && !failed) return snprintf(out, size, "off");
    if (failed) return snprintf(out, size, "%s", status);
    if (ready && !evaluated) return snprintf(out, size, "started, no scene with the game's TAA yet");
    int code = bias_key >> 17 & 15, n = 0;
    if (ready)
        n = snprintf(out, size, "on: %s, %dx%d -> %dx%d", mode_names[feature_mode], feature_w, feature_h, feature_out_w,
                     feature_out_h);
    if (ready && code && n < size) n += snprintf(out + n, size - n, ", texture bias %.2f", (1 - code) * 0.25);
    if (ready) return n;
    return snprintf(out, size, "waiting for a scene with the game's TAA");
}
