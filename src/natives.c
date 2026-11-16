// Natives for the patched bytecode. A NULL signature skips the VM's check: C types must match the fix's HL types.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

__declspec(dllimport) unsigned char *hl_alloc_bytes(int size);
__declspec(dllimport) void hl_blocking(bool b);

#define HL_PRIM(name) \
    __declspec(dllexport) void *hlp_##name(const char **sign) { *sign = NULL; return (void *)name; }

static int read_at(HANDLE h, long long pos, void *buf, DWORD n) {
    LARGE_INTEGER at;
    at.QuadPart = pos;
    DWORD got;
    return SetFilePointerEx(h, at, NULL, FILE_BEGIN) && ReadFile(h, buf, n, &got, NULL) && got == n;
}

#define HEAD_READ 65536
#define HEAD_CHUNKS 16

// Where a save's last chunk starts, or 0. Capped: a file of zeros reads as millions of empty chunks.
static long long last_chunk(const unsigned char *head, long long have, long long total) {
    long long pos = 4;
    for (int chunks = 0; chunks < HEAD_CHUNKS && pos + 4 <= have; chunks++) {
        int n;
        memcpy(&n, head + pos, 4);
        if (n < 0 || pos + 4 + n > total) return 0;
        if (pos + 4 + n == total) return chunks >= 1 ? pos : 0;
        pos += 4 + n;
    }
    return 0;
}

// std@file_contents for savelist: a save is "WTA" then length-prefixed chunks; the last (game state) comes back empty.
// A file without the magic comes back as its first 4 bytes, which readSave rejects at once.
static unsigned char *file_head(unsigned char *path, int *size) {
    hl_blocking(true);
    HANDLE h = CreateFileW((const wchar_t *)path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER total;
    if (h == INVALID_HANDLE_VALUE || !GetFileSizeEx(h, &total) || total.QuadPart > 0x7FFFFFFF) {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        hl_blocking(false);
        return NULL;
    }
    long long have = total.QuadPart < HEAD_READ ? total.QuadPart : HEAD_READ;
    unsigned char *head = (unsigned char *)malloc(HEAD_READ);
    int got = head && read_at(h, 0, head, (DWORD)have);
    long long keep = got ? last_chunk(head, have, total.QuadPart) : 0;
    if (keep > have) keep = 0;
    int foreign = got && have >= 4 && memcmp(head, "WTA", 3);
    int length = foreign ? 4 : keep ? (int)keep + 4 : (int)total.QuadPart;
    hl_blocking(false);
    unsigned char *out = hl_alloc_bytes(length);
    hl_blocking(true);
    int ok = 1;
    if (foreign) {
        memcpy(out, head, 4);
    } else if (keep) {
        memcpy(out, head, keep);
        memset(out + keep, 0, 4);
    } else {
        ok = read_at(h, 0, out, (DWORD)total.QuadPart);
    }
    free(head);
    CloseHandle(h);
    hl_blocking(false);
    if (!ok) return NULL;
    *size = length;
    return out;
}
HL_PRIM(file_head)

// splinecopy: Spline.copy marks its other.save() (nesting counts); Spline.save asks while saving points.
static volatile long spline_copies;
static void spline_copy_mode(bool on) {
    if (on) InterlockedIncrement(&spline_copies);
    else if (spline_copies > 0) InterlockedDecrement(&spline_copies);
}
static bool spline_copying(void) { return spline_copies > 0; }
HL_PRIM(spline_copy_mode)
HL_PRIM(spline_copying)

void wt_log(const char *format, ...);

// friendlyview: the frame the view turns off must close the HUDs and outlines it opened.
static bool friendly_shown;
static bool friendly_was(bool want) {
    bool was = friendly_shown;
    friendly_shown = want;
    return was;
}
__declspec(dllimport) unsigned short *hl_to_string(void *v);

static int unit_errors, frame_errors;
static void log_error(const char *where, void *exc, int *n) {
    if ((*n)++ >= 3) return;
    char text[600] = "?";
    unsigned short *s = exc ? hl_to_string(exc) : NULL;
    if (s) WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)s, -1, text, sizeof text, NULL, NULL);
    wt_log("friendlyview: %s threw: %s", where, text);
}
static void friendly_unit_error(void *exc) { log_error("a unit", exc, &unit_errors); }
static void friendly_frame_error(void *exc) { log_error("the frame", exc, &frame_errors); }
HL_PRIM(friendly_unit_error)
HL_PRIM(friendly_frame_error)

// 4: a game call threw.
static bool friendly_threw;
static void friendly_count(int what) {
    if (what == 4 && !friendly_threw)
        friendly_threw = true, wt_log("friendlyview: a game call threw; the view skipped that unit or frame");
}
HL_PRIM(friendly_was)
HL_PRIM(friendly_count)

// dlccheck: ent.BasePlayer.globalUpdate's DLC check, at most once a second, the first call at once.
static long long dlc_next;
static bool dlc_due(void) {
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    if (now.QuadPart < dlc_next) return false;
    QueryPerformanceFrequency(&freq);
    dlc_next = now.QuadPart + freq.QuadPart;
    return true;
}
HL_PRIM(dlc_due)
