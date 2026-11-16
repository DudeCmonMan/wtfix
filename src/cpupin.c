// [cpu] die: the game's CPUs, by cpupick.h's rules. A CPU set alone gives way to core parking and Game Mode, so the
// process also gets a hard affinity mask; both reach the threads already running.
#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include "cpupick.h"
#include "fix.h"

static CpuInfo cpus[CPU_MAX];
static ULONG set_ids[CPU_MAX];
static int cpu_count;

static const char *read_cpus(void) {
    ULONG size = 0;
    const char *error = NULL;
    GetSystemCpuSetInformation(NULL, 0, &size, GetCurrentProcess(), 0);
    BYTE *sets = malloc(size);
    if (!sets || !GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)sets, size, &size, GetCurrentProcess(), 0)) error = "GetSystemCpuSetInformation failed";
    for (BYTE *at = sets; !error && at < sets + size; at += ((PSYSTEM_CPU_SET_INFORMATION)at)->Size) {
        PSYSTEM_CPU_SET_INFORMATION set = (PSYSTEM_CPU_SET_INFORMATION)at;
        int cpu = set->CpuSet.LogicalProcessorIndex;
        if (set->CpuSet.Group || cpu >= CPU_MAX) {
            error = "more than one processor group (over 64 CPUs) is not supported";
            break;
        }
        set_ids[cpu] = set->CpuSet.Id;
        cpus[cpu] = (CpuInfo){set->CpuSet.CoreIndex, set->CpuSet.LastLevelCacheIndex, set->CpuSet.EfficiencyClass, set->CpuSet.SchedulingClass, 0};
        if (cpu >= cpu_count) cpu_count = cpu + 1;
    }
    free(sets);
    if (!error && !cpu_count) error = "Windows reported no CPU sets";
    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationCache, NULL, &length);
    BYTE *caches = malloc(length);
    if (!error && caches && GetLogicalProcessorInformationEx(RelationCache, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)caches, &length))
        for (BYTE *at = caches; at < caches + length; at += ((PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)at)->Size) {
            CACHE_RELATIONSHIP *relation = &((PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)at)->Cache;
            for (int cpu = 0; cpu < cpu_count; cpu++)
                if (relation->Level == 3 && !relation->GroupMask.Group && relation->GroupMask.Mask >> cpu & 1) cpus[cpu].l3_bytes = relation->CacheSize;
        }
    free(caches);
    return error;
}

uint64_t wt_cpu_pin(const char *setting) {
    char die_text[64], line[900], list[160];
    snprintf(die_text, sizeof die_text, "%s", setting);
    int die = cpu_parse_die(die_text);
    if (die == CPU_DIE_OFF) return wt_log("cpu: left to Windows (wtfix.ini [cpu] die=off)"), 0;
    if (die == CPU_DIE_BAD) return wt_log("cpu: left to Windows: wtfix.ini [cpu] die=%s is not auto, off or a cache group number", die_text), 0;
    const char *error = read_cpus();
    if (error) return wt_log("cpu: left to Windows: %s", error), 0;
    wt_log("cpu: %s", cpu_describe(cpus, cpu_count, line, sizeof line));
    CpuPin pin = cpu_pin(cpus, cpu_count, die);
    if (pin.first_cpu < 0) return wt_log("cpu: left to Windows: wtfix.ini [cpu] die=%d, but %s", die, pin.why_not_pinned), 0;
    const char *cpu_text = cpu_list(cpu_group(cpus, cpu_count, pin.first_cpu), list, sizeof list);
    unsigned long l3_mb = (unsigned long)(cpus[pin.first_cpu].l3_bytes >> 20);
    if (pin.why_not_pinned)
        return wt_log("cpu: left to Windows: cache group %d (CPUs %s, L3 %lu MB: %s) %s", pin.group, cpu_text, l3_mb, pin.why_this_group, pin.why_not_pinned), 0;
    ULONG ids[CPU_MAX], id_count = 0;
    for (int cpu = 0; cpu < cpu_count; cpu++)
        if (pin.pinned >> cpu & 1) ids[id_count++] = set_ids[cpu];
    if (!SetProcessDefaultCpuSets(GetCurrentProcess(), ids, id_count))
        return wt_log("cpu: left to Windows: setting CPUs %s failed (error %lu)", cpu_text, GetLastError()), 0;
    if (!SetProcessAffinityMask(GetCurrentProcess(), (DWORD_PTR)pin.pinned))
        return wt_log("cpu: CPUs %s preferred only (cache group %d): the hard affinity failed (error %lu)", cpu_text, pin.group,
                      GetLastError()), pin.pinned;
    wt_log("cpu: CPUs %s, hard affinity (cache group %d, L3 %lu MB: %s)", cpu_text, pin.group, l3_mb, pin.why_this_group);
    return pin.pinned;
}
