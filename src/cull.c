// battlecull's native half: the culling script run through the game's hscript, when it is due, and its state.
// Each run first undoes what the last one changed, so the panel can switch it off.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "fix.h"
#include "live.h"

extern unsigned char *hl_alloc_bytes(int size);
extern wchar_t *hl_to_string(void *value);
extern void hl_add_root(void *pointer);

// Bound: battle, game, cls, offscreen (0/1), smallPx (0: off), state (kept between runs).
// Spheres, not Bounds: ObjectCollider passes a matrix, which Bounds refuses ("Not implemented"). An exception in the
// renderer reaches the game's crash reporter every frame, so each collider is tried once in a trap first.
static const char script[] =
    "var Is = cls(\"Std\").isOfType;\n"
    "var T = cls(\"Type\");\n"
    "var CMesh = cls(\"h3d.scene.Mesh\");\n"
    "var CSkin = cls(\"h3d.scene.Skin\");\n"
    "var CBatch = cls(\"h3d.scene.MeshBatch\");\n"
    "var CModel = cls(\"h3d.prim.HMDModel\");\n"
    "var models = state.get(\"models\");\n"
    "if (models != null) for (m in models) if (m[0].cullingScreenRatio == m[2]) m[0].cullingScreenRatio = m[1];\n"
    "var colliders = state.get(\"colliders\");\n"
    "if (colliders != null && state.get(\"battle\") == battle)\n"
    "    for (c in colliders) if (c[0].cullingCollider == c[1]) c[0].cullingCollider = null;\n"
    "models = [];\n"
    "colliders = [];\n"
    "state.set(\"battle\", battle);\n"
    "state.set(\"models\", models);\n"
    "state.set(\"colliders\", colliders);\n"
    "var height = cls(\"h3d.Engine\").CURRENT.height;\n"
    "var ratio = smallPx > 0 && height > 0 ? smallPx / height : 0.0;\n"
    "var seen = 0;\n"
    "var failed = 0;\n"
    "var frustum = battle.s3d.camera.frustum;\n"
    "if (offscreen > 0 || ratio > 0) {\n"
    "    var stack = [battle.s3d];\n"
    "    var inMap = [false];\n"
    "    while (stack.length > 0 && seen < 200000) {\n"
    "        var o = stack.pop();\n"
    "        var mapped = inMap.pop();\n"
    "        seen++;\n"
    "        var name = T.getClassName(T.getClass(o));\n"
    "        if (name == \"ent.UnitView\" || name == \"prefab.FXAnimation\" || name == \"hrt.prefab.l3d.MeshSprayObject\") continue;\n"
    "        if (o.name == \"BattleRoot\" || name == \"prefab.MeshBatcherObject\") mapped = true;\n"
    "        if (mapped && Is(o, CMesh) && !Is(o, CSkin) && !Is(o, CBatch) && o.primitive != null && name.indexOf(\"terrain\") < 0) {\n"
    "            var p = o.primitive;\n"
    "            var b = p.getBounds();\n"
    "            if (b != null) {\n"
    "                if (offscreen > 0 && o.cullingCollider == null) {\n"
    "                    var c = new h3d.col.ObjectCollider(o, b.toSphere());\n"
    "                    var ok = true;\n"
    "                    try { c.inFrustum(frustum, null); } catch (e:Dynamic) { ok = false; }\n"
    "                    if (ok) {\n"
    "                        o.cullingCollider = c;\n"
    "                        colliders.push([o, c]);\n"
    "                    } else failed++;\n"
    "                }\n"
    "                if (ratio > 0 && Is(p, CModel) && p.cullingScreenRatio == 0 && p.lodCount() == 1\n"
    "                    && b.xMax - b.xMin < 2 && b.yMax - b.yMin < 2 && b.zMax - b.zMin < 2) {\n"
    "                    models.push([p, p.cullingScreenRatio, ratio]);\n"
    "                    p.cullingScreenRatio = ratio;\n"
    "                }\n"
    "            }\n"
    "        }\n"
    "        var children = o.children;\n"
    "        var k = children.length - 1;\n"
    "        while (k >= 0) {\n"
    "            stack.push(children[k]);\n"
    "            inMap.push(mapped);\n"
    "            k--;\n"
    "        }\n"
    "    }\n"
    "}\n"
    "colliders.length + \" props given culling colliders (\" + failed + \" refused), \" + models.length\n"
    "    + \" small models culled under \" + smallPx + \" px (\" + seen + \" objects walked)\";\n";

static wchar_t *source;
static int source_length = -1;
static void *due_battle, *done_battle;
static bool rerun;
static void *state;
static bool state_rooted;

static void log_value(const char *prefix, void *value) {
    char text[512] = "";
    wchar_t *w = value ? hl_to_string(value) : NULL;
    if (w) WideCharToMultiByte(CP_UTF8, 0, w, -1, text, sizeof text - 1, NULL, NULL);
    wt_log("battlecull: %s%s", prefix, w ? text : "(could not be printed)");
}

// Loading is checked by the caller, which comes back next frame.
static int cull_due(void *battle) {
    if (wt_live_take(LIVE_TAKE_CULL)) rerun = true;
    if (battle != done_battle) rerun = true;
    if (!rerun) return -1;
    if (source_length < 0) {
        source_length = MultiByteToWideChar(CP_UTF8, 0, script, -1, NULL, 0) - 1;
        source = (wchar_t *)malloc((source_length + 1) * sizeof(wchar_t));
        if (!source) return source_length = -1;
        MultiByteToWideChar(CP_UTF8, 0, script, -1, source, source_length + 1);
    }
    due_battle = battle;
    return source_length;
}

static unsigned char *cull_source(int length) {
    unsigned char *bytes = hl_alloc_bytes((length + 1) * (int)sizeof(wchar_t));
    memcpy(bytes, source, (length + 1) * sizeof(wchar_t));
    return bytes;
}

static void *cull_state_get(void) { return state; }

static void cull_state_set(void *value) {
    if (!state_rooted) hl_add_root(&state), state_rooted = true;
    state = value;
}

static void finish(void) {
    done_battle = due_battle;
    rerun = false;
}

static void cull_done(void *result) {
    log_value("", result);
    finish();
}

// Not retried until the next battle or setting change: a failing script would otherwise run every frame.
static void cull_failed(void *error) {
    log_value("the setup failed: ", error);
    finish();
}

#define HL_PRIM(name) \
    __declspec(dllexport) void *hlp_##name(const char **sign) { *sign = NULL; return (void *)name; }
HL_PRIM(cull_due)
HL_PRIM(cull_source)
HL_PRIM(cull_state_get)
HL_PRIM(cull_state_set)
HL_PRIM(cull_done)
HL_PRIM(cull_failed)
