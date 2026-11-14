# Threads

Running several parts of a program at once, and locking what they share.
Include `thread.h`.

A thread shares everything with the rest of its program: globals, the
heap, anything a pointer reaches. Threads of one program run in parallel
on different CPUs: N busy threads on N cores do N times the work in the
same time, and a thread that waits (on the network, a file, a timer)
leaves its CPU to the others. Two threads really do run at the same
instant, so everything they share needs a mutex or atomics. A new thread
goes to the least loaded CPU, and the scheduler may move it later; see
[CPUs, affinity and priority](#cpus-affinity-and-priority). On a single
CPU machine, or when the kernel was booted with `threads1cpu`, all
threads share one CPU (`mos_threads_multicore()` tells).

- `malloc`, `calloc`, `realloc` and `free` are system calls and are safe
  from any thread.
- Anything else two threads touch needs a [`mos_mutex_t`](#mos_mutex_t).
- A thread ends when its function returns. `mos_exit()` from any thread,
  or `main()` returning, ends the whole program, threads included.
- A crash in any thread ends the whole program, like a crash in `main`.
- `mos_vm_unmap`, `mos_vm_protect`, `mos_vm_decommit` and `free` take
  effect on every CPU before they return: a thread that uses the memory
  afterwards faults (and ends the program) instead of reading stale data.
- Windows belong to the thread that created them: keep all window and
  drawing calls on one thread (usually `main`'s).
- A network connection or open file belongs to the thread that opened it.
  Use it from that thread only.

**Example**

```c
#include "thread.h"

static mos_mutex_t lock = MOS_MUTEX_INIT;
static int done;

static void work(void* arg) {
    /* ... a slow download ... */
    mos_mutex_lock(&lock);
    done = 1;
    mos_mutex_unlock(&lock);
}

int main(void) {
    mos_thread_t t;
    if (mos_thread_create(&t, work, NULL, 0) != 0) return 1;
    while (!done) {
        /* handle window events, draw a progress bar ... */
        mos_sleep(16);
    }
    mos_thread_join(&t);
    return 0;
}
```

## `mos_thread_create`

```c
int mos_thread_create(mos_thread_t* t, void (*fn)(void*), void* arg, size_t stack_size);
```

Starts `fn(arg)` on a new thread.

| Parameter | Description |
| --- | --- |
| `t` | Filled in. Must stay valid until [`mos_thread_join`](#mos_thread_join) |
| `fn` | The thread's function. The thread ends when it returns |
| `arg` | Passed to `fn` |
| `stack_size` | Bytes of stack, taken with `malloc`. `0` for `MOS_THREAD_STACK_DEFAULT` (128 KiB) |

**Returns** `0`, or `-1` when there is no memory for the stack or the
kernel refused.

**Notes**

- `main` has an 8 MiB stack by default (a spawning program can choose another size with `stack_size`, see [Spawning](spawn.md#mos_spawn_ex)); a thread gets the stack you ask for, here from `malloc`. See [`mos_thread_create_vm`](#mos_thread_create_vm) for a thread stack that is demand-paged and guarded.

## `mos_thread_create_ex`

```c
int mos_thread_create_ex(mos_thread_t* t, void (*fn)(void*), void* arg,
                         size_t stack_size, void* tls);
```

[`mos_thread_create`](#mos_thread_create) plus two things: the thread
starts with FS base `tls` (see [Thread-local storage](#thread-local-storage)),
and it has an *exit futex*, so [`mos_thread_join`](#mos_thread_join) sleeps
until the kernel reports the end of the thread instead of polling every
millisecond.

| Parameter | Description |
| --- | --- |
| `t`, `fn`, `arg`, `stack_size` | As for `mos_thread_create`. `t` must not move until joined |
| `tls` | The thread's FS base: a block below `0x0000800000000000`, or NULL for none |

**Returns** `0`, or `-1`.

**Notes**

- `mos_thread_t` has two more fields since this call exists
  (`exit_word`, `futex_join`). Code that declares a `mos_thread_t` and
  calls `mos_thread_create` keeps working unchanged.
- The kernel sets `t->exit_word` to 0 and wakes every
  [`mos_futex_wait`](#futexes) on it when the thread has ended. It is 1
  while the thread runs, so you can also wait on it yourself.

## `mos_thread_create_vm`

```c
int mos_thread_create_vm(mos_thread_t* t, void (*fn)(void*), void* arg,
                         size_t stack_size, void* tls);
```

[`mos_thread_create_ex`](#mos_thread_create_ex) with a stack that is a
[`SYS_VM`](vm.md) mapping instead of `malloc` memory. `stack_size` is
rounded up to whole pages (`0`: `MOS_THREAD_STACK_DEFAULT`). The stack takes
physical memory only as the thread uses it, so a big one is cheap, and it
has an unmapped page on each side: running off its low end is a page fault
(the whole program ends, as for any crash) rather than corruption of other
memory. [`mos_thread_join`](#mos_thread_join) unmaps it.

**Returns** `0`, or `-1` (no address space or memory, or the kernel refused).

**Notes** `mos_thread_t` has two more fields for this (`stack_size`,
`stack_vm`); the other creators set them, so existing code is unaffected.

## `mos_thread_join`

```c
void mos_thread_join(mos_thread_t* t);
```

Waits until the thread has ended, then frees its stack (`free`, or unmaps it for a thread made by `mos_thread_create_vm`). Returns at once
for a thread that never started. A thread made by
[`mos_thread_create_ex`](#mos_thread_create_ex) is waited for with a
futex; one made by `mos_thread_create` by polling every millisecond.

## `mos_thread_running`

```c
int mos_thread_running(const mos_thread_t* t);
```

**Returns** nonzero while the thread runs.

## `mos_thread_exit`

```c
void mos_thread_exit(void);
```

Ends the calling thread (only that thread). Never returns. Returning from
the thread's function does the same.

## `mos_thread_yield`

```c
void mos_thread_yield(void);
```

Lets the program's other threads (and other programs) run.

## `mos_mutex_t`

```c
typedef struct { volatile int locked; } mos_mutex_t;
#define MOS_MUTEX_INIT { 0 }

void mos_mutex_lock(mos_mutex_t* m);
int  mos_mutex_trylock(mos_mutex_t* m);    /* 1 when it took the lock */
void mos_mutex_unlock(mos_mutex_t* m);
```

A lock. All zero bytes (or `MOS_MUTEX_INIT`) is unlocked. Not recursive:
a thread that locks a mutex it already holds waits forever. Keep what
happens under a lock short. A thread waiting for a lock sleeps in 1 ms
steps, so the holder can run.

## Futexes

```c
long mos_futex_wait(volatile uint32_t* addr, uint32_t expected, uint64_t timeout_ns);
long mos_futex_wake(volatile uint32_t* addr, uint32_t count);
```

The building block for locks, condition variables and joins: sleep until
another thread says a 32-bit word may have changed.

`mos_futex_wait` sleeps only while `*addr == expected`; the check and the
sleep are atomic against `mos_futex_wake`, so a wake-up cannot be lost
between them.

| Parameter | Description |
| --- | --- |
| `addr` | A 4-byte-aligned `uint32_t` in the program's memory (globals, heap, stack) |
| `expected` | Sleep only if `*addr` still has this value |
| `timeout_ns` | `0` waits without limit; otherwise rounded up to whole milliseconds. A timed wait never returns early |
| `count` | Wake at most this many sleepers (`UINT32_MAX` for all) |

**Returns (wait)** `SYS_SUCCESS` once woken; `SYS_ERR_AGAIN` at once when
`*addr != expected`; `SYS_ERR_TIMEDOUT` when the time ran out;
`SYS_ERR_INVAL` for an unaligned address or one outside the program.

**Returns (wake)** how many sleepers it woke (0 when nobody waited), or a
negative `SYS_ERR_*`.

**Notes**

- A wait can return `SYS_SUCCESS` without a matching wake-up. Always look at
  the word again and wait again if the condition does not hold.
- Waiters and wakers are threads of one program; a futex cannot signal
  another program.

**Example**

```c
static volatile uint32_t ready;

static void producer(void* arg) {
    __atomic_store_n(&ready, 1, __ATOMIC_RELEASE);
    mos_futex_wake(&ready, 1);
}

/* in the consumer */
while (__atomic_load_n(&ready, __ATOMIC_ACQUIRE) == 0)
    mos_futex_wait(&ready, 0, 0);
```

## Thread-local storage

```c
long  mos_thread_set_fs(const void* base);
void* mos_thread_get_fs(void);
```

Every thread has its own FS base, the register compilers use for
thread-local variables (`%fs:offset`). `mos_thread_set_fs` sets the calling
thread's; `mos_thread_get_fs` reads it. The kernel keeps it across context
switches. A new thread starts with the `tls` of
[`mos_thread_create_ex`](#mos_thread_create_ex), or 0 for
`mos_thread_create`; the main thread starts with 0.

| Parameter | Description |
| --- | --- |
| `base` | Address of the thread's TLS block, below `0x0000800000000000`; NULL clears it |

**Returns (set)** `SYS_SUCCESS`, or `SYS_ERR_INVAL` for an address in the
upper half (the CPU would fault on it). The value is unchanged then.

**Notes**

- Reading through `%fs` with a base of 0 faults and ends the program. The
  convention used by C runtimes is a block whose first word points at the
  block itself, so `%fs:0` yields the thread pointer.

## System call

`SYS_THREAD` (47): `rdi` = `SYS_THREAD_CREATE` with `rsi` = a
`syscall_thread_t*` (`entry`, `stack_top`, `arg`), returning the thread's
id; or `SYS_THREAD_ALIVE` with `rsi` = a thread id, returning
`SYS_SUCCESS` while that thread of the calling program runs and
`SYS_ERR_NOTFOUND` after. `entry` must be in executable memory of the
program (its image, or a [`SYS_VM`](vm.md) mapping with
`SYS_VM_PROT_EXEC`; not `malloc` memory or a stack) and `stack_top` in
memory it owns. `thread.h` wraps this; programs should use that.

More operations of `SYS_THREAD`: `SYS_THREAD_SET_FS` (`rsi` = FS base) and
`SYS_THREAD_GET_FS` (returns it); `SYS_THREAD_CREATE_EX` with `rsi` = a
`syscall_thread_ex_t*` (`syscall_thread_t` plus `fs_base` and
`exit_futex`, the address of a `uint32_t` the kernel zeroes and wakes when
the thread has ended).

`SYS_FUTEX` (48): `rdi` = `SYS_FUTEX_WAIT` with `rsi` = address, `rdx` =
expected value, `r10` = timeout in nanoseconds; or `SYS_FUTEX_WAKE` with
`rsi` = address, `rdx` = count.

## CPUs, affinity and priority

```c
unsigned mos_cpu_count(void);
int      mos_threads_multicore(void);
unsigned mos_cpu_current(void);
long     mos_thread_set_affinity(const mos_thread_t* t, uint64_t mask);
long     mos_thread_get_affinity(const mos_thread_t* t);
long     mos_thread_set_priority(const mos_thread_t* t, unsigned priority);
long     mos_thread_get_priority(const mos_thread_t* t);
```

| Function | Description |
| --- | --- |
| `mos_cpu_count` | Online CPUs (`SYS_SYSINFO_CORE_COUNT`) |
| `mos_threads_multicore` | Nonzero when threads run on different CPUs (the default); 0 with the boot flag `threads1cpu` |
| `mos_cpu_current` | The CPU the calling thread runs on now, `0 .. count-1` |
| `mos_thread_set_affinity` | Restricts a thread to the CPUs in `mask` (bit n = CPU n; `0` = any). `t` = `NULL` is the calling thread. Takes effect before it returns |
| `mos_thread_get_affinity` | The mask (all online CPUs while unrestricted) or a negative error |
| `mos_thread_set_priority` | `MOS_PRIO_HIGH` (default), `MOS_PRIO_NORMAL` or `MOS_PRIO_LOW` |
| `mos_thread_get_priority` | The priority set |

**Priority** maps onto the scheduler's three levels. A thread normally
starts at the interactive level and drops a level whenever it uses its
whole time slice; a blocking call brings it back. The priority is the
highest level it may be at: `HIGH` (default) is unrestricted, `NORMAL`
never sits above the normal level, `LOW` stays at the background level, so
it only runs on its CPU while nothing at a better level there waits (a
periodic boost keeps it from starving). It does not change time slices.
New threads inherit priority and affinity from the thread that creates
them.

**Affinity** errors: `SYS_ERR_INVAL` when the mask names no online CPU or
the thread belongs to another program, `SYS_ERR_PERM` when the kernel pins
threads (`threads1cpu`). Moving between CPUs is the scheduler's job; use
affinity only when it matters (cache locality, a dedicated worker).

**What the kernel guarantees across CPUs**

- Memory unmapped, made less accessible (`mos_vm_unmap`,
  `mos_vm_protect`, `mos_vm_decommit`, `free`) is flushed from every CPU's
  TLB before the call returns and before the physical pages are reused.
- A thread that ends the program (`mos_exit`, `main` returning, a crash)
  stops all other threads, even ones running on other CPUs, before the
  program's memory is released. If two threads crash at the same time, the
  first one's crash is the program's exit status.
- Futex wake-ups reach a waiter on any CPU.
