#include "dlr_tar.h"
#include <string.h>

/* ustar header, 512 bytes. Field offsets are fixed by the format. */
#define TAR_BLOCK      512
#define OFF_NAME       0     /* 100 */
#define OFF_SIZE       124   /* 12, octal */
#define OFF_TYPEFLAG   156   /* 1 */
#define OFF_MAGIC      257   /* 6 */
#define OFF_PREFIX     345   /* 155 */
#define OFF_CHECKSUM   148   /* 8, octal */

#define TYPE_FILE_OLD  '\0'
#define TYPE_FILE      '0'
#define TYPE_HARDLINK  '1'
#define TYPE_SYMLINK   '2'
#define TYPE_DIR       '5'
#define TYPE_LONGNAME  'L'   /* GNU: the next entry's name is this entry's data */

static int block_is_zero(const uint8_t* b) {
    for (int i = 0; i < TAR_BLOCK; i++) if (b[i]) return 0;
    return 1;
}

// Octal fields are space- or NUL-terminated and may be space-padded on
// the left. GNU also emits base-256 for large sizes (high bit set on
// the first byte), which a package tar can legitimately contain once a
// member exceeds 8 GiB - handled for completeness, cheap to support.
static uint64_t parse_octal(const uint8_t* field, size_t len) {
    if (len && (field[0] & 0x80)) {
        uint64_t v = (uint64_t)(field[0] & 0x7F);
        for (size_t i = 1; i < len; i++) v = (v << 8) | field[i];
        return v;
    }

    uint64_t value = 0;
    size_t i = 0;
    while (i < len && (field[i] == ' ' || field[i] == '0')) {
        if (field[i] == '0') break;
        i++;
    }
    for (; i < len; i++) {
        if (field[i] < '0' || field[i] > '7') break;
        value = value * 8u + (uint64_t)(field[i] - '0');
    }
    return value;
}

static int header_checksum_ok(const uint8_t* h) {
    uint64_t stored = parse_octal(h + OFF_CHECKSUM, 8);

    // The checksum is computed with its own field treated as spaces.
    uint64_t sum = 0;
    for (int i = 0; i < TAR_BLOCK; i++) {
        sum += (i >= OFF_CHECKSUM && i < OFF_CHECKSUM + 8) ? (uint64_t)' ' : (uint64_t)h[i];
    }
    if (sum == stored) return 1;

    // Some historical writers signed the bytes. Accept that too rather
    // than rejecting an archive GNU tar is perfectly happy with.
    int64_t signed_sum = 0;
    for (int i = 0; i < TAR_BLOCK; i++) {
        signed_sum += (i >= OFF_CHECKSUM && i < OFF_CHECKSUM + 8)
                      ? (int64_t)' ' : (int64_t)(int8_t)h[i];
    }
    return (uint64_t)signed_sum == stored;
}

int dlr_tar_name_is_safe(const char* name) {
    if (!name || !name[0]) return 0;
    if (name[0] == '/' || name[0] == '\\') return 0;

    // "N:/..." would escape onto another MinimaFS drive.
    for (const char* p = name; *p; p++) {
        if (*p == ':') return 0;
        if (*p == '\\') return 0;
    }

    // Any ".." path component, anywhere.
    const char* seg = name;
    for (;;) {
        const char* slash = strchr(seg, '/');
        size_t seg_len = slash ? (size_t)(slash - seg) : strlen(seg);
        if (seg_len == 2 && seg[0] == '.' && seg[1] == '.') return 0;
        if (!slash) break;
        seg = slash + 1;
    }
    return 1;
}

static void build_name(const uint8_t* h, char* out, size_t out_size) {
    char prefix[156];
    char name[101];

    memcpy(name, h + OFF_NAME, 100);
    name[100] = '\0';

    int has_ustar = memcmp(h + OFF_MAGIC, "ustar", 5) == 0;
    if (has_ustar) {
        memcpy(prefix, h + OFF_PREFIX, 155);
        prefix[155] = '\0';
    } else {
        prefix[0] = '\0';
    }

    out[0] = '\0';
    size_t pos = 0;
    if (prefix[0]) {
        for (size_t i = 0; prefix[i] && pos + 1 < out_size; i++) out[pos++] = prefix[i];
        if (pos + 1 < out_size) out[pos++] = '/';
    }
    for (size_t i = 0; name[i] && pos + 1 < out_size; i++) out[pos++] = name[i];
    out[pos] = '\0';
}

long dlr_tar_extract_mem(const uint8_t* archive, size_t len, const dlr_tar_sink* sink) {
    size_t offset = 0;
    long handled = 0;
    int zero_blocks = 0;

    // A GNU 'L' entry supplies the name for the entry that follows it.
    char long_name[512];
    int have_long_name = 0;

    while (offset + TAR_BLOCK <= len) {
        const uint8_t* h = archive + offset;
        offset += TAR_BLOCK;

        if (block_is_zero(h)) {
            // Two consecutive zero blocks terminate the archive.
            if (++zero_blocks >= 2) break;
            continue;
        }
        zero_blocks = 0;

        if (!header_checksum_ok(h)) return -1;

        uint64_t size = parse_octal(h + OFF_SIZE, 12);
        char type = (char)h[OFF_TYPEFLAG];

        // Entry data is padded to a 512-byte boundary.
        uint64_t padded = (size + (TAR_BLOCK - 1)) & ~((uint64_t)TAR_BLOCK - 1);
        if (offset + padded > len) return -1;

        const uint8_t* data = archive + offset;
        offset += (size_t)padded;

        if (type == TYPE_LONGNAME) {
            size_t n = (size < sizeof(long_name) - 1) ? (size_t)size : sizeof(long_name) - 1;
            memcpy(long_name, data, n);
            long_name[n] = '\0';
            have_long_name = 1;
            continue;
        }

        char name[512];
        if (have_long_name) {
            size_t n = strlen(long_name);
            if (n >= sizeof(name)) n = sizeof(name) - 1;
            memcpy(name, long_name, n);
            name[n] = '\0';
            have_long_name = 0;
        } else {
            build_name(h, name, sizeof(name));
        }

        if (!name[0]) continue;
        if (!dlr_tar_name_is_safe(name)) return -1;

        if (type == TYPE_DIR) {
            // Trailing slash is conventional on directory entries.
            size_t n = strlen(name);
            if (n && name[n - 1] == '/') name[n - 1] = '\0';
            if (sink->on_dir && !sink->on_dir(sink->user, name)) return -1;
            handled++;
            continue;
        }

        if (type == TYPE_FILE || type == TYPE_FILE_OLD) {
            // A name ending in '/' with type '0' is a directory in
            // some old writers' output.
            size_t n = strlen(name);
            if (n && name[n - 1] == '/') {
                name[n - 1] = '\0';
                if (sink->on_dir && !sink->on_dir(sink->user, name)) return -1;
                handled++;
                continue;
            }
            if (size > 0xFFFFFFFFu) return -1;
            if (sink->on_file && !sink->on_file(sink->user, name, data, (uint32_t)size)) return -1;
            handled++;
            continue;
        }

        // Symlinks, hardlinks, devices, pax extensions: skipped. The
        // caller reports them; failing the whole install over a link
        // entry would be worse than installing the files that do work.
        (void)TYPE_SYMLINK; (void)TYPE_HARDLINK;
    }

    return handled;
}
