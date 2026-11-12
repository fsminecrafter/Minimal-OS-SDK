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
 */

#include <stddef.h>
#include <stdint.h>
#include "stdlib.h"
#include "x86_64/user_common.h"

#define MOS_THREAD_STACK_DEFAULT (128u * 1024u)

typedef struct {
    long id;                    /* the kernel's thread id; 0 = not running */
    void* stack;
    void (*fn)(void*);
    void* arg;
} mos_thread_t;

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

/* Nonzero while the thread runs. */
static inline int mos_thread_running(const mos_thread_t* t) {
    return t && t->id &&
           (uint64_t)mos_syscall(SYS_THREAD, SYS_THREAD_ALIVE, t->id, 0) == SYS_SUCCESS;
}

/* Waits for the thread to end, then frees its stack. */
static inline void mos_thread_join(mos_thread_t* t) {
    if (!t || !t->id) return;
    while (mos_thread_running(t)) mos_sleep(1);
    free(t->stack);
    t->stack = NULL;
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
