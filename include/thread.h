#ifndef MINIMALOS_THREAD_H
#define MINIMALOS_THREAD_H

/*
 * Threads for Minimal-OS programs (SYS_THREAD).
 *
 * A thread shares the program's memory: globals, heap and everything a
 * pointer reaches. It runs on the same CPU as the rest of the program, so
 * threads do not make computation faster; they keep a program responsive
 * while one thread waits (on the network, a file, a timer) and the others
 * go on. Several threads waiting on the network wait at the same time.
 *
 *   static void work(void* arg) { ... }
 *
 *   mos_thread_t t;
 *   if (mos_thread_create(&t, work, arg, 0) == 0) {
 *       ...
 *       mos_thread_join(&t);       // waits for work() to return
 *   }
 *
 * A thread ends when its function returns (or calls mos_thread_exit()).
 * mos_exit() from any thread, or main() returning, ends the whole
 * program; a crash in any thread ends the whole program too.
 *
 * malloc()/free() are system calls and safe from any thread. Anything
 * else two threads use at once needs a mos_mutex_t.
 *
 * Futexes and TLS (SYS_FUTEX, SYS_THREAD_SET_FS/GET_FS/CREATE_EX):
 *
 *   mos_futex_wait()/mos_futex_wake()   sleep on / wake a 32-bit word
 *   mos_thread_create_ex()              a thread with its own FS base (TLS
 *                                       block) that mos_thread_join() waits
 *                                       for with a futex instead of polling
 *   mos_thread_set_fs()/get_fs()        the calling thread's FS base
 *
 * mos_thread_create() is unchanged and joins by polling every 1 ms.
 */

#include <stddef.h>
#include <stdint.h>
#include "stdlib.h"
#include "x86_64/user_common.h"
#include "vm.h"

#define MOS_THREAD_STACK_DEFAULT (128u * 1024u)

typedef struct {
    long id;                    /* the kernel's thread id; 0 = not running */
    void* stack;
    void (*fn)(void*);
    void* arg;
    /* Exit futex, used by mos_thread_create_ex(): 1 while the thread
     * runs, set to 0 (and its waiters woken) by the kernel when it ends.
     * Unused (0) for mos_thread_create(); joining then polls. */
    volatile uint32_t exit_word;
    uint32_t futex_join;        /* nonzero: created by mos_thread_create_ex() */
    size_t stack_size;          /* mos_thread_create_vm(): the mapping's length */
    uint32_t stack_vm;          /* nonzero: `stack` is a SYS_VM mapping */
} mos_thread_t;

/* ------------------------------------------------------------- futex */

/* Sleeps while *addr == expected (checked atomically against a wake).
 * `addr` is 4-byte aligned memory of the program. timeout_ns 0 waits
 * without limit; otherwise the wait is rounded up to whole milliseconds.
 * Returns SYS_SUCCESS once woken (possibly spuriously: look at the word
 * again), SYS_ERR_AGAIN when *addr != expected at once, SYS_ERR_TIMEDOUT
 * when the time ran out, SYS_ERR_INVAL for a bad address. */
static inline long mos_futex_wait(volatile uint32_t* addr, uint32_t expected,
                                  uint64_t timeout_ns) {
    return mos_syscall4(SYS_FUTEX, SYS_FUTEX_WAIT, (long)(uintptr_t)addr,
                        (long)expected, (long)timeout_ns);
}

/* Wakes at most `count` threads sleeping on `addr` (all of them for
 * UINT32_MAX). Returns how many it woke, or a negative SYS_ERR_* value. */
static inline long mos_futex_wake(volatile uint32_t* addr, uint32_t count) {
    return mos_syscall(SYS_FUTEX, SYS_FUTEX_WAKE, (long)(uintptr_t)addr, (long)count);
}

/* --------------------------------------------------------- FS base (TLS) */

/* Sets the calling thread's FS base: the address of its thread-local
 * block, below 0x0000800000000000 (0 clears it). Every thread has its own;
 * the kernel keeps it across switches. A new thread starts with the value
 * given to mos_thread_create_ex() (0 for mos_thread_create()). Reading
 * through it is up to the program (e.g. `mov %fs:0, %rax` when the block's
 * first word points to itself). Returns SYS_SUCCESS or SYS_ERR_INVAL. */
static inline long mos_thread_set_fs(const void* base) {
    return mos_syscall(SYS_THREAD, SYS_THREAD_SET_FS, (long)(uintptr_t)base, 0);
}

/* The calling thread's FS base. */
static inline void* mos_thread_get_fs(void) {
    return (void*)(uintptr_t)mos_syscall(SYS_THREAD, SYS_THREAD_GET_FS, 0, 0);
}

__attribute__((noreturn)) static inline void mos_thread_exit(void) {
    mos_syscall(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}

/* Every thread starts here (RDI = its mos_thread_t). */
__attribute__((noreturn, used)) static void mos_thread_start_(uint64_t self) {
    mos_thread_t* t = (mos_thread_t*)(uintptr_t)self;
    t->fn(t->arg);
    mos_thread_exit();
}

/* Starts fn(arg) on a new thread with a stack of stack_size bytes (0 for
 * MOS_THREAD_STACK_DEFAULT). `t` must stay valid until mos_thread_join().
 * Returns 0, or -1 when there is no memory or the kernel refused. */
static inline int mos_thread_create(mos_thread_t* t, void (*fn)(void*), void* arg,
                                    size_t stack_size) {
    if (!t || !fn) return -1;
    if (!stack_size) stack_size = MOS_THREAD_STACK_DEFAULT;
    t->id = 0;
    t->fn = fn;
    t->arg = arg;
    t->exit_word = 0;
    t->futex_join = 0;
    t->stack_vm = 0;
    t->stack = malloc(stack_size);
    if (!t->stack) return -1;
    syscall_thread_t request;
    request.entry = (uint64_t)(uintptr_t)mos_thread_start_;
    request.stack_top = ((uint64_t)(uintptr_t)t->stack + stack_size) & ~(uint64_t)15;
    request.arg = (uint64_t)(uintptr_t)t;
    long id = mos_syscall(SYS_THREAD, SYS_THREAD_CREATE, (long)(uintptr_t)&request, 0);
    if (id <= 0) {
        free(t->stack);
        t->stack = NULL;
        return -1;
    }
    t->id = id;
    return 0;
}

/* Like mos_thread_create(), plus the thread's FS base `tls` (NULL for none;
 * see mos_thread_set_fs()) and a futex join: mos_thread_join() then sleeps
 * until the kernel reports the thread's end instead of polling. `t` must
 * stay valid (and not move) until mos_thread_join(). Returns 0, or -1. */
static inline int mos_thread_create_ex(mos_thread_t* t, void (*fn)(void*), void* arg,
                                       size_t stack_size, void* tls) {
    if (!t || !fn) return -1;
    if (!stack_size) stack_size = MOS_THREAD_STACK_DEFAULT;
    t->id = 0;
    t->fn = fn;
    t->arg = arg;
    t->futex_join = 1;
    t->stack_vm = 0;
    t->exit_word = 1;
    t->stack = malloc(stack_size);
    if (!t->stack) return -1;
    syscall_thread_ex_t request;
    request.entry = (uint64_t)(uintptr_t)mos_thread_start_;
    request.stack_top = ((uint64_t)(uintptr_t)t->stack + stack_size) & ~(uint64_t)15;
    request.arg = (uint64_t)(uintptr_t)t;
    request.fs_base = (uint64_t)(uintptr_t)tls;
    request.exit_futex = (uint64_t)(uintptr_t)&t->exit_word;
    long id = mos_syscall(SYS_THREAD, SYS_THREAD_CREATE_EX, (long)(uintptr_t)&request, 0);
    if (id <= 0) {
        free(t->stack);
        t->stack = NULL;
        return -1;
    }
    t->id = id;
    return 0;
}

/* Like mos_thread_create_ex() (futex join, optional `tls`), but the stack
 * is a SYS_VM mapping of stack_size bytes (0: MOS_THREAD_STACK_DEFAULT;
 * rounded up to pages) instead of malloc memory: it takes physical memory
 * only as it is used, so a large one is cheap, and it has an unmapped page
 * on both sides, so running off its low end is a crash (exception 14)
 * rather than corrupting other memory. mos_thread_join() unmaps it.
 * Returns 0, or -1. */
static inline int mos_thread_create_vm(mos_thread_t* t, void (*fn)(void*), void* arg,
                                       size_t stack_size, void* tls) {
    if (!t || !fn) return -1;
    if (!stack_size) stack_size = MOS_THREAD_STACK_DEFAULT;
    stack_size = mos_vm_round_up(stack_size);
    t->id = 0;
    t->fn = fn;
    t->arg = arg;
    t->futex_join = 1;
    t->exit_word = 1;
    t->stack = mos_vm_map(NULL, stack_size, MOS_VM_PROT_RW, 0);
    t->stack_size = stack_size;
    t->stack_vm = 1;
    if (!t->stack) return -1;
    syscall_thread_ex_t request;
    request.entry = (uint64_t)(uintptr_t)mos_thread_start_;
    request.stack_top = ((uint64_t)(uintptr_t)t->stack + stack_size) & ~(uint64_t)15;
    request.arg = (uint64_t)(uintptr_t)t;
    request.fs_base = (uint64_t)(uintptr_t)tls;
    request.exit_futex = (uint64_t)(uintptr_t)&t->exit_word;
    long id = mos_syscall(SYS_THREAD, SYS_THREAD_CREATE_EX, (long)(uintptr_t)&request, 0);
    if (id <= 0) {
        mos_vm_unmap(t->stack, stack_size);
        t->stack = NULL;
        return -1;
    }
    t->id = id;
    return 0;
}

/* Nonzero while the thread runs. */
static inline int mos_thread_running(const mos_thread_t* t) {
    return t && t->id &&
           (uint64_t)mos_syscall(SYS_THREAD, SYS_THREAD_ALIVE, t->id, 0) == SYS_SUCCESS;
}

/* Waits for the thread to end, then frees its stack (unmaps it, for
 * mos_thread_create_vm()). A thread made by mos_thread_create_ex() or
 * mos_thread_create_vm() is waited for with a futex, any other by polling. */
static inline void mos_thread_join(mos_thread_t* t) {
    if (!t || !t->id) return;
    if (t->futex_join) {
        uint32_t word;
        while ((word = __atomic_load_n(&t->exit_word, __ATOMIC_ACQUIRE)) != 0)
            mos_futex_wait(&t->exit_word, word, 0);
    } else {
        while (mos_thread_running(t)) mos_sleep(1);
    }
    if (t->stack_vm) mos_vm_unmap(t->stack, t->stack_size);
    else free(t->stack);
    t->stack = NULL;
    t->stack_vm = 0;
    t->id = 0;
}

/* Gives the CPU to the program's other threads (and everyone else). */
static inline void mos_thread_yield(void) { mos_sleep(1); }

/* ------------------------------------------------------------- mutex */

/* A lock; MOS_MUTEX_INIT or all zero bytes is unlocked. Not recursive. */
typedef struct { volatile int locked; } mos_mutex_t;
#define MOS_MUTEX_INIT { 0 }

static inline int mos_mutex_trylock(mos_mutex_t* m) {
    return !__atomic_exchange_n(&m->locked, 1, __ATOMIC_ACQUIRE);
}

static inline void mos_mutex_lock(mos_mutex_t* m) {
    /* The holder runs on this same CPU: spinning only delays it. */
    for (int tries = 0; !mos_mutex_trylock(m); tries++) {
        if (tries < 4) __asm__ volatile("pause");
        else mos_sleep(1);
    }
}

static inline void mos_mutex_unlock(mos_mutex_t* m) {
    __atomic_store_n(&m->locked, 0, __ATOMIC_RELEASE);
}

#endif
