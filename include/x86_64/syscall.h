#ifndef MINIMALOS_X86_64_SYSCALL_H
#define MINIMALOS_X86_64_SYSCALL_H

// The syscall ABI lives in one header. This file used to be a second,
// slightly older copy behind the same include guard, so whichever of the
// two a program included first silently replaced the other (and
// syscall_input_event_t went missing next to window.h).
#include "../syscall.h"

#endif
