// winmm.dll proxy: libhl.dll imports it, so it loads before main. Forwards every export and serves the VM's boot file
// reads from wtfix.hdll's patcher; on any failure, or without wtfix.hdll, the VM gets the file as it is.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "exports.h"

#define DECLARE_NAMED(name, ordinal) void *real_##name;
#define DECLARE_UNNAMED(ordinal) void *real_ord_##ordinal;
WINMM_EXPORTS(DECLARE_NAMED, DECLARE_UNNAMED)

#define STUB_NAMED(name, ordinal) \
    __attribute__((naked)) void proxy_##name(void) { __asm__("jmp *real_" #name "(%rip)"); }
#define STUB_UNNAMED(ordinal) \
    __attribute__((naked)) void proxy_ord_##ordinal(void) { __asm__("jmp *real_ord_" #ordinal "(%rip)"); }
WINMM_EXPORTS(STUB_NAMED, STUB_UNNAMED)

static wchar_t folder[MAX_PATH];

static void log_line(const char *format, ...) {
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls\\wtfix", folder);
    CreateDirectoryW(path, NULL);
    swprintf(path, MAX_PATH, L"%ls\\wtfix\\proxy.log", folder);
    FILE *f = _wfopen(path, L"a");
    if (!f) return;
    va_list args;
    va_start(args, format);
    vfprintf(f, format, args);
    va_end(args);
    fputc('\n', f);
    fclose(f);
}

static void missing_export(void) {
    MessageBoxW(NULL, L"The game called a winmm function the system winmm.dll does not have. See wtfix\\proxy.log.",
                L"wtfix", MB_ICONERROR);
    ExitProcess(1);
}

// ---------------------------------------------------------------- the boot file

typedef void *(__cdecl *wfopen_t)(const wchar_t *, const wchar_t *);
typedef int(__cdecl *fseek_t)(void *, long, int);
typedef long(__cdecl *ftell_t)(void *);
typedef size_t(__cdecl *fread_t)(void *, size_t, size_t, void *);
typedef int(__cdecl *fclose_t)(void *);
static wfopen_t real_wfopen;
static fseek_t real_fseek;
static ftell_t real_ftell;
static fread_t real_fread;
static fclose_t real_fclose;

typedef int (*patch_t)(const unsigned char *, size_t, unsigned char **, size_t *);

// Unlocked: the VM opens its boot file twice (a check, then the load) on its main thread before other threads exist.
static struct {
    void *file;
    size_t pos;
    int checked; // 1: served from boot_bytes, -1: passed through
    wchar_t path[MAX_PATH];
} open_boot[4];
static unsigned char *boot_bytes;
static size_t boot_size;
static wchar_t boot_path[MAX_PATH];

static int tracked(void *f) {
    for (int i = 0; i < 4; i++)
        if (f && open_boot[i].file == f) return i;
    return -1;
}

static void decide(int k) {
    void *f = open_boot[k].file;
    if (boot_bytes) {
        open_boot[k].checked = _wcsicmp(open_boot[k].path, boot_path) ? -1 : 1;
        return;
    }
    real_fseek(f, 0, SEEK_END);
    long size = real_ftell(f);
    real_fseek(f, 0, SEEK_SET);
    unsigned char *data = size > 3 ? (unsigned char *)malloc(size) : NULL;
    if (!data || real_fread(data, 1, size, f) != (size_t)size || memcmp(data, "HLB", 3)) {
        free(data);
        real_fseek(f, 0, SEEK_SET);
        open_boot[k].checked = -1;
        return;
    }
    boot_bytes = data;
    boot_size = size;
    wcscpy(boot_path, open_boot[k].path);
    open_boot[k].checked = 1;
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls\\wtfix.hdll", folder);
    HMODULE fix = LoadLibraryW(path);
    patch_t patch = fix ? (patch_t)(void *)GetProcAddress(fix, "wtfix_patch_boot") : NULL;
    if (!patch) {
        log_line("no wtfix.hdll (error %lu): running the game unpatched", GetLastError());
        return;
    }
    unsigned char *out = NULL;
    size_t out_size = 0;
    if (patch(data, size, &out, &out_size) > 0 && out) {
        free(boot_bytes);
        boot_bytes = out;
        boot_size = out_size;
    }
}

static void *__cdecl hook_wfopen(const wchar_t *name, const wchar_t *mode) {
    void *f = real_wfopen(name, mode);
    if (f && mode && mode[0] == L'r' && wcslen(name) < MAX_PATH) {
        for (int i = 0; i < 4; i++)
            if (!open_boot[i].file) {
                open_boot[i].file = f;
                open_boot[i].pos = 0;
                open_boot[i].checked = 0;
                GetFullPathNameW(name, MAX_PATH, open_boot[i].path, NULL);
                break;
            }
    }
    return f;
}

static int ready(void *f) {
    int k = tracked(f);
    if (k < 0) return -1;
    if (!open_boot[k].checked) decide(k);
    return open_boot[k].checked > 0 ? k : -1;
}

static int __cdecl hook_fseek(void *f, long offset, int origin) {
    int k = ready(f);
    if (k < 0) return real_fseek(f, offset, origin);
    long base = origin == SEEK_SET ? 0 : origin == SEEK_CUR ? (long)open_boot[k].pos : (long)boot_size;
    if (base + offset < 0) return -1;
    open_boot[k].pos = base + offset;
    return 0;
}

static long __cdecl hook_ftell(void *f) {
    int k = ready(f);
    return k < 0 ? real_ftell(f) : (long)open_boot[k].pos;
}

static size_t __cdecl hook_fread(void *buf, size_t size, size_t count, void *f) {
    int k = ready(f);
    if (k < 0) return real_fread(buf, size, count, f);
    if (!size) return 0;
    size_t pos = open_boot[k].pos, left = pos < boot_size ? boot_size - pos : 0, n = count;
    if (n > left / size) n = left / size;
    memcpy(buf, boot_bytes + pos, n * size);
    open_boot[k].pos = pos + n * size;
    return n;
}

static int __cdecl hook_fclose(void *f) {
    int k = tracked(f);
    if (k >= 0) open_boot[k].file = NULL;
    return real_fclose(f);
}

// All five or none: a VM reading the patched size through one hook and the file through another would load garbage.
static int hook_imports(HMODULE module) {
    unsigned char *base = (unsigned char *)module;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    ULONG_PTR *slot[5] = {0};
    void *hooks[5] = {(void *)hook_wfopen, (void *)hook_fseek, (void *)hook_ftell, (void *)hook_fread, (void *)hook_fclose};
    void **reals[5] = {(void **)&real_wfopen, (void **)&real_fseek, (void **)&real_ftell, (void **)&real_fread,
                       (void **)&real_fclose};
    const char *names_wanted[5] = {"_wfopen", "fseek", "ftell", "fread", "fclose"};
    int vm = 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++)
        if (!_stricmp((char *)base + d->Name, "libhl.dll")) vm = 1;
    if (!vm) return -1; // libhl loaded by another program: forward only
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++) {
        if (_stricmp((char *)base + d->Name, "api-ms-win-crt-stdio-l1-1-0.dll")) continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + d->OriginalFirstThunk);
        IMAGE_THUNK_DATA *slots = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const char *n = (char *)((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name;
            for (int i = 0; i < 5; i++)
                if (!strcmp(n, names_wanted[i])) slot[i] = (ULONG_PTR *)&slots->u1.Function;
        }
    }
    for (int i = 0; i < 5; i++)
        if (!slot[i]) return 0;
    DWORD old[5];
    for (int i = 0; i < 5; i++) {
        if (!VirtualProtect(slot[i], sizeof(void *), PAGE_READWRITE, &old[i])) {
            while (i--) VirtualProtect(slot[i], sizeof(void *), old[i], &old[i]);
            return 0;
        }
    }
    for (int i = 0; i < 5; i++) {
        *reals[i] = (void *)*slot[i];
        *slot[i] = (ULONG_PTR)hooks[i];
    }
    for (int i = 0; i < 5; i++) VirtualProtect(slot[i], sizeof(void *), old[i], &old[i]);
    return 5;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(instance);
    GetModuleFileNameW(instance, folder, MAX_PATH);
    *wcsrchr(folder, L'\\') = 0;
    wchar_t path[MAX_PATH];
    GetSystemDirectoryW(path, MAX_PATH);
    wcscat(path, L"\\winmm.dll");
    HMODULE system = LoadLibraryW(path);
    if (!system) {
        log_line("cannot load %ls: error %lu", path, GetLastError());
        return FALSE;
    }
#define RESOLVE_NAMED(name, ordinal)                              \
    if (!(real_##name = (void *)GetProcAddress(system, #name))) { \
        real_##name = (void *)missing_export;                     \
        log_line("the system winmm.dll has no %s", #name);        \
    }
#define RESOLVE_UNNAMED(ordinal)                                                                 \
    if (!(real_ord_##ordinal = (void *)GetProcAddress(system, (LPCSTR)(ULONG_PTR)(ordinal)))) { \
        real_ord_##ordinal = (void *)missing_export;                                             \
        log_line("the system winmm.dll has no ordinal %d", ordinal);                             \
    }
    WINMM_EXPORTS(RESOLVE_NAMED, RESOLVE_UNNAMED)
    // Only in the game, whose exe imports the HashLink runtime.
    if (GetModuleHandleW(L"libhl.dll")) {
        int hooked = hook_imports(GetModuleHandleW(NULL));
        if (hooked >= 0 && hooked != 5) log_line("hooked %d of the VM's 5 file imports: running the game unpatched", hooked);
    }
    return TRUE;
}
