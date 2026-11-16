// wtfix.hdll: patches the bytecode for the winmm.dll proxy and holds the natives the patched code binds (lib "wtfix").
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fix.h"
#include "live.h"

// folder: wtfix.hdll's own; root: the wtfix folder (wtfix.ini, logs\), folder's parent when that is ...\wtfix\dll.
static wchar_t folder[MAX_PATH], root[MAX_PATH];

const wchar_t *wt_root(void) { return root; }
static FILE *log_file;

static void to_file(const char *line) {
    if (!log_file) {
        wchar_t path[MAX_PATH];
        swprintf(path, MAX_PATH, L"%ls\\logs", root);
        CreateDirectoryW(path, NULL);
        swprintf(path, MAX_PATH, L"%ls\\logs\\wtfix.log", root);
        log_file = _wfopen(path, L"w");
        if (!log_file) return;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(log_file, "%02u:%02u:%02u.%03u %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, line);
    fflush(log_file);
}

static int setting(const char *section, const char *key, int fallback) {
    wchar_t ini[MAX_PATH], s[64], k[64];
    swprintf(ini, MAX_PATH, L"%ls\\wtfix.ini", root);
    swprintf(s, 64, L"%hs", section);
    swprintf(k, 64, L"%hs", key);
    return (int)GetPrivateProfileIntW(s, k, fallback, ini);
}

static int enabled(const char *fix) { return setting("fixes", fix, 1); }

static float decimal(const char *section, const char *key, float fallback) {
    wchar_t ini[MAX_PATH], s[64], k[64], v[64];
    swprintf(ini, MAX_PATH, L"%ls\\wtfix.ini", root);
    swprintf(s, 64, L"%hs", section);
    swprintf(k, 64, L"%hs", key);
    GetPrivateProfileStringW(s, k, L"", v, 64, ini);
    wchar_t *end;
    double d = wcstod(v, &end);
    return end != v && d > 0 && d < 100 ? (float)d : fallback;
}

static double number(const char *section, const char *key, double fallback) {
    wchar_t ini[MAX_PATH];
    swprintf(ini, MAX_PATH, L"%ls\\wtfix.ini", root);
    return wt_ini_number(ini, section, key, fallback);
}

int wt_gc_install(float normal_threshold, float load_threshold);
int wt_gc_installed(void);
static int native_ready(const char *feature) { return !strcmp(feature, "gc") && wt_gc_installed(); }

uint64_t wt_cpu_pin(const char *die);

__declspec(dllexport) int wtfix_patch_boot(const unsigned char *in, size_t size, unsigned char **out, size_t *out_size) {
    wt_log_sink = to_file;
    if (wt_boot_opengl(in, size)) {
        wt_log("Wartales was started with OpenGL; wtfix supports DirectX 11 only: the game was closed");
        MessageBoxW(NULL,
                    L"Wartales was started in OpenGL mode, which wtfix does not support.\n\n"
                    L"Start it again with \"Wartales (DirectX)\" in Steam's launch options, or uninstall wtfix "
                    L"(delete winmm.dll from the game folder) to play with OpenGL.",
                    L"wtfix", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        ExitProcess(1);
    }
    wt_native_ready = native_ready;
    wt_number = number;
    wchar_t ini[MAX_PATH], die[64];
    swprintf(ini, MAX_PATH, L"%ls\\wtfix.ini", root);
    GetPrivateProfileStringW(L"cpu", L"die", L"auto", die, 64, ini);
    char die_text[3 * 64] = "";
    WideCharToMultiByte(CP_UTF8, 0, die, -1, die_text, sizeof die_text, NULL, NULL);
    wt_cpu_pin(die_text);
    if (enabled("gcload")) wt_gc_install(decimal("gc", "threshold", 0.2f), decimal("gc", "load_threshold", 0.5f));
    if (enabled("consts")) wt_read_consts(ini);
    wt_live_load(ini);
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    int applied = wt_patch(in, size, enabled, out, out_size);
    QueryPerformanceCounter(&t1);
    wt_log("patch took %.0f ms", 1000.0 * (t1.QuadPart - t0.QuadPart) / freq.QuadPart);
    return applied;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        GetModuleFileNameW(instance, folder, MAX_PATH);
        *wcsrchr(folder, L'\\') = 0;
        wcscpy(root, folder);
        wchar_t *last = wcsrchr(root, L'\\');
        if (last && !_wcsicmp(last + 1, L"dll")) *last = 0;
    }
    return TRUE;
}
