#ifndef DLR_HOST_SHIMS_H
#define DLR_HOST_SHIMS_H
/*
 * Stand-ins for the three SDK headers main.c pulls in that only exist
 * on Minimal-OS. Everything else (dlr_proto.c, dlr_tar.c, dlr_pkg.c,
 * dlr_crypto.c, dlr_db.c) compiles unmodified on both sides, which is
 * the point of the exercise.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static inline uint32_t mos_ip_parse(const char* s) {
    uint32_t r = 0, o = 0; int parts = 0;
    for (const char* p = s;; p++) {
        if (*p >= '0' && *p <= '9') { o = o * 10 + (uint32_t)(*p - '0'); if (o > 255) return 0; }
        else if (*p == '.' || *p == '\0') { r = (r << 8) | (o & 0xFF); o = 0; parts++; if (!*p) break; }
        else return 0;
    }
    return parts == 4 ? r : 0;
}

static inline void mos_ip_to_string(uint32_t ip, char* out) {
    sprintf(out, "%u.%u.%u.%u", (ip>>24)&0xFF, (ip>>16)&0xFF, (ip>>8)&0xFF, ip&0xFF);
}

#endif
