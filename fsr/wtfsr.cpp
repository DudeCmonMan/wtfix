// wtfsr.dll: AMD FSR 3.1's upscaler on D3D11 behind wtdlss.dll's interface.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_1.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <FidelityFX/host/backends/dx11/ffx_dx11.h>
#include "wtdlss.h"

#define EXPORT extern "C" __declspec(dllexport)

static WtDlssLog log_sink;
static ID3D11Device *fsr_device;
static void *scratch;
static FfxInterface backend;
static FfxFsr3UpscalerContext fsr;
static bool created;
// The upscaler alone takes these from its caller.
static ID3D11Texture2D *shared[3];

static void say(const char *format, ...) {
    if (!log_sink) return;
    char line[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    log_sink(line);
}

static FfxFsr3UpscalerQualityMode fsr_mode(int quality) {
    switch (quality) {
    case WTDLSS_QUALITY: return FFX_FSR3UPSCALER_QUALITY_MODE_QUALITY;
    case WTDLSS_BALANCED: return FFX_FSR3UPSCALER_QUALITY_MODE_BALANCED;
    case WTDLSS_PERF: return FFX_FSR3UPSCALER_QUALITY_MODE_PERFORMANCE;
    case WTDLSS_ULTRA_PERF: return FFX_FSR3UPSCALER_QUALITY_MODE_ULTRA_PERFORMANCE;
    default: return FFX_FSR3UPSCALER_QUALITY_MODE_NATIVEAA;
    }
}

EXPORT int wtfsr_init(void *device, const wchar_t *, const wchar_t *, WtDlssLog log) {
    log_sink = log;
    fsr_device = (ID3D11Device *)device;
    // The luma pyramid pass binds more UAVs than feature level 11.0 allows.
    if (fsr_device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_1) {
        say("fsr: needs a Direct3D 11.1 device; the game's is level %x", fsr_device->GetFeatureLevel());
        return FFX_ERROR_BACKEND_API_ERROR;
    }
    size_t size = ffxGetScratchMemorySizeDX11(FFX_FSR3UPSCALER_CONTEXT_COUNT);
    scratch = calloc(1, size);
    return ffxGetInterfaceDX11(&backend, ffxGetDeviceDX11(fsr_device), scratch, size, FFX_FSR3UPSCALER_CONTEXT_COUNT);
}

EXPORT int wtfsr_optimal(int quality, int out_w, int out_h, int *w, int *h, int *min_w, int *min_h, int *max_w,
                         int *max_h) {
    uint32_t rw = 0, rh = 0;
    FfxErrorCode e = ffxFsr3UpscalerGetRenderResolutionFromQualityMode(&rw, &rh, out_w, out_h, fsr_mode(quality));
    if (e != FFX_OK) return e;
    *w = *min_w = *max_w = (int)rw, *h = *min_h = *max_h = (int)rh;
    return 0;
}

EXPORT void wtfsr_release(void) {
    if (created) ffxFsr3UpscalerContextDestroy(&fsr);
    created = false;
    for (ID3D11Texture2D *&t : shared)
        if (t) t->Release(), t = NULL;
}

// Defined by the backend, not declared in its header.
DXGI_FORMAT ffxGetDX11FormatFromSurfaceFormat(FfxSurfaceFormat surfaceFormat);

static ID3D11Texture2D *shared_texture(const FfxCreateResourceDescription &d) {
    D3D11_TEXTURE2D_DESC t = {};
    t.Width = d.resourceDescription.width, t.Height = d.resourceDescription.height;
    t.MipLevels = 1, t.ArraySize = 1, t.SampleDesc.Count = 1;
    t.Format = ffxGetDX11FormatFromSurfaceFormat(d.resourceDescription.format);
    t.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    ID3D11Texture2D *tex = NULL;
    fsr_device->CreateTexture2D(&t, NULL, &tex);
    return tex;
}

EXPORT int wtfsr_create(void *, int quality, int render_w, int render_h, int out_w, int out_h, int flags, int) {
    wtfsr_release();
    FfxFsr3UpscalerContextDescription d = {};
    d.flags = (flags & WTDLSS_HDR ? FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE : 0) |
              (flags & WTDLSS_DEPTH_INVERTED ? FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED : 0) |
              (flags & WTDLSS_AUTO_EXPOSURE ? FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE : 0) |
              (flags & WTDLSS_MV_JITTERED ? FFX_FSR3UPSCALER_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION : 0) |
              (flags & WTDLSS_MV_LOW_RES ? 0 : FFX_FSR3UPSCALER_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS);
    d.maxRenderSize = {(uint32_t)render_w, (uint32_t)render_h};
    d.maxUpscaleSize = {(uint32_t)out_w, (uint32_t)out_h};
    d.backendInterface = backend;
    FfxErrorCode e;
    // The backend throws on a failed D3D call (a lost device, no memory); the caller wants a code.
    try {
        e = ffxFsr3UpscalerContextCreate(&fsr, &d);
    } catch (...) {
        e = FFX_ERROR_BACKEND_API_ERROR;
    }
    if (e != FFX_OK) {
        say("fsr: create %dx%d -> %dx%d (quality %d) failed (%d)", render_w, render_h, out_w, out_h, quality, e);
        return e;
    }
    created = true;
    FfxFsr3UpscalerSharedResourceDescriptions sd;
    ffxFsr3UpscalerGetSharedResourceDescriptions(&fsr, &sd);
    const FfxCreateResourceDescription *descs[3] = {&sd.dilatedDepth, &sd.dilatedMotionVectors,
                                                    &sd.reconstructedPrevNearestDepth};
    for (int i = 0; i < 3; i++)
        if (!(shared[i] = shared_texture(*descs[i]))) {
            say("fsr: no %ls texture (%dx%d)", descs[i]->name, render_w, render_h);
            wtfsr_release();
            return FFX_ERROR_OUT_OF_MEMORY;
        }
    return 0;
}

// The backend defines this overload (C++, not const); the header's declaration (C, const) has no definition.
FfxResource ffxGetResourceDX11(ID3D11Resource *dx11Resource, FfxResourceDescription ffxResDescription,
                               wchar_t const *ffxResName, FfxResourceStates state);

static FfxResource resource(void *res, FfxResourceStates state) {
    ID3D11Resource *r = (ID3D11Resource *)res;
    return ffxGetResourceDX11(r, GetFfxResourceDescriptionDX11(r), NULL, state);
}

// The game caches its bindings, and an input bound as a render target can't be read: compute and output merger kept.
struct Saved {
    ID3D11ComputeShader *shader;
    ID3D11Buffer *cbs[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT];
    ID3D11ShaderResourceView *srvs[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT];
    ID3D11UnorderedAccessView *uavs[D3D11_1_UAV_SLOT_COUNT];
    ID3D11RenderTargetView *rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    ID3D11DepthStencilView *dsv;
};

template <typename T> static void release_all(T **p, UINT n) {
    for (UINT i = 0; i < n; i++)
        if (p[i]) p[i]->Release();
}

static void save(ID3D11DeviceContext *c, Saved *s) {
    c->CSGetShader(&s->shader, NULL, NULL);
    c->CSGetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, s->cbs);
    c->CSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, s->srvs);
    c->CSGetUnorderedAccessViews(0, D3D11_1_UAV_SLOT_COUNT, s->uavs);
    c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s->rtvs, &s->dsv);
}

static void restore(ID3D11DeviceContext *c, Saved *s) {
    c->CSSetShader(s->shader, NULL, 0);
    c->CSSetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, s->cbs);
    c->CSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, s->srvs);
    c->CSSetUnorderedAccessViews(0, D3D11_1_UAV_SLOT_COUNT, s->uavs, NULL);
    c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s->rtvs, s->dsv);
    if (s->shader) s->shader->Release();
    release_all(s->cbs, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT);
    release_all(s->srvs, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT), release_all(s->uavs, D3D11_1_UAV_SLOT_COUNT);
    release_all(s->rtvs, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT);
    if (s->dsv) s->dsv->Release();
}

EXPORT int wtfsr_evaluate(void *context, const WtDlssFrame *f) {
    ID3D11DeviceContext *c = (ID3D11DeviceContext *)context;
    FfxFsr3UpscalerDispatchDescription x = {};
    x.commandList = ffxGetCommandListDX11(c);
    x.color = resource(f->color, FFX_RESOURCE_STATE_COMPUTE_READ);
    x.depth = resource(f->depth, FFX_RESOURCE_STATE_COMPUTE_READ);
    x.motionVectors = resource(f->motion, FFX_RESOURCE_STATE_COMPUTE_READ);
    x.exposure = resource(f->exposure, FFX_RESOURCE_STATE_COMPUTE_READ);
    x.output = resource(f->output, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    x.dilatedDepth = resource(shared[0], FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    x.dilatedMotionVectors = resource(shared[1], FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    x.reconstructedPrevNearestDepth = resource(shared[2], FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    x.jitterOffset = {f->jitter_x, f->jitter_y};
    x.motionVectorScale = {f->mv_scale_x, f->mv_scale_y};
    x.renderSize = {(uint32_t)f->render_width, (uint32_t)f->render_height};
    x.frameTimeDelta = f->frame_ms;
    x.reset = f->reset != 0;
    x.cameraNear = f->camera_near, x.cameraFar = f->camera_far, x.cameraFovAngleVertical = f->fov_y;
    x.enableSharpening = f->sharpness > 0, x.sharpness = f->sharpness;
    static Saved s;
    save(c, &s);
    c->OMSetRenderTargets(0, NULL, NULL);
    FfxErrorCode e;
    try {
        e = ffxFsr3UpscalerContextDispatch(&fsr, &x);
    } catch (...) {
        e = FFX_ERROR_BACKEND_API_ERROR;
    }
    restore(c, &s);
    return e;
}

EXPORT void wtfsr_shutdown(void) {
    wtfsr_release();
    free(scratch);
    scratch = NULL, fsr_device = NULL;
}
