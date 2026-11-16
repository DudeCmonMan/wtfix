#include "sigscan.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static IMAGE_SECTION_HEADER *section(HMODULE module, const char *name, IMAGE_NT_HEADERS **nt_out) {
    unsigned char *base = (unsigned char *)module;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    if (nt_out) *nt_out = nt;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (!strncmp((const char *)s[i].Name, name, IMAGE_SIZEOF_SHORT_NAME)) return &s[i];
    return NULL;
}

int wt_section(HMODULE module, const char *name, WtRange *out) {
    IMAGE_SECTION_HEADER *s = module ? section(module, name, NULL) : NULL;
    if (!s) return -1;
    out->start = (unsigned char *)module + s->VirtualAddress;
    out->size = s->Misc.VirtualSize;
    return 0;
}

static int parse(const char *pattern, unsigned char *bytes, unsigned char *mask, int max) {
    int n = 0;
    for (const char *p = pattern; *p;) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (n == max) return -1;
        if (p[0] == '?' && p[1] == '?') {
            bytes[n] = 0, mask[n++] = 0;
        } else {
            char hex[3] = {p[0], p[1], 0};
            char *end;
            long v = strtol(hex, &end, 16);
            if (end != hex + 2) return -1;
            bytes[n] = (unsigned char)v, mask[n++] = 0xFF;
        }
        p += 2;
    }
    return n;
}

int wt_find(const WtRange *range, const char *pattern, unsigned char **first) {
    unsigned char bytes[256], mask[256];
    int n = parse(pattern, bytes, mask, 256), count = 0;
    *first = NULL;
    if (n <= 0 || range->size < (size_t)n) return 0;
    for (size_t i = 0; i + n <= range->size; i++) {
        const unsigned char *p = range->start + i;
        if (p[0] != bytes[0] && mask[0]) continue;
        int k = 1;
        while (k < n && (p[k] & mask[k]) == bytes[k]) k++;
        if (k == n) {
            if (!count++) *first = (unsigned char *)p;
        }
    }
    return count;
}

unsigned char *wt_rip_target(const unsigned char *disp, const unsigned char *end) {
    int32_t d;
    memcpy(&d, disp, 4);
    return (unsigned char *)end + d;
}

int wt_write(void *at, const void *bytes, size_t n) {
    DWORD old;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) return -1;
    memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
    return 0;
}

unsigned char *wt_section_slack(HMODULE module, const char *name, size_t size) {
    IMAGE_NT_HEADERS *nt;
    IMAGE_SECTION_HEADER *s = section(module, name, &nt);
    if (!s) return NULL;
    size_t page = nt->OptionalHeader.SectionAlignment;
    size_t used = (s->Misc.VirtualSize + 15) & ~(size_t)15;
    size_t mapped = (s->Misc.VirtualSize + page - 1) / page * page;
    if (used + size > mapped) return NULL;
    unsigned char *p = (unsigned char *)module + s->VirtualAddress + used;
    for (size_t i = 0; i < size; i++)
        if (p[i]) return NULL;
    return p;
}
