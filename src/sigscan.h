// Byte-pattern signatures for native code: addresses that move between builds wildcarded, exactly one match required.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>

typedef struct {
    unsigned char *start;
    size_t size;
} WtRange;

int wt_section(HMODULE module, const char *name, WtRange *out); // 0 on success
// Pattern e.g. "F3 0F 10 15 ?? ?? ?? ??"; returns the match count, *first the first match.
int wt_find(const WtRange *range, const char *pattern, unsigned char **first);
// disp: the 32-bit displacement; end: the instruction's end.
unsigned char *wt_rip_target(const unsigned char *disp, const unsigned char *end);
int wt_write(void *at, const void *bytes, size_t n); // into code or read-only data; 0 on success
// Zeroed bytes after a section's data, in its last page, 16-aligned; NULL if none.
unsigned char *wt_section_slack(HMODULE module, const char *name, size_t size);
