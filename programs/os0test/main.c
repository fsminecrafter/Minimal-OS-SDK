/*
 * os0test - self-test for the OS-0 system calls.
 *
 *     ./build.sh programs/os0test        -> os0test.run
 *     run 0:/programs/os0test.run        (from the terminal)
 *
 * Covers SYS_CLOCK, SYS_THREAD (FS base / TLS, exit futex), SYS_FUTEX,
 * SYS_STAT, SYS_RENAME, SYS_CHDIR/SYS_GETCWD, the environment (envp ->
 * environ), SYS_SPAWN and SYS_WAIT, and SYS_VM / the user address-space
 * layout (vm.h): mappings, protection, guard pages, NX, stack sizes, and
 * SIMD: the CPU feature report (SYS_SYSINFO) and a torture test that keeps
 * xmm/ymm/x87/MXCSR state intact across context switches, syscalls and page
 * faults (the AVX half runs only when the kernel reports AVX usable;
 * otherwise an AVX instruction must die with #UD).
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
#include "vm.h"
#include "user_sysinfo.h"

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

/* ---------------------------------------------------------------- VM children
 * Everything that is supposed to crash runs in a child, as
 * `--child vm_<mode>`. A child that survives its fault prints a line and
 * exits 77, so the parent's report tells "no crash" from "wrong crash". */

#define VM_PAGE      4096u
#define VM_RW        MOS_VM_PROT_RW

typedef int (*vm_stub_fn)(void);
static const uint8_t k_stub42[] = { 0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3 };   /* mov eax,42 ; ret */

static int vm_survived(const char* mode) {
    emit_fd(1, "vm child %s: still alive after the access", mode);
    return 77;
}

static void vm_copy_stub(volatile uint8_t* dst) {
    for (size_t i = 0; i < sizeof(k_stub42); i++) dst[i] = k_stub42[i];
}

/* About 4 KiB of stack per level; volatile so none of it is optimised away. */
static __attribute__((noinline)) unsigned long vm_burn(unsigned depth) {
    volatile unsigned char buf[4000];
    buf[0] = (unsigned char)depth;
    buf[sizeof(buf) - 1] = (unsigned char)(depth * 3u);
    unsigned long below = depth ? vm_burn(depth - 1) : 0;
    return below + buf[0] + buf[sizeof(buf) - 1];
}

static unsigned long vm_burn_expect(unsigned depth) {
    unsigned long sum = 0;
    for (unsigned d = 0; d <= depth; d++) sum += (unsigned char)d + (unsigned char)(d * 3u);
    return sum;
}

static void vm_burn_thread(void* arg) { (void)vm_burn((unsigned)(uintptr_t)arg); }

static int child_vm(const char* mode, long arg) {
    volatile uint8_t* p;
    if (streq(mode, "vm_hole")) {
        p = (volatile uint8_t*)mos_vm_map(NULL, 3 * VM_PAGE, VM_RW, 0);
        if (!p) return 90;
        p[0] = 1; p[VM_PAGE] = 2; p[2 * VM_PAGE] = 3;
        if (mos_vm_unmap((void*)(p + VM_PAGE), VM_PAGE) != 0) return 91;
        if (p[0] != 1 || p[2 * VM_PAGE] != 3) return 92;      /* the pieces are fine */
        (void)*(volatile uint8_t*)(p + VM_PAGE + 5);          /* the hole is not */
        return vm_survived(mode);
    }
    if (streq(mode, "vm_unmapped")) {
        p = (volatile uint8_t*)mos_vm_map(NULL, VM_PAGE, VM_RW, 0);
        if (!p) return 90;
        p[0] = 1;
        if (mos_vm_unmap((void*)p, VM_PAGE) != 0) return 91;
        (void)*p;
        return vm_survived(mode);
    }
    if (streq(mode, "vm_ro_write")) {
        p = (volatile uint8_t*)mos_vm_map(NULL, VM_PAGE, VM_RW, 0);
        if (!p) return 90;
        p[0] = 5;
        if (mos_vm_protect((void*)p, VM_PAGE, MOS_VM_PROT_READ) != 0) return 91;
        if (p[0] != 5) return 92;                              /* reading still works */
        p[0] = 6;                                              /* writing does not */
        return vm_survived(mode);
    }
    if (streq(mode, "vm_none_read") || streq(mode, "vm_none_write")) {
        p = (volatile uint8_t*)mos_vm_map(NULL, VM_PAGE, VM_RW, 0);
        if (!p) return 90;
        p[0] = 5;
        if (mos_vm_protect((void*)p, VM_PAGE, MOS_VM_PROT_NONE) != 0) return 91;
        if (streq(mode, "vm_none_read")) (void)*p; else p[0] = 6;
        return vm_survived(mode);
    }
    if (streq(mode, "vm_guard_before") || streq(mode, "vm_guard_after")) {
        /* 5000 bytes: two pages. The rest of the second page is ours. */
        p = (volatile uint8_t*)mos_vm_map(NULL, 5000, VM_RW, 0);
        if (!p) return 90;
        p[0] = 1; p[5000] = 2; p[2 * VM_PAGE - 1] = 3;
        if (streq(mode, "vm_guard_before")) (void)*(p - 1);
        else (void)*(p + 2 * VM_PAGE);
        return vm_survived(mode);
    }
    if (streq(mode, "vm_nx_data")) {
        p = (volatile uint8_t*)mos_vm_map(NULL, VM_PAGE, VM_RW, 0);
        if (!p) return 90;
        vm_copy_stub(p);
        int v = ((vm_stub_fn)(void*)p)();
        emit_fd(1, "vm child %s: the stub ran and returned %d", mode, v);
        return 77;
    }
    if (streq(mode, "vm_nx_stack")) {
        uint8_t code[32] __attribute__((aligned(16)));
        vm_copy_stub((volatile uint8_t*)code);
        __asm__ volatile("" ::: "memory");
        int v = ((vm_stub_fn)(void*)code)();
        emit_fd(1, "vm child %s: the stub ran and returned %d", mode, v);
        return 77;
    }
    if (streq(mode, "vm_nx_heap")) {
        uint8_t* h = (uint8_t*)malloc(256);
        if (!h) return 90;
        vm_copy_stub(h);
        int v = ((vm_stub_fn)(void*)h)();
        emit_fd(1, "vm child %s: the stub ran and returned %d", mode, v);
        return 77;
    }
    if (streq(mode, "vm_exec")) {
        /* W^X flow: write while RW, run while RX, edit again. */
        uint8_t* q = (uint8_t*)mos_vm_map(NULL, VM_PAGE, VM_RW, 0);
        if (!q) return 90;
        vm_copy_stub(q);
        if (mos_vm_protect(q, VM_PAGE, MOS_VM_PROT_RX) != 0) return 91;
        if (((vm_stub_fn)(void*)q)() != 42) return 92;
        if (mos_vm_protect(q, VM_PAGE, VM_RW) != 0) return 93;
        q[1] = 43;
        if (mos_vm_protect(q, VM_PAGE, MOS_VM_PROT_RX) != 0) return 94;
        if (((vm_stub_fn)(void*)q)() != 43) return 95;
        /* Writable and executable at once, straight from the map call. */
        uint8_t* w = (uint8_t*)mos_vm_map(NULL, VM_PAGE, VM_RW | MOS_VM_PROT_EXEC, 0);
        if (!w) return 96;
        vm_copy_stub(w);
        if (((vm_stub_fn)(void*)w)() != 42) return 97;
        /* PROT_EXEC alone implies readable. */
        uint8_t* x = (uint8_t*)mos_vm_map(NULL, VM_PAGE, VM_RW, 0);
        if (!x) return 98;
        vm_copy_stub(x);
        if (mos_vm_protect(x, VM_PAGE, MOS_VM_PROT_EXEC) != 0) return 99;
        if (x[0] != 0xB8) return 100;
        return 0;
    }
    if (streq(mode, "vm_stack_use")) {
        unsigned depth = (unsigned)(arg / 4);                  /* arg KiB, 4 KiB a level */
        unsigned long got = vm_burn(depth);
        if (got != vm_burn_expect(depth)) {
            emit_fd(1, "vm child %s: sum %lu, expected %lu", mode, got, vm_burn_expect(depth));
            return 1;
        }
        return 0;
    }
    if (streq(mode, "vm_thread_overflow")) {
        mos_thread_t t;
        if (mos_thread_create_vm(&t, vm_burn_thread, (void*)(uintptr_t)256, 64 * 1024, NULL) != 0) return 90;
        mos_thread_join(&t);
        return vm_survived(mode);
    }
    emit_fd(1, "unknown vm child mode %s", mode);
    return 99;
}

static int child_torture(int wide, long ms);
static int child_avx_probe(int wide512);

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
    if (sstarts(mode, "vm_")) return child_vm(mode, arg);
    if (sstarts(mode, "torture_")) return child_torture(streq(mode, "torture_avx"), arg);
    if (streq(mode, "avx_probe")) return child_avx_probe(0);
    if (streq(mode, "avx512_probe")) return child_avx_probe(1);
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
    o.stack_size = 0;
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
    mos_spawn_opts_t o = { g_self, argv, envp, TMP "/work", TMP "/out.txt", TMP "/err.txt", 0 };
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

/* ================================ user pointer checks: the whole surface */

/* Buffers used as call arguments. Bad pointers (input = the kernel only
 * reads it, output = it writes): an address nothing maps (above 4 GiB), a
 * kernel address below 4 GiB, a PROT_NONE mapping, a read-only mapping (output
 * only), and one byte before the end of a mapping (the next page is a
 * guard). Good ones: heap, stack, image (.data, .rodata, .bss), SYS_VM. */
static uint8_t* g_pb_rw;                 /* 1 page, read-write */
static uint8_t* g_pb_ro;                 /* 1 page, read-only, holds a file path */
static uint8_t* g_pb_none;               /* 1 page, PROT_NONE */
static void* g_bad_out[5];
static void* g_bad_in[4];
static const char* const g_tag_out[5] = { "unmapped", "kernel", "prot_none", "readonly", "overrun" };
static const char* const g_tag_in[4]  = { "unmapped", "kernel", "prot_none", "overrun" };
static char g_bss_buf[256];
static char g_data_buf[256] = { 'd' };
static const char g_rodata_path[] = TMP "/ptr_data.bin";
static const char g_rodata_text[] = "RODATA-TEXT-0123456789";

/* The call is made with P set to each bad pointer; `want` is the result. */
#define BAD_OUT_RET(base, want, expr) do { \
    for (int bi_ = 0; bi_ < 5; bi_++) { void* P = g_bad_out[bi_]; char nm_[96]; \
        kfmt(nm_, sizeof(nm_), "%s_%s", base, g_tag_out[bi_]); \
        EXPECT_RET(expr, want, nm_); } } while (0)
#define BAD_IN_RET(base, want, expr) do { \
    for (int bi_ = 0; bi_ < 4; bi_++) { void* P = g_bad_in[bi_]; char nm_[96]; \
        kfmt(nm_, sizeof(nm_), "%s_%s", base, g_tag_in[bi_]); \
        EXPECT_RET(expr, want, nm_); } } while (0)
/* Same, for calls whose gate may answer PERM or INVAL: any error. */
#define BAD_OUT_NEG(base, expr) do { \
    for (int bi_ = 0; bi_ < 5; bi_++) { void* P = g_bad_out[bi_]; char nm_[96]; \
        kfmt(nm_, sizeof(nm_), "%s_%s", base, g_tag_out[bi_]); \
        long r_ = (expr); EXPECT(r_ < 0, nm_, "got %ld", r_); } } while (0)

static int pb_pattern_ok(const uint8_t* buf, size_t n, uint8_t seed, size_t from) {
    for (size_t i = 0; i < n; i++) if (buf[i] != pattern_byte(from + i, seed)) return 0;
    return 1;
}

static void test_bad_pointers_2(void) {
    stage("bad_pointers_2");
    g_pb_rw = (uint8_t*)mos_vm_map(NULL, 4096, MOS_VM_PROT_RW, 0);
    g_pb_ro = (uint8_t*)mos_vm_map(NULL, 4096, MOS_VM_PROT_RW, 0);
    g_pb_none = (uint8_t*)mos_vm_map(NULL, 4096, MOS_VM_PROT_NONE, 0);
    if (!g_pb_rw || !g_pb_ro || !g_pb_none) {
        check_fail("bad_pointers_2_setup", "vm map failed: %ld", mos_vm_last_error());
        return;
    }
    scopy((char*)g_pb_ro, 4096, g_rodata_path);
    EXPECT_RET(mos_vm_protect_raw(g_pb_ro, 4096, MOS_VM_PROT_READ), SYS_SUCCESS, "bp2_make_readonly");
    g_pb_rw[4095] = 'a';                 /* an unterminated "string" at the very end */
    g_bad_out[0] = g_bad_in[0] = (void*)0x300000000000ULL;
    g_bad_out[1] = g_bad_in[1] = (void*)0x200000ULL;
    g_bad_out[2] = g_bad_in[2] = g_pb_none;
    g_bad_out[3] = g_pb_ro;
    g_bad_out[4] = g_bad_in[3] = g_pb_rw + 4095;

    uint8_t* heap = (uint8_t*)malloc(8192);
    EXPECT(heap != NULL, "bp2_heap_alloc", "malloc failed");
    if (!heap) return;
    uint8_t stackbuf[256];

    /* ---- files: SYS_READ, SYS_FWRITE */
    EXPECT_RET(write_pattern(TMP "/ptr_data.bin", 256, 7), 0, "bp2_make_data_file");
    long fd = mos_open(TMP "/ptr_data.bin", SYS_O_RDWR);
    EXPECT(fd > 0, "bp2_open_data_file", "got %ld", fd);
    if (fd <= 0) return;

    EXPECT_RET(mos_seek(fd, 0), 0, "bp2_seek_start");
    BAD_OUT_RET("read_bad_buffer", ERR(SYS_ERR_INVAL), mos_read(fd, P, 64));
    EXPECT_RET(mos_tell(fd), 0, "read_bad_buffer_position_unchanged");
    BAD_OUT_RET("read_bad_buffer_big", ERR(SYS_ERR_INVAL), mos_read(fd, P, 200));
    EXPECT_RET(mos_tell(fd), 0, "read_bad_buffer_big_position_unchanged");
    /* A read-only buffer must stay untouched. */
    EXPECT(g_pb_ro[0] == g_rodata_path[0], "read_readonly_buffer_untouched", "changed");

    memset(heap, 0, 64);
    EXPECT_RET(mos_read(fd, heap, 16), 16, "read_into_heap");
    EXPECT(pb_pattern_ok(heap, 16, 7, 0), "read_into_heap_content", "wrong data");
    memset(stackbuf, 0, sizeof(stackbuf));
    EXPECT_RET(mos_read(fd, stackbuf, 16), 16, "read_into_stack");
    EXPECT(pb_pattern_ok(stackbuf, 16, 7, 16), "read_into_stack_content", "wrong data");
    EXPECT_RET(mos_read(fd, g_bss_buf, 16), 16, "read_into_bss");
    EXPECT(pb_pattern_ok((uint8_t*)g_bss_buf, 16, 7, 32), "read_into_bss_content", "wrong data");
    EXPECT_RET(mos_read(fd, g_data_buf, 16), 16, "read_into_data");
    EXPECT(pb_pattern_ok((uint8_t*)g_data_buf, 16, 7, 48), "read_into_data_content", "wrong data");
    EXPECT_RET(mos_read(fd, g_pb_rw, 16), 16, "read_into_vm");
    EXPECT(pb_pattern_ok(g_pb_rw, 16, 7, 64), "read_into_vm_content", "wrong data");
    EXPECT_RET(mos_tell(fd), 80, "read_positive_position");
    /* A capacity bigger than what is left is fine when the bytes written fit. */
    EXPECT_RET(mos_seek(fd, 0), 0, "bp2_seek_start_again");
    EXPECT_RET(mos_read(fd, g_pb_rw, 1u << 20), 256, "read_oversize_capacity_ok");
    EXPECT_RET(mos_read(fd, g_pb_rw, 1u << 20), 0, "read_at_eof_returns_zero");
    g_pb_rw[4095] = 'a';

    /* SYS_FWRITE: reading from a bad buffer. */
    EXPECT_RET(mos_seek(fd, 256), 0, "bp2_seek_end");
    BAD_IN_RET("fwrite_bad_buffer", ERR(SYS_ERR_INVAL), mos_fwrite(fd, P, 16));
    EXPECT_RET(mos_size(fd), 256, "fwrite_bad_buffer_file_unchanged");
    EXPECT_RET(mos_fwrite(fd, g_rodata_text, 8), 8, "fwrite_from_rodata");
    EXPECT_RET(mos_fwrite(fd, g_data_buf, 8), 8, "fwrite_from_data");
    EXPECT_RET(mos_fwrite(fd, heap, 8), 8, "fwrite_from_heap");
    EXPECT_RET(mos_fwrite(fd, stackbuf, 8), 8, "fwrite_from_stack");
    EXPECT_RET(mos_fwrite(fd, g_pb_rw, 8), 8, "fwrite_from_vm");
    EXPECT_RET(mos_fwrite(fd, g_pb_ro, 8), 8, "fwrite_from_readonly_vm");
    EXPECT_RET(mos_size(fd), 256 + 48, "fwrite_positive_size");
    mos_close(fd);

    /* ---- paths: SYS_EXISTS, SYS_OPEN, SYS_MKDIR, SYS_CREATE, SYS_CHDIR, SYS_GET_METADATA */
    BAD_IN_RET("exists_bad_path", ERR(SYS_ERR_INVAL), mos_exists((const char*)P));
    BAD_IN_RET("open_bad_path", ERR(SYS_ERR_INVAL), mos_open((const char*)P, SYS_O_RDONLY));
    BAD_IN_RET("mkdir_bad_path", ERR(SYS_ERR_INVAL), mos_mkdir((const char*)P));
    BAD_IN_RET("create_bad_path", ERR(SYS_ERR_INVAL), mos_create((const char*)P, NULL, NULL));
    BAD_IN_RET("chdir_bad_path", ERR(SYS_ERR_INVAL), mos_chdir((const char*)P));
    BAD_IN_RET("delete_bad_path", ERR(SYS_ERR_INVAL), mos_delete((const char*)P));
    BAD_IN_RET("create_bad_type", ERR(SYS_ERR_INVAL), mos_create(TMP "/ptr_never.txt", (const char*)P, NULL));
    BAD_IN_RET("create_bad_format", ERR(SYS_ERR_INVAL), mos_create(TMP "/ptr_never.txt", "text", (const char*)P));
    EXPECT(!exists(TMP "/ptr_never.txt"), "create_bad_arguments_created_nothing", "file exists");
    BAD_IN_RET("stat_bad_path_all", ERR(SYS_ERR_INVAL), mos_stat((const char*)P, (syscall_stat_t*)heap));
    EXPECT_RET(mos_exists(g_rodata_path), 1, "exists_path_in_rodata");
    EXPECT_RET(mos_exists((const char*)g_pb_ro), 1, "exists_path_in_readonly_vm");
    scopy((char*)heap, 256, g_rodata_path);
    EXPECT_RET(mos_exists((const char*)heap), 1, "exists_path_in_heap");
    scopy((char*)stackbuf, sizeof(stackbuf), g_rodata_path);
    EXPECT_RET(mos_exists((const char*)stackbuf), 1, "exists_path_on_stack");
    scopy((char*)g_pb_rw, 4096, g_rodata_path);
    EXPECT_RET(mos_exists((const char*)g_pb_rw), 1, "exists_path_in_vm");
    /* A path that fills its mapping to the last byte with a NUL there is fine. */
    memset(g_pb_rw, 'a', 4096);
    g_pb_rw[4095] = '\0';
    EXPECT_RET(mos_exists((const char*)(g_pb_rw + 4090)), 0, "exists_path_ending_at_mapping_end");
    g_pb_rw[4095] = 'a';
    EXPECT_RET(mos_exists((const char*)(g_pb_rw + 4090)), ERR(SYS_ERR_INVAL), "exists_unterminated_path_inval");

    /* ---- output structs: SYS_STAT, SYS_GET_METADATA, SYS_GETTIME, SYS_RANDOM, SYS_GETCWD */
    BAD_OUT_RET("stat_bad_output", ERR(SYS_ERR_INVAL), mos_stat(g_rodata_path, (syscall_stat_t*)P));
    EXPECT_RET(mos_stat(g_rodata_path, (syscall_stat_t*)heap), 0, "stat_output_in_heap");
    EXPECT_RET(mos_stat(g_rodata_path, (syscall_stat_t*)g_pb_rw), 0, "stat_output_in_vm");
    EXPECT_RET(mos_stat(g_rodata_path, (syscall_stat_t*)g_bss_buf), 0, "stat_output_in_bss");
    BAD_OUT_RET("get_metadata_bad_output", ERR(SYS_ERR_INVAL), mos_get_metadata(g_rodata_path, (syscall_file_metadata_t*)P));
    EXPECT_RET(mos_get_metadata(g_rodata_path, (syscall_file_metadata_t*)heap), 0, "get_metadata_output_in_heap");
    BAD_OUT_RET("gettime_bad_output", ERR(SYS_ERR_INVAL), mos_syscall(SYS_GETTIME, (long)(uintptr_t)P, 0, 0));
    EXPECT_RET(mos_syscall(SYS_GETTIME, (long)(uintptr_t)heap, 0, 0), 0, "gettime_output_in_heap");
    EXPECT_RET(mos_syscall(SYS_GETTIME, (long)(uintptr_t)stackbuf, 0, 0), 0, "gettime_output_on_stack");
    BAD_OUT_RET("random_bad_buffer", ERR(SYS_ERR_INVAL), mos_syscall(SYS_RANDOM, (long)(uintptr_t)P, 16, 0));
    EXPECT_RET(mos_syscall(SYS_RANDOM, (long)(uintptr_t)heap, 16, 0), 16, "random_buffer_in_heap");
    EXPECT_RET(mos_syscall(SYS_RANDOM, (long)(uintptr_t)g_pb_rw, 16, 0), 16, "random_buffer_in_vm");
    BAD_OUT_RET("getcwd_bad_buffer", ERR(SYS_ERR_INVAL), mos_getcwd((char*)P, 256));
    EXPECT_RET(mos_getcwd((char*)stackbuf, 4096), 0, "getcwd_oversize_capacity_on_stack_ok");
    EXPECT_RET(mos_getcwd((char*)heap, 4096), 0, "getcwd_buffer_in_heap");

    /* ---- SYS_LISTDIR, SYS_PSLIST: only the entries written have to fit */
    mos_mkdir(TMP "/ptrdir");
    write_text(TMP "/ptrdir/a.txt", "A");
    write_text(TMP "/ptrdir/b.txt", "B");
    BAD_OUT_RET("listdir_bad_output", ERR(SYS_ERR_INVAL), mos_listdir(TMP "/ptrdir", (syscall_dirent_t*)P, 8));
    BAD_IN_RET("listdir_bad_path", ERR(SYS_ERR_INVAL), mos_listdir((const char*)P, (syscall_dirent_t*)heap, 8));
    long n = mos_listdir(TMP "/ptrdir", (syscall_dirent_t*)heap, 8);
    EXPECT(n == 2, "listdir_output_in_heap", "got %ld", n);
    n = mos_listdir(TMP "/ptrdir", (syscall_dirent_t*)heap, 100);   /* capacity 100, buffer for 8+ */
    EXPECT(n == 2, "listdir_oversize_capacity_ok", "got %ld", n);
    n = mos_listdir(TMP "/ptrdir", (syscall_dirent_t*)g_pb_rw, 8);
    EXPECT(n == 2, "listdir_output_in_vm", "got %ld", n);
    BAD_OUT_RET("pslist_bad_output", ERR(SYS_ERR_INVAL), mos_pslist((syscall_process_info_t*)P, 4));
    n = mos_pslist((syscall_process_info_t*)heap, 4);
    EXPECT(n >= 1 && n <= 4, "pslist_output_in_heap", "got %ld", n);
    n = mos_pslist((syscall_process_info_t*)g_pb_rw, 8);
    EXPECT(n >= 1, "pslist_output_in_vm", "got %ld", n);

    /* ---- SYS_WRITE (stdout/stderr and the log descriptor) */
    BAD_IN_RET("write_stdout_bad_buffer", ERR(SYS_ERR_INVAL), mos_write_fd(1, (const char*)P, 16));
    BAD_IN_RET("write_log_bad_buffer", ERR(SYS_ERR_INVAL), mos_write_fd(3, (const char*)P, 16));
    EXPECT_RET(mos_write_fd(3, g_rodata_text, 8), 8, "write_log_from_rodata");
    EXPECT_RET(mos_write_fd(3, g_pb_ro, 8), 8, "write_log_from_readonly_vm");

    /* ---- SYS_WAIT, SYS_FUTEX, SYS_THREAD, SYS_VM, SYS_REGISTER_CLEANUP */
    BAD_OUT_RET("wait_bad_output", ERR(SYS_ERR_INVAL), mos_wait_raw(SYS_WAIT_ANY, SYS_WAIT_NOHANG, (syscall_wait_t*)P));
    BAD_IN_RET("futex_wake_bad_address", ERR(SYS_ERR_INVAL), mos_syscall(SYS_FUTEX, SYS_FUTEX_WAKE, (long)(uintptr_t)P, 1));
    BAD_IN_RET("thread_create_bad_struct", ERR(SYS_ERR_INVAL), mos_syscall(SYS_THREAD, SYS_THREAD_CREATE, (long)(uintptr_t)P, 0));
    BAD_IN_RET("thread_create_ex_bad_struct", ERR(SYS_ERR_INVAL), mos_syscall(SYS_THREAD, SYS_THREAD_CREATE_EX, (long)(uintptr_t)P, 0));
    BAD_IN_RET("vm_map_bad_struct", ERR(SYS_ERR_INVAL), mos_vm_map_raw((const syscall_vm_map_t*)P));
    EXPECT_RET(mos_syscall(SYS_REGISTER_CLEANUP, 0x300000000000LL, 0, 0), ERR(SYS_ERR_INVAL), "register_cleanup_unmapped_inval");
    EXPECT_RET(mos_syscall(SYS_REGISTER_CLEANUP, 0x200000, 0, 0), ERR(SYS_ERR_INVAL), "register_cleanup_kernel_inval");
    EXPECT_RET(mos_syscall(SYS_REGISTER_CLEANUP, (long)(uintptr_t)g_pb_rw, 0, 0), ERR(SYS_ERR_INVAL), "register_cleanup_not_executable_inval");

    /* ---- SYS_PKG: the request's own pointers */
    {
        syscall_pkg_request_t pk;
        memset(&pk, 0, sizeof(pk));
        pk.op = SYS_PKG_INFO;
        pk.path = g_rodata_path;
        pk.out_count = (uint32_t*)g_pb_ro;
        EXPECT_RET(mos_syscall(SYS_PKG, (long)(uintptr_t)&pk, 0, 0), ERR(SYS_ERR_INVAL), "pkg_info_readonly_out_count");
        pk.out_count = NULL;
        for (int i = 0; i < 4; i++) {
            pk.path = (const char*)g_bad_in[i];
            char nm[96]; kfmt(nm, sizeof(nm), "pkg_info_bad_path_%s", g_tag_in[i]);
            EXPECT_RET(mos_syscall(SYS_PKG, (long)(uintptr_t)&pk, 0, 0), ERR(SYS_ERR_INVAL), nm);
        }
        BAD_IN_RET("pkg_bad_request", ERR(SYS_ERR_INVAL), mos_syscall(SYS_PKG, (long)(uintptr_t)P, 0, 0));
    }

    /* ---- SYS_USB keyboard info */
    BAD_OUT_RET("usb_keyboard_info_bad_output", ERR(SYS_ERR_INVAL), mos_syscall(SYS_USB, SYS_USB_KEYBOARD_INFO, (long)(uintptr_t)P, 0));

    /* ---- SYS_GRAPHICS */
    {
        syscall_graphics_request_t gr;
        memset(&gr, 0, sizeof(gr));
        gr.op = SYS_GRAPHICS_INFO;
        for (int i = 0; i < 5; i++) {
            gr.info = (syscall_graphics_info_t*)g_bad_out[i];
            char nm[96]; kfmt(nm, sizeof(nm), "graphics_info_bad_output_%s", g_tag_out[i]);
            EXPECT_RET(mos_syscall(SYS_GRAPHICS, (long)(uintptr_t)&gr, 0, 0), ERR(SYS_ERR_INVAL), nm);
        }
        BAD_OUT_NEG("graphics_bad_request", mos_syscall(SYS_GRAPHICS, (long)(uintptr_t)P, 0, 0));
        gr.op = SYS_GRAPHICS_GET_WIDTH;
        long w = mos_syscall(SYS_GRAPHICS, (long)(uintptr_t)&gr, 0, 0);
        EXPECT(w >= 0, "graphics_get_width_still_works", "got %ld", w);
    }

    /* ---- SYS_WINDOW */
    {
        syscall_window_request_t wr;
        memset(&wr, 0, sizeof(wr));
        wr.size = sizeof(wr);
        wr.version = SYS_WINDOW_ABI_VERSION;
        BAD_IN_RET("window_bad_request", ERR(SYS_ERR_INVAL), mos_syscall(SYS_WINDOW, (long)(uintptr_t)P, 0, 0));
        wr.op = SYS_WINDOW_GET_INFO; wr.id = 1;
        for (int i = 0; i < 5; i++) {
            wr.info = (syscall_window_info_t*)g_bad_out[i];
            char nm[96]; kfmt(nm, sizeof(nm), "window_get_info_bad_output_%s", g_tag_out[i]);
            EXPECT_RET(mos_syscall(SYS_WINDOW, (long)(uintptr_t)&wr, 0, 0), ERR(SYS_ERR_INVAL), nm);
        }
        wr.info = NULL;
        wr.op = SYS_WINDOW_LIST; wr.capacity = 2;
        for (int i = 0; i < 5; i++) {
            wr.windows = (syscall_window_info_t*)g_bad_out[i];
            char nm[96]; kfmt(nm, sizeof(nm), "window_list_bad_output_%s", g_tag_out[i]);
            EXPECT_RET(mos_syscall(SYS_WINDOW, (long)(uintptr_t)&wr, 0, 0), ERR(SYS_ERR_INVAL), nm);
        }
        wr.windows = (syscall_window_info_t*)heap; wr.capacity = 4;
        long c = mos_syscall(SYS_WINDOW, (long)(uintptr_t)&wr, 0, 0);
        EXPECT(c >= 0, "window_list_output_in_heap", "got %ld", c);
        wr.windows = (syscall_window_info_t*)g_pb_rw;
        c = mos_syscall(SYS_WINDOW, (long)(uintptr_t)&wr, 0, 0);
        EXPECT(c >= 0, "window_list_output_in_vm", "got %ld", c);
        wr.windows = NULL; wr.capacity = 0;
        c = mos_syscall(SYS_WINDOW, (long)(uintptr_t)&wr, 0, 0);
        EXPECT(c >= 0, "window_list_count_only", "got %ld", c);
        wr.op = SYS_WINDOW_NEXT_EVENT; wr.capacity = 0; wr.id = 1;
        for (int i = 0; i < 5; i++) {
            wr.event = (syscall_window_event_t*)g_bad_out[i];
            char nm[96]; kfmt(nm, sizeof(nm), "window_next_event_bad_output_%s", g_tag_out[i]);
            EXPECT_RET(mos_syscall(SYS_WINDOW, (long)(uintptr_t)&wr, 0, 0), ERR(SYS_ERR_INVAL), nm);
        }
        wr.event = NULL;
        wr.op = SYS_WINDOW_PRESENT; wr.id = 1; wr.x = 0; wr.y = 0; wr.width = 8; wr.height = 8; wr.pitch = 32;
        for (int i = 0; i < 4; i++) {
            wr.pixels = (const uint32_t*)g_bad_in[i];
            char nm[96]; kfmt(nm, sizeof(nm), "window_present_bad_pixels_%s", g_tag_in[i]);
            EXPECT_RET(mos_syscall(SYS_WINDOW, (long)(uintptr_t)&wr, 0, 0), ERR(SYS_ERR_INVAL), nm);
        }
    }

    /* ---- SYS_INPUT_READ (events written; a request that is fine gets PERM
     * when this program does not own the screen, which is checked after) */
    {
        syscall_input_read_request_t ir;
        memset(&ir, 0, sizeof(ir));
        ir.size = sizeof(ir);
        ir.version = SYS_INPUT_ABI_VERSION;
        ir.capacity = 4;
        BAD_IN_RET("input_read_bad_request", ERR(SYS_ERR_INVAL), mos_syscall(SYS_INPUT_READ, (long)(uintptr_t)P, 0, 0));
        for (int i = 0; i < 5; i++) {
            ir.events = (syscall_input_event_t*)g_bad_out[i];
            char nm[96]; kfmt(nm, sizeof(nm), "input_read_bad_events_%s", g_tag_out[i]);
            EXPECT_RET(mos_syscall(SYS_INPUT_READ, (long)(uintptr_t)&ir, 0, 0), ERR(SYS_ERR_INVAL), nm);
        }
        ir.events = (syscall_input_event_t*)heap;
        for (int i = 0; i < 5; i++) {
            ir.dropped_count = (uint64_t*)g_bad_out[i];
            char nm[96]; kfmt(nm, sizeof(nm), "input_read_bad_dropped_count_%s", g_tag_out[i]);
            EXPECT_RET(mos_syscall(SYS_INPUT_READ, (long)(uintptr_t)&ir, 0, 0), ERR(SYS_ERR_INVAL), nm);
        }
        ir.dropped_count = NULL;
        long r = mos_syscall(SYS_INPUT_READ, (long)(uintptr_t)&ir, 0, 0);
        EXPECT(r == ERR(SYS_ERR_PERM) || r >= 0, "input_read_valid_request_reaches_owner_check", "got %ld", r);
        ir.events = (syscall_input_event_t*)stackbuf;
        r = mos_syscall(SYS_INPUT_READ, (long)(uintptr_t)&ir, 0, 0);
        EXPECT(r == ERR(SYS_ERR_PERM) || r >= 0, "input_read_events_on_stack_ok", "got %ld", r);
    }

    /* ---- SYS_GPU: ops that read or write program memory */
    {
        syscall_gpu_request_t g;
        memset(&g, 0, sizeof(g));
        g.size = sizeof(g);
        g.version = SYS_GPU_ABI_VERSION;
        BAD_OUT_RET("gpu_bad_request", ERR(SYS_ERR_INVAL), mos_syscall(SYS_GPU, (long)(uintptr_t)P, 0, 0));
        g.op = SYS_GPU_INFO;
        for (int i = 0; i < 5; i++) {
            g.out = g_bad_out[i];
            char nm[96]; kfmt(nm, sizeof(nm), "gpu_info_bad_output_%s", g_tag_out[i]);
            EXPECT_RET(mos_syscall(SYS_GPU, (long)(uintptr_t)&g, 0, 0), ERR(SYS_ERR_INVAL), nm);
        }
        g.op = SYS_GPU_SUBMIT; g.bytes = 16;
        for (int i = 0; i < 4; i++) {
            g.data = g_bad_in[i];
            char nm[96]; kfmt(nm, sizeof(nm), "gpu_submit_bad_commands_%s", g_tag_in[i]);
            EXPECT_RET(mos_syscall(SYS_GPU, (long)(uintptr_t)&g, 0, 0), ERR(SYS_ERR_INVAL), nm);
        }
        g.op = SYS_GPU_CREATE_PROGRAM; g.bytes = 64;
        for (int i = 0; i < 4; i++) {
            g.data = g_bad_in[i];
            char nm[96]; kfmt(nm, sizeof(nm), "gpu_create_program_bad_blob_%s", g_tag_in[i]);
            EXPECT_RET(mos_syscall(SYS_GPU, (long)(uintptr_t)&g, 0, 0), ERR(SYS_ERR_INVAL), nm);
        }
        /* A buffer and a surface to upload to / read back from. */
        memset(&g, 0, sizeof(g));
        g.size = sizeof(g); g.version = SYS_GPU_ABI_VERSION;
        g.op = SYS_GPU_CREATE_BUFFER; g.bytes = 4096;
        long cb = mos_syscall(SYS_GPU, (long)(uintptr_t)&g, 0, 0);
        uint32_t buf_id = g.handle;
        memset(&g, 0, sizeof(g));
        g.size = sizeof(g); g.version = SYS_GPU_ABI_VERSION;
        g.op = SYS_GPU_CREATE_SURFACE; g.width = 16; g.height = 16; g.format = FGPU_FORMAT_XRGB8888;
        g.flags = FGPU_SURFACE_SYSTEM_MEMORY;
        long cs = mos_syscall(SYS_GPU, (long)(uintptr_t)&g, 0, 0);
        uint32_t surf_id = g.handle;
        if (cb == 0 && buf_id) {
            g.op = SYS_GPU_UPLOAD_BUFFER; g.handle = buf_id; g.offset = 0; g.bytes = 256;
            for (int i = 0; i < 4; i++) {
                g.data = g_bad_in[i];
                char nm[96]; kfmt(nm, sizeof(nm), "gpu_upload_buffer_bad_data_%s", g_tag_in[i]);
                EXPECT_RET(mos_syscall(SYS_GPU, (long)(uintptr_t)&g, 0, 0), ERR(SYS_ERR_INVAL), nm);
            }
            g.data = heap;
            EXPECT_RET(mos_syscall(SYS_GPU, (long)(uintptr_t)&g, 0, 0), 0, "gpu_upload_buffer_from_heap");
            g.data = g_pb_ro;
            EXPECT_RET(mos_syscall(SYS_GPU, (long)(uintptr_t)&g, 0, 0), 0, "gpu_upload_buffer_from_readonly_vm");
            g.data = g_rodata_text; g.bytes = 16;
            EXPECT_RET(mos_syscall(SYS_GPU, (long)(uintptr_t)&g, 0, 0), 0, "gpu_upload_buffer_from_rodata");
            g.op = SYS_GPU_DESTROY_BUFFER; g.handle = buf_id;
            EXPECT_RET(mos_syscall(SYS_GPU, (long)(uintptr_t)&g, 0, 0), 0, "gpu_destroy_buffer");
        } else {
            check_pass("gpu_buffer_checks (skipped: no buffer could be created)");
        }
        if (cs == 0 && surf_id) {
            memset(&g, 0, sizeof(g));
            g.size = sizeof(g); g.version = SYS_GPU_ABI_VERSION;
            g.handle = surf_id; g.pitch = 64;
            g.op = SYS_GPU_READBACK;
            for (int i = 0; i < 5; i++) {
                g.out = g_bad_out[i];
                char nm[96]; kfmt(nm, sizeof(nm), "gpu_readback_bad_output_%s", g_tag_out[i]);
                EXPECT_RET(mos_syscall(SYS_GPU, (long)(uintptr_t)&g, 0, 0), ERR(SYS_ERR_INVAL), nm);
            }
            g.out = heap;
            EXPECT_RET(mos_syscall(SYS_GPU, (long)(uintptr_t)&g, 0, 0), 0, "gpu_readback_into_heap");
            g.out = g_pb_rw;
            EXPECT_RET(mos_syscall(SYS_GPU, (long)(uintptr_t)&g, 0, 0), 0, "gpu_readback_into_vm");
            g.op = SYS_GPU_WRITE_SURFACE;
            for (int i = 0; i < 4; i++) {
                g.data = g_bad_in[i];
                char nm[96]; kfmt(nm, sizeof(nm), "gpu_write_surface_bad_pixels_%s", g_tag_in[i]);
                EXPECT_RET(mos_syscall(SYS_GPU, (long)(uintptr_t)&g, 0, 0), ERR(SYS_ERR_INVAL), nm);
            }
            g.data = heap;
            EXPECT_RET(mos_syscall(SYS_GPU, (long)(uintptr_t)&g, 0, 0), 0, "gpu_write_surface_from_heap");
            g.data = g_pb_ro;
            EXPECT_RET(mos_syscall(SYS_GPU, (long)(uintptr_t)&g, 0, 0), 0, "gpu_write_surface_from_readonly_vm");
            g.op = SYS_GPU_DESTROY_SURFACE;
            EXPECT_RET(mos_syscall(SYS_GPU, (long)(uintptr_t)&g, 0, 0), 0, "gpu_destroy_surface");
        } else {
            check_pass("gpu_surface_checks (skipped: no surface could be created)");
        }
    }

    /* ---- SYS_MEDIA: the tile decoder (frame is read, pixels written) */
    {
        syscall_media_request_t m;
        memset(&m, 0, sizeof(m));
        m.size = sizeof(m); m.version = SYS_MEDIA_ABI_VERSION; m.op = SYS_MEDIA_DECODE_TILES;
        m.width = 16; m.height = 16; m.frame_bytes = 64;
        BAD_OUT_RET("media_bad_request", ERR(SYS_ERR_INVAL), mos_syscall(SYS_MEDIA, (long)(uintptr_t)P, 0, 0));
        m.pixels = (uint32_t*)heap;
        for (int i = 0; i < 4; i++) {
            m.frame = (const uint8_t*)g_bad_in[i];
            char nm[96]; kfmt(nm, sizeof(nm), "media_tiles_bad_frame_%s", g_tag_in[i]);
            EXPECT_RET(mos_syscall(SYS_MEDIA, (long)(uintptr_t)&m, 0, 0), ERR(SYS_ERR_INVAL), nm);
        }
        m.frame = stackbuf;
        for (int i = 0; i < 5; i++) {
            m.pixels = (uint32_t*)g_bad_out[i];
            char nm[96]; kfmt(nm, sizeof(nm), "media_tiles_bad_pixels_%s", g_tag_out[i]);
            long r = mos_syscall(SYS_MEDIA, (long)(uintptr_t)&m, 0, 0);
            /* unaligned (overrun) pointers are refused by the alignment rule: still an error */
            EXPECT(r == ERR(SYS_ERR_INVAL), nm, "got %ld", r);
        }
    }

    /* ---- SYS_NET */
    {
        syscall_net_request_t nr;
        memset(&nr, 0, sizeof(nr));
        BAD_IN_RET("net_bad_request", ERR(SYS_ERR_INVAL), mos_syscall(SYS_NET, (long)(uintptr_t)P, 0, 0));
        nr.op = SYS_NET_STATUS;
        for (int i = 0; i < 5; i++) {
            nr.out_status = (syscall_net_status_t*)g_bad_out[i];
            char nm[96]; kfmt(nm, sizeof(nm), "net_status_bad_output_%s", g_tag_out[i]);
            EXPECT_RET(mos_syscall(SYS_NET, (long)(uintptr_t)&nr, 0, 0), ERR(SYS_ERR_INVAL), nm);
        }
        nr.out_status = (syscall_net_status_t*)heap;
        EXPECT_RET(mos_syscall(SYS_NET, (long)(uintptr_t)&nr, 0, 0), 0, "net_status_output_in_heap");
        nr.out_status = (syscall_net_status_t*)g_pb_rw;
        EXPECT_RET(mos_syscall(SYS_NET, (long)(uintptr_t)&nr, 0, 0), 0, "net_status_output_in_vm");
        nr.out_status = NULL;
        nr.op = SYS_NET_ADAPTERS; nr.len = sizeof(syscall_net_adapters_t);
        for (int i = 0; i < 5; i++) {
            nr.buf = g_bad_out[i];
            char nm[96]; kfmt(nm, sizeof(nm), "net_adapters_bad_buffer_%s", g_tag_out[i]);
            EXPECT_RET(mos_syscall(SYS_NET, (long)(uintptr_t)&nr, 0, 0), ERR(SYS_ERR_INVAL), nm);
        }
        nr.buf = heap;
        EXPECT_RET(mos_syscall(SYS_NET, (long)(uintptr_t)&nr, 0, 0), 0, "net_adapters_buffer_in_heap");
        nr.buf = NULL; nr.len = 0;
        nr.op = SYS_NET_TCP_CONNECT; nr.ip = 0x7F000001u; nr.port = 80;
        for (int i = 0; i < 5; i++) {
            nr.out_handle = (uint64_t*)g_bad_out[i];
            char nm[96]; kfmt(nm, sizeof(nm), "net_tcp_connect_bad_out_handle_%s", g_tag_out[i]);
            EXPECT_RET(mos_syscall(SYS_NET, (long)(uintptr_t)&nr, 0, 0), ERR(SYS_ERR_INVAL), nm);
        }
        nr.out_handle = NULL;
        nr.op = SYS_NET_RESOLVE; nr.out_ip = (uint32_t*)heap;
        for (int i = 0; i < 4; i++) {
            nr.host = (const char*)g_bad_in[i];
            char nm[96]; kfmt(nm, sizeof(nm), "net_resolve_bad_host_%s", g_tag_in[i]);
            EXPECT_RET(mos_syscall(SYS_NET, (long)(uintptr_t)&nr, 0, 0), ERR(SYS_ERR_INVAL), nm);
        }
        nr.host = "example.invalid";
        for (int i = 0; i < 5; i++) {
            nr.out_ip = (uint32_t*)g_bad_out[i];
            char nm[96]; kfmt(nm, sizeof(nm), "net_resolve_bad_out_ip_%s", g_tag_out[i]);
            EXPECT_RET(mos_syscall(SYS_NET, (long)(uintptr_t)&nr, 0, 0), ERR(SYS_ERR_INVAL), nm);
        }
        nr.out_ip = NULL;
        nr.op = SYS_NET_UDP_SEND; nr.ip = 0x7F000001u; nr.port = 9; nr.local_port = 4000; nr.len = 16;
        for (int i = 0; i < 4; i++) {
            nr.buf = g_bad_in[i];
            char nm[96]; kfmt(nm, sizeof(nm), "net_udp_send_bad_buffer_%s", g_tag_in[i]);
            EXPECT_RET(mos_syscall(SYS_NET, (long)(uintptr_t)&nr, 0, 0), ERR(SYS_ERR_INVAL), nm);
        }
        nr.op = SYS_NET_UDP_RECV; nr.handle = 1;
        for (int i = 0; i < 5; i++) {
            nr.buf = g_bad_out[i];
            char nm[96]; kfmt(nm, sizeof(nm), "net_udp_recv_bad_buffer_%s", g_tag_out[i]);
            EXPECT_RET(mos_syscall(SYS_NET, (long)(uintptr_t)&nr, 0, 0), ERR(SYS_ERR_INVAL), nm);
        }
    }

    /* ---- SYS_STORAGE, SYS_AUDIO*, SYS_TIMESYNC, SYS_ROOT (request pointers and nested ones) */
    {
        syscall_storage_request_t sr;
        memset(&sr, 0, sizeof(sr));
        sr.size = sizeof(sr); sr.version = SYS_STORAGE_ABI_VERSION;
        BAD_OUT_RET("storage_bad_request", ERR(SYS_ERR_INVAL), mos_syscall(SYS_STORAGE, (long)(uintptr_t)P, 0, 0));
        sr.op = SYS_STORAGE_LIST; sr.capacity = 4;
        for (int i = 0; i < 5; i++) {
            sr.devices = (syscall_storage_device_t*)g_bad_out[i];
            char nm[96]; kfmt(nm, sizeof(nm), "storage_list_bad_devices_%s", g_tag_out[i]);
            EXPECT_RET(mos_syscall(SYS_STORAGE, (long)(uintptr_t)&sr, 0, 0), ERR(SYS_ERR_INVAL), nm);
        }
        sr.devices = (syscall_storage_device_t*)heap;
        long sl = mos_syscall(SYS_STORAGE, (long)(uintptr_t)&sr, 0, 0);
        EXPECT(sl >= 0, "storage_list_devices_in_heap", "got %ld", sl);
        sr.op = SYS_STORAGE_STATUS;
        EXPECT_RET(mos_syscall(SYS_STORAGE, (long)(uintptr_t)&sr, 0, 0), 0, "storage_status_ok");

        BAD_OUT_RET("audio_bad_request", ERR(SYS_ERR_INVAL), mos_syscall(SYS_AUDIO, (long)(uintptr_t)P, 0, 0));
        BAD_OUT_RET("audio_mixer_bad_request", ERR(SYS_ERR_INVAL), mos_syscall(SYS_AUDIO_MIXER, (long)(uintptr_t)P, 0, 0));
        BAD_OUT_RET("audio_stream_bad_request", ERR(SYS_ERR_INVAL), mos_syscall(SYS_AUDIO_STREAM, (long)(uintptr_t)P, 0, 0));
        syscall_audio_mixer_t* mx = (syscall_audio_mixer_t*)heap;
        memset(mx, 0, sizeof(*mx));
        mx->op = SYS_AUDIO_MIXER_GET;
        EXPECT_RET(mos_syscall(SYS_AUDIO_MIXER, (long)(uintptr_t)mx, 0, 0), 0, "audio_mixer_get_in_heap");
        syscall_audio_request_t* ar = (syscall_audio_request_t*)heap;
        memset(ar, 0, sizeof(*ar));
        ar->size = sizeof(*ar); ar->version = SYS_AUDIO_ABI_VERSION; ar->op = SYS_AUDIO_STATUS;
        EXPECT_RET(mos_syscall(SYS_AUDIO, (long)(uintptr_t)ar, 0, 0), 0, "audio_status_in_heap");
        memset(ar, 0, sizeof(*ar));
        ar->size = sizeof(*ar); ar->version = SYS_AUDIO_ABI_VERSION; ar->op = SYS_AUDIO_STATUS;
        long ag = mos_syscall(SYS_AUDIO, (long)(uintptr_t)g_pb_rw, 0, 0);   /* garbage contents */
        EXPECT(ag < 0, "audio_garbage_request_no_crash", "got %ld", ag);

        /* SYS_TIMESYNC (38) and SYS_ROOT: a kernel-sized request in the heap, op STATUS */
        BAD_OUT_RET("timesync_bad_request", ERR(SYS_ERR_INVAL), mos_syscall(38, (long)(uintptr_t)P, 0, 0));
        BAD_OUT_RET("root_bad_request", ERR(SYS_ERR_INVAL), mos_syscall(SYS_ROOT, (long)(uintptr_t)P, 0, 0));
        uint32_t* big = (uint32_t*)malloc(4096);
        if (big) {
            memset(big, 0, 4096);
            big[0] = 2;     /* SYS_TIMESYNC_STATUS */
            EXPECT_RET(mos_syscall(38, (long)(uintptr_t)big, 0, 0), 0, "timesync_status_in_heap");
            memset(big, 0, 4096);
            big[0] = 4;     /* SYS_ROOT_STATUS */
            EXPECT_RET(mos_syscall(SYS_ROOT, (long)(uintptr_t)big, 0, 0), 0, "root_status_in_heap");
            free(big);
        }
    }

    /* The mappings are ours to give back. */
    mos_vm_unmap(g_pb_rw, 4096);
    mos_vm_unmap(g_pb_ro, 4096);
    mos_vm_unmap(g_pb_none, 4096);
    free(heap);
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

/* ================================================================== VM */

static char** g_argv;       /* set by main(): the layout checks look at them */
static char** g_envp;
int main(int argc, char** argv, char** envp);

#define TIB            0x10000000000ULL
#define VM_MMAP_BASE   0x20000000000ULL      /* 2 TiB */
#define VM_STACK_TOP   0x7FFFF0000000ULL
#define VM_FIX_A       0x30000000000ULL      /* 3 TiB: free unless something leaked */
#define VM_HINT_A      0x50000000000ULL      /* 5 TiB */
#define VM_PAGES_SLACK 64                    /* noise allowed in page-pool readings */

static void* vm_new(size_t len, int prot) { return mos_vm_map(NULL, len, prot, 0); }

static int vm_is_zero(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) if (p[i]) return 0;
    return 1;
}

static void vm_fill(uint8_t* p, size_t n, uint8_t seed) {
    for (size_t i = 0; i < n; i++) p[i] = pattern_byte(i, seed);
}

static int vm_matches(const uint8_t* p, size_t n, uint8_t seed) {
    for (size_t i = 0; i < n; i++) if (p[i] != pattern_byte(i, seed)) return 0;
    return 1;
}

static long vm_free_pages(void) { return (long)mos_sysinfo_pmm_free_pages(); }

/* Maps and (on success) unmaps again. Returns 1 when the map worked, else
 * the negative error. */
static long vm_map_result(void* hint, size_t len, int prot, int flags) {
    long e = 0;
    void* p = mos_vm_map_ex(hint, len, prot, flags, &e);
    if (!p) return e;
    mos_vm_unmap(p, len);
    return 1;
}

static long spawn_wait_stack(const char* mode, const char* arg, size_t stack_size,
                             mos_wait_result_t* res) {
    const char* argv[6];
    int n = 0;
    argv[n++] = g_self;
    argv[n++] = "--child";
    argv[n++] = mode;
    if (arg) argv[n++] = arg;
    argv[n] = NULL;
    mos_spawn_opts_t o;
    memset(&o, 0, sizeof(o));
    o.path = g_self;
    o.argv = argv;
    o.stack_size = stack_size;
    memset(res, 0, sizeof(*res));
    long pid = mos_spawn_ex(&o);
    if (pid <= 0) return pid;
    long w = mos_waitpid(pid, 0, res);
    return w == pid ? pid : w - 1000;
}

/* A child that must die of a page fault (exception 14). */
static void expect_vm_fault(const char* mode, const char* arg, size_t stack_size, const char* name) {
    mos_wait_result_t r;
    long pid = spawn_wait_stack(mode, arg, stack_size, &r);
    EXPECT(pid > 0 && r.crashed && r.crash_vector == 14, name,
           "pid=%ld crashed=%d vector=%u exit=%d", pid, r.crashed, r.crash_vector, r.exit_code);
}

/* A child that must finish normally with exit code 0. */
static void expect_vm_ok(const char* mode, const char* arg, size_t stack_size, const char* name) {
    mos_wait_result_t r;
    long pid = spawn_wait_stack(mode, arg, stack_size, &r);
    EXPECT(pid > 0 && !r.crashed && r.exit_code == 0, name,
           "pid=%ld crashed=%d vector=%u exit=%d", pid, r.crashed, r.crash_vector, r.exit_code);
}

/* ---------------------------------------------------------- map basics */

static void test_vm_map(void) {
    stage("vm_map");
    long e = 0;
    uint8_t* a = (uint8_t*)mos_vm_map_ex(NULL, 3 * VM_PAGE, VM_RW, 0, &e);
    EXPECT(a != NULL, "vm_map_returns_address", "failed: %ld (%s)", e, ename(e));
    if (!a) return;
    EXPECT(((uintptr_t)a & (VM_PAGE - 1)) == 0, "vm_map_page_aligned", "address 0x%llx", PTR(a));
    EXPECT(PTR(a) >= VM_MMAP_BASE, "vm_map_at_or_above_2tib", "address 0x%llx", PTR(a));
    EXPECT(vm_is_zero(a, 3 * VM_PAGE), "vm_map_fresh_pages_read_zero", "a byte was not zero");
    vm_fill(a, 3 * VM_PAGE, 0x11);
    EXPECT(vm_matches(a, 3 * VM_PAGE, 0x11), "vm_map_writes_stick", "data changed");

    uint8_t* b = (uint8_t*)vm_new(2 * VM_PAGE, VM_RW);
    EXPECT(b != NULL, "vm_map_second_mapping", "failed: %ld", mos_vm_last_error());
    if (b) {
        uint64_t ua = PTR(a), ub = PTR(b);
        int gap = ub > ua ? ub >= ua + 3 * VM_PAGE + VM_PAGE : ua >= ub + 2 * VM_PAGE + VM_PAGE;
        EXPECT(gap, "vm_map_mappings_have_a_gap_between", "a=0x%llx b=0x%llx", ua, ub);
        vm_fill(b, 2 * VM_PAGE, 0x22);
        EXPECT(vm_matches(a, 3 * VM_PAGE, 0x11) && vm_matches(b, 2 * VM_PAGE, 0x22),
               "vm_map_mappings_independent", "data of one mapping changed");
        mos_vm_unmap(b, 2 * VM_PAGE);
    }
    mos_vm_unmap(a, 3 * VM_PAGE);

    /* Length rounding: exactly whole pages. */
    uint8_t* p = (uint8_t*)vm_new(1, VM_RW);
    EXPECT(p != NULL, "vm_len_1_maps", "failed: %ld", mos_vm_last_error());
    if (p) {
        p[VM_PAGE - 1] = 7;
        EXPECT(p[VM_PAGE - 1] == 7, "vm_len_1_whole_page_usable", "last byte of the page lost");
        EXPECT_RET(mos_vm_protect(p, 2 * VM_PAGE, VM_RW), ERR(SYS_ERR_INVAL), "vm_len_1_is_not_two_pages");
        EXPECT_RET(mos_vm_protect(p, VM_PAGE, MOS_VM_PROT_READ), SYS_SUCCESS, "vm_len_1_is_one_page");
        mos_vm_unmap(p, VM_PAGE);
    }
    p = (uint8_t*)vm_new(VM_PAGE + 1, VM_RW);
    EXPECT(p != NULL, "vm_len_4097_maps", "failed: %ld", mos_vm_last_error());
    if (p) {
        EXPECT_RET(mos_vm_protect(p, 3 * VM_PAGE, VM_RW), ERR(SYS_ERR_INVAL), "vm_len_4097_is_not_three_pages");
        EXPECT_RET(mos_vm_protect(p, 2 * VM_PAGE, VM_RW), SYS_SUCCESS, "vm_len_4097_is_two_pages");
        mos_vm_unmap(p, 2 * VM_PAGE);
    }

    /* Bad arguments. */
    EXPECT_RET(vm_map_result(NULL, 0, VM_RW, 0), ERR(SYS_ERR_INVAL), "vm_map_zero_length_inval");
    EXPECT_RET(vm_map_result(NULL, VM_PAGE, 8, 0), ERR(SYS_ERR_INVAL), "vm_map_bad_prot_inval");
    EXPECT_RET(vm_map_result(NULL, VM_PAGE, VM_RW, 4), ERR(SYS_ERR_INVAL), "vm_map_bad_flags_inval");
    EXPECT_RET(vm_map_result(NULL, (size_t)1 << 47, VM_RW, 0), ERR(SYS_ERR_INVAL), "vm_map_huge_length_inval");
    EXPECT_RET(vm_map_result(NULL, (size_t)-1, VM_RW, 0), ERR(SYS_ERR_INVAL), "vm_map_max_length_inval");
    syscall_vm_map_t req;
    req.size = 8; req.address = 0; req.length = VM_PAGE; req.prot = VM_RW; req.flags = 0;
    EXPECT_RET(mos_vm_map_raw(&req), ERR(SYS_ERR_INVAL), "vm_map_small_request_size_inval");
    EXPECT_RET(mos_vm_map_raw(NULL), ERR(SYS_ERR_INVAL), "vm_map_null_request_inval");
    EXPECT_RET(mos_vm_map_raw((const syscall_vm_map_t*)(uintptr_t)8), ERR(SYS_ERR_INVAL), "vm_map_bad_request_pointer_inval");
    EXPECT_RET(mos_syscall(SYS_VM, 99, 0, 0), ERR(SYS_ERR_INVAL), "vm_unknown_operation_inval");

    void* none = vm_new(0, VM_RW);
    EXPECT(none == NULL && mos_vm_last_error() == ERR(SYS_ERR_INVAL), "vm_last_error_after_failure",
           "ptr=%llx err=%ld", PTR(none), mos_vm_last_error());
    p = (uint8_t*)vm_new(VM_PAGE, VM_RW);
    EXPECT(p != NULL && mos_vm_last_error() == 0, "vm_last_error_cleared_by_success", "err=%ld", mos_vm_last_error());
    if (p) mos_vm_unmap(p, VM_PAGE);

    /* reserve / commit */
    uint8_t* res = (uint8_t*)mos_vm_reserve(1u << 20);
    EXPECT(res != NULL, "vm_reserve_maps_none", "failed: %ld", mos_vm_last_error());
    if (res) {
        EXPECT_RET(mos_vm_commit(res, 16 * VM_PAGE), SYS_SUCCESS, "vm_commit_prefix");
        vm_fill(res, 16 * VM_PAGE, 0x33);
        EXPECT(vm_matches(res, 16 * VM_PAGE, 0x33), "vm_commit_prefix_usable", "data changed");
        EXPECT_RET(mos_vm_commit(res + 16 * VM_PAGE, 16 * VM_PAGE), SYS_SUCCESS, "vm_commit_next_part");
        EXPECT(vm_is_zero(res + 16 * VM_PAGE, 16 * VM_PAGE), "vm_commit_next_part_zero", "not zero");
        mos_vm_unmap(res, 1u << 20);
    }
}

static void test_vm_fixed_and_hint(void) {
    stage("vm_fixed");
    long e = 0;
    uint8_t* f = (uint8_t*)mos_vm_map_ex((void*)(uintptr_t)VM_FIX_A, 2 * VM_PAGE, VM_RW, MOS_VM_MAP_FIXED, &e);
    EXPECT(PTR(f) == VM_FIX_A, "vm_fixed_at_free_address", "got 0x%llx, error %ld (%s)", PTR(f), e, ename(e));
    if (!f) return;
    EXPECT(vm_is_zero(f, 2 * VM_PAGE), "vm_fixed_pages_read_zero", "not zero");
    vm_fill(f, 2 * VM_PAGE, 0x44);
    uintptr_t fa = (uintptr_t)VM_FIX_A;
    EXPECT_RET(vm_map_result((void*)fa, 2 * VM_PAGE, VM_RW, MOS_VM_MAP_FIXED), ERR(SYS_ERR_INVAL), "vm_fixed_over_existing_fails");
    EXPECT_RET(vm_map_result((void*)(fa + VM_PAGE), 2 * VM_PAGE, VM_RW, MOS_VM_MAP_FIXED), ERR(SYS_ERR_INVAL), "vm_fixed_over_tail_fails");
    EXPECT_RET(vm_map_result((void*)(fa - VM_PAGE), 2 * VM_PAGE, VM_RW, MOS_VM_MAP_FIXED), ERR(SYS_ERR_INVAL), "vm_fixed_over_head_fails");
    EXPECT_RET(vm_map_result((void*)(fa + 0x100000 + 1), VM_PAGE, VM_RW, MOS_VM_MAP_FIXED), ERR(SYS_ERR_INVAL), "vm_fixed_unaligned_fails");
    EXPECT_RET(vm_map_result(NULL, VM_PAGE, VM_RW, MOS_VM_MAP_FIXED), ERR(SYS_ERR_INVAL), "vm_fixed_null_address_fails");
    EXPECT_RET(vm_map_result((void*)0x1000, VM_PAGE, VM_RW, MOS_VM_MAP_FIXED), ERR(SYS_ERR_INVAL), "vm_fixed_low_address_fails");
    EXPECT_RET(vm_map_result((void*)(uintptr_t)TIB, VM_PAGE, VM_RW, MOS_VM_MAP_FIXED), ERR(SYS_ERR_INVAL), "vm_fixed_over_program_image_fails");
    EXPECT_RET(vm_map_result((void*)(uintptr_t)0x7FFFFFE00000ULL, VM_PAGE, VM_RW, MOS_VM_MAP_FIXED), ERR(SYS_ERR_INVAL), "vm_fixed_above_user_space_fails");
    EXPECT(vm_matches(f, 2 * VM_PAGE, 0x44), "vm_fixed_failures_leave_data_alone", "data changed");

    /* No guard pages for a fixed mapping: the next page can be taken. */
    uint8_t* g = (uint8_t*)mos_vm_map((void*)(fa + 2 * VM_PAGE), VM_PAGE, VM_RW, MOS_VM_MAP_FIXED);
    EXPECT(PTR(g) == VM_FIX_A + 2 * VM_PAGE, "vm_fixed_adjacent_allowed", "got 0x%llx err %ld", PTR(g), mos_vm_last_error());
    EXPECT_RET(mos_vm_unmap(f, 3 * VM_PAGE), SYS_SUCCESS, "vm_fixed_unmap_both");
    uint8_t* again = (uint8_t*)mos_vm_map((void*)fa, VM_PAGE, VM_RW, MOS_VM_MAP_FIXED);
    EXPECT(PTR(again) == VM_FIX_A, "vm_fixed_after_unmap_succeeds", "got 0x%llx err %ld", PTR(again), mos_vm_last_error());
    EXPECT(again && vm_is_zero(again, VM_PAGE), "vm_fixed_reuse_reads_zero", "old data came back");
    if (again) mos_vm_unmap(again, VM_PAGE);

    /* Hints. */
    uint8_t* h = (uint8_t*)mos_vm_map((void*)(uintptr_t)VM_HINT_A, 2 * VM_PAGE, VM_RW, 0);
    EXPECT(PTR(h) == VM_HINT_A, "vm_hint_honoured_when_free", "got 0x%llx err %ld", PTR(h), mos_vm_last_error());
    uint8_t* h2 = (uint8_t*)mos_vm_map((void*)(uintptr_t)VM_HINT_A, 2 * VM_PAGE, VM_RW, 0);
    EXPECT(h2 != NULL && PTR(h2) >= VM_HINT_A + 3 * VM_PAGE, "vm_hint_busy_picks_a_higher_spot",
           "got 0x%llx err %ld", PTR(h2), mos_vm_last_error());
    uint8_t* h3 = (uint8_t*)mos_vm_map((void*)(uintptr_t)(VM_HINT_A + 0x400000 + 5), VM_PAGE, VM_RW, 0);
    EXPECT(h3 != NULL && ((uintptr_t)h3 & (VM_PAGE - 1)) == 0 && PTR(h3) >= VM_HINT_A + 0x400000,
           "vm_hint_unaligned_rounds_up", "got 0x%llx err %ld", PTR(h3), mos_vm_last_error());
    if (h) mos_vm_unmap(h, 2 * VM_PAGE);
    if (h2) mos_vm_unmap(h2, 2 * VM_PAGE);
    if (h3) mos_vm_unmap(h3, VM_PAGE);
}

/* ------------------------------------------------------ unmap / protect */

static void test_vm_unmap(void) {
    stage("vm_unmap");
    uint8_t* p = (uint8_t*)vm_new(3 * VM_PAGE, VM_RW);
    if (!p) { check_fail("vm_unmap_setup", "map failed: %ld", mos_vm_last_error()); return; }
    for (int i = 0; i < 3; i++) vm_fill(p + i * VM_PAGE, VM_PAGE, (uint8_t)(0x50 + i));

    EXPECT_RET(mos_vm_unmap(p + VM_PAGE, VM_PAGE), SYS_SUCCESS, "vm_unmap_middle");
    EXPECT(vm_matches(p, VM_PAGE, 0x50) && vm_matches(p + 2 * VM_PAGE, VM_PAGE, 0x52),
           "vm_unmap_middle_pieces_keep_data", "a remaining piece lost its data");
    uint8_t* hole = (uint8_t*)mos_vm_map(p + VM_PAGE, VM_PAGE, VM_RW, MOS_VM_MAP_FIXED);
    EXPECT(hole == p + VM_PAGE, "vm_unmap_middle_hole_is_free", "fixed map in the hole: %llx err %ld", PTR(hole), mos_vm_last_error());
    EXPECT(hole && vm_is_zero(hole, VM_PAGE), "vm_unmap_middle_hole_reads_zero_when_remapped", "old data came back");
    if (hole) mos_vm_unmap(hole, VM_PAGE);
    EXPECT_RET(mos_vm_unmap(p, VM_PAGE), SYS_SUCCESS, "vm_unmap_first_piece");
    EXPECT(vm_matches(p + 2 * VM_PAGE, VM_PAGE, 0x52), "vm_unmap_last_piece_keeps_data", "data lost");
    EXPECT_RET(mos_vm_unmap(p, 3 * VM_PAGE), SYS_SUCCESS, "vm_unmap_range_with_holes_succeeds");
    EXPECT_RET(mos_vm_unmap(p, 3 * VM_PAGE), SYS_SUCCESS, "vm_unmap_twice_succeeds");
    uint8_t* same = (uint8_t*)mos_vm_map(p, 3 * VM_PAGE, VM_RW, MOS_VM_MAP_FIXED);
    EXPECT(same == p, "vm_unmap_whole_range_is_free", "fixed map got %llx err %ld", PTR(same), mos_vm_last_error());
    if (same) mos_vm_unmap(same, 3 * VM_PAGE);

    EXPECT_RET(mos_vm_unmap((void*)(uintptr_t)(VM_HINT_A + 0x10000000), 8 * VM_PAGE), SYS_SUCCESS, "vm_unmap_unmapped_range_succeeds");
    p = (uint8_t*)vm_new(2 * VM_PAGE, VM_RW);
    if (p) {
        EXPECT_RET(mos_vm_unmap(p + 1, VM_PAGE), ERR(SYS_ERR_INVAL), "vm_unmap_unaligned_inval");
        EXPECT_RET(mos_vm_unmap(p, 0), ERR(SYS_ERR_INVAL), "vm_unmap_zero_length_inval");
        EXPECT_RET(mos_vm_unmap((void*)0x1000, VM_PAGE), ERR(SYS_ERR_INVAL), "vm_unmap_low_address_inval");
        EXPECT_RET(mos_vm_unmap(p, 1), SYS_SUCCESS, "vm_unmap_length_rounds_up");
        EXPECT_RET(mos_vm_protect(p + VM_PAGE, VM_PAGE, VM_RW), SYS_SUCCESS, "vm_unmap_length_1_left_second_page");
        EXPECT_RET(mos_vm_protect(p, VM_PAGE, VM_RW), ERR(SYS_ERR_INVAL), "vm_unmap_length_1_took_first_page");
        mos_vm_unmap(p, 2 * VM_PAGE);
    }

    expect_vm_fault("vm_hole", NULL, 0, "vm_unmap_middle_hole_faults");
    expect_vm_fault("vm_unmapped", NULL, 0, "vm_unmap_whole_then_access_faults");
}

static void test_vm_protect(void) {
    stage("vm_protect");
    uint8_t* p = (uint8_t*)vm_new(4 * VM_PAGE, VM_RW);
    if (!p) { check_fail("vm_protect_setup", "map failed: %ld", mos_vm_last_error()); return; }
    vm_fill(p, 4 * VM_PAGE, 0x61);
    EXPECT_RET(mos_vm_protect(p, 4 * VM_PAGE, MOS_VM_PROT_READ), SYS_SUCCESS, "vm_protect_rw_to_r");
    EXPECT(vm_matches(p, 4 * VM_PAGE, 0x61), "vm_protect_r_keeps_data", "data changed");
    EXPECT_RET(mos_vm_protect(p, 4 * VM_PAGE, VM_RW), SYS_SUCCESS, "vm_protect_r_to_rw");
    p[5] = 0xEE;
    EXPECT(p[5] == 0xEE, "vm_protect_rw_writable_again", "write did not stick");
    vm_fill(p, 4 * VM_PAGE, 0x62);

    EXPECT_RET(mos_vm_protect(p, 4 * VM_PAGE, MOS_VM_PROT_NONE), SYS_SUCCESS, "vm_protect_to_none");
    EXPECT_RET(mos_vm_protect(p, 4 * VM_PAGE, VM_RW), SYS_SUCCESS, "vm_protect_none_to_rw");
    EXPECT(vm_matches(p, 4 * VM_PAGE, 0x62), "vm_protect_none_keeps_contents", "data changed");

    /* Part of a mapping: the rest stays as it was. */
    EXPECT_RET(mos_vm_protect(p + VM_PAGE, 2 * VM_PAGE, MOS_VM_PROT_READ), SYS_SUCCESS, "vm_protect_middle_to_r");
    p[0] = 0x71; p[3 * VM_PAGE] = 0x72;
    EXPECT(p[0] == 0x71 && p[3 * VM_PAGE] == 0x72, "vm_protect_middle_leaves_ends_writable", "ends not writable");
    EXPECT_RET(mos_vm_protect(p, 4 * VM_PAGE, VM_RW), SYS_SUCCESS, "vm_protect_whole_back_to_rw");

    /* A hole in the range: INVAL, and nothing changes. */
    EXPECT_RET(mos_vm_unmap(p + VM_PAGE, VM_PAGE), SYS_SUCCESS, "vm_protect_make_hole");
    EXPECT_RET(mos_vm_protect(p, 4 * VM_PAGE, MOS_VM_PROT_READ), ERR(SYS_ERR_INVAL), "vm_protect_over_hole_inval");
    p[0] = 0x73; p[3 * VM_PAGE] = 0x74;
    EXPECT(p[0] == 0x73 && p[3 * VM_PAGE] == 0x74, "vm_protect_over_hole_changes_nothing", "pages became read-only");
    EXPECT_RET(mos_vm_protect((void*)(uintptr_t)(VM_HINT_A + 0x10000000), VM_PAGE, VM_RW), ERR(SYS_ERR_INVAL), "vm_protect_unmapped_inval");
    EXPECT_RET(mos_vm_protect(p + 1, VM_PAGE, VM_RW), ERR(SYS_ERR_INVAL), "vm_protect_unaligned_inval");
    EXPECT_RET(mos_vm_protect(p, 0, VM_RW), ERR(SYS_ERR_INVAL), "vm_protect_zero_length_inval");
    EXPECT_RET(mos_vm_protect(p, VM_PAGE, 8), ERR(SYS_ERR_INVAL), "vm_protect_bad_prot_inval");
    mos_vm_unmap(p, 4 * VM_PAGE);

    expect_vm_fault("vm_ro_write", NULL, 0, "vm_protect_write_to_read_only_faults");
    expect_vm_fault("vm_none_read", NULL, 0, "vm_protect_read_of_none_faults");
    expect_vm_fault("vm_none_write", NULL, 0, "vm_protect_write_to_none_faults");
}

static void test_vm_decommit(void) {
    stage("vm_decommit");
    uint8_t* p = (uint8_t*)vm_new(4 * VM_PAGE, VM_RW);
    if (!p) { check_fail("vm_decommit_setup", "map failed: %ld", mos_vm_last_error()); return; }
    for (int i = 0; i < 4; i++) vm_fill(p + i * VM_PAGE, VM_PAGE, (uint8_t)(0x81 + i));
    EXPECT_RET(mos_vm_decommit(p + VM_PAGE, 2 * VM_PAGE), SYS_SUCCESS, "vm_decommit_middle");
    EXPECT(vm_is_zero(p + VM_PAGE, 2 * VM_PAGE), "vm_decommit_reads_zero", "data survived");
    EXPECT(vm_matches(p, VM_PAGE, 0x81) && vm_matches(p + 3 * VM_PAGE, VM_PAGE, 0x84),
           "vm_decommit_neighbours_keep_data", "neighbour data lost");
    vm_fill(p + VM_PAGE, 2 * VM_PAGE, 0x90);
    EXPECT(vm_matches(p + VM_PAGE, 2 * VM_PAGE, 0x90), "vm_decommit_pages_still_mapped_and_writable", "data did not stick");
    EXPECT_RET(mos_vm_protect(p + VM_PAGE, 2 * VM_PAGE, VM_RW), SYS_SUCCESS, "vm_decommit_pages_still_protectable");
    EXPECT_RET(mos_vm_decommit(p, 4 * VM_PAGE), SYS_SUCCESS, "vm_decommit_whole");
    EXPECT(vm_is_zero(p, 4 * VM_PAGE), "vm_decommit_whole_reads_zero", "data survived");
    EXPECT_RET(mos_vm_decommit(p, 4 * VM_PAGE), SYS_SUCCESS, "vm_decommit_twice_succeeds");
    EXPECT_RET(mos_vm_decommit(p + 1, VM_PAGE), ERR(SYS_ERR_INVAL), "vm_decommit_unaligned_inval");
    EXPECT_RET(mos_vm_decommit(p, 0), ERR(SYS_ERR_INVAL), "vm_decommit_zero_length_inval");
    EXPECT_RET(mos_vm_decommit(p, 5 * VM_PAGE), ERR(SYS_ERR_INVAL), "vm_decommit_past_the_end_inval");
    EXPECT_RET(mos_vm_decommit((void*)(uintptr_t)(VM_HINT_A + 0x10000000), VM_PAGE), ERR(SYS_ERR_INVAL), "vm_decommit_unmapped_inval");
    mos_vm_unmap(p, 4 * VM_PAGE);
}

/* ------------------------------------------------- physical memory use */

static void test_vm_memory(void) {
    stage("vm_memory");
    const size_t GIB = (size_t)1 << 30;
    if (vm_free_pages() < 0 || (uint64_t)vm_free_pages() == (uint64_t)ERR(SYS_ERR_INVAL)) {
        check_fail("vm_memory_sysinfo", "SYS_SYSINFO page counters unavailable");
        return;
    }
    long f0 = vm_free_pages();
    uint8_t* big = (uint8_t*)mos_vm_map(NULL, GIB, VM_RW, 0);
    EXPECT(big != NULL, "vm_map_1gib_succeeds", "failed: %ld (%s)", mos_vm_last_error(), ename(mos_vm_last_error()));
    if (big) {
        long f1 = vm_free_pages();
        EXPECT(f0 - f1 <= VM_PAGES_SLACK, "vm_untouched_gib_costs_no_pages", "used %ld pages", f0 - f1);
        EXPECT(big[0] == 0 && big[GIB / 2] == 0 && big[GIB - 1] == 0, "vm_gib_reads_zero_at_ends_and_middle", "not zero");
        long f1b = vm_free_pages();
        const int N = 256;
        for (int i = 0; i < N; i++) big[(size_t)i * VM_PAGE + 1] = (uint8_t)(i + 1);
        long f2 = vm_free_pages();
        long d = f1b - f2;
        EXPECT(d >= N - 16 && d <= N + VM_PAGES_SLACK, "vm_touching_256_pages_costs_about_256", "used %ld pages", d);
        int ok = 1;
        for (int i = 0; i < N; i++) if (big[(size_t)i * VM_PAGE + 1] != (uint8_t)(i + 1)) ok = 0;
        EXPECT(ok, "vm_touched_pages_keep_data", "data changed");
        EXPECT_RET(mos_vm_decommit(big, 128 * VM_PAGE), SYS_SUCCESS, "vm_decommit_128_touched_pages");
        long f3 = vm_free_pages();
        EXPECT(f3 - f2 >= 112 && f3 - f2 <= 128 + VM_PAGES_SLACK, "vm_decommit_gives_pages_back", "got back %ld pages", f3 - f2);
        EXPECT_RET(mos_vm_unmap(big, GIB), SYS_SUCCESS, "vm_unmap_1gib");
        long f4 = vm_free_pages();
        EXPECT(f4 - f3 >= 112, "vm_unmap_gives_pages_back", "got back %ld pages", f4 - f3);
        EXPECT(f0 - f4 <= VM_PAGES_SLACK, "vm_gib_cycle_leaks_nothing", "%ld pages still gone", f0 - f4);
    }

    /* POPULATE: the pages are taken at once. */
    f0 = vm_free_pages();
    uint8_t* pop = (uint8_t*)mos_vm_map(NULL, 64 * VM_PAGE, VM_RW, MOS_VM_MAP_POPULATE);
    EXPECT(pop != NULL, "vm_populate_small_succeeds", "failed: %ld", mos_vm_last_error());
    if (pop) {
        long f1 = vm_free_pages();
        EXPECT(f0 - f1 >= 60 && f0 - f1 <= 64 + VM_PAGES_SLACK, "vm_populate_takes_pages_immediately", "used %ld pages", f0 - f1);
        EXPECT(vm_is_zero(pop, 64 * VM_PAGE), "vm_populate_pages_read_zero", "not zero");
        long f2 = vm_free_pages();
        EXPECT(f1 - f2 <= VM_PAGES_SLACK, "vm_populate_touching_costs_nothing_more", "used %ld more pages", f1 - f2);
        mos_vm_unmap(pop, 64 * VM_PAGE);
        long f3 = vm_free_pages();
        EXPECT(f3 - f2 >= 60, "vm_populate_unmap_gives_pages_back", "got back %ld pages", f3 - f2);
    }

    /* POPULATE of something that cannot fit: NOMEM, no crash, no leak. */
    f0 = vm_free_pages();
    long e = 0;
    void* huge = mos_vm_map_ex(NULL, (size_t)64 << 30, VM_RW, MOS_VM_MAP_POPULATE, &e);
    EXPECT(huge == NULL && e == ERR(SYS_ERR_NOMEM), "vm_populate_64gib_fails_nomem", "ptr=%llx err=%ld (%s)", PTR(huge), e, ename(e));
    if (huge) mos_vm_unmap(huge, (size_t)64 << 30);
    long f1 = vm_free_pages();
    EXPECT(f0 - f1 <= VM_PAGES_SLACK, "vm_populate_failure_leaks_no_pages", "%ld pages gone", f0 - f1);
    uint8_t* after = (uint8_t*)vm_new(VM_PAGE, VM_RW);
    EXPECT(after != NULL && (after[0] = 1, after[0] == 1), "vm_map_works_after_failed_populate", "failed: %ld", mos_vm_last_error());
    if (after) mos_vm_unmap(after, VM_PAGE);

    /* Many map/unmap cycles. */
    f0 = vm_free_pages();
    int failures = 0, dirty = 0, bad_addr = 0;
    for (int i = 0; i < 2000; i++) {
        size_t len = (size_t)(1 + i % 8) * VM_PAGE;
        uint8_t* p = (uint8_t*)vm_new(len, VM_RW);
        if (!p) { failures++; continue; }
        if (PTR(p) < VM_MMAP_BASE || ((uintptr_t)p & (VM_PAGE - 1))) bad_addr++;
        if (p[0] || p[len - 1]) dirty++;            /* a reused frame must come back zeroed */
        p[0] = (uint8_t)(i | 1);
        p[len - 1] = (uint8_t)(i | 1);
        if (p[0] != (uint8_t)(i | 1)) failures++;
        if (mos_vm_unmap(p, len) != 0) failures++;
    }
    long fe = vm_free_pages();
    EXPECT(failures == 0 && bad_addr == 0, "vm_2000_map_unmap_cycles_work", "failures=%d bad addresses=%d", failures, bad_addr);
    EXPECT(dirty == 0, "vm_2000_cycles_fresh_pages_always_zero", "%d mappings started with old data", dirty);
    EXPECT(f0 - fe <= VM_PAGES_SLACK, "vm_2000_map_unmap_cycles_leak_no_pages", "%ld pages gone", f0 - fe);

    /* Cycles with POPULATE too (frames taken and given back every time). */
    f0 = vm_free_pages();
    failures = 0;
    for (int i = 0; i < 300; i++) {
        uint8_t* p = (uint8_t*)mos_vm_map(NULL, 8 * VM_PAGE, VM_RW, MOS_VM_MAP_POPULATE);
        if (!p) { failures++; continue; }
        if (mos_vm_unmap(p, 8 * VM_PAGE) != 0) failures++;
    }
    fe = vm_free_pages();
    EXPECT(failures == 0 && f0 - fe <= VM_PAGES_SLACK, "vm_300_populate_cycles_leak_no_pages", "failures=%d, %ld pages gone", failures, f0 - fe);
}

/* ------------------------------------------- guards, NX, code in memory */

static void test_vm_faults(void) {
    stage("vm_faults");
    expect_vm_fault("vm_guard_before", NULL, 0, "vm_guard_page_before_mapping_faults");
    expect_vm_fault("vm_guard_after", NULL, 0, "vm_guard_page_after_mapping_faults");
    expect_vm_fault("vm_nx_data", NULL, 0, "vm_nx_exec_in_non_exec_mapping_faults");
    expect_vm_fault("vm_nx_stack", NULL, 0, "vm_nx_exec_on_main_stack_faults");
    /* The heap is non-executable (proc_map_user_heap_range sets NX). */
    expect_vm_fault("vm_nx_heap", NULL, 0, "vm_nx_exec_in_malloc_memory_faults");
    expect_vm_ok("vm_exec", NULL, 0, "vm_exec_in_exec_mapping_runs");
}

/* ------------------------------------------------------------- stacks */

static void test_vm_stacks(void) {
    stage("vm_stacks");
    const size_t MIB = 1024u * 1024u;
    expect_vm_ok("vm_stack_use", "4096", 0, "vm_stack_default_uses_4mib");
    expect_vm_fault("vm_stack_use", "16384", 0, "vm_stack_default_16mib_overflows_into_guard");
    expect_vm_ok("vm_stack_use", "128", 256 * 1024, "vm_stack_256k_uses_128k");
    expect_vm_fault("vm_stack_use", "1024", 256 * 1024, "vm_stack_256k_using_1mib_faults");
    expect_vm_ok("vm_stack_use", "16384", 32 * MIB, "vm_stack_32mib_uses_16mib");
    expect_vm_fault("vm_stack_use", "65536", 32 * MIB, "vm_stack_32mib_using_64mib_faults");

    /* Out-of-range sizes are clamped, not refused. */
    expect_vm_ok("exit", "0", 1, "vm_stack_size_1_is_clamped_to_minimum");
    expect_vm_ok("exit", "0", (size_t)4 << 30, "vm_stack_size_4gib_is_clamped_to_maximum");
    expect_vm_ok("exit", "0", 64 * 1024, "vm_stack_size_64k_minimum_works");
    expect_vm_ok("exit", "0", 1024 * MIB, "vm_stack_size_1gib_maximum_works");

    /* A thread on a SYS_VM stack that runs off its end hits the guard. */
    expect_vm_fault("vm_thread_overflow", NULL, 0, "vm_thread_stack_overflow_faults");
}

/* ------------------------------------------------------------- layout */

static void test_vm_layout(void) {
    stage("vm_layout");
    volatile char local = 0;
    uint64_t sp = PTR(&local);
    uint64_t main_addr = PTR((int (*)(int, char**, char**))main);
    EXPECT(main_addr >= TIB && main_addr < VM_MMAP_BASE, "layout_image_at_or_above_1tib", "main is at 0x%llx", main_addr);
    EXPECT(PTR(&g_checks) >= TIB && PTR(&g_checks) < VM_MMAP_BASE, "layout_data_in_the_image", "g_checks is at 0x%llx", PTR(&g_checks));
    EXPECT(sp < VM_STACK_TOP && sp >= VM_STACK_TOP - 8u * 1024u * 1024u, "layout_stack_below_0x7ffff0000000", "a local is at 0x%llx", sp);
    EXPECT(VM_STACK_TOP - sp <= 1024u * 1024u, "layout_stack_use_starts_at_the_top", "a local is %llu bytes below the top", VM_STACK_TOP - sp);
    EXPECT(PTR(g_argv) >= VM_STACK_TOP, "layout_argv_above_the_stack_top", "argv is at 0x%llx", PTR(g_argv));
    EXPECT(g_argv && PTR(g_argv[0]) >= VM_STACK_TOP, "layout_argv0_string_above_the_stack_top", "argv[0] is at 0x%llx", PTR(g_argv ? g_argv[0] : 0));
    EXPECT(PTR(g_envp) >= VM_STACK_TOP, "layout_envp_above_the_stack_top", "envp is at 0x%llx", PTR(g_envp));
    void* h = malloc(1024);
    EXPECT(h != NULL && PTR(h) < 0x100000000ULL, "layout_malloc_below_4gib", "malloc gave 0x%llx", PTR(h));
    uint8_t* v = (uint8_t*)vm_new(VM_PAGE, VM_RW);
    EXPECT(v != NULL && PTR(v) >= VM_MMAP_BASE && PTR(v) < VM_STACK_TOP - 1024u * 1024u * 1024u, "layout_vm_map_between_2tib_and_stack", "mapping at 0x%llx", PTR(v));
    if (v) mos_vm_unmap(v, VM_PAGE);
    free(h);
}

/* ------------------------------------------- system calls and mappings */

static void test_vm_syscalls(void) {
    stage("vm_syscalls");
    const size_t N = 6000;
    const char* path = TMP "/vm_io.bin";
    uint8_t* a = (uint8_t*)vm_new(2 * VM_PAGE, VM_RW);
    uint8_t* b = (uint8_t*)vm_new(2 * VM_PAGE, VM_RW);
    if (!a || !b) { check_fail("vm_syscalls_setup", "map failed: %ld", mos_vm_last_error()); return; }
    vm_fill(a, N, 0x5A);

    if (exists(path)) mos_delete(path);
    EXPECT_RET(mos_create(path, NULL, NULL), 0, "vm_io_create_file");
    long fd = mos_open(path, SYS_O_RDWR);
    long w = fd >= 0 ? mos_fwrite(fd, a, N) : fd;
    if (fd >= 0) mos_close(fd);
    EXPECT(w == (long)N, "vm_io_write_file_from_mapping", "wrote %ld of %lu", w, (unsigned long)N);
    EXPECT(mos_file_size(path) == (long)N, "vm_io_file_has_the_size", "size %ld", mos_file_size(path));

    fd = mos_open(path, SYS_O_RDONLY);
    long got = 0, r = 0;
    while (fd >= 0 && got < (long)N && (r = mos_read(fd, b + got, N - (size_t)got)) > 0) got += r;
    if (fd >= 0) mos_close(fd);
    EXPECT(got == (long)N, "vm_io_read_file_into_mapping", "read %ld of %lu (last result %ld)", got, (unsigned long)N, r);
    EXPECT(got == (long)N && vm_matches(b, N, 0x5A), "vm_io_data_round_trips", "data differs");

    syscall_stat_t* st = (syscall_stat_t*)(void*)(b + 4096);
    memset(st, 0xCC, sizeof(*st));
    EXPECT_RET(mos_stat(path, st), SYS_SUCCESS, "vm_stat_into_mapping");
    EXPECT(st->type == SYS_STAT_FILE && st->size == N, "vm_stat_result_in_mapping", "type %d size %llu", (int)st->type, (unsigned long long)st->size);

    char* mp = (char*)a + 100;
    scopy(mp, 200, path);
    syscall_stat_t local;
    memset(&local, 0, sizeof(local));
    EXPECT_RET(mos_stat(mp, &local), SYS_SUCCESS, "vm_stat_path_in_mapping");
    EXPECT(local.size == N, "vm_stat_path_in_mapping_result", "size %llu", (unsigned long long)local.size);

    /* Buffers that are not all in one usable mapping: INVAL. */
    syscall_stat_t* edge = (syscall_stat_t*)(void*)(a + VM_PAGE + VM_PAGE - sizeof(*st) / 2);
    EXPECT_RET(mos_stat(path, edge), ERR(SYS_ERR_INVAL), "vm_stat_out_straddling_mapping_end_inval");
    EXPECT_RET(mos_stat(path, (syscall_stat_t*)(void*)(a + 2 * VM_PAGE)), ERR(SYS_ERR_INVAL), "vm_stat_out_in_guard_page_inval");
    syscall_stat_t* ro = (syscall_stat_t*)(void*)b;
    EXPECT_RET(mos_vm_protect(b, VM_PAGE, MOS_VM_PROT_READ), SYS_SUCCESS, "vm_stat_make_read_only");
    EXPECT_RET(mos_stat(path, ro), ERR(SYS_ERR_INVAL), "vm_stat_out_in_read_only_mapping_inval");
    EXPECT_RET(mos_vm_protect(b, VM_PAGE, MOS_VM_PROT_NONE), SYS_SUCCESS, "vm_stat_make_no_access");
    EXPECT_RET(mos_stat(path, ro), ERR(SYS_ERR_INVAL), "vm_stat_out_in_no_access_mapping_inval");
    EXPECT_RET(mos_stat((const char*)b, &local), ERR(SYS_ERR_INVAL), "vm_stat_path_in_no_access_mapping_inval");
    char* tail = (char*)a + 2 * VM_PAGE - 12;
    for (int i = 0; i < 12; i++) tail[i] = 'a';           /* no NUL before the guard page */
    EXPECT_RET(mos_stat(tail, &local), ERR(SYS_ERR_INVAL), "vm_stat_unterminated_path_at_mapping_end_inval");

    /* Note: SYS_READ does not validate its buffer in the kernel today, so
     * reading a file into a read-only mapping is deliberately not asserted. */

    mos_vm_unmap(a, 2 * VM_PAGE);
    mos_vm_unmap(b, 2 * VM_PAGE);
    mos_delete(path);
}

/* ------------------------------------------------------------ threads */

static volatile uint64_t g_vmt_value;
static volatile uint64_t g_vmt_sp;
static volatile uint32_t g_vmt_done[4];
static volatile uint64_t g_vmt_stack[4];
static volatile uint32_t g_stub_word;     /* the machine-code thread stores 42 here */
static volatile uint32_t g_stub_exit;     /* its exit futex */

static void vm_thread_fn(void* arg) {
    volatile uint8_t big[16384];
    unsigned sum = 0;
    for (unsigned i = 0; i < sizeof(big); i++) big[i] = (uint8_t)i;
    for (unsigned i = 0; i < sizeof(big); i++) sum += big[i];
    g_vmt_sp = PTR(big);
    g_vmt_value = (uint64_t)(uintptr_t)arg + sum;
}

static void vm_thread_multi(void* arg) {
    uintptr_t i = (uintptr_t)arg;
    volatile uint8_t buf[2048];
    buf[0] = (uint8_t)i;
    mos_sleep(2 + (long)i);
    g_vmt_stack[i] = PTR(buf);
    g_vmt_done[i] = buf[0] == (uint8_t)i ? 1u : 2u;
}

/* Thread: mov dword [rdi],42 ; mov eax,SYS_EXIT ; xor edi,edi ; int 0x80 */
static const uint8_t k_thread_stub[] = {
    0xC7, 0x07, 0x2A, 0x00, 0x00, 0x00,
    0xB8, (uint8_t)SYS_EXIT, 0x00, 0x00, 0x00,
    0x31, 0xFF,
    0xCD, 0x80,
};

/* Starts a thread at `entry` on the stack [base, base+len) and waits (up to
 * about 2 s) for it. Returns the kernel's answer: thread id or error;
 * *finished says whether it ended. */
static long vm_raw_thread(void* entry, void* base, size_t len, int* finished) {
    g_stub_word = 0;
    g_stub_exit = 1;
    syscall_thread_ex_t rq;
    rq.entry = PTR(entry);
    rq.stack_top = PTR(base) + len;
    rq.arg = PTR(&g_stub_word);
    rq.fs_base = 0;
    rq.exit_futex = PTR(&g_stub_exit);
    long id = mos_syscall(SYS_THREAD, SYS_THREAD_CREATE_EX, (long)(uintptr_t)&rq, 0);
    *finished = 0;
    if (id <= 0) return id;
    for (int i = 0; i < 40 && g_stub_exit; i++) mos_futex_wait(&g_stub_exit, 1, 50ull * 1000000ull);
    *finished = g_stub_exit == 0;
    return id;
}

static void test_vm_threads(void) {
    stage("vm_threads");
    mos_thread_t t;
    g_vmt_value = 0;
    g_vmt_sp = 0;
    int created = mos_thread_create_vm(&t, vm_thread_fn, (void*)1000, 64 * 1024, NULL);
    EXPECT(created == 0, "vm_thread_on_vm_stack_created", "create returned %d", created);
    if (created == 0) {
        uint8_t* stack = (uint8_t*)t.stack;
        size_t size = t.stack_size;
        EXPECT(PTR(stack) >= VM_MMAP_BASE && size == 64 * 1024, "vm_thread_stack_is_a_vm_mapping", "stack 0x%llx size %lu", PTR(stack), (unsigned long)size);
        mos_thread_join(&t);
        unsigned sum = 0;
        for (unsigned i = 0; i < 16384; i++) sum += (uint8_t)i;
        EXPECT(g_vmt_value == 1000u + sum, "vm_thread_ran_and_joined", "value %llu, expected %u", (unsigned long long)g_vmt_value, 1000u + sum);
        EXPECT(g_vmt_sp >= PTR(stack) && g_vmt_sp < PTR(stack) + size, "vm_thread_used_its_vm_stack", "local at 0x%llx, stack 0x%llx..", (unsigned long long)g_vmt_sp, PTR(stack));
        EXPECT(t.stack == NULL && t.id == 0, "vm_thread_join_clears_the_handle", "stack=%llx id=%ld", PTR(t.stack), t.id);
        void* again = mos_vm_map(stack, size, VM_RW, MOS_VM_MAP_FIXED);
        EXPECT(again == (void*)stack, "vm_thread_join_unmaps_the_stack", "fixed map at the old stack: %llx err %ld", PTR(again), mos_vm_last_error());
        if (again) mos_vm_unmap(again, size);
    }

    /* Several at once: distinct stacks, all finish. */
    mos_thread_t ts[4];
    int started = 0;
    for (uintptr_t i = 0; i < 4; i++) {
        g_vmt_done[i] = 0;
        if (mos_thread_create_vm(&ts[i], vm_thread_multi, (void*)i, 32 * 1024, NULL) == 0) started++;
    }
    for (int i = 0; i < started; i++) mos_thread_join(&ts[i]);
    int finished = 0, distinct = 1;
    for (int i = 0; i < 4; i++) {
        if (g_vmt_done[i] == 1) finished++;
        for (int j = 0; j < i; j++) {
            uint64_t d = g_vmt_stack[i] > g_vmt_stack[j] ? g_vmt_stack[i] - g_vmt_stack[j] : g_vmt_stack[j] - g_vmt_stack[i];
            if (d < 32 * 1024) distinct = 0;
        }
    }
    EXPECT(started == 4 && finished == 4, "vm_four_threads_on_vm_stacks_finish", "started %d finished %d", started, finished);
    EXPECT(distinct, "vm_four_thread_stacks_do_not_overlap", "two stacks too close");

    /* Existing creators are unchanged: malloc stacks still join and free. */
    mos_thread_t m;
    g_vmt_value = 0;
    int mc = mos_thread_create_ex(&m, vm_thread_fn, (void*)5, 0, NULL);
    if (mc == 0) mos_thread_join(&m);
    EXPECT(mc == 0 && g_vmt_value >= 5, "vm_thread_create_ex_with_malloc_stack_still_works", "create %d value %llu", mc, (unsigned long long)g_vmt_value);
    EXPECT(g_vmt_sp < 0x100000000ULL, "vm_thread_create_ex_stack_is_still_heap", "stack local at 0x%llx", (unsigned long long)g_vmt_sp);

    /* Entry points: code in a PROT_EXEC mapping runs; anything else is refused. */
    uint8_t* stk = (uint8_t*)vm_new(2 * VM_PAGE, VM_RW);
    uint8_t* code = (uint8_t*)vm_new(VM_PAGE, VM_RW);
    if (!stk || !code) { check_fail("vm_thread_stub_setup", "map failed: %ld", mos_vm_last_error()); return; }
    for (size_t i = 0; i < sizeof(k_thread_stub); i++) code[i] = k_thread_stub[i];

    int fin = 0;
    long id = vm_raw_thread(code, stk, 2 * VM_PAGE, &fin);
    EXPECT(id < 0, "vm_thread_entry_in_non_exec_mapping_refused", "thread id %ld (%s)", id, ename(id));
    if (id > 0 && !fin) mos_sleep(50);
    EXPECT_RET(id, ERR(SYS_ERR_INVAL), "vm_thread_entry_in_non_exec_mapping_is_inval");

    EXPECT_RET(mos_vm_protect(code, VM_PAGE, MOS_VM_PROT_RX), SYS_SUCCESS, "vm_thread_stub_make_executable");
    id = vm_raw_thread(code, stk, 2 * VM_PAGE, &fin);
    EXPECT(id > 0, "vm_thread_entry_in_exec_mapping_created", "thread id %ld (%s)", id, ename(id));
    EXPECT(fin && g_stub_word == 42, "vm_thread_in_exec_mapping_ran", "finished=%d word=%u", fin, (unsigned)g_stub_word);

    EXPECT_RET(mos_vm_protect(code, VM_PAGE, MOS_VM_PROT_NONE), SYS_SUCCESS, "vm_thread_stub_make_no_access");
    id = vm_raw_thread(code, stk, 2 * VM_PAGE, &fin);
    EXPECT_RET(id, ERR(SYS_ERR_INVAL), "vm_thread_entry_in_no_access_mapping_inval");
    EXPECT_RET(mos_vm_protect(code, VM_PAGE, MOS_VM_PROT_RX), SYS_SUCCESS, "vm_thread_stub_executable_again");

    /* Stack that is not usable memory: refused even with good code. */
    id = vm_raw_thread(code, (void*)(uintptr_t)(VM_HINT_A + 0x20000000), 2 * VM_PAGE, &fin);
    EXPECT_RET(id, ERR(SYS_ERR_INVAL), "vm_thread_stack_in_unmapped_memory_inval");
    id = vm_raw_thread(code, stk + 2 * VM_PAGE, VM_PAGE, &fin);           /* stack = the guard page */
    EXPECT_RET(id, ERR(SYS_ERR_INVAL), "vm_thread_stack_in_guard_page_inval");

    /* Entry in malloc memory or on the main stack: not executable. */
    uint8_t* heap_code = (uint8_t*)malloc(64);
    if (heap_code) {
        for (size_t i = 0; i < sizeof(k_thread_stub); i++) heap_code[i] = k_thread_stub[i];
        id = vm_raw_thread(heap_code, stk, 2 * VM_PAGE, &fin);
        EXPECT_RET(id, ERR(SYS_ERR_INVAL), "vm_thread_entry_in_malloc_memory_inval");
        free(heap_code);
    }
    uint8_t stack_code[32] __attribute__((aligned(16)));
    for (size_t i = 0; i < sizeof(k_thread_stub); i++) ((volatile uint8_t*)stack_code)[i] = k_thread_stub[i];
    id = vm_raw_thread(stack_code, stk, 2 * VM_PAGE, &fin);
    EXPECT_RET(id, ERR(SYS_ERR_INVAL), "vm_thread_entry_on_main_stack_inval");

    mos_vm_unmap(stk, 2 * VM_PAGE);
    mos_vm_unmap(code, VM_PAGE);
}

static void test_vm(void) {
    test_vm_layout();
    test_vm_map();
    test_vm_fixed_and_hint();
    test_vm_unmap();
    test_vm_protect();
    test_vm_decommit();
    test_vm_syscalls();
    test_vm_threads();
    test_vm_memory();
    test_vm_faults();
    test_vm_stacks();
}

/* ================================================================== SIMD
 * Register state across context switches. Each worker loads a pattern of its
 * own into the vector registers (and x87 / MXCSR), lets the kernel do things
 * to it (a sleep that switches to another thread, a page fault on a fresh
 * SYS_VM page, a plain syscall) and stores everything back for comparison.
 * The load, the disturbance and the store are one assembly routine: C code
 * in between would use xmm registers itself. The AVX routine is assembly
 * too, so the rest of the program stays SSE-only. */

#define SIMD_BUF 640     /* [0,512) vector regs, [512,576) 8 doubles, 576 mxcsr, 580 fcw */

/* torture_*(in, out, page, mode): mode bit0 = sleep 1 tick (switches), bit1 =
 * write to `page` (page fault when it is fresh), bit2 = three getpid calls. */
extern void torture_avx(const uint8_t* in, uint8_t* out, uint8_t* page, uint64_t mode);
extern void torture_sse(const uint8_t* in, uint8_t* out, uint8_t* page, uint64_t mode);

#define TORTURE_BODY(LOAD, STORE) \
    "mov r8, rdi\n" "mov r9, rsi\n" "mov r10, rdx\n" "mov r11, rcx\n" \
    "fldcw word ptr [r8+580]\n" "ldmxcsr dword ptr [r8+576]\n" \
    ".irp i,0,1,2,3,4,5,6,7\n" "fld qword ptr [r8+512+8*\\i]\n" ".endr\n" \
    LOAD \
    "test r11, 1\n" "jz 1f\n" "mov eax, 5\n" "mov edi, 1\n" "int 0x80\n" "1:\n" \
    "test r11, 2\n" "jz 2f\n" "mov byte ptr [r10], 0x5a\n" "mov al, byte ptr [r10+64]\n" "2:\n" \
    "test r11, 4\n" "jz 3f\n" "mov eax, 3\n" "int 0x80\n" "mov eax, 3\n" "int 0x80\n" \
    "mov eax, 3\n" "int 0x80\n" "3:\n" \
    STORE \
    ".irp i,7,6,5,4,3,2,1,0\n" "fstp qword ptr [r9+512+8*\\i]\n" ".endr\n" \
    "stmxcsr dword ptr [r9+576]\n" "fnstcw word ptr [r9+580]\n" \
    "sub rsp, 8\n" "mov dword ptr [rsp], 0x1f80\n" "ldmxcsr dword ptr [rsp]\n" \
    "mov word ptr [rsp], 0x37f\n" "fldcw word ptr [rsp]\n" "add rsp, 8\n" "ret\n"

__asm__(".pushsection .text\n.intel_syntax noprefix\n"
        ".globl torture_avx\n.type torture_avx,@function\ntorture_avx:\n"
        TORTURE_BODY(".irp i,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15\n" "vmovdqu ymm\\i, ymmword ptr [r8+32*\\i]\n" ".endr\n",
                     ".irp i,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15\n" "vmovdqu ymmword ptr [r9+32*\\i], ymm\\i\n" ".endr\n")
        ".size torture_avx,.-torture_avx\n"
        ".globl torture_sse\n.type torture_sse,@function\ntorture_sse:\n"
        TORTURE_BODY(".irp i,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15\n" "movdqu xmm\\i, xmmword ptr [r8+16*\\i]\n" ".endr\n",
                     ".irp i,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15\n" "movdqu xmmword ptr [r9+16*\\i], xmm\\i\n" ".endr\n")
        ".size torture_sse,.-torture_sse\n"
        ".att_syntax prefix\n.popsection\n");

typedef struct {
    int id;                      /* distinct per worker: seeds the patterns */
    int wide;                    /* 1: ymm0-15 (AVX), 0: xmm0-15 */
    uint64_t end_ms;
    unsigned long rounds, errors;
    char first_error[160];
} torture_t;

static void torture_fill(uint8_t* in, int id, unsigned long round, int wide) {
    unsigned n = wide ? 512 : 256;
    for (unsigned i = 0; i < n; i++)
        in[i] = (uint8_t)(id * 37 + i * 11 + (i >> 4) * 29 + (unsigned)round * 5 + 1);
    double v[8];
    for (int k = 0; k < 8; k++) v[k] = (double)(id * 1000 + (long)(round % 997) + k) + 0.25 * k;
    memcpy(in + 512, v, sizeof(v));
    uint32_t mxcsr = 0x1F80u | (uint32_t)((id + round) & 3) << 13 | ((id & 1) ? 0x8000u : 0);
    uint16_t fcw = (uint16_t)((0x037F & ~0xF00) | ((id + round) % 3 == 0 ? 0x000 : (id + round) % 3 == 1 ? 0x200 : 0x300)
                              | (((id + round) & 3) << 10));
    memcpy(in + 576, &mxcsr, 4);
    memcpy(in + 580, &fcw, 2);
}

static void torture_one(torture_t* t, unsigned long round, uint8_t* in, uint8_t* out, uint8_t* scratch) {
    torture_fill(in, t->id, round, t->wide);
    memset(out, 0, SIMD_BUF);
    unsigned mode = (unsigned)(round % 8);
    uint8_t* page = scratch;
    if (mode & 2) {
        page = (uint8_t*)mos_vm_map(NULL, VM_PAGE, VM_RW, 0);    /* fresh: the first write faults */
        if (!page) page = scratch;
    }
    if (t->wide) torture_avx(in, out, page, mode);
    else torture_sse(in, out, page, mode);
    if (page != scratch) mos_vm_unmap(page, VM_PAGE);
    unsigned n = t->wide ? 512 : 256;
    for (unsigned i = 0; i < n + 70; i++) {
        unsigned off = i < n ? i : 512 + (i - n);
        if (in[off] == out[off]) continue;
        t->errors++;
        if (!t->first_error[0])
            kfmt(t->first_error, sizeof(t->first_error), "worker %d round %lu mode %u: %s byte %u got %x want %x",
                 t->id, round, mode, off < 512 ? (t->wide ? "ymm" : "xmm") : "x87/mxcsr/fcw",
                 off, out[off], in[off]);
        break;
    }
}

static void torture_loop(torture_t* t) {
    static uint8_t scratch_pages[4][VM_PAGE];
    uint8_t in[SIMD_BUF] __attribute__((aligned(32)));
    uint8_t out[SIMD_BUF] __attribute__((aligned(32)));
    uint8_t* scratch = scratch_pages[t->id & 3];
    unsigned long round = 0;
    while ((uint64_t)mos_uptime() < t->end_ms) torture_one(t, round++, in, out, scratch);
    t->rounds = round;
}

static void torture_thread(void* arg) { torture_loop((torture_t*)arg); }

#define TORTURE_THREADS 3
#define TORTURE_MS      3000

/* os0test.run --child torture_avx|torture_sse <ms>: one more process doing the same. */
static int child_torture(int wide, long ms) {
    torture_t t;
    memset(&t, 0, sizeof(t));
    t.id = 9;
    t.wide = wide;
    t.end_ms = (uint64_t)mos_uptime() + (uint64_t)ms;
    torture_loop(&t);
    emit_fd(1, "torture child: %lu rounds, %lu errors %s", t.rounds, t.errors, t.first_error);
    return t.errors ? 1 : (t.rounds ? 0 : 2);
}

static void run_torture(int wide, const char* name) {
    char nm[96];
    torture_t main_t, th[TORTURE_THREADS];
    mos_thread_t ts[TORTURE_THREADS];
    uint64_t end = (uint64_t)mos_uptime() + TORTURE_MS;
    memset(&main_t, 0, sizeof(main_t));
    main_t.id = 0; main_t.wide = wide; main_t.end_ms = end;
    int started = 0;
    for (int i = 0; i < TORTURE_THREADS; i++) {
        memset(&th[i], 0, sizeof(th[i]));
        th[i].id = i + 1; th[i].wide = wide; th[i].end_ms = end;
        if (mos_thread_create_vm(&ts[i], torture_thread, &th[i], 64 * 1024, NULL) == 0) started++;
        else ts[i].id = 0;
    }
    char ms[16];
    kfmt(ms, sizeof(ms), "%d", TORTURE_MS);
    mos_wait_result_t r;
    memset(&r, 0, sizeof(r));
    long pid = spawn_child(wide ? "torture_avx" : "torture_sse", ms, TMP "/torture.txt", NULL, NULL, NULL);
    torture_loop(&main_t);
    for (int i = 0; i < TORTURE_THREADS; i++) if (ts[i].id) mos_thread_join(&ts[i]);
    long w = pid > 0 ? mos_waitpid(pid, 0, &r) : -1;
    static char out[256];
    read_text(TMP "/torture.txt", out, sizeof(out));

    kfmt(nm, sizeof(nm), "%s_torture_threads_started", name);
    EXPECT(started == TORTURE_THREADS, nm, "started %d of %d", started, TORTURE_THREADS);
    kfmt(nm, sizeof(nm), "%s_torture_main_thread_state_intact", name);
    EXPECT(main_t.errors == 0 && main_t.rounds > 0, nm, "%lu rounds, %lu errors: %s", main_t.rounds, main_t.errors, main_t.first_error);
    unsigned long errs = 0, rounds = 0;
    const char* first = "";
    for (int i = 0; i < TORTURE_THREADS; i++) {
        errs += th[i].errors; rounds += th[i].rounds;
        if (th[i].errors && !first[0]) first = th[i].first_error;
    }
    kfmt(nm, sizeof(nm), "%s_torture_worker_threads_state_intact", name);
    EXPECT(errs == 0 && rounds > 0, nm, "%lu rounds, %lu errors: %s", rounds, errs, first);
    kfmt(nm, sizeof(nm), "%s_torture_other_process_state_intact", name);
    EXPECT(pid > 0 && w == pid && !r.crashed && r.exit_code == 0, nm,
           "pid=%ld wait=%ld crashed=%d vector=%u exit=%d: %s", pid, w, r.crashed, r.crash_vector, r.exit_code, out);
    emit_fd(1, "OS0TEST info %s torture: main %lu rounds, threads %lu rounds", name, main_t.rounds, rounds);
}

/* A child that runs one VEX (or EVEX) instruction and exits 0 if it is allowed. */
static int child_avx_probe(int wide512) {
    if (wide512) __asm__ volatile(".intel_syntax noprefix\nvpxord zmm0, zmm0, zmm0\n.att_syntax prefix" ::: "memory");
    else         __asm__ volatile(".intel_syntax noprefix\nvpxor ymm0, ymm0, ymm0\n.att_syntax prefix" ::: "memory");
    __asm__ volatile("vzeroupper" ::: "memory");
    emit_fd(1, "avx probe: instruction ran");
    return 0;
}

static void cpuid_leaf(uint32_t leaf, uint32_t sub, uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}

static void test_simd(void) {
    stage("simd");
    uint64_t f = mos_sysinfo_cpu_features();
    uint64_t xcr0 = mos_sysinfo_xcr0();
    EXPECT(f != SYS_ERR_INVAL && xcr0 != SYS_ERR_INVAL, "simd_sysinfo_cpu_queries_accepted", "features=0x%llx xcr0=0x%llx",
           (unsigned long long)f, (unsigned long long)xcr0);
    int avx = mos_cpu_has(SYS_CPU_AVX);
    emit_fd(1, "OS0TEST info cpu features=0x%llx xcr0=0x%llx avx=%d avx2=%d avx512f=%d", (unsigned long long)f,
            (unsigned long long)xcr0, avx, mos_cpu_has(SYS_CPU_AVX2), mos_cpu_has(SYS_CPU_AVX512F));

    EXPECT(avx == ((xcr0 & 6) == 6), "simd_avx_bit_matches_enabled_xcr0", "avx=%d xcr0=0x%llx", avx, (unsigned long long)xcr0);
    EXPECT(!mos_cpu_has(SYS_CPU_AVX2) || avx, "simd_avx2_implies_avx", "features 0x%llx", (unsigned long long)f);
    EXPECT(!mos_cpu_has(SYS_CPU_FMA | SYS_CPU_F16C) || avx, "simd_fma_f16c_imply_avx", "features 0x%llx", (unsigned long long)f);
    EXPECT(!mos_cpu_has(SYS_CPU_AVX512F) || (avx && (xcr0 & 0xE0) == 0xE0), "simd_avx512_needs_its_state", "features 0x%llx xcr0 0x%llx",
           (unsigned long long)f, (unsigned long long)xcr0);
    EXPECT(mos_cpu_has(0), "simd_cpu_has_empty_mask_is_true", "returned 0");

    uint32_t a, b, c, d;
    cpuid_leaf(1, 0, &a, &b, &c, &d);
    int osxsave = (c >> 27) & 1;
    EXPECT(osxsave == (xcr0 != 0), "simd_osxsave_cpuid_bit_matches_xcr0", "cpuid OSXSAVE=%d xcr0=0x%llx", osxsave, (unsigned long long)xcr0);
    EXPECT(((c >> 0) & 1) == (mos_cpu_has(SYS_CPU_SSE3) ? 1u : 0u), "simd_sse3_bit_matches_cpuid", "cpuid %u", (c >> 0) & 1);
    EXPECT(((c >> 23) & 1) == (mos_cpu_has(SYS_CPU_POPCNT) ? 1u : 0u), "simd_popcnt_bit_matches_cpuid", "cpuid %u", (c >> 23) & 1);
    if (osxsave) {
        uint32_t lo, hi;
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        EXPECT((((uint64_t)hi << 32) | lo) == xcr0, "simd_xgetbv_matches_sysinfo_xcr0", "xgetbv 0x%x%08x sysinfo 0x%llx", hi, lo, (unsigned long long)xcr0);
    }

    /* The SSE path always: xmm0-15, x87, MXCSR. */
    run_torture(0, "sse");

    mos_wait_result_t r;
    long pid = spawn_wait("avx_probe", NULL, NULL, NULL, NULL, NULL, &r);
    if (avx) {
        EXPECT(pid > 0 && !r.crashed && r.exit_code == 0, "simd_avx_instruction_runs_when_reported",
               "pid=%ld crashed=%d vector=%u exit=%d", pid, r.crashed, r.crash_vector, r.exit_code);
        run_torture(1, "avx");
    } else {
        EXPECT(pid > 0 && r.crashed && r.crash_vector == 6, "simd_avx_instruction_faults_ud_when_not_reported",
               "pid=%ld crashed=%d vector=%u exit=%d", pid, r.crashed, r.crash_vector, r.exit_code);
        emit_fd(1, "OS0TEST info AVX not usable here (CPU without AVX, or booted with noavx): YMM torture skipped");
    }
    pid = spawn_wait("avx512_probe", NULL, NULL, NULL, NULL, NULL, &r);
    if (mos_cpu_has(SYS_CPU_AVX512F))
        EXPECT(pid > 0 && !r.crashed && r.exit_code == 0, "simd_avx512_instruction_runs_when_reported",
               "pid=%ld crashed=%d vector=%u exit=%d", pid, r.crashed, r.crash_vector, r.exit_code);
    else
        EXPECT(pid > 0 && r.crashed && r.crash_vector == 6, "simd_avx512_instruction_faults_ud_when_not_reported",
               "pid=%ld crashed=%d vector=%u exit=%d", pid, r.crashed, r.crash_vector, r.exit_code);
}

/* ================================================================== main */

int main(int argc, char** argv, char** envp) {
    g_argv = argv;
    g_envp = envp;
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
    test_bad_pointers_2();
    test_spawn_failure_cleanup();
    test_exec_relative();
    test_vm();
    test_simd();

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
