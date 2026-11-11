#ifndef DOOM_LIBC_UNISTD_H
#define DOOM_LIBC_UNISTD_H
#include <stddef.h>
#include <sys/types.h>
#define F_OK 0
#define R_OK 4
#define W_OK 2
int access(const char* path, int mode);
int unlink(const char* path);
int isatty(int fd);
unsigned sleep(unsigned seconds);
int usleep(unsigned usec);
#endif
