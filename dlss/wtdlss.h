// wtdlss.dll: NGX's D3D11 DLSS behind a C interface, built with MSVC as NGX needs its static runtime; NGX's values.
// wtfsr.dll has the same interface over AMD FSR 3.1.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

enum {
    WTDLSS_PERF = 0,
    WTDLSS_BALANCED = 1,
    WTDLSS_QUALITY = 2,
    WTDLSS_ULTRA_PERF = 3,
    WTDLSS_DLAA = 5,
};

enum {
    WTDLSS_HDR = 1 << 0,
    WTDLSS_MV_LOW_RES = 1 << 1,
    WTDLSS_MV_JITTERED = 1 << 2,
    WTDLSS_DEPTH_INVERTED = 1 << 3,
    WTDLSS_AUTO_EXPOSURE = 1 << 6,
};

typedef struct WtDlssFrame {
    void *color, *depth, *motion, *output, *exposure; // ID3D11Resource*; exposure may be NULL
    float jitter_x, jitter_y;                         // render pixels, as applied to the projection
    float mv_scale_x, mv_scale_y;                     // to render pixels pointing at the previous frame
    int render_width, render_height;
    int reset;
    float frame_ms;
    float camera_near, camera_far, fov_y; // fov_y in radians; FSR takes them, DLSS doesn't
    float sharpness;                      // FSR's own sharpening (RCAS): 0 off to 1 strongest; DLSS has none
} WtDlssFrame;

typedef void (*WtDlssLog)(const char *line);

typedef int (*WtDlssInit)(void *device, const wchar_t *data_dir, const wchar_t *dll_dir, WtDlssLog log);
typedef int (*WtDlssOptimal)(int quality, int out_w, int out_h, int *w, int *h, int *min_w, int *min_h, int *max_w,
                             int *max_h);
typedef int (*WtDlssCreate)(void *context, int quality, int render_w, int render_h, int out_w, int out_h, int flags,
                            int preset);
typedef int (*WtDlssEvaluate)(void *context, const WtDlssFrame *frame);
typedef int (*WtDlssVram)(unsigned long long *bytes);
typedef void (*WtDlssRelease)(void);
typedef void (*WtDlssShutdown)(void);

#ifdef __cplusplus
}
#endif
