// DLSS in place of the game's TAA blend (natives in dlss.c, patches in fixes/dlss.c).
#pragma once
#include "hlcode.h"

// DLSS_RENDERER is the renderer object itself; the rest are boxed D3D11 resources or matrix objects. UP_: the
// output-size textures after an upscale (colour, R32F depth, D24S8).
enum { DLSS_COLOR, DLSS_OUTPUT, DLSS_DEPTH, DLSS_VELOCITY, DLSS_CUR_INV_VP, DLSS_PREV_VP, DLSS_RENDERER, DLSS_SCENE_DS,
       DLSS_UP_COLOR, DLSS_UP_DEPTH, DLSS_UP_DS, DLSS_SLOTS };
// The camera's jitter (NDC), vertical field of view (degrees) and planes; only FSR reads the last three.
enum { DLSS_JITTER_X, DLSS_JITTER_Y, DLSS_FOV_Y, DLSS_Z_NEAR, DLSS_Z_FAR, DLSS_NUMBERS };

// h3d.MatrixImpl's _11.._44 (f64), from the bytecode's types.
typedef struct {
    int matrix[16];
} WtDlssLayout;

extern WtDlssLayout wt_dlss_layout;
int wt_dlss_layout_build(HlCode *c);
int wt_dlss_stats(char *out, int size);
