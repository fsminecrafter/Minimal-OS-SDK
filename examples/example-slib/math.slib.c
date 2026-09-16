#include "math.slib.h"

uint64_t math_slib_counter;

uint64_t math_slib_add(uint64_t left, uint64_t right) {
    return left + right;
}

uint64_t math_slib_increment_counter(void) {
    return ++math_slib_counter;
}
