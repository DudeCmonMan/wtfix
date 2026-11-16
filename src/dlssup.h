// Passes after a DLSS upscale run at output size and test depth and stencil, so both are rebuilt from the scene's.
#pragma once
#include <stdbool.h>
#include <d3d11.h>

typedef struct {
    ID3D11Resource *depth, *depth_stencil;         // render size, R32F and D24S8
    ID3D11Resource *out_depth, *out_depth_stencil; // output size, same formats
    int w, h, out_w, out_h;
    float jitter_x, jitter_y; // render pixels, y down, as given to DLSS
    ID3D11Resource *motion;   // render size, RG16F, UV current to previous; may be NULL
    bool reset;
} WtDlssUp;

bool wt_dlss_up_start(ID3D11Device *device, char *error, int size);
// Makes the stencil history ahead of the first upscaled frame.
bool wt_dlss_up_prepare(int out_w, int out_h);
// Leaves the context's state as found; false with a reason, nothing drawn, when a resource is not as expected.
bool wt_dlss_up_run(ID3D11DeviceContext *context, const WtDlssUp *up, char *error, int size);

// What Heaps binds on the immediate context, saved around calls into other code since Heaps caches its state.
typedef struct WtD3dState WtD3dState;
WtD3dState *wt_d3d_save(ID3D11DeviceContext *context);
void wt_d3d_restore(ID3D11DeviceContext *context, WtD3dState *saved);
