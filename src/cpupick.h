// The [cpu] die rules, apart from Windows calls: they read what Windows reports per CPU, never a CPU model.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CPU_MAX 64
#define CPU_MIN 10
#define CPU_TEXT(x) #x
#define CPU_NUMBER_TEXT(x) CPU_TEXT(x)
#define CPU_DIE_AUTO -1
#define CPU_DIE_OFF -2
#define CPU_DIE_BAD -3

typedef struct {
    uint8_t core_index, cache_index, efficiency_class, scheduling_class;
    uint32_t l3_bytes;
} CpuInfo;

typedef struct {
    uint64_t pinned;
    int group, first_cpu;
    const char *why_this_group, *why_not_pinned;
} CpuPin;

static inline uint64_t cpu_all(int count) {
    return count >= 64 ? ~0ull : (1ull << count) - 1;
}

static inline int cpu_same_core(const CpuInfo *cpus, int a, int b) {
    return cpus[a].core_index == cpus[b].core_index && cpus[a].cache_index == cpus[b].cache_index;
}

static inline uint64_t cpu_core_of(const CpuInfo *cpus, int count, int cpu) {
    uint64_t mask = 0;
    for (int c = 0; c < count; c++)
        if (cpu_same_core(cpus, c, cpu)) mask |= 1ull << c;
    return mask;
}

static inline uint64_t cpu_one_per_core(const CpuInfo *cpus, int count, uint64_t mask) {
    uint64_t picked = 0;
    for (int c = 0; c < count; c++)
        if (mask >> c & 1 && !(picked & cpu_core_of(cpus, count, c))) picked |= 1ull << c;
    return picked;
}

static inline int cpu_count_of(uint64_t mask) {
    return __builtin_popcountll(mask);
}

// Type first: on a hybrid CPU the slower cores share the L3 too, or sit on a cache of their own.
static inline uint64_t cpu_group(const CpuInfo *cpus, int count, int cpu) {
    uint8_t fastest = 0;
    uint64_t mask = 0;
    for (int c = 0; c < count; c++)
        if (cpus[c].efficiency_class > fastest) fastest = cpus[c].efficiency_class;
    for (int c = 0; c < count; c++)
        if (cpus[c].efficiency_class == fastest && cpus[c].cache_index == cpus[cpu].cache_index) mask |= 1ull << c;
    return mask;
}

// Groups are numbered by their first CPU, so group 0 holds CPU 0 unless CPU 0 is a slower type.
static inline int cpu_groups(const CpuInfo *cpus, int count, int *firsts) {
    int groups = 0;
    for (int c = 0; c < count; c++) {
        uint64_t group = cpu_group(cpus, count, c);
        if (group && __builtin_ctzll(group) == c) firsts[groups++] = c;
    }
    return groups;
}

static inline int cpu_group_number(const CpuInfo *cpus, int count, int first_cpu) {
    int firsts[CPU_MAX] = {0}, groups = cpu_groups(cpus, count, firsts);
    for (int g = 0; g < groups; g++)
        if (firsts[g] == first_cpu) return g;
    return -1;
}

static inline uint8_t cpu_best_rank(const CpuInfo *cpus, int count, uint64_t mask) {
    uint8_t best = 0;
    for (int c = 0; c < count; c++)
        if (mask >> c & 1 && cpus[c].scheduling_class > best) best = cpus[c].scheduling_class;
    return best;
}

// Largest L3 before strongest core (SchedulingClass): an X3D's strongest cores are on its frequency die, where the
// game runs slower. One die only: the game has one busy main thread.
static inline int cpu_pick_group(const CpuInfo *cpus, int count, int number, const char **why) {
    int firsts[CPU_MAX] = {0}, groups = cpu_groups(cpus, count, firsts);
    if (number >= 0) {
        *why = "set in wtfix.ini";
        return number < groups ? firsts[number] : -1;
    }
    int best = firsts[0], sizes_differ = 0, ranks_differ = 0;
    uint8_t best_rank = cpu_best_rank(cpus, count, cpu_group(cpus, count, best));
    for (int g = 1; g < groups; g++) {
        uint32_t bytes = cpus[firsts[g]].l3_bytes;
        uint8_t rank = cpu_best_rank(cpus, count, cpu_group(cpus, count, firsts[g]));
        sizes_differ |= bytes != cpus[firsts[0]].l3_bytes;
        ranks_differ |= rank != best_rank;
        if (bytes > cpus[best].l3_bytes || (bytes == cpus[best].l3_bytes && rank > best_rank)) {
            best = firsts[g];
            best_rank = rank;
        }
    }
    *why = groups == 1 ? "the only one" : sizes_differ ? "the largest L3" : ranks_differ ? "the strongest core" : "the first of equal groups";
    return best;
}

static inline CpuPin cpu_pin(const CpuInfo *cpus, int count, int die) {
    CpuPin pin = {0, -1, -1, "", NULL};
    pin.first_cpu = cpu_pick_group(cpus, count, die, &pin.why_this_group);
    if (pin.first_cpu < 0) {
        pin.why_not_pinned = "this CPU has no such cache group";
        return pin;
    }
    pin.group = cpu_group_number(cpus, count, pin.first_cpu);
    uint64_t group = cpu_group(cpus, count, pin.first_cpu);
    if (group == cpu_all(count)) pin.why_not_pinned = "holds every CPU";
    else if (cpu_count_of(group) < CPU_MIN) pin.why_not_pinned = "has fewer than " CPU_NUMBER_TEXT(CPU_MIN) " CPUs";
    else pin.pinned = group;
    return pin;
}

// Trims text in place: the ini value may carry a ; comment.
static inline int cpu_parse_die(char *text) {
    char *comment = strchr(text, ';');
    if (comment) *comment = 0;
    size_t length = strlen(text);
    while (length && (text[length - 1] == ' ' || text[length - 1] == '\t')) text[--length] = 0;
    if (!strcmp(text, "auto")) return CPU_DIE_AUTO;
    if (!strcmp(text, "off")) return CPU_DIE_OFF;
    char *end;
    long number = strtol(text, &end, 10);
    return end != text && !*end && text[0] >= '0' && text[0] <= '9' && number < CPU_MAX ? (int)number : CPU_DIE_BAD;
}

static inline const char *cpu_list(uint64_t mask, char *text, size_t size) {
    size_t used = 0;
    text[0] = 0;
    for (int c = 0; c < 64 && used < size; c++) {
        if (!(mask >> c & 1)) continue;
        int last = c;
        while (last < 63 && mask >> (last + 1) & 1) last++;
        int written = last > c ? snprintf(text + used, size - used, "%s%d-%d", used ? "," : "", c, last) : snprintf(text + used, size - used, "%s%d", used ? "," : "", c);
        used += written > 0 ? (size_t)written : 0;
        c = last;
    }
    return text;
}

// For the log: the cache groups as die= numbers them, then any slower cores.
static inline const char *cpu_describe(const CpuInfo *cpus, int count, char *text, size_t size) {
    size_t used = 0;
    text[0] = 0;
    for (int c = 0; c < count && used + 1 < size; c++) {
        uint64_t set = 0;
        for (int o = 0; o < count; o++)
            if (cpus[o].cache_index == cpus[c].cache_index && cpus[o].efficiency_class == cpus[c].efficiency_class) set |= 1ull << o;
        if (__builtin_ctzll(set) != c) continue;
        int group = cpu_group(cpus, count, c) == set ? cpu_group_number(cpus, count, c) : -1;
        uint64_t cores = cpu_one_per_core(cpus, count, set);
        char name[32], list[160], ranks[4 * CPU_MAX + 1] = "";
        for (int o = 0; o < count; o++)
            if (cores >> o & 1) snprintf(ranks + strlen(ranks), sizeof ranks - strlen(ranks), " %u", cpus[o].scheduling_class);
        if (group >= 0) snprintf(name, sizeof name, "cache group %d", group);
        else snprintf(name, sizeof name, "slower cores (class %u)", cpus[c].efficiency_class);
        int written = snprintf(text + used, size - used, "%s%s: CPUs %s, %d cores, L3 %lu MB, ranks by core%s", used ? "; " : "", name,
                               cpu_list(set, list, sizeof list), cpu_count_of(cores), (unsigned long)(cpus[c].l3_bytes >> 20), ranks);
        used = written < 0 ? size : used + (size_t)written;
    }
    return text;
}
