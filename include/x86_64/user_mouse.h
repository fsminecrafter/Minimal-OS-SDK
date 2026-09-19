#ifndef MINIMALOS_X86_64_USER_MOUSE_H
#define MINIMALOS_X86_64_USER_MOUSE_H

#include <stdint.h>
#include "x86_64/user_syscalls.h"

static inline uint64_t mos_mouse_init(void) {
    return mos_mouse(SYS_MOUSE_INIT, 0, 0);
}

static inline uint64_t mos_mouse_poll(void) {
    return mos_mouse(SYS_MOUSE_POLL, 0, 0);
}

static inline uint64_t mos_mouse_has_mouse(void) {
    return mos_mouse(SYS_MOUSE_HAS_MOUSE, 0, 0);
}

// Reads and resets accumulated dx/dy/wheel + current buttons since the
// last call.
static inline uint64_t mos_mouse_get_state(syscall_mouse_state_t* out_state) {
    return mos_mouse(SYS_MOUSE_GET_STATE, (uint64_t)(uintptr_t)out_state, 0);
}

#endif
