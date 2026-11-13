# Spawning and waiting

Starting other programs with their own arguments, environment, working
directory and redirected output, waiting for them, and reading the
environment. Include `process.h` (it includes `minimalos.h`). The raw calls
it builds on (`mos_spawn_raw`, `mos_wait_raw`, `mos_stat`, `mos_rename`,
`mos_chdir`, `mos_clock_*`) are in [Process and terminal](process.md).

```c
#include "process.h"
#include "stdio.h"

int main(void) {
    const char* const argv[] = { "0:/programs/tool.run", "--fast", NULL };
    mos_spawn_opts_t o = { "0:/programs/tool.run", argv, NULL, "0:/work",
                           "0:/work/out.txt", "0:/work/out.txt",
                           0 /* stack_size: the 8 MiB default */ };
    long pid = mos_spawn_ex(&o);
    if (pid < 0) { printf("spawn failed (%ld)\n", pid); return 1; }

    mos_wait_result_t r;
    mos_waitpid(pid, 0, &r);                       /* sleeps until it ends */
    printf("exit %d%s\n", r.exit_code, r.crashed ? " (crashed)" : "");
    /* 0:/work/out.txt now holds everything it printed */
    return 0;
}
```

A program started this way is a *child* of the caller. A child is waited for
once; programs started with `mos_exec` count as children too.

## `environ`

```c
extern char** environ;
```

The environment the program started with: a NULL-terminated array of
`"NAME=value"` strings. `crt0.c` sets it from the entry point's third
argument (`main` also receives it as an optional third parameter,
`int main(int argc, char** argv, char** envp)`). It is never NULL; it is an
empty array when the program started with no environment, which is the case
for programs run from the terminal. A spawned program gets the environment
its parent passes (see [`mos_spawn_ex`](#mos_spawn_ex)).

## `mos_getenv`

```c
char* mos_getenv(const char* name);
```

**Returns** the value of `name` (an empty string for `NAME=`), or NULL when
it is not set. `name` must match whole: `OS` does not find `OSNAME=x`. The
string belongs to `environ`; do not modify it.

## `mos_spawn_ex`

```c
typedef struct {
    const char* path;                 /* the program (a .run bundle or ELF) */
    const char* const* argv;          /* NULL-terminated; NULL: just { path } */
    const char* const* envp;          /* NULL-terminated "NAME=value"; NULL: inherit */
    const char* cwd;                  /* NULL: the caller's working directory */
    const char* stdout_path;          /* NULL: the caller's output; else a file */
    const char* stderr_path;          /* (replaced) receiving it; may be the same */
    size_t stack_size;                /* main stack in bytes; 0: 8 MiB */
} mos_spawn_opts_t;

long mos_spawn_ex(const mos_spawn_opts_t* o);
```

Starts a program as a child of the caller and returns at once.

| Field | Description |
| --- | --- |
| `path` | The program |
| `argv` | What the child sees as `argv[0..argc-1]`; `argv[0]` is normally the path. The array must end with NULL. NULL means `{ path }`. An empty string is a valid argument |
| `envp` | The child's environment. NULL: a copy of the environment the caller started with |
| `cwd` | The child's working directory; it must be an existing directory. NULL: the caller's current one |
| `stdout_path` | NULL: the child writes where the caller's output goes (the terminal, or the caller's own redirection). Otherwise a file that is created, replacing an existing one, and receives everything the child writes to stdout |
| `stderr_path` | The same for stderr. May name the same file as `stdout_path`; the two streams are then one, in order |
| `stack_size` | Bytes of main stack for the child. 0: the default, 8 MiB. Values outside 64 KiB..1 GiB are clamped to that range (no error); the size is rounded up to whole pages. Stack pages take memory only once they are used, so a large stack is cheap. Running off the end of it is a page fault: the child crashes with `crash_vector == 14` |

**Returns** the child's pid (greater than 0), or `SYS_ERR_NOTFOUND` (no such
program or it is a directory; `cwd` is not a directory), `SYS_ERR_INVAL` (a
NULL or unreadable pointer, a path that is too long, too many or too long
arguments or environment strings), `SYS_ERR_GENERIC` (an output file could
not be created, or the program could not be loaded), `SYS_ERR_NOMEM`.

**Notes**

- The struct has grown at the end (`stack_size`). A positional initializer
  such as `{ path, argv, NULL, NULL, NULL, NULL }` leaves it 0, which is the
  default. If you fill a `mos_spawn_opts_t` field by field, set `stack_size`
  too (or `memset` the struct first): it is read by the call.
- Output redirected to a file is buffered by the kernel and written when the
  buffer fills and when the child ends. The file is complete once
  [`mos_waitpid`](#mos_waitpid) has returned for that child, also after a
  crash. Read it only after that.
- Everything the child's own children print without a redirection of their
  own goes to the same file.

## `mos_spawn`

```c
long mos_spawn(const char* path, const char* const* argv);
```

`mos_spawn_ex` with only a path and an argv (NULL-terminated, or NULL).
Environment, working directory and output are inherited.

## `mos_waitpid`

```c
typedef struct {
    long pid;                 /* the child that ended */
    int exit_code;            /* mos_exit()'s value (0-255); 255 after a crash */
    int crashed;              /* nonzero: it died of a CPU exception */
    int killed;               /* nonzero: it was ended from outside */
    unsigned crash_vector;    /* the exception number when crashed */
} mos_wait_result_t;

long mos_waitpid(long pid, int options, mos_wait_result_t* out);
long mos_wait(mos_wait_result_t* out);               /* any child */
```

Waits for a child to end.

| Parameter | Description |
| --- | --- |
| `pid` | A child's pid, or `MOS_WAIT_ANY` (0) for whichever ends first |
| `options` | `0` to sleep until it ends, or `MOS_WAIT_NOHANG` to return at once |
| `out` | Filled in on success; may be NULL |

**Returns** the pid of the child that ended (also in `out->pid`);
`SYS_ERR_NOTFOUND` when there is no such child: it was never started, was
already waited for, or (with `MOS_WAIT_ANY`) no child is left;
`SYS_ERR_AGAIN` with `MOS_WAIT_NOHANG` while the child still runs;
`SYS_ERR_INVAL` for other `options` or an invalid `out`.

**Notes**

- A child can be waited for once. Waiting again gives `SYS_ERR_NOTFOUND`.
- The exit status of a child nobody waits for is kept for a limited time (64
  children); after that waiting for an old child gives `SYS_ERR_NOTFOUND`.
- A program that ends does not wait for its children.

**Example**

```c
/* run four at once, collect them as they finish */
for (int i = 0; i < 4; i++) mos_spawn(worker, args[i]);
mos_wait_result_t r;
while (mos_wait(&r) > 0)
    printf("pid %ld: exit %d\n", r.pid, r.exit_code);
```

## Clocks and files

```c
long     mos_file_size(const char* path);
uint64_t mos_monotonic_ms(void);
int64_t  mos_time_unix(void);
```

| Function | Returns |
| --- | --- |
| `mos_file_size` | Size in bytes of a file; `SYS_ERR_NOTFOUND` if it does not exist, `SYS_ERR_INVAL` for a directory (built on [`mos_stat`](process.md#mos_stat)) |
| `mos_monotonic_ms` | Milliseconds on the monotonic clock (`mos_clock_monotonic_ns() / 1000000`) |
| `mos_time_unix` | Seconds since 1970-01-01 UTC by the system clock |

## System calls

`SYS_SPAWN` (53): `rdi` = a `syscall_spawn_t*` (`size`, `path`, `argv`,
`argc`, `envp`, `cwd`, `stdout_path`, `stderr_path`, `flags` which must be
0). `SYS_WAIT` (54): `rdi` = pid (`SYS_WAIT_ANY` = 0), `rsi` = flags
(`SYS_WAIT_NOHANG`), `rdx` = a `syscall_wait_t*` or NULL
(`exit_code`, `flags` with `SYS_WAIT_CRASHED` / `SYS_WAIT_KILLED`,
`crash_vector`). `SYS_CHDIR` (52), `SYS_STAT` (50), `SYS_RENAME` (51) and
`SYS_CLOCK` (49) are described with their wrappers in
[Process and terminal](process.md).
