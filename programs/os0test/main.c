/*
 * os0test - self-test for the OS-0 system calls.
 *
 *     ./build.sh programs/os0test        -> os0test.run
 *     run 0:/programs/os0test.run        (from the terminal)
 *
 * Covers SYS_CLOCK, SYS_THREAD (FS base / TLS, exit futex), SYS_FUTEX,
 * SYS_STAT, SYS_RENAME, SYS_CHDIR/SYS_GETCWD, the environment (envp ->
 * environ), SYS_SPAWN and SYS_WAIT.
 *
 * Output: one line per check,
 *
 *     OS0TEST ok <name>
 *     OS0TEST FAIL <name>: <detail>
 *
 * and a last line, OS0TEST PASS (every check passed) or OS0TEST FAIL.
 * Program output (fd 1) is written to the screen and, by the kernel, to
 * the serial log as "[run] <line>". The verdict is also written to fd 3
 * (mos_log), the kernel-log-only descriptor, so the serial log has it
 * even if another CPU interleaves its own serial output with a "[run]"
 * line. Exit code: 0 for PASS, 1 for FAIL.
 *
 * The test starts itself as a child (spawn + wait are among the things
 * under test): `os0test.run --child <mode> [arg]` is the child side; see
 * child_main(). Scratch files live in 0:/os0test_tmp and are removed.
 *
 * A watchdog thread turns a hang (a wait that is never woken) into an
 * `OS0TEST FAIL watchdog` verdict instead of a silent stall.
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include "minimalos.h"
#include "stdlib.h"
#include "thread.h"
#include "process.h"
#include "string.h"

#define TMP        "0:/os0test_tmp"
#define ERR(x)     ((long)(x))
#define PTR(p)     ((unsigned long long)(uintptr_t)(p))

/* =============================================================== output */

/* Tiny formatter: %s %c %d %ld %lld %lu %llu %x %%. */
static size_t kfmtv(char* out, size_t cap, const char* fmt, va_list ap) {
    size_t n = 0;
#define PUT(ch) do { if (n + 1 < cap) out[n] = (ch); n++; } while (0)
    for (; *fmt; fmt++) {
        if (*fmt != '%') { PUT(*fmt); continue; }
        fmt++;
        int longs = 0;
        while (*fmt == 'l') { longs++; fmt++; }
        if (*fmt == 's') {
            const char* s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            while (*s) { PUT(*s); s++; }
        } else if (*fmt == 'c') {
            PUT((char)va_arg(ap, int));
        } else if (*fmt == '%') {
            PUT('%');
        } else if (*fmt == 'd' || *fmt == 'u' || *fmt == 'x') {
            uint64_t v;
            int neg = 0;
            if (*fmt == 'd') {
                int64_t sv = longs ? va_arg(ap, long long) : va_arg(ap, int);
                if (longs == 1) sv = (long)sv;
                if (sv < 0) { neg = 1; v = (uint64_t)0 - (uint64_t)sv; } else v = (uint64_t)sv;
            } else {
                v = longs ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned int);
            }
            unsigned base = *fmt == 'x' ? 16u : 10u;
            char tmp[24];
            int len = 0;
            do { unsigned d = (unsigned)(v % base); tmp[len++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v /= base; } while (v);
            if (neg) PUT('-');
            while (len) PUT(tmp[--len]);
        } else {
            PUT('%');
            if (*fmt) PUT(*fmt);
        }
    }
#undef PUT
    if (cap) out[n < cap ? n : cap - 1] = '\0';
    return n < cap ? n : (cap ? cap - 1 : 0);
}

static size_t kfmt(char* out, size_t cap, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    size_t n = kfmtv(out, cap, fmt, ap);
    va_end(ap);
    return n;
}

/* One line, one write (the kernel logs a write to serial as one piece). */
static void emit_fd(int fd, const char* fmt, ...) {
    char line[384];
    va_list ap;
    va_start(ap, fmt);
    size_t n = kfmtv(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    line[n++] = '\n';
    mos_write_fd(fd, line, n);
}

static int g_checks;
static int g_failed;
static const char* volatile g_stage = "start";
static volatile uint64_t g_stage_start_ms;

static void stage(const char* name) {
    g_stage = name;
    g_stage_start_ms = (uint64_t)mos_uptime();
}

static void check_pass(const char* name) {
    g_checks++;
    emit_fd(1, "OS0TEST ok %s", name);
}

static void check_fail(const char* name, const char* fmt, ...) {
    char detail[256];
    va_list ap;
    va_start(ap, fmt);
    kfmtv(detail, sizeof(detail), fmt, ap);
    va_end(ap);
    g_checks++;
    g_failed++;
    emit_fd(1, "OS0TEST FAIL %s: %s", name, detail);
}

/* EXPECT(cond, name, detail format, args...): the detail is only built on failure. */
#define EXPECT(cond, name, ...) \
    do { if (cond) check_pass(name); else check_fail(name, __VA_ARGS__); } while (0)

static const char* ename(long r) {
    switch (r) {
        case 0: return "SUCCESS";
        case -1: return "ERR_GENERIC";
        case -2: return "ERR_BADFD";
        case -3: return "ERR_NOTFOUND";
        case -4: return "ERR_INVAL";
        case -5: return "ERR_PERM";
        case -6: return "ERR_BUSY";
        case -7: return "ERR_AGAIN";
        case -8: return "ERR_NOMEM";
        case -9: return "ERR_TIMEDOUT";
        default: return "(other)";
    }
}

/* EXPECT_ERR(call, wanted SYS_ERR_* or SYS_SUCCESS, name) */
#define EXPECT_RET(expr, want, name) \
    do { long r_ = (expr); long w_ = (long)(want); \
         if (r_ == w_) check_pass(name); \
         else check_fail(name, "got %ld (%s), expected %ld (%s)", r_, ename(r_), w_, ename(w_)); \
    } while (0)

/* ============================================================ utilities */

static char g_self[512];          /* absolute path of this program */
static char g_start_cwd[512];

static int streq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static size_t slen(const char* s) { size_t n = 0; while (s[n]) n++; return n; }

static int sstarts(const char* s, const char* p) {
    while (*p) { if (*s++ != *p++) return 0; }
    return 1;
}

static void scopy(char* dst, size_t cap, const char* src) {
    size_t i = 0;
    for (; src[i] && i + 1 < cap; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static long parse_long(const char* s) {
    long v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return v;
}

static int has_line(const char* text, const char* line) {
    size_t n = slen(line);
    const char* p = text;
    while (*p) {
        const char* e = p;
        while (*e && *e != '\n') e++;
        if ((size_t)(e - p) == n) {
            size_t i = 0;
            while (i < n && p[i] == line[i]) i++;
            if (i == n) return 1;
        }
        p = *e ? e + 1 : e;
    }
    return 0;
}

/* Position of a line in the text, or -1. */
static long line_index(const char* text, const char* line) {
    size_t n = slen(line);
    const char* p = text;
    while (*p) {
        const char* e = p;
        while (*e && *e != '\n') e++;
        if ((size_t)(e - p) == n) {
            size_t i = 0;
            while (i < n && p[i] == line[i]) i++;
            if (i == n) return (long)(p - text);
        }
        p = *e ? e + 1 : e;
    }
    return -1;
}

/* Value of a "key=value" line (the number after `key`), or -1. */
static long line_number(const char* text, const char* key) {
    size_t n = slen(key);
    const char* p = text;
    while (*p) {
        if (sstarts(p, key)) return parse_long(p + n);
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    return -1;
}

static int exists(const char* path) { return mos_exists(path) == 1; }
static int is_dir(const char* path) { return mos_is_dir(path) == 1; }

/* Creates (replacing) a file holding `len` bytes of pattern (seed + i*31+7). */
static uint8_t pattern_byte(size_t i, uint8_t seed) { return (uint8_t)(seed + i * 31u + 7u); }

static int write_pattern(const char* path, size_t len, uint8_t seed) {
    if (exists(path) && mos_delete(path) != 0) return -1;
    if (mos_create(path, NULL, NULL) != 0) return -2;
    long fd = mos_open(path, SYS_O_RDWR);
    if (fd < 0) return -3;
    uint8_t chunk[2048];
    size_t done = 0;
    while (done < len) {
        size_t n = len - done < sizeof(chunk) ? len - done : sizeof(chunk);
        for (size_t i = 0; i < n; i++) chunk[i] = pattern_byte(done + i, seed);
        long w = mos_fwrite(fd, chunk, n);
        if (w != (long)n) { mos_close(fd); return -4; }
        done += n;
    }
    mos_close(fd);
    return 0;
}

static int write_text(const char* path, const char* text) {
    size_t len = slen(text);
    if (exists(path) && mos_delete(path) != 0) return -1;
    if (mos_create(path, "text", "txt") != 0) return -2;
    if (!len) return 0;
    long fd = mos_open(path, SYS_O_RDWR);
    if (fd < 0) return -3;
    long w = mos_fwrite(fd, text, len);
    mos_close(fd);
    return w == (long)len ? 0 : -4;
}

/* Reads up to cap-1 bytes into buf (NUL-terminated); returns the count or <0. */
static long read_text(const char* path, char* buf, size_t cap) {
    long fd = mos_open(path, SYS_O_RDONLY);
    if (fd < 0) { buf[0] = '\0'; return fd; }
    size_t got = 0;
    for (;;) {
        if (got + 1 >= cap) break;
        long n = mos_read(fd, buf + got, cap - 1 - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    mos_close(fd);
    buf[got] = '\0';
    return (long)got;
}

/* Offset of the first line that starts with `prefix`, or -1. */
static long line_prefix_index(const char* text, const char* prefix) {
    const char* p = text;
    while (*p) {
        if (sstarts(p, prefix)) return (long)(p - text);
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    return -1;
}

/* 1 when the file holds exactly `len` bytes of that pattern. */
static int verify_pattern(const char* path, size_t len, uint8_t seed) {
    long fd = mos_open(path, SYS_O_RDONLY);
    if (fd < 0) return 0;
    int good = mos_size(fd) == (long)len;
    uint8_t chunk[2048];
    size_t at = 0;
    while (good) {
        long n = mos_read(fd, chunk, sizeof(chunk));
        if (n < 0) { good = 0; break; }
        if (n == 0) break;
        for (long i = 0; i < n; i++)
            if (chunk[i] != pattern_byte(at + (size_t)i, seed)) { good = 0; break; }
        at += (size_t)n;
    }
    if (at != len) good = 0;
    mos_close(fd);
    return good;
}

static void join_path(char* out, size_t cap, const char* a, const char* b) {
    kfmt(out, cap, "%s/%s", a, b);
}

/* Removes a file or a directory tree. */
static void rm_rf(const char* path) {
    if (!exists(path)) return;
    if (!is_dir(path)) { mos_delete(path); return; }
    syscall_dirent_t* ents = (syscall_dirent_t*)malloc(sizeof(syscall_dirent_t) * 64);
    if (ents) {
        long n = mos_listdir(path, ents, 64);
        for (long i = 0; i < n; i++) {
            if (streq(ents[i].name, ".") || streq(ents[i].name, "..")) continue;
            char child[512];
            join_path(child, sizeof(child), path, ents[i].name);
            rm_rf(child);
        }
        free(ents);
    }
    mos_rmdir(path);
}

/* True when a listing of `dir` has an entry called `name`. */
static int dir_has(const char* dir, const char* name) {
    syscall_dirent_t* ents = (syscall_dirent_t*)malloc(sizeof(syscall_dirent_t) * 64);
    if (!ents) return 0;
    long n = mos_listdir(dir, ents, 64);
    int found = 0;
    for (long i = 0; i < n; i++) if (streq(ents[i].name, name)) found = 1;
    free(ents);
    return found;
}

/* ============================================================ child side */

static uint64_t read_fs0(void) {
    uint64_t v;
    __asm__ volatile("movq %%fs:0, %0" : "=r"(v) :: "memory");
    return v;
}

/* Thread-local block for the hardware TLS check: word 0 points at itself. */
typedef struct {
    uint64_t self;
    long index;
    volatile int errors;
    volatile int finished;
    uint8_t pad[32];
} tls_block_t;

static tls_block_t g_hw_tls[4] __attribute__((aligned(64)));

static void hw_tls_thread(void* arg) {
    tls_block_t* b = (tls_block_t*)arg;
    for (int i = 0; i < 40; i++) {
        if (read_fs0() != (uint64_t)(uintptr_t)b) b->errors++;
        if (mos_thread_get_fs() != (void*)b) b->errors++;
        if (i % 3 == 0) mos_thread_yield();
        else if (i % 3 == 1) mos_sleep(1 + b->index);
        else for (volatile int spin = 0; spin < 20000; spin++) { }
    }
    __atomic_store_n(&b->finished, 1, __ATOMIC_RELEASE);
}

static int child_tls_hw(void) {
    int errors = 0;
    tls_block_t* main_block = &g_hw_tls[3];
    main_block->self = (uint64_t)(uintptr_t)main_block;
    if (mos_thread_set_fs(main_block) != 0) { emit_fd(1, "tls_hw: set_fs failed"); return 1; }
    mos_thread_t t[3];
    for (int i = 0; i < 3; i++) {
        g_hw_tls[i].self = (uint64_t)(uintptr_t)&g_hw_tls[i];
        g_hw_tls[i].index = i;
        g_hw_tls[i].errors = 0;
        g_hw_tls[i].finished = 0;
        if (mos_thread_create_ex(&t[i], hw_tls_thread, &g_hw_tls[i], 0, &g_hw_tls[i]) != 0) {
            emit_fd(1, "tls_hw: thread %d not created", i);
            return 1;
        }
    }
    for (int i = 0; i < 30; i++) {
        if (read_fs0() != (uint64_t)(uintptr_t)main_block) errors++;
        if (i & 1) mos_sleep(1); else mos_thread_yield();
    }
    for (int i = 0; i < 3; i++) {
        mos_thread_join(&t[i]);
        if (g_hw_tls[i].errors || !g_hw_tls[i].finished) {
            emit_fd(1, "tls_hw: thread %d errors=%d finished=%d", i, g_hw_tls[i].errors, g_hw_tls[i].finished);
            errors++;
        }
    }
    if (read_fs0() != (uint64_t)(uintptr_t)main_block) errors++;
    emit_fd(1, errors ? "tls_hw: FAILED" : "tls_hw: all ok");
    return errors ? 1 : 0;
}

static int child_tsc(void) {
    uint64_t hz = mos_clock_tsc_hz();
    emit_fd(1, "tsc_hz=%llu", (unsigned long long)hz);
    if (!hz) return 0;
    uint32_t lo, hi;
    uint64_t m0 = mos_clock_monotonic_ns();
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t t0 = ((uint64_t)hi << 32) | lo;
    mos_sleep(100);
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t t1 = ((uint64_t)hi << 32) | lo;
    uint64_t m1 = mos_clock_monotonic_ns();
    uint64_t dms = (m1 - m0) / 1000000u;
    uint64_t expected = (hz / 1000u) * dms;          /* ticks the reported rate predicts */
    emit_fd(1, "tsc_ticks=%llu", (unsigned long long)(t1 - t0));
    emit_fd(1, "tsc_pct=%llu", (unsigned long long)(expected ? (t1 - t0) * 100u / expected : 0));
    return 0;
}

static int child_info(int argc, char** argv, char** envp) {
    emit_fd(1, "argc=%d", argc);
    for (int i = 0; i < argc; i++) emit_fd(1, "argv[%d]=%s", i, argv[i]);
    emit_fd(1, "argv_null=%d", argv[argc] == NULL);
    emit_fd(1, "envp_is_environ=%d", envp == environ);
    int count = 0;
    for (char** e = environ; *e; e++) count++;
    emit_fd(1, "environ_count=%d", count);
    const char* names[] = { "OS0_A", "OS0_B", "OS0_C", "OS0_INH", "OS0_NOPE", "OS0_", "OS0" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char* v = mos_getenv(names[i]);
        if (v) emit_fd(1, "getenv.%s=[%s]", names[i], v);
        else emit_fd(1, "getenv.%s=(null)", names[i]);
    }
    char cwd[512];
    long r = mos_getcwd(cwd, sizeof(cwd));
    emit_fd(1, "cwd=%s", r == 0 ? cwd : "(error)");
    emit_fd(2, "stderr-line-from-child");
    return 7;
}

static int child_relay(char** argv) {
    /* Starts the program again with no cwd/envp/output given: all of them
     * must be inherited from this process. */
    const char* const child_argv[] = { argv[0], "--child", "info", "inherit", NULL };
    long pid = mos_spawn(argv[0], child_argv);
    if (pid <= 0) { emit_fd(1, "relay: spawn failed %ld", pid); return 1; }
    mos_wait_result_t r;
    long w = mos_waitpid(pid, 0, &r);
    emit_fd(1, "relay-done wait=%ld exit=%d crashed=%d", w, r.exit_code, r.crashed);
    return (w == pid && r.exit_code == 7 && !r.crashed) ? 0 : 1;
}

/* os0test.run --child <mode> [arg] */
static int child_main(int argc, char** argv, char** envp) {
    const char* mode = argc > 2 ? argv[2] : "";
    long arg = argc > 3 ? parse_long(argv[3]) : 0;
    if (streq(mode, "exit")) return (int)arg;                 /* returns from main */
    if (streq(mode, "exitcall")) mos_exit((int)arg);          /* SYS_EXIT directly */
    if (streq(mode, "sleep")) { mos_sleep(arg); return 3; }
    if (streq(mode, "info")) return child_info(argc, argv, envp);
    if (streq(mode, "relay")) return child_relay(argv);
    if (streq(mode, "tls_hw")) return child_tls_hw();
    if (streq(mode, "tsc")) return child_tsc();
    if (streq(mode, "streams")) {
        emit_fd(1, "out1");
        emit_fd(2, "err1");
        emit_fd(1, "out2");
        return 0;
    }
    if (streq(mode, "crash")) {
        emit_fd(1, "before-crash");
        __asm__ volatile("ud2");            /* #UD, CPU exception 6 */
        return 0;
    }
    emit_fd(1, "unknown child mode %s", mode);
    return 99;
}

/* ============================================================== watchdog */

#define STAGE_LIMIT_MS 60000u

static volatile int g_finished;

static void watchdog(void* arg) {
    (void)arg;
    for (;;) {
        mos_sleep(100);
        if (g_finished) return;
        uint64_t now = (uint64_t)mos_uptime();
        if (now - g_stage_start_ms > STAGE_LIMIT_MS) {
            emit_fd(1, "OS0TEST FAIL watchdog: stuck for %lu ms in stage '%s'",
                    (unsigned long)(now - g_stage_start_ms), g_stage);
            emit_fd(1, "OS0TEST FAIL");
            mos_log("OS0TEST FAIL");
            mos_exit(1);
        }
    }
}

/* ================================================================ clocks */

static void test_clocks(void) {
    stage("clocks");
    uint64_t prev = mos_clock_monotonic_ns();
    int back = 0;
    uint64_t first = prev;
    for (int i = 0; i < 2000; i++) {
        uint64_t now = mos_clock_monotonic_ns();
        if (now < prev) back++;
        prev = now;
    }
    EXPECT(back == 0, "clock_monotonic_nondecreasing", "went backwards %d times", back);
    EXPECT(prev > 0, "clock_monotonic_nonzero", "value is 0");
    EXPECT(prev >= first, "clock_monotonic_advances", "last %llu < first %llu",
           (unsigned long long)prev, (unsigned long long)first);

    uint64_t m0 = mos_clock_monotonic_ns();
    long u0 = mos_uptime();
    uint64_t r0 = mos_clock_realtime_ns();
    mos_sleep(100);
    uint64_t m1 = mos_clock_monotonic_ns();
    long u1 = mos_uptime();
    uint64_t r1 = mos_clock_realtime_ns();
    uint64_t dm = (m1 - m0) / 1000000u;
    EXPECT(m1 >= m0 && dm >= 95 && dm <= 400, "clock_monotonic_sleep_100ms",
           "slept %llu ms by the monotonic clock", (unsigned long long)dm);
    long du = u1 - u0;
    long diff = (long)dm - du;
    if (diff < 0) diff = -diff;
    EXPECT(diff <= 30, "clock_monotonic_vs_uptime",
           "monotonic says %llu ms, uptime says %ld ms", (unsigned long long)dm, du);

    const uint64_t y2020 = 1577836800ull, y2100 = 4102444800ull;
    EXPECT(r0 / 1000000000ull > y2020 && r0 / 1000000000ull < y2100, "clock_realtime_sane",
           "unix time %llu s", (unsigned long long)(r0 / 1000000000ull));
    uint64_t dr = r1 >= r0 ? (r1 - r0) / 1000000u : 0;
    EXPECT(r1 >= r0 && dr >= 50 && dr <= 1500, "clock_realtime_advances",
           "advanced %llu ms over a 100 ms sleep", (unsigned long long)dr);
    EXPECT(r0 % 1000000ull == 0 && r1 % 1000000ull == 0, "clock_realtime_ms_steps",
           "not whole milliseconds: %llu", (unsigned long long)r0);
    EXPECT(mos_time_unix() > (int64_t)y2020, "clock_time_unix", "process.h mos_time_unix() wrong");

    /* Consecutive realtime reads, across at least one second step of the
     * kernel's date/time, never jump by a minute or an hour (a read torn by
     * a carry in the timer interrupt would). */
    {
        uint64_t t_end = mos_clock_monotonic_ns() + 1300ull * 1000000ull;
        uint64_t last = mos_clock_realtime_ns();
        uint64_t worst_fwd = 0, worst_back = 0;
        unsigned reads = 0;
        while (mos_clock_monotonic_ns() < t_end && reads < 2000000u) {
            uint64_t now = mos_clock_realtime_ns();
            if (now >= last) { if (now - last > worst_fwd) worst_fwd = now - last; }
            else if (last - now > worst_back) worst_back = last - now;
            last = now;
            reads++;
        }
        EXPECT(worst_fwd < 1000000000ull && worst_back < 1000000000ull, "clock_realtime_reads_consistent",
               "jumped forward %llu ns / back %llu ns within %u reads",
               (unsigned long long)worst_fwd, (unsigned long long)worst_back, reads);
    }

    EXPECT_RET((long)mos_syscall(SYS_CLOCK, 99, 0, 0), ERR(SYS_ERR_INVAL), "clock_bad_which");

    uint64_t hz = mos_clock_tsc_hz();
    if (hz == 0) {
        check_pass("clock_tsc_hz (0: no invariant TSC, RDTSC not offered)");
    } else {
        EXPECT(hz >= 100000000ull && hz <= 10000000000ull, "clock_tsc_hz_range",
               "%llu Hz", (unsigned long long)hz);
    }
}

/* Runs RDTSC in a child: if the kernel promised it usable and it is not,
 * only the child dies. */
static long spawn_wait(const char* mode, const char* arg, const char* out_path,
                       const char* err_path, const char* const* envp, const char* cwd,
                       mos_wait_result_t* res);

static void test_tsc(void) {
    stage("tsc");
    char out[512];
    mos_wait_result_t r;
    long pid = spawn_wait("tsc", NULL, TMP "/tsc.txt", NULL, NULL, NULL, &r);
    read_text(TMP "/tsc.txt", out, sizeof(out));
    EXPECT(pid > 0 && !r.crashed && r.exit_code == 0, "tsc_child_runs",
           "pid=%ld crashed=%d exit=%d vector=%u", pid, r.crashed, r.exit_code, r.crash_vector);
    long hz = line_number(out, "tsc_hz=");
    uint64_t want = mos_clock_tsc_hz();
    EXPECT(hz >= 0 && (uint64_t)hz == want, "tsc_hz_consistent", "child %ld, parent %llu",
           hz, (unsigned long long)want);
    if (want) {
        long pct = line_number(out, "tsc_pct=");
        EXPECT(pct >= 90 && pct <= 110, "tsc_rdtsc_matches_hz",
               "RDTSC over 100 ms implies %ld%% of the reported rate", pct);
    }
}

/* ================================================================== TLS */

static tls_block_t g_tls[4] __attribute__((aligned(64)));
static volatile int g_tls_thread_fs_ok;
static volatile int g_tls_plain_fs_seen = -1;
static uint64_t g_alt_block[8] __attribute__((aligned(64)));

static void tls_worker(void* arg) {
    tls_block_t* b = (tls_block_t*)arg;
    int errors = 0;
    for (int i = 0; i < 30; i++) {
        if (mos_thread_get_fs() != (void*)b) errors++;
        switch (i % 3) {
            case 0: mos_thread_yield(); break;
            case 1: mos_sleep(1 + b->index); break;
            default: for (volatile int s = 0; s < 30000; s++) { } break;
        }
    }
    b->errors = errors;
    __atomic_store_n(&b->finished, 1, __ATOMIC_RELEASE);
}

static void tls_plain_worker(void* arg) {
    (void)arg;
    g_tls_plain_fs_seen = mos_thread_get_fs() == NULL ? 0 : 1;
}

static void tls_setter(void* arg) {
    (void)arg;
    /* Changing this thread's FS base must not touch the main thread's. */
    long r = mos_thread_set_fs(g_alt_block);
    g_tls_thread_fs_ok = (r == 0 && mos_thread_get_fs() == (void*)g_alt_block);
    mos_sleep(5);
    if (mos_thread_get_fs() != (void*)g_alt_block) g_tls_thread_fs_ok = 0;
}

static void test_tls(void) {
    stage("tls");
    EXPECT(mos_thread_get_fs() == NULL, "tls_main_initial_zero", "FS base is 0x%llx", PTR(mos_thread_get_fs()));
    EXPECT_RET(mos_thread_set_fs(&g_tls[3]), SYS_SUCCESS, "tls_main_set");
    EXPECT(mos_thread_get_fs() == (void*)&g_tls[3], "tls_main_get", "got 0x%llx want 0x%llx",
           PTR(mos_thread_get_fs()), PTR(&g_tls[3]));
    mos_thread_yield();
    mos_sleep(3);
    EXPECT(mos_thread_get_fs() == (void*)&g_tls[3], "tls_main_survives_yield_sleep", "got 0x%llx",
           PTR(mos_thread_get_fs()));
    EXPECT_RET(mos_syscall(SYS_THREAD, SYS_THREAD_SET_FS, (long)0x8000000000000000ull, 0),
               ERR(SYS_ERR_INVAL), "tls_set_noncanonical_rejected");
    EXPECT_RET(mos_syscall(SYS_THREAD, SYS_THREAD_SET_FS, (long)0x0000800000000000ull, 0),
               ERR(SYS_ERR_INVAL), "tls_set_upper_half_rejected");
    EXPECT(mos_thread_get_fs() == (void*)&g_tls[3], "tls_rejected_set_keeps_value", "got 0x%llx",
           PTR(mos_thread_get_fs()));

    /* A thread made with mos_thread_create() starts with FS base 0, even
     * though this thread has one. */
    mos_thread_t plain;
    int pc = mos_thread_create(&plain, tls_plain_worker, NULL, 0);
    if (pc == 0) mos_thread_join(&plain);
    EXPECT(pc == 0 && g_tls_plain_fs_seen == 0, "tls_plain_thread_starts_at_zero",
           "create=%d seen=%d", pc, g_tls_plain_fs_seen);

    /* Several threads, each with its own block, running at once. */
    mos_thread_t t[3];
    int created = 0;
    for (int i = 0; i < 3; i++) {
        g_tls[i].self = (uint64_t)(uintptr_t)&g_tls[i];
        g_tls[i].index = i;
        g_tls[i].errors = -1;
        g_tls[i].finished = 0;
        if (mos_thread_create_ex(&t[i], tls_worker, &g_tls[i], 0, &g_tls[i]) == 0) created++;
    }
    EXPECT(created == 3, "tls_threads_created", "created %d of 3", created);
    int main_errors = 0;
    for (int i = 0; i < 20; i++) {
        if (mos_thread_get_fs() != (void*)&g_tls[3]) main_errors++;
        if (i & 1) mos_sleep(1); else mos_thread_yield();
    }
    EXPECT(main_errors == 0, "tls_main_unchanged_while_threads_run", "%d mismatches", main_errors);
    for (int i = 0; i < created; i++) {
        mos_thread_join(&t[i]);
        char name[48];
        kfmt(name, sizeof(name), "tls_thread_%d_own_fs_base", i);
        EXPECT(g_tls[i].finished && g_tls[i].errors == 0, name, "finished=%d errors=%d",
               g_tls[i].finished, g_tls[i].errors);
    }

    /* A thread changing its own FS base. */
    mos_thread_t st;
    g_tls_thread_fs_ok = 0;
    int sc = mos_thread_create_ex(&st, tls_setter, NULL, 0, NULL);
    if (sc == 0) mos_thread_join(&st);
    EXPECT(sc == 0 && g_tls_thread_fs_ok, "tls_thread_set_fs", "create=%d ok=%d", sc, g_tls_thread_fs_ok);
    EXPECT(mos_thread_get_fs() == (void*)&g_tls[3], "tls_main_unchanged_by_thread_set_fs",
           "got 0x%llx", PTR(mos_thread_get_fs()));

    EXPECT_RET(mos_thread_set_fs(NULL), SYS_SUCCESS, "tls_main_clear");
    EXPECT(mos_thread_get_fs() == NULL, "tls_main_cleared", "got 0x%llx", PTR(mos_thread_get_fs()));

    /* The real thing: %fs:0 reads through the base. In a child process,
     * where a wrong base only kills the child. */
    char out[1024];
    mos_wait_result_t r;
    long pid = spawn_wait("tls_hw", NULL, TMP "/tls_hw.txt", NULL, NULL, NULL, &r);
    read_text(TMP "/tls_hw.txt", out, sizeof(out));
    EXPECT(pid > 0 && !r.crashed && r.exit_code == 0 && has_line(out, "tls_hw: all ok"),
           "tls_hardware_fs_reads",
           "pid=%ld exit=%d crashed=%d vector=%u output: %s", pid, r.exit_code, r.crashed,
           r.crash_vector, out);

    /* CREATE_EX validation. */
    syscall_thread_ex_t bad;
    bad.entry = 0; bad.stack_top = 0; bad.arg = 0; bad.fs_base = 0; bad.exit_futex = 0;
    long id = mos_syscall(SYS_THREAD, SYS_THREAD_CREATE_EX, (long)(uintptr_t)&bad, 0);
    EXPECT(id < 0, "thread_create_ex_bad_entry_rejected", "returned %ld", id);
}

/* ================================================================ futex */

static volatile uint32_t g_fx_word;
static volatile uint32_t g_fx_ready;
static volatile long g_fx_result[4];
static volatile uint32_t g_fx_woke_flag;

static void fx_waiter(void* arg) {
    long idx = (long)(uintptr_t)arg;
    __atomic_add_fetch(&g_fx_ready, 1, __ATOMIC_SEQ_CST);
    g_fx_result[idx] = mos_futex_wait(&g_fx_word, 0, 5ull * 1000000000ull);
    __atomic_store_n(&g_fx_woke_flag, 1, __ATOMIC_SEQ_CST);
}

static void fx_sleeper(void* arg) {
    mos_sleep((long)(uintptr_t)arg);
}

static void fx_quick(void* arg) { (void)arg; }

static void wait_ready(uint32_t want) {
    for (int i = 0; i < 2000 && __atomic_load_n(&g_fx_ready, __ATOMIC_SEQ_CST) < want; i++)
        mos_sleep(1);
    mos_sleep(40);               /* let them get into the kernel's wait */
}

static void test_futex(void) {
    stage("futex");
    static volatile uint32_t word;
    word = 5;
    EXPECT_RET(mos_futex_wait(&word, 4, 0), ERR(SYS_ERR_AGAIN), "futex_wait_mismatch_again");
    EXPECT_RET(mos_futex_wait(&word, 6, 1000000000ull), ERR(SYS_ERR_AGAIN),
               "futex_wait_mismatch_again_with_timeout");
    word = 0;
    EXPECT_RET(mos_futex_wake(&word, 1), 0, "futex_wake_nobody_returns_0");

    static const uint64_t waits_ms[] = { 20, 100, 250 };
    for (size_t i = 0; i < 3; i++) {
        uint64_t t0 = mos_clock_monotonic_ns();
        long r = mos_futex_wait(&word, 0, waits_ms[i] * 1000000ull);
        uint64_t el = (mos_clock_monotonic_ns() - t0) / 1000u;       /* us */
        char name[48];
        kfmt(name, sizeof(name), "futex_timeout_%lums", (unsigned long)waits_ms[i]);
        /* never early (allow 1 ms of clock-source slop), not much late */
        EXPECT(r == ERR(SYS_ERR_TIMEDOUT) && el >= waits_ms[i] * 1000u - 1000u &&
               el <= waits_ms[i] * 1000u + 150000u,
               name, "returned %ld (%s) after %llu us", r, ename(r), (unsigned long long)el);
    }
    {
        uint64_t t0 = mos_clock_monotonic_ns();
        long r = mos_futex_wait(&word, 0, 1);              /* 1 ns: rounds up to a millisecond */
        uint64_t el = (mos_clock_monotonic_ns() - t0) / 1000u;
        EXPECT(r == ERR(SYS_ERR_TIMEDOUT) && el < 100000u, "futex_timeout_1ns_rounds_up",
               "returned %ld (%s) after %llu us", r, ename(r), (unsigned long long)el);
    }

    /* Argument checks. */
    EXPECT_RET(mos_futex_wait(NULL, 0, 1000000), ERR(SYS_ERR_INVAL), "futex_wait_null_inval");
    EXPECT_RET(mos_futex_wait((volatile uint32_t*)((uintptr_t)&word + 1), 0, 1000000),
               ERR(SYS_ERR_INVAL), "futex_wait_unaligned_inval");
    EXPECT_RET(mos_futex_wake((volatile uint32_t*)((uintptr_t)&word + 2), 1), ERR(SYS_ERR_INVAL),
               "futex_wake_unaligned_inval");
    EXPECT_RET(mos_futex_wake(NULL, 1), ERR(SYS_ERR_INVAL), "futex_wake_null_inval");
    EXPECT_RET(mos_syscall4(SYS_FUTEX, 99, (long)(uintptr_t)&word, 0, 0), ERR(SYS_ERR_INVAL),
               "futex_bad_op_inval");
    EXPECT_RET(mos_futex_wait((volatile uint32_t*)0x1000, 0, 1000000), ERR(SYS_ERR_INVAL),
               "futex_wait_foreign_address_inval");

    /* wake wakes a waiting thread */
    g_fx_word = 0; g_fx_ready = 0; g_fx_woke_flag = 0; g_fx_result[0] = 12345;
    mos_thread_t w1;
    int c = mos_thread_create_ex(&w1, fx_waiter, (void*)0, 0, NULL);
    EXPECT(c == 0, "futex_waiter_created", "create returned %d", c);
    if (c == 0) {
        wait_ready(1);
        EXPECT(__atomic_load_n(&g_fx_woke_flag, __ATOMIC_SEQ_CST) == 0, "futex_waiter_sleeps",
               "the waiter returned (%ld) before any wake", (long)g_fx_result[0]);
        g_fx_word = 1;
        long woke = mos_futex_wake(&g_fx_word, 1);
        EXPECT(woke == 1, "futex_wake_returns_1", "returned %ld", woke);
        mos_thread_join(&w1);
        EXPECT(g_fx_result[0] == 0 && g_fx_woke_flag, "futex_wake_wakes_waiter",
               "waiter result %ld (%s)", (long)g_fx_result[0], ename(g_fx_result[0]));
    }

    /* wake count: 4 waiters, wake 2, then the rest */
    g_fx_word = 0; g_fx_ready = 0; g_fx_woke_flag = 0;
    mos_thread_t w[4];
    int made = 0;
    for (long i = 0; i < 4; i++) {
        g_fx_result[i] = 12345;
        if (mos_thread_create_ex(&w[i], fx_waiter, (void*)(uintptr_t)i, 0, NULL) == 0) made++;
    }
    EXPECT(made == 4, "futex_4_waiters_created", "created %d", made);
    wait_ready(4);
    long first = mos_futex_wake(&g_fx_word, 2);
    EXPECT(first == 2, "futex_wake_count_2", "returned %ld", first);
    mos_sleep(40);
    int returned = 0;
    for (int i = 0; i < 4; i++) if (g_fx_result[i] != 12345) returned++;
    EXPECT(returned == 2, "futex_wake_count_only_2_woke", "%d waiters returned", returned);
    long second = mos_futex_wake(&g_fx_word, 100);
    EXPECT(second == 2, "futex_wake_rest_returns_2", "returned %ld", second);
    long third = mos_futex_wake(&g_fx_word, 100);
    EXPECT(third == 0, "futex_wake_after_all_woken_returns_0", "returned %ld", third);
    int good = 0;
    for (int i = 0; i < made; i++) {
        mos_thread_join(&w[i]);
        if (g_fx_result[i] == 0) good++;
    }
    EXPECT(good == 4, "futex_all_waiters_woke_success", "%d of 4 returned SUCCESS (results %ld %ld %ld %ld)",
           good, (long)g_fx_result[0], (long)g_fx_result[1], (long)g_fx_result[2], (long)g_fx_result[3]);

    /* thread join through the exit futex */
    mos_thread_t j;
    uint64_t t0 = mos_clock_monotonic_ns();
    int jc = mos_thread_create_ex(&j, fx_sleeper, (void*)(uintptr_t)60, 0, NULL);
    if (jc == 0) mos_thread_join(&j);
    uint64_t ms = (mos_clock_monotonic_ns() - t0) / 1000000u;
    EXPECT(jc == 0 && ms >= 55 && ms <= 400, "thread_join_exit_futex_waits", "create=%d joined after %llu ms", jc,
           (unsigned long long)ms);
    EXPECT(j.exit_word == 0 && j.id == 0 && j.stack == NULL, "thread_join_exit_futex_cleans_up",
           "exit_word=%u id=%ld stack=0x%llx", (unsigned)j.exit_word, j.id, PTR(j.stack));

    jc = mos_thread_create_ex(&j, fx_quick, NULL, 0, NULL);
    mos_sleep(50);
    t0 = mos_clock_monotonic_ns();
    if (jc == 0) mos_thread_join(&j);
    ms = (mos_clock_monotonic_ns() - t0) / 1000000u;
    EXPECT(jc == 0 && ms < 30, "thread_join_already_ended_returns_at_once", "create=%d took %llu ms", jc,
           (unsigned long long)ms);

    /* waiting on the exit word by hand: the kernel zeroes it and wakes the waiter */
    static mos_thread_t jm;
    jc = mos_thread_create_ex(&jm, fx_sleeper, (void*)(uintptr_t)50, 0, NULL);
    if (jc == 0) {
        long wr = ERR(SYS_ERR_AGAIN);
        uint32_t cur = __atomic_load_n(&jm.exit_word, __ATOMIC_SEQ_CST);
        if (cur != 0) wr = mos_futex_wait(&jm.exit_word, cur, 3ull * 1000000000ull);
        uint32_t after = __atomic_load_n(&jm.exit_word, __ATOMIC_SEQ_CST);
        EXPECT(cur == 1 && wr == 0 && after == 0, "thread_exit_futex_zeroed_and_waiter_woken",
               "word before=%u wait=%ld (%s) word after=%u", (unsigned)cur, wr, ename(wr), (unsigned)after);
        mos_thread_join(&jm);
    } else {
        check_fail("thread_exit_futex_zeroed_and_waiter_woken", "thread not created (%d)", jc);
    }

    /* the old polling join still works */
    jc = mos_thread_create(&j, fx_sleeper, (void*)(uintptr_t)30, 0);
    t0 = mos_clock_monotonic_ns();
    if (jc == 0) mos_thread_join(&j);
    ms = (mos_clock_monotonic_ns() - t0) / 1000000u;
    EXPECT(jc == 0 && ms >= 25 && ms <= 400 && j.id == 0, "thread_join_polling_still_works",
           "create=%d joined after %llu ms id=%ld", jc, (unsigned long long)ms, j.id);
}

/* ================================================================== stat */

static void test_stat(void) {
    stage("stat");
    static const size_t sizes[] = { 0, 1, 511, 512, 513, 4096, 5000, 70000 };
    syscall_stat_t st;
    int all_ok = 1;
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        char path[128], name[64];
        kfmt(path, sizeof(path), TMP "/size_%lu.bin", (unsigned long)sizes[i]);
        kfmt(name, sizeof(name), "stat_file_size_%lu", (unsigned long)sizes[i]);
        int w = write_pattern(path, sizes[i], (uint8_t)i);
        memset(&st, 0xAB, sizeof(st));
        long r = mos_stat(path, &st);
        long fd = mos_open(path, SYS_O_RDONLY);
        long fsz = fd >= 0 ? mos_size(fd) : -1;
        if (fd >= 0) mos_close(fd);
        int good = w == 0 && r == 0 && st.type == SYS_STAT_FILE && st.size == sizes[i] &&
                   fsz == (long)sizes[i];
        if (!good) all_ok = 0;
        EXPECT(good, name, "write=%d stat=%ld (%s) type=%u size=%llu mos_size=%ld",
               w, r, ename(r), (unsigned)st.type, (unsigned long long)st.size, fsz);
    }
    (void)all_ok;
    EXPECT(verify_pattern(TMP "/size_70000.bin", 70000, 7), "stat_file_content_roundtrip", "70000-byte file differs");

    /* grown by a second write */
    {
        long fd = mos_open(TMP "/size_1.bin", SYS_O_RDWR);
        mos_seek(fd, mos_size(fd));
        mos_fwrite(fd, "xyz", 3);
        mos_close(fd);
        long r = mos_stat(TMP "/size_1.bin", &st);
        EXPECT(r == 0 && st.size == 4, "stat_size_after_append", "stat=%ld size=%llu", r, (unsigned long long)st.size);
    }

    /* file attributes and times */
    write_pattern(TMP "/attr.bin", 10, 1);
    long r = mos_stat(TMP "/attr.bin", &st);
    EXPECT(r == 0 && !(st.flags & SYS_STAT_HIDDEN) && !(st.flags & SYS_STAT_RUNNABLE),
           "stat_plain_file_flags", "stat=%ld flags=%x", r, (unsigned)st.flags);
    {
        int64_t now = mos_time_unix();
        const int64_t slack = 2 * 86400;      /* MinimaFS keeps the day only */
        EXPECT(r == 0 && st.created > 1577836800 && st.created <= now + slack && st.created >= now - slack,
               "stat_file_created_time", "created=%lld now=%lld", (long long)st.created, (long long)now);
        EXPECT(r == 0 && st.modified > 1577836800 && st.modified <= now + slack && st.modified >= now - slack,
               "stat_file_modified_time", "modified=%lld now=%lld", (long long)st.modified, (long long)now);
    }

    /* directories */
    mos_mkdir(TMP "/statdir");
    memset(&st, 0xAB, sizeof(st));
    r = mos_stat(TMP "/statdir", &st);
    EXPECT(r == 0 && st.type == SYS_STAT_DIR && st.size == 0, "stat_directory",
           "stat=%ld type=%u size=%llu", r, (unsigned)st.type, (unsigned long long)st.size);
    EXPECT(r == 0 && st.created == 0 && st.modified == 0, "stat_directory_has_no_times",
           "created=%lld modified=%lld", (long long)st.created, (long long)st.modified);
    memset(&st, 0xAB, sizeof(st));
    r = mos_stat(TMP "/statdir/", &st);
    EXPECT(r == 0 && st.type == SYS_STAT_DIR, "stat_directory_trailing_slash", "stat=%ld (%s) type=%u",
           r, ename(r), (unsigned)st.type);
    memset(&st, 0xAB, sizeof(st));
    r = mos_stat("0:/", &st);
    EXPECT(r == 0 && st.type == SYS_STAT_DIR, "stat_drive_root", "stat=%ld (%s) type=%u", r, ename(r),
           (unsigned)st.type);
    memset(&st, 0xAB, sizeof(st));
    r = mos_stat("/os0test_tmp", &st);
    EXPECT(r == 0 && st.type == SYS_STAT_DIR, "stat_slash_path_is_drive_root_relative", "stat=%ld (%s)", r, ename(r));

    /* errors */
    EXPECT_RET(mos_stat(TMP "/no_such_file", &st), ERR(SYS_ERR_NOTFOUND), "stat_missing_notfound");
    EXPECT_RET(mos_stat(TMP "/no_such_dir/file", &st), ERR(SYS_ERR_NOTFOUND), "stat_missing_parent_notfound");
    EXPECT_RET(mos_stat(TMP "/attr.bin/inside", &st), ERR(SYS_ERR_NOTFOUND), "stat_path_through_file_notfound");
    EXPECT_RET(mos_stat(NULL, &st), ERR(SYS_ERR_INVAL), "stat_null_path_inval");
    EXPECT_RET(mos_stat(TMP "/attr.bin", NULL), ERR(SYS_ERR_INVAL), "stat_null_out_inval");
    EXPECT(mos_file_size(TMP "/size_513.bin") == 513 && mos_file_size(TMP "/statdir") < 0 &&
           mos_file_size(TMP "/nope") == ERR(SYS_ERR_NOTFOUND),
           "stat_process_h_file_size", "sizes %ld %ld %ld", mos_file_size(TMP "/size_513.bin"),
           mos_file_size(TMP "/statdir"), mos_file_size(TMP "/nope"));

    /* this program's own file: a runnable bundle */
    memset(&st, 0, sizeof(st));
    r = mos_stat(g_self, &st);
    EXPECT(r == 0 && st.type == SYS_STAT_FILE && st.size > 1000, "stat_own_executable",
           "stat(%s)=%ld (%s) type=%u size=%llu", g_self, r, ename(r), (unsigned)st.type,
           (unsigned long long)st.size);
    emit_fd(1, "OS0TEST info own executable: size=%llu flags=%x (RUNNABLE=%d)",
            (unsigned long long)st.size, (unsigned)st.flags, (st.flags & SYS_STAT_RUNNABLE) != 0);
}

/* ================================================================ rename */

static void test_rename(void) {
    stage("rename");
    const char* d = TMP "/ren";
    mos_mkdir(d);
    char a[160], b[160], c[160];
    char buf[256];
    syscall_stat_t st;

    /* file, same directory */
    kfmt(a, sizeof(a), "%s/a.txt", d);
    kfmt(b, sizeof(b), "%s/b.txt", d);
    kfmt(c, sizeof(c), "%s/c.txt", d);
    write_text(a, "AAA");
    EXPECT_RET(mos_rename(a, b), SYS_SUCCESS, "rename_file");
    long n = read_text(b, buf, sizeof(buf));
    EXPECT(!exists(a) && exists(b) && n == 3 && streq(buf, "AAA"), "rename_file_old_gone_new_has_data",
           "old exists=%d new exists=%d read=%ld '%s'", exists(a), exists(b), n, buf);
    EXPECT(mos_stat(b, &st) == 0 && st.size == 3 && st.type == SYS_STAT_FILE, "rename_file_stat_new",
           "size=%llu", (unsigned long long)st.size);
    EXPECT_RET(mos_stat(a, &st), ERR(SYS_ERR_NOTFOUND), "rename_file_stat_old_notfound");
    EXPECT(dir_has(d, "b.txt") && !dir_has(d, "a.txt"), "rename_file_listing",
           "listing has b.txt=%d a.txt=%d", dir_has(d, "b.txt"), dir_has(d, "a.txt"));

    /* replace an existing file */
    write_text(c, "CCCCC");
    EXPECT_RET(mos_rename(c, b), SYS_SUCCESS, "rename_replaces_existing_file");
    n = read_text(b, buf, sizeof(buf));
    EXPECT(!exists(c) && n == 5 && streq(buf, "CCCCC"), "rename_replace_data", "c exists=%d read=%ld '%s'",
           exists(c), n, buf);
    EXPECT(mos_stat(b, &st) == 0 && st.size == 5, "rename_replace_stat", "size=%llu", (unsigned long long)st.size);

    /* a bigger file replaces a smaller one and the data is intact */
    char big[160];
    kfmt(big, sizeof(big), "%s/big.bin", d);
    write_pattern(big, 9000, 3);
    EXPECT_RET(mos_rename(big, b), SYS_SUCCESS, "rename_big_over_small");
    EXPECT(verify_pattern(b, 9000, 3) && !exists(big), "rename_big_over_small_data", "content wrong or old name left");

    /* into a directory */
    char d1[160], d1b[160];
    kfmt(d1, sizeof(d1), "%s/d1", d);
    kfmt(d1b, sizeof(d1b), "%s/d1/b.txt", d);
    mos_mkdir(d1);
    write_text(b, "BBBB");
    EXPECT_RET(mos_rename(b, d1b), SYS_SUCCESS, "rename_file_into_directory");
    n = read_text(d1b, buf, sizeof(buf));
    EXPECT(!exists(b) && n == 4 && streq(buf, "BBBB"), "rename_into_directory_data", "old exists=%d read=%ld '%s'",
           exists(b), n, buf);
    EXPECT(dir_has(d1, "b.txt") && !dir_has(d, "b.txt"), "rename_into_directory_listing", "d1 has=%d ren has=%d",
           dir_has(d1, "b.txt"), dir_has(d, "b.txt"));

    /* out of a directory, with a new name, replacing */
    write_text(a, "old");
    EXPECT_RET(mos_rename(d1b, a), SYS_SUCCESS, "rename_file_out_of_directory_replacing");
    n = read_text(a, buf, sizeof(buf));
    EXPECT(!exists(d1b) && n == 4 && streq(buf, "BBBB"), "rename_out_of_directory_data", "read=%ld '%s'", n, buf);

    /* directory, same parent; it keeps its contents */
    char d2[160], d2f[160];
    kfmt(d2, sizeof(d2), "%s/d2", d);
    kfmt(d2f, sizeof(d2f), "%s/d2/in.txt", d);
    write_text(d1b, "inside");
    EXPECT_RET(mos_rename(d1, d2), SYS_SUCCESS, "rename_directory");
    EXPECT(is_dir(d2) && !exists(d1) && dir_has(d, "d2") && !dir_has(d, "d1"), "rename_directory_listing",
           "d2 is dir=%d d1 exists=%d", is_dir(d2), exists(d1));
    char moved[160];
    kfmt(moved, sizeof(moved), "%s/d2/b.txt", d);
    n = read_text(moved, buf, sizeof(buf));
    EXPECT(n == 6 && streq(buf, "inside"), "rename_directory_keeps_contents", "read=%ld '%s'", n, buf);
    EXPECT(mos_stat(d2, &st) == 0 && st.type == SYS_STAT_DIR, "rename_directory_stat", "type=%u", (unsigned)st.type);
    write_text(d2f, "new in renamed dir");
    EXPECT(exists(d2f), "rename_directory_can_create_inside", "file not created in the renamed directory");

    /* directory into another directory */
    char d3[160], d3d2[160], d3d2f[160];
    kfmt(d3, sizeof(d3), "%s/d3", d);
    kfmt(d3d2, sizeof(d3d2), "%s/d3/d2", d);
    kfmt(d3d2f, sizeof(d3d2f), "%s/d3/d2/in.txt", d);
    mos_mkdir(d3);
    EXPECT_RET(mos_rename(d2, d3d2), SYS_SUCCESS, "rename_directory_into_directory");
    EXPECT(is_dir(d3d2) && exists(d3d2f) && !exists(d2), "rename_directory_into_directory_contents",
           "dir=%d file=%d old=%d", is_dir(d3d2), exists(d3d2f), exists(d2));

    /* errors */
    char missing[160], missing2[160];
    kfmt(missing, sizeof(missing), "%s/missing", d);
    kfmt(missing2, sizeof(missing2), "%s/missing2", d);
    EXPECT_RET(mos_rename(missing, missing2), ERR(SYS_ERR_NOTFOUND), "rename_missing_source_notfound");
    EXPECT_RET(mos_rename(NULL, a), ERR(SYS_ERR_INVAL), "rename_null_from_inval");
    EXPECT_RET(mos_rename(a, NULL), ERR(SYS_ERR_INVAL), "rename_null_to_inval");
    EXPECT(exists(a), "rename_errors_leave_source", "source vanished");

    char e1[160], e2[160];
    kfmt(e1, sizeof(e1), "%s/e1", d);
    kfmt(e2, sizeof(e2), "%s/e2", d);
    mos_mkdir(e1);
    mos_mkdir(e2);
    EXPECT_RET(mos_rename(a, e1), ERR(SYS_ERR_GENERIC), "rename_file_onto_directory_is_error");
    EXPECT(exists(a) && is_dir(e1), "rename_file_onto_directory_left_alone", "a=%d e1 dir=%d", exists(a), is_dir(e1));
    EXPECT_RET(mos_rename(e1, e2), ERR(SYS_ERR_GENERIC), "rename_directory_onto_directory_is_error");
    EXPECT_RET(mos_rename(e1, a), ERR(SYS_ERR_GENERIC), "rename_directory_onto_file_is_error");
    EXPECT(is_dir(e1) && is_dir(e2) && exists(a), "rename_dir_errors_left_alone", "e1=%d e2=%d a=%d",
           is_dir(e1), is_dir(e2), exists(a));
    char e1in[160], e1deep[160];
    kfmt(e1in, sizeof(e1in), "%s/e1/in", d);
    kfmt(e1deep, sizeof(e1deep), "%s/e1/in/deeper", d);
    mos_mkdir(e1in);
    EXPECT_RET(mos_rename(e1, e1in), ERR(SYS_ERR_GENERIC), "rename_directory_into_itself_is_error");
    EXPECT_RET(mos_rename(e1, e1deep), ERR(SYS_ERR_GENERIC), "rename_directory_into_its_subtree_is_error");
    EXPECT(is_dir(e1) && is_dir(e1in), "rename_into_itself_left_alone", "e1=%d in=%d", is_dir(e1), is_dir(e1in));
    char nodest[160];
    kfmt(nodest, sizeof(nodest), "%s/no_such_dir/x.txt", d);
    EXPECT_RET(mos_rename(a, nodest), ERR(SYS_ERR_GENERIC), "rename_into_missing_directory_is_error");
    char viafile[160];
    kfmt(viafile, sizeof(viafile), "%s/a.txt/x", d);
    EXPECT_RET(mos_rename(e2, viafile), ERR(SYS_ERR_GENERIC), "rename_into_a_file_is_error");
    { long rr = mos_rename("0:/", TMP "/rootmove");
      EXPECT(rr < 0 && !exists(TMP "/rootmove"), "rename_drive_root_is_error", "returned %ld (%s)", rr, ename(rr)); }

    /* same path: undocumented in the header; MinimaFS treats it as a no-op */
    EXPECT_RET(mos_rename(a, a), SYS_SUCCESS, "rename_onto_itself_is_noop");
    EXPECT(exists(a), "rename_onto_itself_keeps_file", "file gone");

    /* long data survives a rename and a following rewrite */
    char p1[160], p2[160];
    kfmt(p1, sizeof(p1), "%s/p1.bin", d);
    kfmt(p2, sizeof(p2), "%s/p2.bin", d);
    write_pattern(p1, 20000, 9);
    mos_rename(p1, p2);
    long fd = mos_open(p2, SYS_O_RDWR);
    long w = fd >= 0 ? mos_fwrite(fd, "ZZ", 2) : -1;
    if (fd >= 0) mos_close(fd);
    n = read_text(p2, buf, 3);
    EXPECT(fd >= 0 && w == 2 && n == 2 && buf[0] == 'Z' && mos_file_size(p2) == 20000, "rename_then_write_through_new_name",
           "open=%ld write=%ld read=%ld size=%ld", fd, w, n, mos_file_size(p2));
}

/* ======================================================= cwd and paths */

static void getcwd_is(const char* want, const char* name) {
    char cwd[512];
    long r = mos_getcwd(cwd, sizeof(cwd));
    EXPECT(r == 0 && streq(cwd, want), name, "getcwd=%ld '%s', expected '%s'", r, cwd, want);
}

static void cwd_thread(void* arg) {
    char* out = (char*)arg;
    if (mos_getcwd(out, 256) != 0) out[0] = '\0';
}

static void test_cwd(void) {
    stage("cwd");
    char cwd[512];
    EXPECT(mos_getcwd(g_start_cwd, sizeof(g_start_cwd)) == 0 && g_start_cwd[0], "cwd_initial_is_terminal_cwd",
           "getcwd failed");
    emit_fd(1, "OS0TEST info start cwd=%s", g_start_cwd);

    mos_mkdir(TMP "/cw");
    mos_mkdir(TMP "/cw/sub");
    EXPECT_RET(mos_chdir(TMP "/cw"), SYS_SUCCESS, "chdir_absolute");
    getcwd_is(TMP "/cw", "getcwd_after_chdir");

    /* relative paths */
    EXPECT(write_text("rel.txt", "R") == 0, "relative_create", "could not create rel.txt in the working directory");
    EXPECT(exists(TMP "/cw/rel.txt"), "relative_create_lands_in_cwd", "0:/os0test_tmp/cw/rel.txt missing");
    EXPECT(exists("rel.txt") && exists("./rel.txt") && exists("sub/../rel.txt"), "relative_exists_forms",
           "rel=%d dot=%d dotdot=%d", exists("rel.txt"), exists("./rel.txt"), exists("sub/../rel.txt"));
    EXPECT(exists("../cw/rel.txt") && exists("../../os0test_tmp/cw/rel.txt"), "relative_dotdot_forms", "..-paths wrong");
    EXPECT(!exists("sub/rel.txt") && !exists("nope.txt"), "relative_missing", "reports files that are not there");
    EXPECT_RET(mos_mkdir("made"), SYS_SUCCESS, "relative_mkdir");
    EXPECT(is_dir(TMP "/cw/made") && is_dir("made") && !is_dir("rel.txt"), "relative_is_dir", "is_dir wrong");
    syscall_stat_t st;
    EXPECT(mos_stat("rel.txt", &st) == 0 && st.size == 1 && st.type == SYS_STAT_FILE, "relative_stat",
           "stat failed or wrong size");
    EXPECT(mos_stat("sub", &st) == 0 && st.type == SYS_STAT_DIR, "relative_stat_dir", "stat of sub failed");
    EXPECT_RET(mos_rename("rel.txt", "sub/moved.txt"), SYS_SUCCESS, "relative_rename");
    EXPECT(exists(TMP "/cw/sub/moved.txt") && !exists("rel.txt"), "relative_rename_result", "rename went elsewhere");
    long fd = mos_open("sub/moved.txt", SYS_O_RDONLY);
    char c = 0;
    long n = fd >= 0 ? mos_read(fd, &c, 1) : -1;
    if (fd >= 0) mos_close(fd);
    EXPECT(fd >= 0 && n == 1 && c == 'R', "relative_open_read", "open=%ld read=%ld", fd, n);
    EXPECT_RET(mos_delete("sub/moved.txt"), SYS_SUCCESS, "relative_delete");
    EXPECT_RET(mos_rmdir("made"), SYS_SUCCESS, "relative_rmdir");
    EXPECT(dir_has(".", "sub") && dir_has("", "sub"), "relative_listdir_dot_and_empty", "listing of '.' or '' lacks sub");

    /* chdir with relative paths, and the drive-root forms */
    EXPECT_RET(mos_chdir("sub"), SYS_SUCCESS, "chdir_relative");
    getcwd_is(TMP "/cw/sub", "getcwd_after_relative_chdir");
    EXPECT_RET(mos_chdir(".."), SYS_SUCCESS, "chdir_dotdot");
    getcwd_is(TMP "/cw", "getcwd_after_dotdot");
    EXPECT_RET(mos_chdir("../.."), SYS_SUCCESS, "chdir_dotdot_twice");
    getcwd_is("0:/", "getcwd_is_drive_root");
    EXPECT_RET(mos_chdir("../../.."), SYS_SUCCESS, "chdir_dotdot_above_root_stays_at_root");
    getcwd_is("0:/", "getcwd_dotdot_above_root");
    EXPECT_RET(mos_chdir("os0test_tmp/cw"), SYS_SUCCESS, "chdir_relative_from_root");
    getcwd_is(TMP "/cw", "getcwd_after_relative_from_root");

    /* "/" is the root of the working directory's drive, "0:/" names it */
    EXPECT_RET(mos_chdir("/"), SYS_SUCCESS, "chdir_slash");
    getcwd_is("0:/", "getcwd_after_chdir_slash");
    EXPECT_RET(mos_chdir("/os0test_tmp/cw/sub"), SYS_SUCCESS, "chdir_slash_absolute");
    getcwd_is(TMP "/cw/sub", "getcwd_after_chdir_slash_absolute");
    EXPECT(exists("/os0test_tmp/cw") && exists("0:/os0test_tmp/cw") && exists("../../cw") ,
           "slash_vs_drive_forms_agree", "forms disagree");
    EXPECT_RET(mos_chdir("0:/"), SYS_SUCCESS, "chdir_drive_root");
    getcwd_is("0:/", "getcwd_after_chdir_drive_root");
    EXPECT_RET(mos_chdir(TMP "/cw/"), SYS_SUCCESS, "chdir_trailing_slash");
    getcwd_is(TMP "/cw", "getcwd_trailing_slash_dropped");
    EXPECT_RET(mos_chdir("./sub/./"), SYS_SUCCESS, "chdir_dot_components");
    getcwd_is(TMP "/cw/sub", "getcwd_dot_components_dropped");

    /* failures leave the directory alone */
    EXPECT_RET(mos_chdir("0:/os0test_tmp/size_1.bin"), ERR(SYS_ERR_NOTFOUND), "chdir_to_file_notfound");
    EXPECT_RET(mos_chdir("no_such_dir"), ERR(SYS_ERR_NOTFOUND), "chdir_missing_notfound");
    EXPECT_RET(mos_chdir(NULL), ERR(SYS_ERR_INVAL), "chdir_null_inval");
    static char longpath[1200];
    for (int i = 0; i < 1190; i++) longpath[i] = 'x';
    longpath[0] = '0'; longpath[1] = ':'; longpath[2] = '/';
    longpath[1190] = '\0';
    EXPECT_RET(mos_chdir(longpath), ERR(SYS_ERR_INVAL), "chdir_too_long_inval");
    getcwd_is(TMP "/cw/sub", "cwd_unchanged_after_failed_chdirs");

    /* getcwd buffer rules */
    char tiny[4];
    EXPECT_RET(mos_getcwd(tiny, sizeof(tiny)), ERR(SYS_ERR_INVAL), "getcwd_small_buffer_inval");
    EXPECT_RET(mos_getcwd(NULL, 64), ERR(SYS_ERR_INVAL), "getcwd_null_inval");
    EXPECT_RET(mos_getcwd(cwd, 0), ERR(SYS_ERR_INVAL), "getcwd_zero_size_inval");
    n = mos_getcwd(cwd, slen(TMP "/cw/sub") + 1);
    EXPECT(n == 0 && streq(cwd, TMP "/cw/sub"), "getcwd_exact_fit", "getcwd=%ld '%s'", n, cwd);

    /* the working directory is the program's, shared by its threads */
    static char thread_cwd[256];
    thread_cwd[0] = '\0';
    mos_thread_t t;
    int tc = mos_thread_create_ex(&t, cwd_thread, thread_cwd, 0, NULL);
    if (tc == 0) mos_thread_join(&t);
    EXPECT(tc == 0 && streq(thread_cwd, TMP "/cw/sub"), "cwd_shared_with_threads", "thread saw '%s'", thread_cwd);

    /* back to the start directory */
    EXPECT_RET(mos_chdir(g_start_cwd), SYS_SUCCESS, "chdir_back_to_start");
    getcwd_is(g_start_cwd, "getcwd_back_at_start");
}

/* ============================================================ environment */

static void test_env(void) {
    stage("env");
    EXPECT(environ != NULL, "environ_not_null", "environ is NULL");
    int terminated = 0;
    if (environ) { for (int i = 0; i < 100000; i++) if (!environ[i]) { terminated = 1; break; } }
    EXPECT(terminated, "environ_null_terminated", "no NULL terminator found");
    EXPECT(mos_getenv("OS0_DEFINITELY_NOT_SET") == NULL, "getenv_missing_is_null", "found a value");
    EXPECT(mos_getenv("") == NULL && mos_getenv(NULL) == NULL, "getenv_empty_and_null_name", "returned a value");

    /* lookups against a made-up environment */
    static char e0[] = "OS0_AB=1";
    static char e1[] = "OS0_A=x";
    static char e2[] = "OS0_EMPTY=";
    static char e3[] = "OS0_EQ=a=b";
    static char e4[] = "NOEQUALS";
    static char* fake[] = { e0, e1, e2, e3, e4, NULL };
    char** saved = environ;
    environ = fake;
    char* v;
    v = mos_getenv("OS0_A");
    EXPECT(v && streq(v, "x"), "getenv_exact_name_not_prefix", "got '%s'", v);
    v = mos_getenv("OS0_AB");
    EXPECT(v && streq(v, "1"), "getenv_longer_name", "got '%s'", v);
    v = mos_getenv("OS0_EMPTY");
    EXPECT(v && v[0] == '\0', "getenv_empty_value", "got %s", v ? "non-empty" : "NULL");
    v = mos_getenv("OS0_EQ");
    EXPECT(v && streq(v, "a=b"), "getenv_value_with_equals", "got '%s'", v);
    EXPECT(mos_getenv("OS0") == NULL && mos_getenv("OS0_") == NULL && mos_getenv("OS0_A=x") == NULL &&
           mos_getenv("NOEQUALS") == NULL && mos_getenv("OS0_ABC") == NULL,
           "getenv_no_partial_matches", "a partial name matched");
    environ = NULL;
    EXPECT(mos_getenv("OS0_A") == NULL, "getenv_null_environ", "returned a value");
    environ = saved;
}

/* =========================================================== spawn / wait */

/* Starts `os0test.run --child <mode> [arg]` and waits for it. Returns the pid
 * (>0), or the spawn/wait error. */
static long spawn_child(const char* mode, const char* arg, const char* out_path, const char* err_path,
                        const char* const* envp, const char* cwd) {
    const char* argv[6];
    int n = 0;
    argv[n++] = g_self;
    argv[n++] = "--child";
    argv[n++] = mode;
    if (arg) argv[n++] = arg;
    argv[n] = NULL;
    mos_spawn_opts_t o;
    o.path = g_self;
    o.argv = argv;
    o.envp = envp;
    o.cwd = cwd;
    o.stdout_path = out_path;
    o.stderr_path = err_path;
    return mos_spawn_ex(&o);
}

static long spawn_wait(const char* mode, const char* arg, const char* out_path,
                       const char* err_path, const char* const* envp, const char* cwd,
                       mos_wait_result_t* res) {
    memset(res, 0, sizeof(*res));
    long pid = spawn_child(mode, arg, out_path, err_path, envp, cwd);
    if (pid <= 0) return pid;
    long w = mos_waitpid(pid, 0, res);
    return w == pid ? pid : w - 1000;      /* a wait error shows up as a big negative */
}

static void test_spawn_basic(void) {
    stage("spawn_basic");
    mos_wait_result_t r;

    /* exit codes: returned from main, and via mos_exit() */
    static const int codes[] = { 0, 42, 255 };
    for (size_t i = 0; i < 3; i++) {
        char arg[8], name[48];
        kfmt(arg, sizeof(arg), "%d", codes[i]);
        long pid = spawn_child("exit", arg, NULL, NULL, NULL, NULL);
        long w = pid > 0 ? mos_waitpid(pid, 0, &r) : pid;
        kfmt(name, sizeof(name), "spawn_wait_exit_code_%d", codes[i]);
        EXPECT(pid > 0 && w == pid && r.exit_code == codes[i] && !r.crashed && !r.killed && r.pid == pid, name,
               "spawn=%ld wait=%ld exit=%d crashed=%d killed=%d", pid, w, r.exit_code, r.crashed, r.killed);
    }
    {
        long pid = spawn_child("exitcall", "17", NULL, NULL, NULL, NULL);
        long w = pid > 0 ? mos_waitpid(pid, 0, &r) : pid;
        EXPECT(pid > 0 && w == pid && r.exit_code == 17 && !r.crashed, "spawn_wait_mos_exit_code_17",
               "spawn=%ld wait=%ld exit=%d", pid, w, r.exit_code);
    }

    /* argv, environment, cwd, redirected stdout and stderr */
    mos_mkdir(TMP "/work");
    const char* argv[] = { g_self, "--child", "info", "hello world", "", "--x=1", NULL };
    const char* envp[] = { "OS0_A=alpha", "OS0_B=", "OS0_C=a=b=c", NULL };
    mos_spawn_opts_t o = { g_self, argv, envp, TMP "/work", TMP "/out.txt", TMP "/err.txt" };
    write_text(TMP "/out.txt", "OLD CONTENT THAT MUST BE REPLACED\n");
    long pid = mos_spawn_ex(&o);
    EXPECT(pid > 0, "spawn_info_started", "spawn returned %ld (%s)", pid, ename(pid));
    long w = pid > 0 ? mos_waitpid(pid, 0, &r) : -1;
    EXPECT(w == pid && r.exit_code == 7 && !r.crashed && !r.killed, "spawn_info_exit_7",
           "wait=%ld exit=%d crashed=%d killed=%d vector=%u", w, r.exit_code, r.crashed, r.killed, r.crash_vector);
    static char out[4096], err[512];
    long on = read_text(TMP "/out.txt", out, sizeof(out));
    long en = read_text(TMP "/err.txt", err, sizeof(err));
    EXPECT(on > 0, "spawn_stdout_file_written", "read %ld bytes", on);
    EXPECT(line_index(out, "OLD CONTENT THAT MUST BE REPLACED") < 0, "spawn_stdout_file_replaced", "old content still there");
    char want[300];
    kfmt(want, sizeof(want), "argv[0]=%s", g_self);
    EXPECT(has_line(out, "argc=6"), "spawn_child_argc", "output:\n%s", out);
    EXPECT(has_line(out, want), "spawn_child_argv0", "want '%s' in:\n%s", want, out);
    EXPECT(has_line(out, "argv[1]=--child") && has_line(out, "argv[2]=info"), "spawn_child_argv_1_2", "output:\n%s", out);
    EXPECT(has_line(out, "argv[3]=hello world"), "spawn_child_argv_with_space", "output:\n%s", out);
    EXPECT(has_line(out, "argv[4]="), "spawn_child_argv_empty_string", "output:\n%s", out);
    EXPECT(has_line(out, "argv[5]=--x=1"), "spawn_child_argv_with_equals", "output:\n%s", out);
    EXPECT(has_line(out, "argv_null=1"), "spawn_child_argv_null_terminated", "output:\n%s", out);
    EXPECT(has_line(out, "envp_is_environ=1"), "spawn_child_envp_third_arg_equals_environ", "output:\n%s", out);
    EXPECT(has_line(out, "environ_count=3"), "spawn_child_environ_count", "output:\n%s", out);
    EXPECT(has_line(out, "getenv.OS0_A=[alpha]") && has_line(out, "getenv.OS0_B=[]") &&
           has_line(out, "getenv.OS0_C=[a=b=c]"), "spawn_child_getenv_values", "output:\n%s", out);
    EXPECT(has_line(out, "getenv.OS0_NOPE=(null)") && has_line(out, "getenv.OS0_=(null)") &&
           has_line(out, "getenv.OS0=(null)") && has_line(out, "getenv.OS0_INH=(null)"),
           "spawn_child_getenv_missing", "output:\n%s", out);
    EXPECT(has_line(out, "cwd=" TMP "/work"), "spawn_child_cwd", "output:\n%s", out);
    EXPECT(en > 0 && has_line(err, "stderr-line-from-child") && line_index(out, "stderr-line") < 0,
           "spawn_stderr_separate_file", "stderr file (%ld bytes): '%s'", en, err);

    /* same file for stdout and stderr: one stream, in order */
    long pid2 = spawn_child("streams", NULL, TMP "/both.txt", TMP "/both.txt", NULL, NULL);
    long w2 = pid2 > 0 ? mos_waitpid(pid2, 0, &r) : -1;
    static char both[256];
    read_text(TMP "/both.txt", both, sizeof(both));
    long i1 = line_index(both, "out1"), i2 = line_index(both, "err1"), i3 = line_index(both, "out2");
    EXPECT(pid2 > 0 && w2 == pid2 && r.exit_code == 0 && i1 >= 0 && i2 > i1 && i3 > i2, "spawn_stdout_stderr_same_file_in_order",
           "spawn=%ld wait=%ld exit=%d file: '%s'", pid2, w2, r.exit_code, both);

    /* nothing redirected: the child's output goes where ours goes. We only
     * check that it runs; the child prints nothing in this mode. */
    long pid3 = spawn_child("exit", "0", NULL, NULL, NULL, NULL);
    long w3 = pid3 > 0 ? mos_waitpid(pid3, 0, NULL) : -1;
    EXPECT(pid3 > 0 && w3 == pid3, "wait_with_null_result", "spawn=%ld wait=%ld", pid3, w3);

    /* a child that starts a child with nothing given: environment, working
     * directory and output are inherited */
    const char* renv[] = { "OS0_INH=yes", NULL };
    long pid4 = spawn_child("relay", NULL, TMP "/relay.txt", NULL, renv, TMP "/work");
    long w4 = pid4 > 0 ? mos_waitpid(pid4, 0, &r) : -1;
    static char relay[2048];
    read_text(TMP "/relay.txt", relay, sizeof(relay));
    EXPECT(pid4 > 0 && w4 == pid4 && r.exit_code == 0 && !r.crashed, "spawn_nested_relay_ok",
           "spawn=%ld wait=%ld exit=%d crashed=%d output:\n%s", pid4, w4, r.exit_code, r.crashed, relay);
    EXPECT(has_line(relay, "getenv.OS0_INH=[yes]") && has_line(relay, "environ_count=1"), "spawn_inherits_environment",
           "output:\n%s", relay);
    EXPECT(has_line(relay, "cwd=" TMP "/work"), "spawn_inherits_cwd", "output:\n%s", relay);
    EXPECT(has_line(relay, "argv[3]=inherit") && line_prefix_index(relay, "relay-done wait=") > line_index(relay, "argc=4"),
           "spawn_inherits_stdout_sink", "output:\n%s", relay);
}

static void test_spawn_crash_and_wait(void) {
    stage("spawn_wait");
    mos_wait_result_t r;
    static char out[256];

    /* a crash: CPU exception 6 (#UD) */
    long pid = spawn_child("crash", NULL, TMP "/crash.txt", NULL, NULL, NULL);
    long w = pid > 0 ? mos_waitpid(pid, 0, &r) : -1;
    read_text(TMP "/crash.txt", out, sizeof(out));
    EXPECT(pid > 0 && w == pid && r.crashed && !r.killed, "wait_reports_crash",
           "spawn=%ld wait=%ld crashed=%d killed=%d exit=%d", pid, w, r.crashed, r.killed, r.exit_code);
    EXPECT(r.crashed && r.crash_vector == 6, "wait_crash_vector_is_ud", "vector=%u", r.crash_vector);
    EXPECT(r.crashed && r.exit_code == 255, "wait_crash_exit_code_255", "exit=%d", r.exit_code);
    EXPECT(has_line(out, "before-crash"), "wait_crashed_child_output_flushed", "file: '%s'", out);

    /* waiting twice, and for a pid that was never a child */
    EXPECT_RET(mos_waitpid(pid, 0, NULL), ERR(SYS_ERR_NOTFOUND), "wait_twice_notfound");
    EXPECT_RET(mos_waitpid(pid, MOS_WAIT_NOHANG, NULL), ERR(SYS_ERR_NOTFOUND), "wait_nohang_after_wait_notfound");
    EXPECT_RET(mos_waitpid(999999, 0, NULL), ERR(SYS_ERR_NOTFOUND), "wait_unknown_pid_notfound");
    EXPECT_RET(mos_waitpid(1, 0, NULL), ERR(SYS_ERR_NOTFOUND), "wait_not_our_child_notfound");
    EXPECT_RET(mos_wait(NULL), ERR(SYS_ERR_NOTFOUND), "wait_any_without_children_notfound");
    EXPECT_RET(mos_waitpid(MOS_WAIT_ANY, MOS_WAIT_NOHANG, NULL), ERR(SYS_ERR_NOTFOUND),
               "wait_any_nohang_without_children_notfound");
    EXPECT_RET(mos_wait_raw(1, 2, NULL), ERR(SYS_ERR_INVAL), "wait_bad_flags_inval");
    EXPECT_RET(mos_wait_raw(1, 0, (syscall_wait_t*)(uintptr_t)8), ERR(SYS_ERR_INVAL), "wait_bad_result_pointer_inval");

    /* WAIT_NOHANG while the child runs, then a real wait */
    uint64_t t0 = mos_clock_monotonic_ns();
    pid = spawn_child("sleep", "400", NULL, NULL, NULL, NULL);
    long first = pid > 0 ? mos_waitpid(pid, MOS_WAIT_NOHANG, &r) : -1;
    EXPECT(pid > 0 && first == ERR(SYS_ERR_AGAIN), "wait_nohang_while_running_again", "spawn=%ld wait=%ld (%s)",
           pid, first, ename(first));
    long again = pid > 0 ? mos_waitpid(pid, MOS_WAIT_NOHANG, NULL) : -1;
    EXPECT(again == ERR(SYS_ERR_AGAIN), "wait_nohang_again_still_running", "wait=%ld (%s)", again, ename(again));
    w = pid > 0 ? mos_waitpid(pid, 0, &r) : -1;
    uint64_t ms = (mos_clock_monotonic_ns() - t0) / 1000000u;
    EXPECT(w == pid && r.exit_code == 3 && !r.crashed && ms >= 380, "wait_blocks_until_exit",
           "wait=%ld exit=%d after %llu ms", w, r.exit_code, (unsigned long long)ms);

    /* polling with NOHANG until it is done */
    pid = spawn_child("sleep", "60", NULL, NULL, NULL, NULL);
    long polled = -1;
    int spins = 0;
    for (; pid > 0 && spins < 2000; spins++) {
        polled = mos_waitpid(pid, MOS_WAIT_NOHANG, &r);
        if (polled != ERR(SYS_ERR_AGAIN)) break;
        mos_sleep(5);
    }
    EXPECT(pid > 0 && polled == pid && r.exit_code == 3 && spins > 2, "wait_nohang_polling_until_done",
           "pid=%ld polled=%ld exit=%d after %d polls", pid, polled, r.exit_code, spins);
    EXPECT_RET(mos_waitpid(pid, MOS_WAIT_NOHANG, NULL), ERR(SYS_ERR_NOTFOUND), "wait_nohang_consumed_the_child");

    /* a specific pid while an earlier child is still running */
    long slow = spawn_child("sleep", "300", NULL, NULL, NULL, NULL);
    long fast = spawn_child("exit", "21", NULL, NULL, NULL, NULL);
    long wf = (slow > 0 && fast > 0) ? mos_waitpid(fast, 0, &r) : -1;
    EXPECT(wf == fast && r.exit_code == 21, "wait_specific_pid_not_the_first_child", "slow=%ld fast=%ld wait=%ld exit=%d",
           slow, fast, wf, r.exit_code);
    long still = slow > 0 ? mos_waitpid(slow, MOS_WAIT_NOHANG, NULL) : -1;
    EXPECT(still == ERR(SYS_ERR_AGAIN) || still == slow, "wait_other_child_unaffected", "wait=%ld (%s)", still, ename(still));
    long ws = (still == ERR(SYS_ERR_AGAIN)) ? mos_waitpid(slow, 0, &r) : still;
    EXPECT(ws == slow && r.exit_code == 3, "wait_slow_child_afterwards", "wait=%ld exit=%d", ws, r.exit_code);

    /* SYS_WAIT_ANY with several children */
    long pids[4];
    static const char* const args[4] = { "11", "12", "13", "14" };
    int started = 0;
    for (int i = 0; i < 4; i++) {
        pids[i] = spawn_child("exit", args[i], NULL, NULL, NULL, NULL);
        if (pids[i] > 0) started++;
    }
    EXPECT(started == 4, "wait_any_children_started", "started %d of 4", started);
    int seen[4] = { 0, 0, 0, 0 };
    int mapping_ok = 1, collected = 0;
    for (int i = 0; i < started; i++) {
        mos_wait_result_t ar;
        long got = mos_wait(&ar);
        if (got <= 0) { mapping_ok = 0; break; }
        collected++;
        int idx = -1;
        for (int k = 0; k < 4; k++) if (pids[k] == got) idx = k;
        if (idx < 0 || seen[idx] || ar.exit_code != 11 + idx || ar.pid != got) mapping_ok = 0;
        else seen[idx] = 1;
    }
    EXPECT(collected == 4 && mapping_ok, "wait_any_collects_each_child_once", "collected %d, mapping ok=%d", collected, mapping_ok);
    EXPECT_RET(mos_wait(NULL), ERR(SYS_ERR_NOTFOUND), "wait_any_after_all_collected_notfound");

    /* any: a blocking wait wakes when a slow child finishes */
    pid = spawn_child("sleep", "150", NULL, NULL, NULL, NULL);
    t0 = mos_clock_monotonic_ns();
    long any = mos_wait(&r);
    ms = (mos_clock_monotonic_ns() - t0) / 1000000u;
    EXPECT(pid > 0 && any == pid && r.exit_code == 3 && ms >= 50, "wait_any_blocks_for_a_running_child",
           "pid=%ld wait=%ld exit=%d after %llu ms", pid, any, r.exit_code, (unsigned long long)ms);

    /* programs started with mos_exec are children too */
    long ep = mos_exec(g_self, (const char*[]){ "--child", "exit", "5" }, 3);
    mos_wait_result_t er;
    long ew = ep > 0 ? mos_waitpid(ep, 0, &er) : ep;
    EXPECT(ep > 0 && ew == ep && er.exit_code == 5, "wait_for_mos_exec_child", "exec=%ld wait=%ld exit=%d", ep, ew, er.exit_code);

    /* a burst of children */
    long burst[6];
    int up = 0;
    for (int i = 0; i < 6; i++) {
        char a[8];
        kfmt(a, sizeof(a), "%d", 30 + i);
        burst[i] = spawn_child("exit", a, NULL, NULL, NULL, NULL);
        if (burst[i] > 0) up++;
    }
    int sum = 0, got_n = 0;
    for (int i = 0; i < up; i++) {
        mos_wait_result_t br;
        if (mos_wait(&br) > 0) { sum += br.exit_code; got_n++; }
    }
    EXPECT(up == 6 && got_n == 6 && sum == 30 + 31 + 32 + 33 + 34 + 35, "spawn_burst_of_six", "up=%d collected=%d sum=%d", up, got_n, sum);
}

static void test_spawn_errors(void) {
    stage("spawn_errors");
    mos_spawn_opts_t o;
    memset(&o, 0, sizeof(o));
    EXPECT_RET(mos_spawn(TMP "/no_such_program.run", NULL), ERR(SYS_ERR_NOTFOUND), "spawn_missing_program_notfound");
    EXPECT_RET(mos_spawn(TMP, NULL), ERR(SYS_ERR_NOTFOUND), "spawn_directory_notfound");
    EXPECT_RET(mos_spawn(NULL, NULL), ERR(SYS_ERR_INVAL), "spawn_null_path_inval");
    EXPECT_RET(mos_spawn_raw(NULL), ERR(SYS_ERR_INVAL), "spawn_null_request_inval");

    syscall_spawn_t req;
    memset(&req, 0, sizeof(req));
    req.size = sizeof(req);
    req.path = g_self;
    req.flags = 1;
    EXPECT_RET(mos_spawn_raw(&req), ERR(SYS_ERR_INVAL), "spawn_nonzero_flags_inval");
    req.flags = 0;
    req.size = 8;
    EXPECT_RET(mos_spawn_raw(&req), ERR(SYS_ERR_INVAL), "spawn_small_size_inval");

    const char* argv[] = { g_self, "--child", "exit", "0", NULL };
    o.path = g_self;
    o.argv = argv;
    o.cwd = TMP "/no_such_dir";
    EXPECT_RET(mos_spawn_ex(&o), ERR(SYS_ERR_NOTFOUND), "spawn_cwd_missing_notfound");
    o.cwd = TMP "/size_1.bin";
    EXPECT_RET(mos_spawn_ex(&o), ERR(SYS_ERR_NOTFOUND), "spawn_cwd_is_file_notfound");
    o.cwd = NULL;
    o.stdout_path = TMP "/no_such_dir/out.txt";
    long r = mos_spawn_ex(&o);
    EXPECT(r < 0, "spawn_unwritable_stdout_path_fails", "returned %ld", r);
    EXPECT_RET(mos_wait(NULL), ERR(SYS_ERR_NOTFOUND), "spawn_failures_leave_no_children");

    /* Spawning a file that is not a program: any error, and nothing to wait for. */
    write_text(TMP "/notaprogram.run", "this is not a program\n");
    long np = mos_spawn(TMP "/notaprogram.run", NULL);
    long leftover = np > 0 ? mos_waitpid(np, 0, NULL) : 0;
    EXPECT(np < 0 || leftover > 0, "spawn_non_program_file", "returned %ld", np);
    if (np > 0) emit_fd(1, "OS0TEST info a plain text file was started as pid %ld (wait=%ld)", np, leftover);
}

/* ===================================================== user pointer checks */

static void test_bad_pointers(void) {
    stage("bad_pointers");
    syscall_stat_t st;
    static const uintptr_t bad[] = { 0x1000, 0x200000 };
    static const char* const names_stat[] = { "stat_path_low_pointer_inval", "stat_path_kernel_pointer_inval" };
    static const char* const names_from[] = { "rename_from_low_pointer_inval", "rename_from_kernel_pointer_inval" };
    static const char* const names_to[] = { "rename_to_low_pointer_inval", "rename_to_kernel_pointer_inval" };
    static const char* const names_xp[] = { "exec_path_low_pointer_inval", "exec_path_kernel_pointer_inval" };
    static const char* const names_xv[] = { "exec_argv_low_pointer_inval", "exec_argv_kernel_pointer_inval" };
    static const char* const names_xs[] = { "exec_argv_string_low_pointer_inval", "exec_argv_string_kernel_pointer_inval" };

    write_text(TMP "/ptr_a.txt", "A");
    for (int i = 0; i < 2; i++) {
        const char* p = (const char*)bad[i];
        EXPECT_RET(mos_stat(p, &st), ERR(SYS_ERR_INVAL), names_stat[i]);
        EXPECT_RET(mos_rename(p, TMP "/ptr_b.txt"), ERR(SYS_ERR_INVAL), names_from[i]);
        EXPECT_RET(mos_rename(TMP "/ptr_a.txt", p), ERR(SYS_ERR_INVAL), names_to[i]);
        EXPECT_RET(mos_exec(p, NULL, 0), ERR(SYS_ERR_INVAL), names_xp[i]);
        EXPECT_RET(mos_exec(g_self, (const char**)bad[i], 1), ERR(SYS_ERR_INVAL), names_xv[i]);
        const char* av[2] = { (const char*)bad[i], NULL };
        EXPECT_RET(mos_exec(g_self, av, 1), ERR(SYS_ERR_INVAL), names_xs[i]);
    }
    EXPECT(exists(TMP "/ptr_a.txt") && !exists(TMP "/ptr_b.txt"), "bad_pointer_rename_changed_nothing",
           "a=%d b=%d", exists(TMP "/ptr_a.txt"), exists(TMP "/ptr_b.txt"));
    /* argc > 0 with no argv */
    EXPECT_RET(mos_exec(g_self, NULL, 2), ERR(SYS_ERR_INVAL), "exec_argc_without_argv_inval");
}

/* ======================================== failed spawn leaves files alone */

static void test_spawn_failure_cleanup(void) {
    stage("spawn_failure_cleanup");
    static char buf[128];
    mos_spawn_opts_t o;
    memset(&o, 0, sizeof(o));
    o.path = TMP "/no_such_program.run";

    /* Missing program, stdout_path naming an existing file: untouched. */
    write_text(TMP "/keep_out.txt", "KEEP-OUT");
    write_text(TMP "/keep_err.txt", "KEEP-ERR");
    o.stdout_path = TMP "/keep_out.txt";
    o.stderr_path = TMP "/keep_err.txt";
    EXPECT_RET(mos_spawn_ex(&o), ERR(SYS_ERR_NOTFOUND), "spawn_fail_missing_program_with_sinks");
    long n = read_text(TMP "/keep_out.txt", buf, sizeof(buf));
    EXPECT(n == 8 && streq(buf, "KEEP-OUT"), "spawn_fail_keeps_existing_stdout_file", "read %ld: '%s'", n, buf);
    n = read_text(TMP "/keep_err.txt", buf, sizeof(buf));
    EXPECT(n == 8 && streq(buf, "KEEP-ERR"), "spawn_fail_keeps_existing_stderr_file", "read %ld: '%s'", n, buf);

    /* Fresh names: not created. */
    o.stdout_path = TMP "/fresh_out.txt";
    o.stderr_path = TMP "/fresh_err.txt";
    EXPECT_RET(mos_spawn_ex(&o), ERR(SYS_ERR_NOTFOUND), "spawn_fail_missing_program_fresh_sinks");
    EXPECT(!exists(TMP "/fresh_out.txt") && !exists(TMP "/fresh_err.txt"), "spawn_fail_creates_no_files",
           "out=%d err=%d", exists(TMP "/fresh_out.txt"), exists(TMP "/fresh_err.txt"));

    /* A real program, but stderr cannot be created: stdout untouched/not created. */
    const char* argv[] = { g_self, "--child", "exit", "0", NULL };
    o.path = g_self;
    o.argv = argv;
    o.stdout_path = TMP "/keep_out.txt";
    o.stderr_path = TMP "/no_such_dir/err.txt";
    long r = mos_spawn_ex(&o);
    EXPECT(r < 0, "spawn_fail_bad_stderr_path_fails", "returned %ld", r);
    n = read_text(TMP "/keep_out.txt", buf, sizeof(buf));
    EXPECT(n == 8 && streq(buf, "KEEP-OUT"), "spawn_fail_bad_stderr_keeps_stdout_file", "read %ld: '%s'", n, buf);
    o.stdout_path = TMP "/fresh_out2.txt";
    r = mos_spawn_ex(&o);
    EXPECT(r < 0 && !exists(TMP "/fresh_out2.txt"), "spawn_fail_bad_stderr_creates_no_stdout_file",
           "returned %ld, exists=%d", r, exists(TMP "/fresh_out2.txt"));

    /* stderr naming a directory: same. */
    mos_mkdir(TMP "/errdir");
    o.stdout_path = TMP "/keep_out.txt";
    o.stderr_path = TMP "/errdir";
    r = mos_spawn_ex(&o);
    n = read_text(TMP "/keep_out.txt", buf, sizeof(buf));
    EXPECT(r < 0 && n == 8 && streq(buf, "KEEP-OUT"), "spawn_fail_stderr_is_directory_keeps_stdout",
           "returned %ld, read %ld: '%s'", r, n, buf);
    EXPECT_RET(mos_wait(NULL), ERR(SYS_ERR_NOTFOUND), "spawn_fail_cleanup_leaves_no_children");
}

/* ============================================ SYS_EXEC and relative paths */

static void test_exec_relative(void) {
    stage("exec_relative");
    /* Split our own path into directory and file name. */
    char dir[512], base[128];
    size_t len = slen(g_self), cut = 0;
    for (size_t i = 0; i < len; i++) if (g_self[i] == '/') cut = i;
    if (cut == 0 || cut + 1 >= len || len - cut > sizeof(base)) {
        check_pass("exec_relative_path (skipped: unusual program path)");
        return;
    }
    for (size_t i = 0; i < cut; i++) dir[i] = g_self[i];
    dir[cut] = '\0';
    scopy(base, sizeof(base), g_self + cut + 1);

    /* In an unrelated directory, a relative name that is nowhere fails. */
    EXPECT_RET(mos_chdir(TMP), SYS_SUCCESS, "exec_relative_chdir_elsewhere");
    long r = mos_exec("os0test_no_such_program.run", NULL, 0);
    EXPECT(r < 0, "exec_relative_missing_fails", "returned %ld", r);

    /* In the program's own directory, the bare name starts it. */
    mos_wait_result_t res;
    EXPECT_RET(mos_chdir(dir), SYS_SUCCESS, "exec_relative_chdir_to_program_dir");
    const char* av[] = { "--child", "exit", "5" };
    long pid = mos_exec(base, av, 3);
    long w = pid > 0 ? mos_waitpid(pid, 0, &res) : -1;
    EXPECT(pid > 0 && w == pid && res.exit_code == 5 && !res.crashed, "exec_relative_name_uses_program_cwd",
           "exec=%ld wait=%ld exit=%d", pid, w, res.exit_code);
    pid = mos_exec("./", NULL, 0);
    EXPECT(pid < 0, "exec_relative_directory_fails", "returned %ld", pid);
    EXPECT_RET(mos_chdir(g_start_cwd), SYS_SUCCESS, "exec_relative_chdir_back");
}

/* ================================================================== main */

int main(int argc, char** argv, char** envp) {
    if (argc >= 3 && streq(argv[1], "--child")) return child_main(argc, argv, envp);

    emit_fd(1, "OS0TEST begin");
    stage("setup");

    /* Our own absolute path, for spawning ourselves. */
    if (argc >= 1 && argv[0] && argv[0][0]) {
        const char* a0 = argv[0];
        int has_colon = 0;
        for (const char* p = a0; *p; p++) if (*p == ':') has_colon = 1;
        if (has_colon || a0[0] == '/') {
            scopy(g_self, sizeof(g_self), a0);
        } else {
            char cwd[400];
            if (mos_getcwd(cwd, sizeof(cwd)) == 0) kfmt(g_self, sizeof(g_self), "%s/%s", cwd, a0);
            else scopy(g_self, sizeof(g_self), a0);
        }
    } else {
        scopy(g_self, sizeof(g_self), "0:/programs/os0test.run");
    }
    emit_fd(1, "OS0TEST info self=%s argc=%d", g_self, argc);

    mos_thread_t dog;
    mos_thread_create(&dog, watchdog, NULL, 16 * 1024);

    rm_rf(TMP);
    EXPECT_RET(mos_mkdir(TMP), SYS_SUCCESS, "setup_scratch_directory");

    test_clocks();
    test_env();
    test_futex();
    test_tls();
    test_stat();
    test_rename();
    test_cwd();
    test_spawn_basic();
    test_tsc();
    test_spawn_crash_and_wait();
    test_spawn_errors();
    test_bad_pointers();
    test_spawn_failure_cleanup();
    test_exec_relative();

    stage("cleanup");
    mos_chdir(g_start_cwd[0] ? g_start_cwd : "0:/");
    rm_rf(TMP);
    EXPECT(!exists(TMP), "cleanup_scratch_directory", "%s is still there", TMP);

    g_finished = 1;
    emit_fd(1, "OS0TEST summary: %d checks, %d failed", g_checks, g_failed);
    if (g_failed == 0) {
        emit_fd(1, "OS0TEST PASS");
        mos_log("OS0TEST PASS");
        return 0;
    }
    emit_fd(1, "OS0TEST FAIL");
    mos_log("OS0TEST FAIL");
    return 1;
}
