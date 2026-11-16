// gcload's native half: libhl runs a full collection when allocations since the last pass t x the heap (or t x its
// blocks), t = 0.2, a read-only constant loaded once into xmm2. Its load is pointed at our float in .data slack (in rip
// range). The test fires after t/(1-t) x live data and never at t >= 1 (unbounded growth), hence the 0.9 cap.
#include "sigscan.h"
#include "fix.h"
#include <stdbool.h>
#include <string.h>

// movss xmm2,[t]; then both tests: allocated bytes vs heap size, allocation count vs blocks.
static const char *TRIGGER = "F3 0F 10 15 ?? ?? ?? ?? F3 48 0F 2A C8 F3 0F 59 C2 0F 2F C8 77 ?? 48 8B 05 ?? ?? ?? ?? "
                             "0F 57 C9 48 2B 05 ?? ?? ?? ?? 66 0F 6E 05 ?? ?? ?? ?? 0F 5B C0 F3 48 0F 2A C8 F3 0F 59 C2 "
                             "0F 2F C8 77";

static volatile float *threshold;
static float normal, loading;
static volatile long depth;

int wt_gc_install(float normal_threshold, float load_threshold) {
    HMODULE hl = GetModuleHandleW(L"libhl.dll");
    WtRange text;
    unsigned char *hit;
    if (!hl || wt_section(hl, ".text", &text)) return wt_log("gc: no libhl.dll loaded"), -1;
    int n = wt_find(&text, TRIGGER, &hit);
    if (n != 1) return wt_log("gc: the trigger's code matched %d times in libhl.dll (want 1): this build differs", n), -1;
    float *constant = (float *)wt_rip_target(hit + 4, hit + 8);
    WtRange rdata;
    if (wt_section(hl, ".rdata", &rdata) || (unsigned char *)constant < rdata.start ||
        (unsigned char *)constant + 4 > rdata.start + rdata.size || *constant != 0.2f)
        return wt_log("gc: the trigger reads %p, not libhl's 0.2 constant", (void *)constant), -1;
    float *slot = (float *)wt_section_slack(hl, ".data", 16);
    if (!slot) return wt_log("gc: no free bytes after libhl's .data"), -1;
    long long disp = (unsigned char *)slot - (hit + 8);
    if (disp != (int)disp) return wt_log("gc: slot out of reach"), -1;
    normal = normal_threshold > 0.9f ? 0.9f : normal_threshold;
    loading = load_threshold > 0.9f ? 0.9f : load_threshold < normal ? normal : load_threshold;
    *slot = normal;
    int d = (int)disp;
    if (wt_write(hit + 4, &d, 4)) return wt_log("gc: could not patch the trigger"), -1;
    threshold = slot;
    wt_log("gc: trigger at libhl+0x%llx now reads libhl+0x%llx (%.2f, %.2f while loading)",
           (unsigned long long)(hit - (unsigned char *)hl), (unsigned long long)((unsigned char *)slot - (unsigned char *)hl),
           normal, loading);
    return 0;
}

int wt_gc_installed(void) { return threshold != NULL; }

// Nests: Game.start runs switchToMode.
static void gc_load(bool on) {
    if (!threshold) return;
    if (on) {
        if (InterlockedIncrement(&depth) == 1) *threshold = loading;
    } else if (depth > 0 && InterlockedDecrement(&depth) == 0) {
        *threshold = normal;
    }
}

__declspec(dllexport) void *hlp_gc_load(const char **sign) {
    *sign = NULL;
    return (void *)gc_load;
}
