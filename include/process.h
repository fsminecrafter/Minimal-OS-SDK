#ifndef MINIMALOS_PROCESS_H
#define MINIMALOS_PROCESS_H

/*
 * Processes, working directory, stat/rename, clocks and the environment
 * (SYS_SPAWN, SYS_WAIT, SYS_CHDIR, SYS_STAT, SYS_RENAME, SYS_CLOCK).
 *
 *   const char* const argv[] = { "0:/programs/tool.run", "--fast", NULL };
 *   long pid = mos_spawn("0:/programs/tool.run", argv);
 *   if (pid > 0) {
 *       mos_wait_result_t r;
 *       mos_waitpid(pid, 0, &r);              // sleeps until it ends
 *       printf("exit %d%s\n", r.exit_code, r.crashed ? " (crashed)" : "");
 *   }
 *
 * Paths are MinimaFS paths. "0:/dir/file" names a drive; "/dir/file" is the
 * root of the working directory's drive; anything else is relative to the
 * program's working directory (mos_chdir(); mos_getcwd() reads it).
 *
 * Errors are the kernel's negative SYS_ERR_* values (compare with
 * `r == (long)SYS_ERR_NOTFOUND`, or just test `r < 0`). The raw calls
 * (mos_stat, mos_rename, mos_chdir, mos_spawn_raw, mos_wait_raw and the
 * mos_clock_* functions) live in minimalos.h; this header adds the
 * friendlier layer on top and the environment.
 */

#include <stddef.h>
#include <stdint.h>
#include "minimalos.h"

/* ------------------------------------------------------- environment */

/* The environment the program started with: a NULL-terminated array of
 * "NAME=value" strings, set by crt0.c from the entry point's third
 * argument. Never NULL (an empty array when the kernel gave none). */
extern char** environ;

/* The value of NAME ("" is a valid value), or NULL when it is not set. */
static inline char* mos_getenv(const char* name) {
    if (!name || !*name || !environ) return NULL;
    size_t n = 0;
    while (name[n]) n++;
    for (char** e = environ; *e; e++) {
        const char* s = *e;
        size_t i = 0;
        while (i < n && s[i] == name[i]) i++;
        if (i == n && s[n] == '=') return (char*)(s + n + 1);
    }
    return NULL;
}

/* ------------------------------------------------------------ spawn */

typedef struct {
    const char* path;                 /* the program (a .run bundle or ELF) */
    const char* const* argv;          /* NULL-terminated; NULL: just { path } */
    const char* const* envp;          /* NULL-terminated "NAME=value"; NULL: inherit */
    const char* cwd;                  /* NULL: the caller's working directory */
    const char* stdout_path;          /* NULL: the caller's output; else a file */
    const char* stderr_path;          /* (replaced) receiving it; may be the same */
} mos_spawn_opts_t;

/* Starts a program as a child of the caller. argv[0] is what the child
 * sees as its own name (normally the path). Output redirected with
 * stdout_path/stderr_path goes into those files, which are complete once
 * mos_waitpid() has returned for the child. Returns the child's pid (> 0)
 * or a negative SYS_ERR_*: SYS_ERR_NOTFOUND (no such program, or `cwd` is
 * not a directory), SYS_ERR_INVAL (a bad pointer, too long or too many
 * arguments), SYS_ERR_GENERIC (could not create an output file / launch). */
static inline long mos_spawn_ex(const mos_spawn_opts_t* o) {
    if (!o) return (long)SYS_ERR_INVAL;
    syscall_spawn_t r;
    r.size = sizeof(r);
    r.path = o->path;
    r.argv = o->argv;
    r.argc = 0;
    if (o->argv) while (o->argv[r.argc]) r.argc++;
    r.envp = o->envp;
    r.cwd = o->cwd;
    r.stdout_path = o->stdout_path;
    r.stderr_path = o->stderr_path;
    r.flags = 0;
    return mos_spawn_raw(&r);
}

/* mos_spawn_ex() with only a path and argv (NULL-terminated, or NULL). */
static inline long mos_spawn(const char* path, const char* const* argv) {
    mos_spawn_opts_t o = { path, argv, NULL, NULL, NULL, NULL };
    return mos_spawn_ex(&o);
}

/* ------------------------------------------------------------- wait */

#define MOS_WAIT_ANY    SYS_WAIT_ANY      /* pid: any child */
#define MOS_WAIT_NOHANG SYS_WAIT_NOHANG   /* options: do not sleep */

typedef struct {
    long pid;                 /* the child that ended */
    int exit_code;            /* mos_exit()'s value (0-255); 0xFF after a crash */
    int crashed;              /* nonzero: it died of a CPU exception */
    int killed;               /* nonzero: it was ended from outside */
    unsigned crash_vector;    /* the exception number when crashed */
} mos_wait_result_t;

/* Waits for the child `pid` (MOS_WAIT_ANY for any child of this program)
 * to end and fills *out (may be NULL). options: 0, or MOS_WAIT_NOHANG.
 * Returns the child's pid; SYS_ERR_NOTFOUND when there is no such child
 * (never started, or already waited for: a child can be waited for once);
 * SYS_ERR_AGAIN with MOS_WAIT_NOHANG while it still runs. Programs started
 * with mos_exec() are children too. */
static inline long mos_waitpid(long pid, int options, mos_wait_result_t* out) {
    syscall_wait_t w;
    w.exit_code = 0; w.flags = 0; w.crash_vector = 0; w.reserved = 0;
    long r = mos_wait_raw(pid, options, &w);
    if (out) {
        out->pid = r > 0 ? r : 0;
        out->exit_code = (int)w.exit_code;
        out->crashed = (w.flags & SYS_WAIT_CRASHED) != 0;
        out->killed = (w.flags & SYS_WAIT_KILLED) != 0;
        out->crash_vector = w.crash_vector;
    }
    return r;
}

/* Waits for any child. */
static inline long mos_wait(mos_wait_result_t* out) {
    return mos_waitpid(MOS_WAIT_ANY, 0, out);
}

/* ------------------------------------------------- files and clocks */

/* Size in bytes of the file at `path`; a negative SYS_ERR_* value when it
 * does not exist (SYS_ERR_NOTFOUND) or is a directory (SYS_ERR_INVAL). */
static inline long mos_file_size(const char* path) {
    syscall_stat_t st;
    long r = mos_stat(path, &st);
    if (r < 0) return r;
    if (st.type != SYS_STAT_FILE) return (long)SYS_ERR_INVAL;
    return (long)st.size;
}

/* Milliseconds on the monotonic clock (mos_clock_monotonic_ns() / 1e6). */
static inline uint64_t mos_monotonic_ms(void) {
    return mos_clock_monotonic_ns() / 1000000u;
}

/* Seconds since 1970-01-01 UTC by the system clock. */
static inline int64_t mos_time_unix(void) {
    return (int64_t)(mos_clock_realtime_ns() / 1000000000u);
}

#endif
