#ifndef MINIMASSL_RAND_H
#define MINIMASSL_RAND_H

#ifdef __cplusplus
extern "C" {
#endif

/* Returns 1 on success, 0 on failure. Never returns partial output as success. */
int RAND_bytes(unsigned char* buf, int num);

#ifdef __cplusplus
}
#endif
#endif