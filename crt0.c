#include "minimalos.h"

extern int main(int argc, char** argv);

/*
 * Compiler-emitted memory primitives.
 *
 * GCC generates calls to memcpy/memmove/memset/memcmp on its own for
 * struct assignment, array initialization and large copies, even under
 * -ffreestanding -fno-builtin. There is no libc to link against here,
 * so without these definitions any program doing something as ordinary
 * as `syscall_net_request_t r = {0};` fails to link. They live in
 * crt0.c because crt0.c is linked into every .run program.
 *
 * Deliberately simple byte loops - correctness first. If a program
 * ever becomes copy-bound, this is the place to add an 8-byte-at-a-
 * time fast path.
 */

void* memcpy(void* dst, const void* src, unsigned long n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    for (unsigned long i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void* memmove(void* dst, const void* src, unsigned long n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        for (unsigned long i = 0; i < n; i++) d[i] = s[i];
    } else {
        // Overlapping and moving up: copy backwards so we read each
        // byte before we overwrite it.
        for (unsigned long i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dst;
}

void* memset(void* dst, int value, unsigned long n) {
    unsigned char* d = (unsigned char*)dst;
    for (unsigned long i = 0; i < n; i++) d[i] = (unsigned char)value;
    return dst;
}

int memcmp(const void* a, const void* b, unsigned long n) {
    const unsigned char* x = (const unsigned char*)a;
    const unsigned char* y = (const unsigned char*)b;
    for (unsigned long i = 0; i < n; i++) {
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    }
    return 0;
}

/*
 * Static constructors.
 *
 * link.ld now emits __init_array_start/__init_array_end around
 * .init_array, so C++ globals with non-trivial constructors and
 * __attribute__((constructor)) functions both work. Same
 * ctor-section-walk pattern commandhandler.c uses for
 * REGISTER_COMMAND on the kernel side.
 *
 * Destructors (.fini_array) are still not run: a program ends via
 * mos_exit(), which does not return, and process teardown is the
 * kernel's job. Register a cleanup callback with
 * mos_register_cleanup() if you need to release something on exit.
 */
extern void (*__init_array_start[])(void) __attribute__((weak));
extern void (*__init_array_end[])(void) __attribute__((weak));

static void run_init_array(void) {
    if (!__init_array_start || !__init_array_end) return;
    unsigned long count = (unsigned long)(__init_array_end - __init_array_start);
    for (unsigned long i = 0; i < count; i++) {
        if (__init_array_start[i]) __init_array_start[i]();
    }
}

void _start(long argc, char** argv) {
    run_init_array();
    int rc = main((int)argc, argv);
    mos_exit(rc);
}
