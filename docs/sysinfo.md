# System information

Machine-wide statistics over `SYS_SYSINFO`. Include `user_sysinfo.h`.
Every call is allowed for user programs, returns a `uint64_t`, and returns
`SYS_ERR_INVAL` for an unknown query.

**Example** - a one-line status bar:

```c
#include "user_sysinfo.h"
#include "stdio.h"

int main(void) {
    printf("cpu %lu%%  ram %lu/%lu MiB  %lu cores  %lu procs  up %lu s\n",
           (unsigned long)mos_sysinfo_cpu_usage(),
           (unsigned long)(mos_sysinfo_ram_used() >> 20),
           (unsigned long)(mos_sysinfo_ram_total() >> 20),
           (unsigned long)mos_sysinfo_core_count(),
           (unsigned long)mos_sysinfo_process_count(),
           (unsigned long)(mos_sysinfo_uptime_ms() / 1000));
    return 0;
}
```

## `mos_sysinfo_cpu_usage`

```c
uint64_t mos_sysinfo_cpu_usage(void);
```

**Returns** average CPU usage across all online cores, in percent (0-100).

## `mos_sysinfo_core_usage`

```c
uint64_t mos_sysinfo_core_usage(uint32_t core_id);
```

**Returns** the usage of one core in percent, or `SYS_ERR_INVAL` if
`core_id` is out of range or that core is not online.

**Example**

```c
uint64_t cores = mos_sysinfo_core_count();
for (uint32_t i = 0; i < cores; i++)
    printf("core %u: %lu%%\n", i, (unsigned long)mos_sysinfo_core_usage(i));
```

## `mos_sysinfo_core_count`

```c
uint64_t mos_sysinfo_core_count(void);
```

**Returns** the number of online cores.

## `mos_sysinfo_ram_total`

```c
uint64_t mos_sysinfo_ram_total(void);
```

**Returns** total RAM in bytes.

## `mos_sysinfo_ram_used`

```c
uint64_t mos_sysinfo_ram_used(void);
```

**Returns** RAM in use in bytes: kernel heap in use plus physical pages in
use. An approximation.

## `mos_sysinfo_ram_free`

```c
uint64_t mos_sysinfo_ram_free(void);
```

**Returns** `ram_total - ram_used`, or 0 if the estimate of used memory
exceeds the total.

## `mos_sysinfo_heap_used`

```c
uint64_t mos_sysinfo_heap_used(void);
```

**Returns** bytes allocated from the kernel heap, which also backs every
program's `malloc`.

## `mos_sysinfo_heap_free`

```c
uint64_t mos_sysinfo_heap_free(void);
```

**Returns** free bytes in the kernel heap.

## `mos_sysinfo_pmm_used_pages`

```c
uint64_t mos_sysinfo_pmm_used_pages(void);
```

**Returns** physical 4 KiB pages in use.

## `mos_sysinfo_pmm_free_pages`

```c
uint64_t mos_sysinfo_pmm_free_pages(void);
```

**Returns** free physical 4 KiB pages.

## `mos_sysinfo_uptime_ms`

```c
uint64_t mos_sysinfo_uptime_ms(void);
```

**Returns** milliseconds since boot, the same value as
[`mos_uptime`](process.md#mos_uptime).

## `mos_sysinfo_process_count`

```c
uint64_t mos_sysinfo_process_count(void);
```

**Returns** the number of processes. To list them use
[`mos_pslist`](process.md#mos_pslist).

## `mos_cpu_has`

```c
int mos_cpu_has(uint64_t mask);
```

**Returns** 1 when every `SYS_CPU_*` feature in `mask` is usable, else 0.
The AVX-family bits (`SYS_CPU_AVX`, `SYS_CPU_AVX2`, `SYS_CPU_FMA`,
`SYS_CPU_F16C`, `SYS_CPU_AVX512F` and the other AVX-512 bits) are set only
when the kernel enabled the register state (`CR4.OSXSAVE` and `XCR0`), so
"1" means the instructions will not fault. They are 0 on a CPU without
XSAVE/AVX and when the kernel was booted with the `noavx` flag, even though
CPUID would still list them. Always test before running VEX or EVEX code;
without the bit the instruction raises #UD and ends the program.

The other bits are CPUID reports: `SYS_CPU_SSE3`, `SSSE3`, `SSE41`, `SSE42`,
`POPCNT`, `BMI1`, `BMI2`, `LZCNT`, `AES`, `PCLMUL`, `MOVBE`, `RDRAND`,
`RDSEED`, `ADX`, `SHA`. SSE2 is always there (x86-64).

**Example** - choosing a code path:

```c
#include "user_sysinfo.h"

if (mos_cpu_has(SYS_CPU_AVX2 | SYS_CPU_FMA))
    render_avx2();          /* compiled with __attribute__((target("avx2,fma"))) */
else
    render_sse2();
```

Compile only the functions that use AVX with `__attribute__((target("avx2")))`
(or `-mavx2` for a separate file); the rest of the program must stay SSE-only
so it runs everywhere. The kernel saves and restores all YMM/ZMM state around
every system call, interrupt and context switch, so no extra care is needed
between threads.

## `mos_sysinfo_cpu_features`, `mos_sysinfo_xcr0`

```c
uint64_t mos_sysinfo_cpu_features(void);   /* SYS_SYSINFO_CPU_FEATURES, 0x0C */
uint64_t mos_sysinfo_xcr0(void);           /* SYS_SYSINFO_XCR0, 0x0D */
```

The raw feature mask (see `mos_cpu_has`) and the `XCR0` value the kernel runs
with: 0 without XSAVE/AVX, 7 for x87+SSE+AVX, 0xE7 with AVX-512.

## `mos_sysinfo`

```c
uint64_t mos_sysinfo(uint64_t op);
```

Issues `SYS_SYSINFO` with a `SYS_SYSINFO_*` query, or
`SYS_SYSINFO_CORE_USAGE_BASE + core_id` for one core. The functions above
wrap it.
