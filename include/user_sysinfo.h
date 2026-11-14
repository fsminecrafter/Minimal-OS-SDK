#ifndef MINIMALOS_X86_64_USER_SYSINFO_H
#define MINIMALOS_X86_64_USER_SYSINFO_H

#include <stdint.h>
#include "x86_64/user_syscalls.h"

// All of these return SYS_ERR_INVAL ((uint64_t)-4) on failure -
// mos_sysinfo_core_usage() in particular, for a core_id that's out of
// range or not currently online.

static inline uint64_t mos_sysinfo_cpu_usage(void)      { return mos_sysinfo(SYS_SYSINFO_CPU_USAGE); }
static inline uint64_t mos_sysinfo_ram_total(void)       { return mos_sysinfo(SYS_SYSINFO_RAM_TOTAL); }
static inline uint64_t mos_sysinfo_ram_used(void)        { return mos_sysinfo(SYS_SYSINFO_RAM_USED); }
static inline uint64_t mos_sysinfo_ram_free(void)        { return mos_sysinfo(SYS_SYSINFO_RAM_FREE); }
static inline uint64_t mos_sysinfo_uptime_ms(void)       { return mos_sysinfo(SYS_SYSINFO_UPTIME_MS); }
static inline uint64_t mos_sysinfo_process_count(void)   { return mos_sysinfo(SYS_SYSINFO_PROCESS_COUNT); }
static inline uint64_t mos_sysinfo_core_count(void)      { return mos_sysinfo(SYS_SYSINFO_CORE_COUNT); }
static inline uint64_t mos_sysinfo_heap_used(void)       { return mos_sysinfo(SYS_SYSINFO_HEAP_USED); }
static inline uint64_t mos_sysinfo_heap_free(void)       { return mos_sysinfo(SYS_SYSINFO_HEAP_FREE); }
static inline uint64_t mos_sysinfo_pmm_used_pages(void)  { return mos_sysinfo(SYS_SYSINFO_PMM_USED_PAGES); }
static inline uint64_t mos_sysinfo_pmm_free_pages(void)  { return mos_sysinfo(SYS_SYSINFO_PMM_FREE_PAGES); }

/* CPU features (SYS_CPU_* in syscall.h). The AVX family is only reported when
 * the OS enabled the state, so a set bit means the instructions are usable. */
static inline uint64_t mos_sysinfo_cpu_features(void) { return mos_sysinfo(SYS_SYSINFO_CPU_FEATURES); }
static inline uint64_t mos_sysinfo_xcr0(void)         { return mos_sysinfo(SYS_SYSINFO_XCR0); }

/* 1 when every feature in `mask` (SYS_CPU_* bits or'ed together) is usable. */
static inline int mos_cpu_has(uint64_t mask) {
    uint64_t f = mos_sysinfo_cpu_features();
    return f != SYS_ERR_INVAL && (f & mask) == mask;
}

static inline uint64_t mos_sysinfo_core_usage(uint32_t core_id) {
    return mos_sysinfo(SYS_SYSINFO_CORE_USAGE_BASE + core_id);
}

#endif
