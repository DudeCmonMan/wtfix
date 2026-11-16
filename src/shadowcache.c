// shadowcache's native half: the battle setup script, the held light camera and when the static map is redrawn.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fix.h"
#include "live.h"
#include "shadowfit.h"

extern unsigned char *hl_alloc_bytes(int size);
extern wchar_t *hl_to_string(void *value);
extern void hl_add_root(void *pointer);

// Materials are read as fields: getMaterials' optional ref<bool> argument is not filled by a call from hscript.
// A pass shared by static and moving meshes stays dynamic; a check run makes static meshes that moved dynamic.
static const char script[] =
    "var Is = cls(\"Std\").isOfType;\n"
    "var T = cls(\"Type\");\n"
    "var CMesh = cls(\"h3d.scene.Mesh\");\n"
    "var CSkin = cls(\"h3d.scene.Skin\");\n"
    "var CMulti = cls(\"h3d.scene.MultiMaterial\");\n"
    "var changes = state.get(\"changes\");\n"
    "var result = \"\";\n"
    "if (check > 0) {\n"
    "    var entries = state.get(\"entries\");\n"
    "    var moved = 0;\n"
    "    if (entries != null && changes != null) for (e in entries) {\n"
    "        if (e[7]) continue;\n"
    "        var m = e[0].absPos;\n"
    "        if (m._41 != e[1] || m._42 != e[2] || m._43 != e[3] || m._11 != e[4] || m._22 != e[5] || m._33 != e[6]) {\n"
    "            e[7] = true;\n"
    "            moved++;\n"
    "            for (p in e[8]) if (p.isStatic) { changes.push([p, true, false]); p.isStatic = false; }\n"
    "        }\n"
    "    }\n"
    "    result = moved + \" static meshes moved and were made dynamic\";\n"
    "} else {\n"
    "    var restored = 0;\n"
    "    if (changes != null) {\n"
    "        var i = changes.length - 1;\n"
    "        while (i >= 0) {\n"
    "            var c = changes[i];\n"
    "            if (c[0].isStatic == c[2]) { c[0].isStatic = c[1]; restored++; }\n"
    "            i--;\n"
    "        }\n"
    "    }\n"
    "    changes = [];\n"
    "    state.set(\"changes\", changes);\n"
    "    state.set(\"entries\", null);\n"
    "    result = restored + \" shadow passes put back\";\n"
    "    if (on > 0) {\n"
    "        var stat = [];\n"
    "        var dyn = [];\n"
    "        var entries = [];\n"
    "        var stack = [battle.s3d];\n"
    "        var inMap = [false];\n"
    "        var inMoving = [false];\n"
    "        var seen = 0;\n"
    "        while (stack.length > 0 && seen < 200000) {\n"
    "            var o = stack.pop();\n"
    "            var mapped = inMap.pop();\n"
    "            var moving = inMoving.pop();\n"
    "            seen++;\n"
    "            var name = T.getClassName(T.getClass(o));\n"
    "            if (name == \"ent.UnitView\" || name == \"prefab.FXAnimation\" || Is(o, CSkin) || o.currentAnimation != null) moving = true;\n"
    "            if (o.name == \"BattleRoot\" || name == \"prefab.MeshBatcherObject\" || name == \"hrt.prefab.l3d.MeshSprayObject\"\n"
    "                || name.indexOf(\"terrain.\") >= 0) mapped = true;\n"
    "            if (Is(o, CMesh)) {\n"
    "                var mine = [];\n"
    "                for (mat in (Is(o, CMulti) ? o.materials : [o.material])) {\n"
    "                    var p = mat == null ? null : mat.getPass(\"shadow\");\n"
    "                    if (p != null) mine.push(p);\n"
    "                }\n"
    "                if (mine.length > 0) {\n"
    "                    var still = !moving && mapped;\n"
    "                    if (!moving && !still) for (p in mine) if (p.isStatic) still = true;\n"
    "                    if (still) {\n"
    "                        for (p in mine) stat.push(p);\n"
    "                        var a = o.absPos;\n"
    "                        entries.push([o, a._41, a._42, a._43, a._11, a._22, a._33, false, mine]);\n"
    "                    } else for (p in mine) dyn.push(p);\n"
    "                }\n"
    "            }\n"
    "            var children = o.children;\n"
    "            var k = children.length - 1;\n"
    "            while (k >= 0) {\n"
    "                stack.push(children[k]);\n"
    "                inMap.push(mapped);\n"
    "                inMoving.push(moving);\n"
    "                k--;\n"
    "            }\n"
    "        }\n"
    "        var marked = 0;\n"
    "        var cleared = 0;\n"
    "        for (p in stat) if (!p.isStatic) { changes.push([p, false, true]); p.isStatic = true; marked++; }\n"
    "        for (p in dyn) if (p.isStatic) { changes.push([p, true, false]); p.isStatic = false; cleared++; }\n"
    "        state.set(\"entries\", entries);\n"
    "        result = entries.length + \" static meshes (\" + stat.length + \" shadow passes, \" + marked + \" marked here), \"\n"
    "            + dyn.length + \" dynamic (\" + cleared + \" unmarked), \" + seen + \" objects walked\";\n"
    "    }\n"
    "}\n"
    "result;\n";

static wchar_t *source;
static int source_length = -1;
static void *state;
static bool state_rooted;

static void *frame_battle, *due_battle, *done_battle;
static bool rerun, run_on, run_check, setup_on, failed, check_pending;

static double slot[SHADOW_SLOTS];
static bool active, keep, refit;
static int last_frame = INT_MIN, last_check_frame = INT_MIN, moved_frames[3] = {INT_MIN, INT_MIN, INT_MIN};
static double last_margin = -1;
static ShadowFit fit;
static ShadowStatic statics;
static unsigned long long frames, drawn, refits, checks;

static bool switched_on(void) { return wt_live[LIVE_SHADOW_CACHE].value >= 0.5; }

static void forget(void) {
    fit.valid = false;
    statics.valid = false;
}

static void log_value(const char *prefix, void *value) {
    char text[512] = "";
    wchar_t *w = value ? hl_to_string(value) : NULL;
    if (w) WideCharToMultiByte(CP_UTF8, 0, w, -1, text, sizeof text - 1, NULL, NULL);
    wt_log("shadowcache: %s%s", prefix, w ? text : "(could not be printed)");
}

// The script's length when a run is due, else -1; the caller checks the battle is built and shown.
static int shadow_due(void *battle) {
    frame_battle = battle;
    if (wt_live_take(LIVE_TAKE_SHADOW)) rerun = true, failed = false;
    if (battle != done_battle) rerun = true, failed = false, check_pending = false;
    if (!rerun && !(check_pending && setup_on && !failed)) return -1;
    if (rerun && !switched_on() && !setup_on) {
        done_battle = battle;
        rerun = false;
        return -1;
    }
    if (source_length < 0) {
        source_length = MultiByteToWideChar(CP_UTF8, 0, script, -1, NULL, 0) - 1;
        source = (wchar_t *)malloc((source_length + 1) * sizeof(wchar_t));
        if (!source) return source_length = -1;
        MultiByteToWideChar(CP_UTF8, 0, script, -1, source, source_length + 1);
    }
    run_check = !rerun;
    run_on = run_check ? setup_on : switched_on();
    due_battle = battle;
    return source_length;
}

static unsigned char *shadow_source(int length) {
    unsigned char *bytes = hl_alloc_bytes((length + 1) * (int)sizeof(wchar_t));
    memcpy(bytes, source, (length + 1) * sizeof(wchar_t));
    return bytes;
}

static int shadow_on(void) { return run_on; }
static int shadow_check(void) { return run_check; }
static void *shadow_state_get(void) { return state; }

static void shadow_state_set(void *value) {
    if (!state_rooted) hl_add_root(&state), state_rooted = true;
    state = value;
}

static void shadow_done(void *result) {
    log_value(run_check ? "check: " : run_on ? "on: " : "off: ", result);
    done_battle = due_battle;
    if (run_check) {
        checks++;
    } else {
        rerun = false;
        setup_on = run_on;
    }
    check_pending = false;
    forget();
}

// Any error turns the cache off until the next battle or a change of the switch; the frame goes on as the game's.
static void shadow_error(void *error) {
    static int logged;
    if (logged++ < 5) log_value("turned off, an error: ", error);
    failed = true;
    rerun = false;
    check_pending = false;
    done_battle = frame_battle;
    active = keep = false;
    forget();
}

// Whether this draw of the battle sun's map is the cache's: the first in its frame, once the setup has run.
static int shadow_begin(void *battle, int frame) {
    active = keep = refit = false;
    if (failed || !setup_on || !switched_on() || battle != done_battle || frame == last_frame) return 0;
    long long gap = (long long)frame - last_frame;
    if (gap < 1 || gap > 4) forget(); // frames the cache did not see: the static map may be stale
    last_frame = frame;
    active = true;
    frames++;
    return 1;
}

static void shadow_put(int i, double value) {
    if (i >= 0 && i < SHADOW_SLOTS) slot[i] = value;
}

static double shadow_get(int i) { return i >= 0 && i < SHADOW_SLOTS ? slot[i] : 0; }

// After Heaps fitted the light camera: 0 leaves its fit, else the camera to use is in the slots (1 held, 2 new).
static int shadow_fit(void) {
    if (!active) return 0;
    double margin = wt_live[LIVE_SHADOW_MARGIN].value / 100;
    margin = margin < 0 ? 0 : margin > 0.5 ? 0.5 : margin;
    if (margin != last_margin) fit.valid = false, last_margin = margin;
    ShadowCam in, out;
    memcpy(in.pos, slot + SHADOW_POS, sizeof in.pos);
    memcpy(in.target, slot + SHADOW_TARGET, sizeof in.target);
    memcpy(in.box, slot + SHADOW_BOX, sizeof in.box);
    int r = shadow_fit_decide(&fit, &in, slot + SHADOW_ROT, margin, &out);
    if (!r) {
        active = false;
        statics.valid = false;
        return 0;
    }
    if (r == 2) refit = true, refits++;
    memcpy(slot + SHADOW_POS, out.pos, sizeof out.pos);
    memcpy(slot + SHADOW_TARGET, out.target, sizeof out.target);
    memcpy(slot + SHADOW_BOX, out.box, sizeof out.box);
    return r;
}

// A check run for static meshes that keep moving: 3 redraws for moved casters within 120 frames, once per 600.
static void note_moved(void) {
    moved_frames[0] = moved_frames[1], moved_frames[1] = moved_frames[2], moved_frames[2] = last_frame;
    if (moved_frames[0] != INT_MIN && (long long)moved_frames[2] - moved_frames[0] < 120 &&
        (long long)last_frame - last_check_frame > 600) {
        check_pending = true;
        last_check_frame = last_frame;
    }
}

// 0: run the frame as the game does; 1: the static map holds; 2: draw it again first.
static int shadow_plan(int new_texture) {
    if (!active) return 0;
    bool moved;
    int r = shadow_plan_decide(&statics, new_texture != 0, refit, (int)slot[SHADOW_COUNT], slot[SHADOW_SIG], &moved);
    if (moved) note_moved();
    if (r == 2) drawn++;
    refit = false;
    return r;
}

static bool shadow_flag(int which) {
    switch (which) {
    case SHADOW_ACTIVE: return active;
    case SHADOW_KEEP: return active && keep;
    case SHADOW_KEEP_TAKE: {
        bool k = active && keep;
        keep = false;
        return k;
    }
    default: return false;
    }
}

static void shadow_set(int which, int value) {
    if (which == SHADOW_ACTIVE) {
        active = value != 0;
        if (!active) keep = false;
    } else if (which == SHADOW_KEEP) {
        keep = active && value;
    } else if (which == SHADOW_COPY_FAILED) {
        wt_log("shadowcache: turned off, the static map could not be copied into the shadow map");
        failed = true;
        done_battle = frame_battle;
        active = keep = false;
        forget();
    }
}

int wt_shadow_stats(char *out, int size) {
    if (!switched_on()) return snprintf(out, size, "off");
    if (failed) return snprintf(out, size, "off after an error (wtfix.log)");
    if (!setup_on || !frames) return snprintf(out, size, "waiting for a battle");
    return snprintf(out, size, "static map redrawn in %.1f%% of %llu frames (%llu new cameras), %d static draws, %llu mover checks",
                    100.0 * (double)drawn / (double)frames, frames, refits, statics.count, checks);
}

#define HL_PRIM(name) \
    __declspec(dllexport) void *hlp_##name(const char **sign) { *sign = NULL; return (void *)name; }
HL_PRIM(shadow_due)
HL_PRIM(shadow_source)
HL_PRIM(shadow_on)
HL_PRIM(shadow_check)
HL_PRIM(shadow_state_get)
HL_PRIM(shadow_state_set)
HL_PRIM(shadow_done)
HL_PRIM(shadow_error)
HL_PRIM(shadow_begin)
HL_PRIM(shadow_put)
HL_PRIM(shadow_get)
HL_PRIM(shadow_fit)
HL_PRIM(shadow_plan)
HL_PRIM(shadow_flag)
HL_PRIM(shadow_set)
