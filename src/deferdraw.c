// The main pass split with worker threads recording D3D11 deferred contexts, played back in order: the same image.
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "deferdraw.h"
#include "fix.h"
#include "live.h"
#include "passes.h"

#define HL_PRIM(name) \
    __declspec(dllexport) void *hlp_##name(const char **sign) { *sign = NULL; return (void *)name; }

#define MAX_WORKERS 7
#define VBS 16
#define CBS D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT
#define SRVS 32
#define SAMPLERS D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT
#define RTS D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT
#define VIEWPORTS 16

// Everything Heaps binds (and the topology, set once at reset): what a list must start with and hand back.
typedef struct {
    ID3D11InputLayout *layout;
    ID3D11Buffer *vb[VBS], *ib, *vs_cb[CBS], *ps_cb[CBS];
    UINT strides[VBS], offsets[VBS], ib_offset, sample_mask, stencil_ref, nviewports, nscissors;
    DXGI_FORMAT ib_format;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11ShaderResourceView *vs_srv[SRVS], *ps_srv[SRVS];
    ID3D11SamplerState *vs_samp[SAMPLERS], *ps_samp[SAMPLERS];
    ID3D11RasterizerState *raster;
    ID3D11BlendState *blend;
    ID3D11DepthStencilState *depth;
    FLOAT blend_factor[4];
    D3D11_VIEWPORT viewports[VIEWPORTS];
    D3D11_RECT scissors[VIEWPORTS];
    ID3D11RenderTargetView *rtv[RTS];
    ID3D11DepthStencilView *dsv;
} State;

static void capture(ID3D11DeviceContext *x, State *s) {
    memset(s, 0, sizeof *s);
    ID3D11DeviceContext_IAGetInputLayout(x, &s->layout);
    ID3D11DeviceContext_IAGetVertexBuffers(x, 0, VBS, s->vb, s->strides, s->offsets);
    ID3D11DeviceContext_IAGetIndexBuffer(x, &s->ib, &s->ib_format, &s->ib_offset);
    ID3D11DeviceContext_IAGetPrimitiveTopology(x, &s->topology);
    ID3D11DeviceContext_VSGetShader(x, &s->vs, NULL, NULL);
    ID3D11DeviceContext_PSGetShader(x, &s->ps, NULL, NULL);
    ID3D11DeviceContext_VSGetConstantBuffers(x, 0, CBS, s->vs_cb);
    ID3D11DeviceContext_PSGetConstantBuffers(x, 0, CBS, s->ps_cb);
    ID3D11DeviceContext_VSGetShaderResources(x, 0, SRVS, s->vs_srv);
    ID3D11DeviceContext_PSGetShaderResources(x, 0, SRVS, s->ps_srv);
    ID3D11DeviceContext_VSGetSamplers(x, 0, SAMPLERS, s->vs_samp);
    ID3D11DeviceContext_PSGetSamplers(x, 0, SAMPLERS, s->ps_samp);
    ID3D11DeviceContext_RSGetState(x, &s->raster);
    s->nviewports = VIEWPORTS, s->nscissors = VIEWPORTS;
    ID3D11DeviceContext_RSGetViewports(x, &s->nviewports, s->viewports);
    ID3D11DeviceContext_RSGetScissorRects(x, &s->nscissors, s->scissors);
    ID3D11DeviceContext_OMGetBlendState(x, &s->blend, s->blend_factor, &s->sample_mask);
    ID3D11DeviceContext_OMGetDepthStencilState(x, &s->depth, &s->stencil_ref);
    ID3D11DeviceContext_OMGetRenderTargets(x, RTS, s->rtv, &s->dsv);
}

static void apply(ID3D11DeviceContext *x, const State *s) {
    ID3D11DeviceContext_IASetInputLayout(x, s->layout);
    ID3D11DeviceContext_IASetVertexBuffers(x, 0, VBS, s->vb, s->strides, s->offsets);
    ID3D11DeviceContext_IASetIndexBuffer(x, s->ib, s->ib_format, s->ib_offset);
    ID3D11DeviceContext_IASetPrimitiveTopology(x, s->topology);
    ID3D11DeviceContext_VSSetShader(x, s->vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(x, s->ps, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(x, 0, CBS, s->vs_cb);
    ID3D11DeviceContext_PSSetConstantBuffers(x, 0, CBS, s->ps_cb);
    ID3D11DeviceContext_VSSetShaderResources(x, 0, SRVS, s->vs_srv);
    ID3D11DeviceContext_PSSetShaderResources(x, 0, SRVS, s->ps_srv);
    ID3D11DeviceContext_VSSetSamplers(x, 0, SAMPLERS, s->vs_samp);
    ID3D11DeviceContext_PSSetSamplers(x, 0, SAMPLERS, s->ps_samp);
    ID3D11DeviceContext_RSSetState(x, s->raster);
    ID3D11DeviceContext_RSSetViewports(x, s->nviewports, s->viewports);
    ID3D11DeviceContext_RSSetScissorRects(x, s->nscissors, s->scissors);
    ID3D11DeviceContext_OMSetBlendState(x, s->blend, s->blend_factor, s->sample_mask);
    ID3D11DeviceContext_OMSetDepthStencilState(x, s->depth, s->stencil_ref);
    ID3D11DeviceContext_OMSetRenderTargets(x, RTS, s->rtv, s->dsv);
}

static void RELEASE(void *p) {
    if (p) IUnknown_Release((IUnknown *)p);
}

static void release(State *s) {
    RELEASE(s->layout), RELEASE(s->ib), RELEASE(s->vs), RELEASE(s->ps), RELEASE(s->raster), RELEASE(s->blend);
    RELEASE(s->depth), RELEASE(s->dsv);
    for (int i = 0; i < VBS; i++) RELEASE(s->vb[i]);
    for (int i = 0; i < CBS; i++) RELEASE(s->vs_cb[i]), RELEASE(s->ps_cb[i]);
    for (int i = 0; i < SRVS; i++) RELEASE(s->vs_srv[i]), RELEASE(s->ps_srv[i]);
    for (int i = 0; i < SAMPLERS; i++) RELEASE(s->vs_samp[i]), RELEASE(s->ps_samp[i]);
    for (int i = 0; i < RTS; i++) RELEASE(s->rtv[i]);
}

static char status[160] = "waiting for the first frame";

// libhl, for worker threads and the copies.
static struct {
    void *(*alloc_obj)(void *type);
    void *(*alloc_array)(void *type, int size);
    void *(*alloc_bytes)(int size);
    int (*memsize)(void *p);
    void *(*hialloc)(void);
    void (*hiset)(void *map, int key, void *value);
    void *(*higet)(void *map, int key);
    void *(*hikeys)(void *map);
    void (*add_root)(void *p);
    void (*register_thread)(void *stack_top);
    void (*blocking)(int on);
    void *(*call_safe)(void *closure, void **args, int nargs, unsigned char *exception);
    void (*throw_)(void *value);
} hl;

static int hl_ready(void) {
    static const char *const names[] = {"hl_alloc_obj", "hl_alloc_array", "hl_alloc_bytes", "hl_gc_get_memsize",
                                        "hl_hialloc", "hl_hiset", "hl_higet", "hl_hikeys", "hl_add_root",
                                        "hl_register_thread", "hl_blocking", "hl_dyn_call_safe", "hl_throw"};
    static int result = -1;
    if (result >= 0) return result;
    _Static_assert(sizeof hl == sizeof names / sizeof names[0] * sizeof(void *), "one name per libhl function");
    HMODULE m = GetModuleHandleW(L"libhl.dll");
    void **slots = (void **)&hl;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!(slots[i] = m ? (void *)GetProcAddress(m, names[i]) : NULL))
            return snprintf(status, sizeof status, "off: libhl has no %s", names[i]), result = 0;
    return result = 1;
}

static struct {
    uint64_t frames, splits, unsplit, redrawn, worker_errors, redraw_errors, aborted, put_back, put_back_threw;
} stats;
static double pass_ticks;
static uint64_t pass_at;
static int pass_timed;

static void **driver;
static ID3D11Device *device;
static ID3D11DeviceContext *immediate;
static int depth, count, applied = -1;
static char passes_wanted[256] = "default";
static void *(*driver_source)(void);
// Set only by test builds.
static int (*test_range)(void);
static void (*test_spoil)(void);

// HMDModel.curMaterial per thread, as several threads draw one model's meshes.
static __thread int material = -1;

// The dx_driver's device must be d3d11's and its context that device's immediate one.
static int ready(void) {
    if (immediate) return 1;
    if (!driver_source) {
        HMODULE dx = GetModuleHandleW(L"directx.hdll");
        driver_source = dx ? (void *(*)(void))(void *)GetProcAddress(dx, "directx_get_driver") : NULL;
    }
    void **d = driver_source ? (void **)driver_source() : NULL;
    if (!d || !d[0]) return snprintf(status, sizeof status, "off: no dx_driver in directx.hdll"), 0;
    ID3D11DeviceContext *imm = NULL;
    ID3D11Device_GetImmediateContext((ID3D11Device *)d[0], &imm);
    RELEASE(imm); // the dx_driver holds it
    if ((void *)imm != d[1])
        return snprintf(status, sizeof status, "off: the dx_driver's context is not its device's immediate one"), 0;
    driver = d, device = (ID3D11Device *)d[0], immediate = imm;
    return 1;
}

static int wanted(const char *name) {
    size_t n = strlen(name);
    for (const char *p = passes_wanted; n && (p = strstr(p, name)); p += n)
        if ((p == passes_wanted || p[-1] == ',') && (p[n] == 0 || p[n] == ',')) return 1;
    return 0;
}

static uint64_t now(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)t.QuadPart;
}

// Each thunk jumps to the calling thread's own context's method, so directx.hdll's natives stay as they are.
#define THUNKS 128
#define THUNK_SIZE 48
static DWORD tls_context = TLS_OUT_OF_INDEXES;
static void *proxy_vtable[THUNKS];
static struct {
    void **vtable;
} proxy = {proxy_vtable};

static int build_proxy(void) {
    if (proxy_vtable[0]) return 0;
    if (tls_context == TLS_OUT_OF_INDEXES) tls_context = TlsAlloc();
    if (tls_context >= 64 + 1024) return snprintf(status, sizeof status, "off: TLS slot %lu", tls_context), -1;
    unsigned char *code = VirtualAlloc(NULL, THUNKS * THUNK_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!code) return snprintf(status, sizeof status, "off: no memory for the proxy"), -1;
    // A thread that never set the slot reads null and gets the immediate context.
    int expansion = tls_context >= 64;
    uint32_t slot = expansion ? 8 * (tls_context - 64) : 0x1480 + 8 * tls_context, slots = 0x1780;
    uint64_t imm = (uint64_t)(uintptr_t)immediate;
    for (int i = 0; i < THUNKS; i++) {
        unsigned char *p = code + i * THUNK_SIZE;
        uint32_t method = 8 * i;
        const unsigned char head[] = {0x65, 0x48, 0x8B, 0x0C, 0x25}; // mov rcx, gs:[disp32]
        memcpy(p, head, 5), memcpy(p + 5, expansion ? &slots : &slot, 4), p += 9;
        if (expansion) {
            const unsigned char deref[] = {0x48, 0x85, 0xC9, 0x74, 0x0C, 0x48, 0x8B, 0x89}; // test; jz fallback; mov rcx,[rcx+d]
            memcpy(p, deref, 8), memcpy(p + 8, &slot, 4), p += 12;
        }
        const unsigned char test[] = {0x48, 0x85, 0xC9, 0x75, 0x0A, 0x48, 0xB9}; // test rcx,rcx; jnz +10; mov rcx, imm
        memcpy(p, test, 7), memcpy(p + 7, &imm, 8), p += 15;
        const unsigned char jump[] = {0x48, 0x8B, 0x01, 0xFF, 0xA0}; // mov rax,[rcx]; jmp [rax+method]
        memcpy(p, jump, 5), memcpy(p + 5, &method, 4);
        proxy_vtable[i] = code + i * THUNK_SIZE;
    }
    DWORD old;
    VirtualProtect(code, THUNKS * THUNK_SIZE, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), code, THUNKS * THUNK_SIZE);
    return 0;
}

// Copies kept between frames and refreshed at each split, so each worker writes its own.
static void *type_of(void *p) { return *(void **)p; }

static int element_size(void *array_type) {
    int kind = *(int *)array_type; // hl_type.kind
    return kind == 1 || kind == 7 ? 1 : kind == 2 ? 2 : kind == 3 || kind == 5 ? 4 : 8;
}

static void *copy(void *dst, void *src, int kind, int cls);

static void *copy_obj(void *dst, void *src, int cls) {
    int size = hl.memsize(src);
    if (!dst || type_of(dst) != type_of(src) || hl.memsize(dst) < size) dst = hl.alloc_obj(type_of(src));
    void *kids[16];
    int n = 0;
    for (int i = 0; i < wt_defer_nowned && n < 16; i++)
        if (wt_defer_owned[i].cls == cls) kids[n++] = *(void **)((char *)dst + wt_defer_owned[i].offset);
    memcpy((char *)dst + 8, (char *)src + 8, size - 8);
    n = 0;
    for (int i = 0; i < wt_defer_nowned && n < 16; i++) {
        const WtOwned *o = &wt_defer_owned[i];
        if (o->cls != cls) continue;
        void *child = *(void **)((char *)src + o->offset);
        *(void **)((char *)dst + o->offset) = child ? copy(kids[n], child, o->kind, o->child) : NULL;
        n++;
    }
    return dst;
}

static void *copy(void *dst, void *src, int kind, int cls) {
    if (kind == COPY_OBJ) return copy_obj(dst, src, cls);
    if (kind == COPY_BYTES) {
        int size = hl.memsize(src);
        if (!dst || hl.memsize(dst) < size) dst = hl.alloc_bytes(size);
        memcpy(dst, src, size);
        return dst;
    }
    if (kind == COPY_ARRAY) { // varray: type, element type, size, then the elements from +24
        void *at = *(void **)((char *)src + 8);
        int size = *(int *)((char *)src + 16);
        if (!dst || *(void **)((char *)dst + 8) != at || *(int *)((char *)dst + 16) != size) dst = hl.alloc_array(at, size);
        memcpy((char *)dst + 24, (char *)src + 24, (size_t)size * element_size(at));
        return dst;
    }
    if (!dst) dst = hl.hialloc();
    void *keys = hl.hikeys(src);
    int nkeys = *(int *)((char *)keys + 16);
    for (int i = 0; i < nkeys; i++) {
        int key = ((int *)((char *)keys + 24))[i];
        hl.hiset(dst, key, hl.higet(src, key));
    }
    return dst;
}

static void *field(void *obj, int off) { return obj && off >= 0 ? *(void **)((char *)obj + off) : NULL; }

static int owned_offset(int cls, const char *name) {
    for (int i = 0; i < wt_defer_nowned; i++)
        if (wt_defer_owned[i].cls == cls && !strcmp(wt_defer_owned[i].field, name)) return wt_defer_owned[i].offset;
    return -1;
}

static void *engine_of(void *output) {
    return field(field(output, owned_offset(C_OUTPUT, "ctx")), owned_offset(C_CONTEXT, "engine"));
}

// Range 0 is the main thread's.
typedef struct {
    HANDLE thread, go, finished;
    void *output;
    ID3D11DeviceContext *context;
    ID3D11CommandList *list;
    void *first;
    int count, left, failed, threw, put_back;
    int calls0, switches0, dispatches0;
    double tris0;
} Worker;
static Worker workers[MAX_WORKERS + 1];
static int nworkers, splitting, nsplit, main_left;
static void *range_closure, *main_output;
static __thread Worker *ranged;
static __thread bool on_worker;

static DWORD WINAPI worker_main(void *arg) {
    Worker *w = (Worker *)arg;
    int top;
    if (!test_range) hl.register_thread(&top), hl.blocking(1);
    on_worker = true;
    for (;;) {
        WaitForSingleObject(w->go, INFINITE);
        if (!test_range) hl.blocking(0);
        TlsSetValue(tls_context, w->context);
        ranged = w, material = -1, w->put_back = 0; // a range given up inside render left material set
        unsigned char exception = 0;
        void *thrown = NULL;
        if (test_range) exception = (unsigned char)test_range();
        else {
            void *args[3] = {w->output, NULL, NULL};
            thrown = hl.call_safe(range_closure, args, 3, &exception);
        }
        TlsSetValue(tls_context, NULL);
        ranged = NULL;
        if (FAILED(ID3D11DeviceContext_FinishCommandList(w->context, FALSE, &w->list))) w->list = NULL, exception = 1;
        w->failed = exception, w->threw = thrown != NULL;
        if (!test_range) hl.blocking(1);
        SetEvent(w->finished);
    }
    return 0;
}

static int start_workers(int n) {
    for (int i = nworkers + 1; i <= n; i++) {
        Worker *w = &workers[i];
        w->go = CreateEventW(NULL, FALSE, FALSE, NULL), w->finished = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (FAILED(ID3D11Device_CreateDeferredContext(device, 0, &w->context)))
            return snprintf(status, sizeof status, "off: CreateDeferredContext failed"), -1;
        if (!test_range) hl.add_root(&w->output);
        w->thread = CreateThread(NULL, 1 << 20, worker_main, w, 0, NULL);
        if (!w->thread) return snprintf(status, sizeof status, "off: CreateThread failed"), -1;
        nworkers = i;
    }
    return 0;
}

static void join(void) {
    HANDLE finished[MAX_WORKERS];
    for (int k = 1; k < nsplit; k++) finished[k - 1] = workers[k].finished;
    if (!test_range) hl.blocking(1);
    WaitForMultipleObjects(nsplit - 1, finished, TRUE, INFINITE);
    if (!test_range) hl.blocking(0);
}

// At a frame start on the main thread: what a pass that threw left open, then the panel's worker count.
static void defer_frame(void) {
    stats.frames++, material = -1;
    if (splitting) { // a throw in the main thread's range: the plain draw has none of the later ranges
        join();
        for (int k = 1; k < nsplit; k++) RELEASE(workers[k].list), workers[k].list = NULL;
        splitting = 0, stats.aborted++;
    }
    depth = 0;
    int want = (int)(wt_live[LIVE_DRAW_THREADS].value + 0.5);
    if (want == applied) return;
    applied = want, count = 0;
    if (!want) snprintf(status, sizeof status, "off");
    else if (!ready() || (!test_range && !hl_ready()) || build_proxy() || start_workers(want)) wt_log("deferdraw: %s", status);
    else count = want, snprintf(status, sizeof status, "on");
    if (driver) driver[1] = count ? (void *)&proxy : (void *)immediate;
}
HL_PRIM(defer_frame)

static void defer_closure(void *closure) { range_closure = closure; }
HL_PRIM(defer_closure)

static void threads_begin(void *passes, void *output) {
    void *firsts[MAX_WORKERS + 1];
    int counts[MAX_WORKERS + 1];
    int parts = wt_pass_ranges(passes, wt_defer_off_obj, count + 1, firsts, counts);
    if (parts < 2) {
        stats.unsplit++;
        return;
    }
    State s;
    capture(immediate, &s);
    for (int k = 1; k < parts; k++) {
        Worker *w = &workers[k];
        if (!test_range) {
            w->output = copy_obj(w->output, output, C_OUTPUT);
            void *engine = engine_of(w->output);
            w->calls0 = *(int *)((char *)engine + wt_defer_off_calls);
            w->switches0 = *(int *)((char *)engine + wt_defer_off_switches);
            w->dispatches0 = *(int *)((char *)engine + wt_defer_off_dispatches);
            w->tris0 = *(double *)((char *)engine + wt_defer_off_tris);
        }
        apply(w->context, &s);
        w->first = firsts[k], w->count = w->left = counts[k];
    }
    release(&s);
    for (int k = 1; k < parts; k++) SetEvent(workers[k].go);
    main_left = counts[0], main_output = output, splitting = 1, nsplit = parts;
    stats.splits++;
}

// The split pass's time on the main thread, for the panel.
static void defer_begin(void *passes, void *output) {
    if (depth++ > 0) return;
    char name[40];
    wt_pass_name(passes, name, sizeof name);
    if (!wanted(name)) return;
    pass_at = now(), pass_timed = 1;
    if (count) threads_begin(passes, output);
}
HL_PRIM(defer_begin)

// Split, the main thread stops at the end of its share.
static int defer_object(void) { return !splitting || main_left-- > 0; }
HL_PRIM(defer_object)

// Played lists leave their bytes in the constant buffers, so the main thread's copies of them go stale.
static void shadows_spoil(void *output) {
    void *driver_obj = field(engine_of(output), owned_offset(C_ENGINE, "driver"));
    void *map = field(field(driver_obj, wt_defer_off_shaders), owned_offset(C_INTMAP, "h"));
    if (!map) return;
    void *keys = hl.hikeys(map);
    int n = *(int *)((char *)keys + 16);
    for (int i = 0; i < n; i++) {
        void *compiled = hl.higet(map, ((int *)((char *)keys + 24))[i]);
        void *stages[2] = {field(compiled, wt_defer_off_vertex), field(compiled, wt_defer_off_fragment)};
        for (int k = 0; k < 2; k++) {
            void *content = field(stages[k], wt_defer_off_params_content);
            if (content) memset(content, 0xff, hl.memsize(content));
        }
    }
}

static void spoil(void) {
    if (test_spoil) test_spoil();
    else shadows_spoil(main_output);
}

// A range a worker gave up, drawn by the main thread after the lists before it changed the constant buffers.
static void *redraw_thrown;
static int redraw(Worker *w) {
    spoil();
    w->left = w->count;
    ranged = w;
    unsigned char exception = 0;
    if (test_range) test_range();
    else {
        void *args[3] = {main_output, NULL, NULL};
        redraw_thrown = hl.call_safe(range_closure, args, 3, &exception);
    }
    ranged = NULL;
    stats.redrawn++, stats.redraw_errors += exception;
    return exception;
}

// Logged at 1, 10, 100... times: the case that once left a scene black.
static void put_back_seen(int threw) {
    uint64_t n = ++stats.put_back, m = n;
    stats.put_back_threw += threw;
    while (m % 10 == 0) m /= 10;
    if (m == 1)
        wt_log("deferdraw: a worker gave up mid-draw with a pass object's index swapped, put back: %llu times now, "
               "%llu of their redraws threw", (unsigned long long)n, (unsigned long long)stats.put_back_threw);
}

// 1 when a redraw threw: the plain draw stops there, so the later ranges are dropped.
static int threads_end(void) {
    join();
    void *engine = test_range ? NULL : engine_of(main_output);
    int threw = 0;
    for (int k = 1; k < nsplit; k++) {
        Worker *w = &workers[k];
        if (threw) RELEASE(w->list);
        else if (w->failed) {
            RELEASE(w->list), threw = redraw(w);
            if (w->put_back) put_back_seen(threw);
        }
        else ID3D11DeviceContext_ExecuteCommandList(immediate, w->list, TRUE), RELEASE(w->list);
        w->list = NULL;
        stats.worker_errors += w->threw;
        if (engine && !w->failed) { // a redrawn range counted on the main thread's engine
            void *own = engine_of(w->output);
            *(int *)((char *)engine + wt_defer_off_calls) += *(int *)((char *)own + wt_defer_off_calls) - w->calls0;
            *(int *)((char *)engine + wt_defer_off_switches) += *(int *)((char *)own + wt_defer_off_switches) - w->switches0;
            *(int *)((char *)engine + wt_defer_off_dispatches) +=
                *(int *)((char *)own + wt_defer_off_dispatches) - w->dispatches0;
            *(double *)((char *)engine + wt_defer_off_tris) += *(double *)((char *)own + wt_defer_off_tris) - w->tris0;
        }
    }
    spoil();
    splitting = 0;
    return threw;
}

static void defer_end(void) {
    if (--depth > 0) return;
    int threw = splitting ? threads_end() : 0;
    if (pass_timed) {
        double ticks = (double)(now() - pass_at);
        pass_ticks = pass_ticks ? pass_ticks * 0.98 + ticks * 0.02 : ticks;
        pass_timed = 0;
    }
    if (threw) hl.throw_(redraw_thrown); // as the plain draw would have
}
HL_PRIM(defer_end)

// The workers' loop copy takes its range here.
static void *range_first(void) { return ranged->first; }
HL_PRIM(range_first)

static int range_more(void) { return ranged->left-- > 0; }
HL_PRIM(range_more)

static void range_put_back(void) { ranged->put_back = 1; }
HL_PRIM(range_put_back)

// Before anything that fills a cache shared by every thread: a worker gives its range up (the main thread draws it).
static void worker_check(void) {
    if (on_worker) hl.throw_(NULL);
}
HL_PRIM(worker_check)

static void material_set(int value) { material = value; }
static int material_get(void) { return material; }
HL_PRIM(material_set)
HL_PRIM(material_get)

// Workers upload every parameter buffer: the driver's skip compares with one copy shared by every thread.
static int params_upload_all(void) { return on_worker; }
HL_PRIM(params_upload_all)

// haxe.io.FPHelper.i32ToFloat went through one static scratch buffer.
static float i32_to_f32(int bits) {
    float f;
    memcpy(&f, &bits, 4);
    return f;
}
HL_PRIM(i32_to_f32)

int wt_defer_stats(char *out, int size) {
    LARGE_INTEGER hz;
    QueryPerformanceFrequency(&hz);
    int n = count ? snprintf(out, size, "%d workers", count) : snprintf(out, size, "%s", status);
    if (pass_ticks && n < size)
        n += snprintf(out + n, size - n, "; the main pass %.2f ms on the main thread", 1000 * pass_ticks / hz.QuadPart);
    if (stats.redrawn && n < size)
        n += snprintf(out + n, size - n, "; %llu ranges redrawn", (unsigned long long)stats.redrawn);
    if (stats.put_back && n < size)
        n += snprintf(out + n, size - n, " (%llu indexes put back)", (unsigned long long)stats.put_back);
    return n;
}
