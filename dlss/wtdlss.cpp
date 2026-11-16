// NGX's D3D11 path keeps the immediate context's state (DLSS Programming Guide 5.2.5), so Heaps' cached state holds.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <stdarg.h>
#include <stdio.h>
#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers.h"
#include "wtdlss.h"

#define EXPORT extern "C" __declspec(dllexport)

// NGX identifies a custom engine by a GUID-like project ID.
static const char *project_id = "d31a7973-0543-4a70-ad4d-376e6cca151e";

static WtDlssLog log_sink;
static ID3D11Device *ngx_device;
static NVSDK_NGX_Parameter *params;
static NVSDK_NGX_Handle *feature;

static void say(const char *format, ...) {
    if (!log_sink) return;
    char line[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    log_sink(line);
}

// Only errors and warnings, less the expected failed probes for NGXCore in the game folder before the driver's.
static void NVSDK_CONV ngx_log(const char *message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    if ((!strstr(message, "error") && !strstr(message, "warning") && !strstr(message, "Warning")) ||
        strstr(message, "failed to load NGXCore"))
        return;
    char line[1024];
    snprintf(line, sizeof line, "ngx: %s", message);
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (log_sink) log_sink(line);
}

EXPORT int wtdlss_init(void *device, const wchar_t *data_dir, const wchar_t *dll_dir, WtDlssLog log) {
    log_sink = log;
    const wchar_t *paths[1] = {dll_dir};
    NVSDK_NGX_FeatureCommonInfo info = {};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = dll_dir ? 1 : 0;
    info.LoggingInfo.LoggingCallback = ngx_log;
    info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    info.LoggingInfo.DisableOtherLoggingSinks = true;
    NVSDK_NGX_Result r = NVSDK_NGX_D3D11_Init_with_ProjectID(project_id, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "wtfix", data_dir,
                                                             (ID3D11Device *)device, &info);
    if (NVSDK_NGX_FAILED(r)) {
        say("init failed: 0x%08x", r);
        return r;
    }
    ngx_device = (ID3D11Device *)device;
    r = NVSDK_NGX_D3D11_GetCapabilityParameters(&params);
    if (NVSDK_NGX_FAILED(r)) {
        say("capability parameters failed: 0x%08x", r);
        return r;
    }
    int available = 0, needs_driver = 0, init_result = 0;
    unsigned int major = 0, minor = 0;
    params->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    params->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
    params->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &major);
    params->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minor);
    params->Get(NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &init_result);
    if (!available) {
        say("DLSS not available (feature init 0x%08x%s, driver %u.%u needed)", init_result,
            needs_driver ? ", driver too old" : "", major, minor);
        return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    }
    return 0;
}

EXPORT int wtdlss_optimal(int quality, int out_w, int out_h, int *w, int *h, int *min_w, int *min_h, int *max_w,
                          int *max_h) {
    if (!params) return NVSDK_NGX_Result_FAIL_NotInitialized;
    unsigned int ow = 0, oh = 0, xw = 0, xh = 0, nw = 0, nh = 0;
    float sharpness = 0;
    NVSDK_NGX_Result r = NGX_DLSS_GET_OPTIMAL_SETTINGS(params, out_w, out_h, (NVSDK_NGX_PerfQuality_Value)quality, &ow,
                                                       &oh, &xw, &xh, &nw, &nh, &sharpness);
    if (NVSDK_NGX_FAILED(r)) return r;
    *w = ow, *h = oh, *min_w = nw, *min_h = nh, *max_w = xw, *max_h = xh;
    return 0;
}

EXPORT void wtdlss_release(void) {
    if (feature) NVSDK_NGX_D3D11_ReleaseFeature(feature);
    feature = NULL;
}

EXPORT int wtdlss_create(void *context, int quality, int render_w, int render_h, int out_w, int out_h, int flags,
                         int preset) {
    if (!params) return NVSDK_NGX_Result_FAIL_NotInitialized;
    wtdlss_release();
    if (preset) {
        const char *hints[] = {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
                               NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                               NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
                               NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                               NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance};
        for (const char *hint : hints) params->Set(hint, (unsigned int)preset);
    }
    NVSDK_NGX_DLSS_Create_Params create = {};
    create.Feature.InWidth = render_w;
    create.Feature.InHeight = render_h;
    create.Feature.InTargetWidth = out_w;
    create.Feature.InTargetHeight = out_h;
    create.Feature.InPerfQualityValue = (NVSDK_NGX_PerfQuality_Value)quality;
    create.InFeatureCreateFlags = flags;
    NVSDK_NGX_Result r = NGX_D3D11_CREATE_DLSS_EXT((ID3D11DeviceContext *)context, &feature, params, &create);
    if (NVSDK_NGX_FAILED(r)) {
        say("create %dx%d -> %dx%d (quality %d) failed: 0x%08x", render_w, render_h, out_w, out_h, quality, r);
        feature = NULL;
        return r;
    }
    return 0;
}

EXPORT int wtdlss_evaluate(void *context, const WtDlssFrame *f) {
    if (!feature) return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    NVSDK_NGX_D3D11_DLSS_Eval_Params e = {};
    e.Feature.pInColor = (ID3D11Resource *)f->color;
    e.Feature.pInOutput = (ID3D11Resource *)f->output;
    e.pInDepth = (ID3D11Resource *)f->depth;
    e.pInMotionVectors = (ID3D11Resource *)f->motion;
    e.pInExposureTexture = (ID3D11Resource *)f->exposure;
    e.InJitterOffsetX = f->jitter_x;
    e.InJitterOffsetY = f->jitter_y;
    e.InMVScaleX = f->mv_scale_x;
    e.InMVScaleY = f->mv_scale_y;
    e.InReset = f->reset;
    e.InRenderSubrectDimensions.Width = f->render_width;
    e.InRenderSubrectDimensions.Height = f->render_height;
    e.InFrameTimeDeltaInMsec = f->frame_ms;
    NVSDK_NGX_Result r = NGX_D3D11_EVALUATE_DLSS_EXT((ID3D11DeviceContext *)context, feature, params, &e);
    return NVSDK_NGX_FAILED(r) ? r : 0;
}

EXPORT int wtdlss_vram(unsigned long long *bytes) {
    if (!params) return NVSDK_NGX_Result_FAIL_NotInitialized;
    NVSDK_NGX_Result r = NGX_DLSS_GET_STATS(params, bytes);
    return NVSDK_NGX_FAILED(r) ? r : 0;
}

EXPORT void wtdlss_shutdown(void) {
    wtdlss_release();
    if (params) NVSDK_NGX_D3D11_DestroyParameters(params);
    params = NULL;
    if (ngx_device) NVSDK_NGX_D3D11_Shutdown1(ngx_device);
    ngx_device = NULL;
}
