#include "dlr_mpkg.h"
#include <string.h>
#include <stdlib.h>

/* Layout from the kernel's pkgformat.h. Little-endian throughout. */
#define MPKG_MAGIC          "MPKG0001"
#define MPKG_MAGIC_SIZE     8
#define MPKG_VERSION        1
#define MPKG_HEADER_SIZE    16
#define MPKG_NAME_SIZE      128
#define MPKG_ENTRY_SIZE     148
#define MPKG_FLAG_DIRECTORY 1u
#define MPKG_METHOD_STORE   0u
#define MPKG_METHOD_LZSS    1u

/* Nothing legitimate needs more, and it bounds the table-size arithmetic. */
#define MPKG_MAX_ENTRIES    65536u

/* Must match the kernel (lzss.c) and tools/mkpkg/mkpkg.py exactly. */
#define LZSS_N          4096
#define LZSS_F          18
#define LZSS_THRESHOLD  2

static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint32_t dlr_lzss_decode(const uint8_t* in, uint32_t in_size, uint8_t* out, uint32_t out_cap) {
    if (!in || !out || out_cap == 0 || in_size == 0) return 0;

    /* Static rather than a local: 4 KiB is too much for the stack this
     * program is entitled to count on, and decoding is never reentrant
     * here. */
    static uint8_t text_buf[LZSS_N + LZSS_F - 1];
    for (uint32_t i = 0; i < LZSS_N - LZSS_F; i++) text_buf[i] = ' ';

    uint32_t r = LZSS_N - LZSS_F;
    uint32_t in_pos = 0, out_pos = 0, flags = 0;

    while (in_pos < in_size) {
        flags >>= 1;
        if ((flags & 0x100) == 0) {
            flags = in[in_pos++] | 0xFF00u;
        }

        if (flags & 1) {
            if (in_pos >= in_size) break;
            uint8_t c = in[in_pos++];
            if (out_pos >= out_cap) return 0;
            out[out_pos++] = c;
            text_buf[r++] = c;
            r &= (LZSS_N - 1);
        } else {
            if (in_pos + 2 > in_size) break;
            uint32_t b0 = in[in_pos++];
            uint32_t b1 = in[in_pos++];
            uint32_t match_pos = b0 | ((b1 & 0xF0u) << 4);
            uint32_t match_len = (b1 & 0x0Fu) + LZSS_THRESHOLD;   /* copies match_len + 1 bytes */
            for (uint32_t k = 0; k <= match_len; k++) {
                uint8_t c = text_buf[(match_pos + k) & (LZSS_N - 1)];
                if (out_pos >= out_cap) return 0;
                out[out_pos++] = c;
                text_buf[r++] = c;
                r &= (LZSS_N - 1);
            }
        }
    }
    return out_pos;
}

int dlr_mpkg_is_archive(const uint8_t* archive, size_t len) {
    if (!archive || len < MPKG_HEADER_SIZE) return 0;
    if (memcmp(archive, MPKG_MAGIC, MPKG_MAGIC_SIZE) != 0) return 0;
    return rd32(archive + 8) == MPKG_VERSION;
}

long dlr_mpkg_extract_mem(const uint8_t* archive, size_t len, const dlr_tar_sink* sink) {
    if (!dlr_mpkg_is_archive(archive, len) || !sink) return -1;

    uint32_t count = rd32(archive + 12);
    if (count > MPKG_MAX_ENTRIES) return -1;

    uint64_t table_end = (uint64_t)MPKG_HEADER_SIZE + (uint64_t)count * MPKG_ENTRY_SIZE;
    if (table_end > len) return -1;

    long handled = 0;

    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* e = archive + MPKG_HEADER_SIZE + (size_t)i * MPKG_ENTRY_SIZE;

        char name[MPKG_NAME_SIZE];
        memcpy(name, e, MPKG_NAME_SIZE);
        name[MPKG_NAME_SIZE - 1] = '\0';

        uint32_t flags  = rd32(e + MPKG_NAME_SIZE + 0);
        uint32_t method = rd32(e + MPKG_NAME_SIZE + 4);
        uint32_t usize  = rd32(e + MPKG_NAME_SIZE + 8);
        uint32_t csize  = rd32(e + MPKG_NAME_SIZE + 12);
        uint32_t offset = rd32(e + MPKG_NAME_SIZE + 16);

        if (!name[0]) continue;
        if (!dlr_tar_name_is_safe(name)) return -1;

        if (flags & MPKG_FLAG_DIRECTORY) {
            size_t n = strlen(name);
            if (n && name[n - 1] == '/') name[n - 1] = '\0';
            if (sink->on_dir && !sink->on_dir(sink->user, name)) return -1;
            handled++;
            continue;
        }

        /* An entry's payload must lie inside the archive, after the
         * table. Checked in 64 bits: offset + csize can wrap 32. */
        if (csize > 0) {
            uint64_t end = (uint64_t)offset + (uint64_t)csize;
            if ((uint64_t)offset < table_end || end > len) return -1;
        }
        const uint8_t* payload = archive + offset;

        if (method == MPKG_METHOD_STORE) {
            if (usize != csize) return -1;
            if (sink->on_file && !sink->on_file(sink->user, name, payload, usize)) return -1;
        } else if (method == MPKG_METHOD_LZSS) {
            uint8_t* data = NULL;
            uint32_t produced = 0;
            if (usize) {
                data = (uint8_t*)malloc(usize);
                if (!data) return -1;
                produced = dlr_lzss_decode(payload, csize, data, usize);
                if (produced != usize) { free(data); return -1; }
            }
            int ok = sink->on_file ? sink->on_file(sink->user, name, data, usize) : 1;
            if (data) free(data);
            if (!ok) return -1;
        } else {
            return -1;      /* unknown method: refuse rather than half-install */
        }
        handled++;
    }
    return handled;
}
