# Threads

Running several parts of a program at once, and locking what they share.
Include `thread.h`.

A thread shares everything with the rest of its program: globals, the
heap, anything a pointer reaches. All of a program's threads run on the
same CPU, so threads do not make a computation faster. What they do is
keep a program responsive: while one thread waits (on the network, a
file, a timer), the others carry on. Several threads waiting on the
network wait at the same time.

- `malloc`, `calloc`, `realloc` and `free` are system calls and are safe
  from any thread.
- Anything else two threads touch needs a [`mos_mutex_t`](#mos_mutex_t).
- A thread ends when its function returns. `mos_exit()` from any thread,
  or `main()` returning, ends the whole program, threads included.
- A crash in any thread ends the whole program, like a crash in `main`.
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

- `main` has a fixed 64 KiB stack; a thread gets the stack you ask for.

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

## `mos_thread_join`

```c
void mos_thread_join(mos_thread_t* t);
```

Waits until the thread has ended, then frees its stack. Returns at once
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
`SYS_ERR_NOTFOUND` after. `entry` must be in the program and `stack_top`
in memory it owns. `thread.h` wraps this; programs should use that.

More operations of `SYS_THREAD`: `SYS_THREAD_SET_FS` (`rsi` = FS base) and
`SYS_THREAD_GET_FS` (returns it); `SYS_THREAD_CREATE_EX` with `rsi` = a
`syscall_thread_ex_t*` (`syscall_thread_t` plus `fs_base` and
`exit_futex`, the address of a `uint32_t` the kernel zeroes and wakes when
the thread has ended).

`SYS_FUTEX` (48): `rdi` = `SYS_FUTEX_WAIT` with `rsi` = address, `rdx` =
expected value, `r10` = timeout in nanoseconds; or `SYS_FUTEX_WAKE` with
`rsi` = address, `rdx` = count.
