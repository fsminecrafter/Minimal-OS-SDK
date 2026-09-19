#ifndef MINIMALOS_EXAMPLE_SLIB_MATH_H
#define MINIMALOS_EXAMPLE_SLIB_MATH_H

#include <stdint.h>

// Public data exported by the example library.
extern uint64_t math_slib_counter;

// Public functions exported by the example library.
uint64_t math_slib_add(uint64_t left, uint64_t right);
uint64_t math_slib_increment_counter(void);

#endif
