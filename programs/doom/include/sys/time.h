#ifndef DOOM_LIBC_SYS_TIME_H
#define DOOM_LIBC_SYS_TIME_H
#include <sys/types.h>
struct timeval { long tv_sec; long tv_usec; };
int gettimeofday(struct timeval* tv, void* tz);
#endif
