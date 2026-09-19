#include <openssl/rand.h>
#include <stddef.h>

#if defined(MSSL_TARGET_MINIMALOS) || defined(MINIMALOS_TARGET)
  #include "stdlib.h"          /* SDK header: mos_random_bytes() */

  static int fill(unsigned char* buf, size_t n) {
      return mos_random_bytes(buf, n) == (long)n;   /* short count == failure */
  }
#else
  #include <stdio.h>
  #if defined(__linux__)
    #include <sys/random.h>
  #endif

  static int fill(unsigned char* buf, size_t n) {
  #if defined(__linux__)
      size_t got = 0;
      while (got < n) {
          ssize_t r = getrandom(buf + got, n - got, 0);
          if (r <= 0) break;                         /* fall back below */
          got += (size_t)r;
      }
      if (got == n) return 1;
  #endif
      FILE* f = fopen("/dev/urandom", "rb");
      if (!f) return 0;
      size_t r = fread(buf, 1, n, f);
      fclose(f);
      return r == n;
  }
#endif

int RAND_bytes(unsigned char* buf, int num) {
    if (num < 0 || (!buf && num > 0)) return 0;
    if (num == 0) return 1;
    return fill(buf, (size_t)num);
}