// mtop.cpp — a lightweight system/process monitor for MinimalOS.
//
// Build with the Minimal-OS SDK:
//   ./build.sh examples/mtop.cpp
// This produces mtop.run at the SDK root. Import it into MinimaFS
// (e.g. as 0:/programs/mtop.run) and run it with:
//   run 0:/programs/mtop.run
//
// Notes on the environment this runs in (freestanding userspace,
// no libc): no malloc, no <vector>/<string>, no snprintf. Everything
// here uses fixed-size stack/static buffers and a hand-rolled integer
// formatter instead.

#include "minimalos.h"
#include "stdio.h"

#define MAX_PROCS   128
#define REFRESH_MS  1000

static const char* state_name(uint8_t state) {
    switch (state) {
        case SYSCALL_PROC_STATE_READY:      return "READY";
        case SYSCALL_PROC_STATE_RUNNING:    return "RUN";
        case SYSCALL_PROC_STATE_WAITING:    return "WAIT";
        case SYSCALL_PROC_STATE_PAUSED:     return "PAUSE";
        case SYSCALL_PROC_STATE_ZOMBIE:     return "ZOMBIE";
        case SYSCALL_PROC_STATE_TERMINATED: return "TERM";
        default:                            return "?";
    }
}

static const char* priv_name(uint8_t priv) {
    return priv == SYSCALL_PROC_PRIV_USER ? "user" : "kernel";
}

static long sysinfo(long op) {
    return mos_syscall(SYS_SYSINFO, op, 0, 0);
}

// Clears the text terminal so each refresh redraws in place instead
// of scrolling forever. Built directly on mos_syscall/SYS_GRAPHICS
// since minimalos.h doesn't ship a graphics wrapper.
static void terminal_clear(void) {
    syscall_graphics_request_t request = {};
    request.op = SYS_GRAPHICS_TERMINAL_CLEAR;
    mos_syscall(SYS_GRAPHICS, (long)(uintptr_t)&request, 0, 0);
}

// Minimal unsigned-integer-to-string (no snprintf available here).
// Returns the number of characters written (not counting the NUL).
static int fmt_uint(char* out, unsigned long value) {
    char tmp[21];
    int n = 0;
    if (value == 0) {
        out[0] = '0';
        out[1] = '\0';
        return 1;
    }
    while (value > 0 && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (value % 10));
        value /= 10;
    }
    int len = 0;
    while (n > 0) out[len++] = tmp[--n];
    out[len] = '\0';
    return len;
}

// Writes `src` left-justified into a fixed-width field inside `out`,
// starting at `pos`, padding with spaces, followed by one separating
// space. Returns the new write position. Never reads past `width`
// bytes of `src`, so it's safe even on a non-NUL-terminated-looking
// (but always NUL-terminated) source.
static int put_field(char* out, int pos, const char* src, int width) {
    int i = 0;
    for (; i < width && src[i]; i++) out[pos++] = src[i];
    for (; i < width; i++) out[pos++] = ' ';
    out[pos++] = ' ';
    return pos;
}

static void print_bar(long percent, int width) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    int filled = (int)((percent * (long)width) / 100);

    printf("[");
    for (int i = 0; i < width; i++) {
        printf(i < filled ? "|" : " ");
    }
    printf("] ");
    printf("%ld", percent);
    printf("%%\n");
}

int main(void) {
    static syscall_process_info_t procs[MAX_PROCS];

    for (;;) {
        terminal_clear();

        long uptime_ms  = sysinfo(SYS_SYSINFO_UPTIME_MS);
        long cpu_percent = sysinfo(SYS_SYSINFO_CPU_USAGE);
        long core_count = sysinfo(SYS_SYSINFO_CORE_COUNT);
        long proc_count = sysinfo(SYS_SYSINFO_PROCESS_COUNT);
        long ram_total  = sysinfo(SYS_SYSINFO_RAM_TOTAL);
        long ram_used   = sysinfo(SYS_SYSINFO_RAM_USED);
        long heap_used  = sysinfo(SYS_SYSINFO_HEAP_USED);
        long heap_free  = sysinfo(SYS_SYSINFO_HEAP_FREE);
        long pmm_used   = sysinfo(SYS_SYSINFO_PMM_USED_PAGES);
        long pmm_free   = sysinfo(SYS_SYSINFO_PMM_FREE_PAGES);

        printf("MinimalOS mtop  -  uptime ");
        printf("%ld", uptime_ms / 1000);
        printf("s  -  ");
        printf("%ld", proc_count);
        printf(" process(es)  -  ");
        printf("%ld", core_count);
        printf(" core(s)\n\n");

        printf("CPU  ");
        print_bar(cpu_percent, 30);

        if (core_count > 0) {
            long shown = core_count;
            if (shown > 16) shown = 16; // MAX_CPUS
            for (long c = 0; c < shown; c++) {
                long usage = sysinfo(SYS_SYSINFO_CORE_USAGE_BASE + c);
                printf(" c");
                printf("%ld", c);
                printf("  ");
                print_bar(usage, 30);
            }
        }

        long ram_percent = (ram_total > 0) ? (ram_used * 100 / ram_total) : 0;
        printf("MEM  ");
        print_bar(ram_percent, 30);

        printf("\nRAM total: ");
        printf("%ld", ram_total / 1024);
        printf(" KB   used: ");
        printf("%ld", ram_used / 1024);
        printf(" KB\n");

        printf("Heap used: ");
        printf("%ld", heap_used / 1024);
        printf(" KB   free: ");
        printf("%ld", heap_free / 1024);
        printf(" KB\n");

        printf("PMM pages  used: ");
        printf("%ld", pmm_used);
        printf("  free: ");
        printf("%ld", pmm_free);
        printf("\n\n");

        long got = mos_pslist(procs, MAX_PROCS);
        if (got < 0) got = 0;

        char header[96];
        int hp = 0;
        hp = put_field(header, hp, "PID", 6);
        hp = put_field(header, hp, "STATE", 8);
        hp = put_field(header, hp, "PRIV", 8);
        hp = put_field(header, hp, "CPU", 4);
        hp = put_field(header, hp, "LVL", 4);
        hp = put_field(header, hp, "NAME", 40);
        header[hp] = '\0';
        printf("%s\n", header);
        for (int i = 0; i < 74; i++) printf("-");
        printf("\n");

        for (long i = 0; i < got; i++) {
            char pidbuf[24];
            char cpubuf[12];
            char lvlbuf[12];
            fmt_uint(pidbuf, (unsigned long)procs[i].pid);
            fmt_uint(cpubuf, (unsigned long)procs[i].sched_cpu);
            fmt_uint(lvlbuf, (unsigned long)procs[i].sched_level);

            char line[192];
            int lp = 0;
            lp = put_field(line, lp, pidbuf, 6);
            lp = put_field(line, lp, state_name(procs[i].state), 8);
            lp = put_field(line, lp, priv_name(procs[i].privilege), 8);
            lp = put_field(line, lp, cpubuf, 4);
            lp = put_field(line, lp, lvlbuf, 4);
            lp = put_field(line, lp, procs[i].name, 40);
            line[lp] = '\0';
            printf("%s\n", line);
        }

        printf("\n(refreshing every 1s - Ctrl+C to exit)\n");

        mos_sleep(REFRESH_MS);
    }

    return 0;
}