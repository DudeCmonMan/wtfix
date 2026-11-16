// Battle effects that follow the zoom past the game's limit: an op block inserted before a game op, which it jumps on
// to (`skip`, an hl_fun_insert target) as soon as a check fails. A failed lookup makes zoom_insert refuse.
#pragma once
#include "fix.h"

#define VANILLA_MAX_ZOOM 40.0

typedef struct {
    HlCode *c;
    HlEdit *e;
    HlFun *f;
    HlOp *ops;
    int n, bad, tf64;
} Zoom;

void zoom_begin(Zoom *z, HlCode *c, HlEdit *e, HlFun *f);
int zoom_reg(Zoom *z, int type);
void zoom_op(Zoom *z, HlOp op);
int zoom_float(Zoom *z, double value);
int zoom_object(Zoom *z, int robj, const char *name, int skip); // skips when null
void zoom_live(Zoom *z, int live_id, int skip);           // skips unless the panel's switch is on
void zoom_battle_mode(Zoom *z, int skip);                 // skips unless the battle is Game.mode
int zoom_distance(Zoom *z, int rcam, int skip);           // |camera.pos - camera.target|
int zoom_insert(Zoom *z, int at);                         // frees the block either way
