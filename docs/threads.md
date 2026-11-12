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

## `mos_thread_join`

```c
void mos_thread_join(mos_thread_t* t);
```

Waits until the thread has ended, then frees its stack. Returns at once
for a thread that never started.

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

## System call

`SYS_THREAD` (47): `rdi` = `SYS_THREAD_CREATE` with `rsi` = a
`syscall_thread_t*` (`entry`, `stack_top`, `arg`), returning the thread's
id; or `SYS_THREAD_ALIVE` with `rsi` = a thread id, returning
`SYS_SUCCESS` while that thread of the calling program runs and
`SYS_ERR_NOTFOUND` after. `entry` must be in the program and `stack_top`
in memory it owns. `thread.h` wraps this; programs should use that.
