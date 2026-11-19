#ifndef FGLCACHE_H
#define FGLCACHE_H
/*
 * fglcache.h - the FarcadeGL shader cache (0:/cache/shaders).
 *
 * fglCreateShader() looks a program up here before it compiles it: the key
 * is a hash of both sources (and the compiler's version), the value the
 * compiled FGSL program (blob + uniform table). Every program that uses
 * FarcadeGL shares the cache, so a game's second start, or a renderer that
 * switches back and forth, skips the compiler.
 *
 * Size limit, from the size of the volume the cache is on (0:):
 *     under 256 MB  -> 20 MB
 *     under 80 GB   -> 5 GB
 *     under 300 GB  -> 30 GB
 *     300 GB and up -> 50 GB
 * but never more than half the volume. The least recently used programs
 * go first when it is full (an index file keeps the order).
 *
 * Programs normally need none of the calls below:
 *     fglShaderCacheEnable(0)          turn it off for this program
 *     fglShaderCacheSetDir(path)       another directory (tests)
 *     fglShaderCacheSetLimit(bytes)    another limit (0: the default)
 *     fglShaderCacheStats(&s)          hits, misses, bytes, limit
 *     fglShaderCacheClear()            delete every cached program
 *     fglShaderCacheFlush()            write the index (fglDestroyContext does)
 *
 * File access goes through the fglc__fs_* functions below; a host build
 * defines FGLC_FS_PROVIDED and supplies its own (Farcade-Engine does).
 *
 * Included by farcadegl.h; needs fglsl.h first.
 */
#include <stdint.h>
#include <stddef.h>

#define FGLC_FILE_MAGIC  0x31434746u   /* "FGC1" */
#define FGLC_INDEX_MAGIC 0x31494746u   /* "FGI1" */
#define FGLC_MAX_ENTRIES 16384u
#define FGLC_PATH_MAX    128
/* Bump when fglsl.h starts producing different code for the same source. */
#define FGLC_COMPILER_REV 1u

typedef struct {
    uint32_t hits, misses, stores, evictions;
    uint64_t bytes;             // in the cache now
    uint64_t limit;             // what it may hold
    uint32_t entries;
    int enabled;
} fgl_shader_cache_stats_t;

/* ---------------------------------------------------------- file access */

#ifndef FGLC_FS_PROVIDED
#define FGLC_DEFAULT_DIR "0:/cache/shaders"
static inline long fglc__sys(uint64_t n, uint64_t a, uint64_t b, uint64_t c) {
    return (long)mos_syscall3(n, a, b, c);
}
// Reads a whole file into buf; bytes read, or -1.
static long fglc__fs_read(const char* path, void* buf, uint32_t cap) {
    long fd = fglc__sys(SYS_OPEN, (uint64_t)(uintptr_t)path, SYS_O_RDONLY, 0), got = 0, n;
    if (fd <= 0) return -1;
    while ((uint32_t)got < cap && (n = fglc__sys(SYS_READ, (uint64_t)fd, (uint64_t)(uintptr_t)((char*)buf + got), cap - (uint32_t)got)) > 0)
        got += n;
    fglc__sys(SYS_CLOSE, (uint64_t)fd, 0, 0);
    return got;
}
static long fglc__fs_size(const char* path) {
    syscall_stat_t st;
    if (fglc__sys(SYS_STAT, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)&st, 0) != 0) return -1;
    return st.type == SYS_STAT_FILE ? (long)st.size : -1;
}
static int fglc__fs_delete(const char* path) { return fglc__sys(SYS_DELETE, (uint64_t)(uintptr_t)path, 0, 0) == 0; }
static void fglc__fs_mkdirs(const char* path) {          // every missing directory of path
    char p[FGLC_PATH_MAX];
    size_t n = strlen(path);
    if (n >= sizeof(p)) return;
    memcpy(p, path, n + 1);
    for (size_t i = 3; i <= n; i++) {
        if (p[i] != '/' && p[i] != 0) continue;
        char keep = p[i];
        p[i] = 0;
        if (fglc__sys(SYS_IS_DIR, (uint64_t)(uintptr_t)p, 0, 0) != 1) fglc__sys(SYS_MKDIR, (uint64_t)(uintptr_t)p, 0, 0);
        p[i] = keep;
    }
}
// Writes up to three pieces as one file: to a temporary name first, then
// renamed over the old one, so a reader never sees half a file.
static int fglc__fs_write(const char* path, const void* a, uint32_t an, const void* b, uint32_t bn, const void* c,
                          uint32_t cn) {
    char tmp[FGLC_PATH_MAX + 8];
    size_t n = strlen(path);
    if (n + 5 >= sizeof(tmp)) return 0;
    memcpy(tmp, path, n);
    memcpy(tmp + n, ".tmp", 5);
    fglc__fs_delete(tmp);
    if ((long)fglc__sys(SYS_CREATE, (uint64_t)(uintptr_t)tmp, (uint64_t)(uintptr_t)"cache", (uint64_t)(uintptr_t)"fgc") < 0) return 0;
    long fd = fglc__sys(SYS_OPEN, (uint64_t)(uintptr_t)tmp, SYS_O_RDWR, 0);
    if (fd <= 0) return 0;
    const void* part[3] = { a, b, c };
    uint32_t len[3] = { an, bn, cn };
    int ok = 1;
    for (int k = 0; k < 3 && ok; k++) {
        uint32_t done = 0;
        while (done < len[k]) {
            long w = fglc__sys(SYS_FWRITE, (uint64_t)fd, (uint64_t)(uintptr_t)((const char*)part[k] + done), len[k] - done);
            if (w <= 0) { ok = 0; break; }
            done += (uint32_t)w;
        }
    }
    fglc__sys(SYS_CLOSE, (uint64_t)fd, 0, 0);
    if (ok) ok = fglc__sys(SYS_RENAME, (uint64_t)(uintptr_t)tmp, (uint64_t)(uintptr_t)path, 0) == 0;
    if (!ok) fglc__fs_delete(tmp);
    return ok;
}
// Calls fn for every file in dir.
static void fglc__fs_list(const char* dir, void (*fn)(void* ctx, const char* name), void* ctx) {
    const uint32_t cap = 4096;
    syscall_dirent_t* e = (syscall_dirent_t*)malloc(sizeof(syscall_dirent_t) * cap);
    if (!e) return;
    long n = fglc__sys(SYS_LISTDIR, (uint64_t)(uintptr_t)dir, (uint64_t)(uintptr_t)e, cap);
    if (n > (long)cap) n = (long)cap;
    for (long i = 0; i < n; i++) {
        e[i].name[SYSCALL_DIRENT_NAME_SIZE - 1] = 0;
        if (e[i].type != SYSCALL_DIRENT_TYPE_DIR) fn(ctx, e[i].name);
    }
    free(e);
}
// Size of the volume mounted as 0:, 0 when it is not known (a RAM disk).
static uint64_t fglc__fs_volume_bytes(void) {
    syscall_storage_device_t dev[32];
    syscall_storage_request_t r;
    memset(&r, 0, sizeof(r));
    r.size = sizeof(r);
    r.version = SYS_STORAGE_ABI_VERSION;
    r.op = SYS_STORAGE_LIST;
    r.capacity = 32;
    r.devices = dev;
    long n = fglc__sys(SYS_STORAGE, (uint64_t)(uintptr_t)&r, 0, 0);
    if (n < 0) return 0;
    if (n > 32) n = 32;
    for (long i = 0; i < n; i++)
        if (dev[i].mounted_drive == 0) return dev[i].size_bytes;
    return 0;
}
#endif /* FGLC_FS_PROVIDED */

/* ---------------------------------------------------------------- state */

typedef struct {
    uint32_t magic, salt;
    uint64_t key;
    uint32_t check;             // a second hash of the sources
    uint32_t vs_len, fs_len;
    uint32_t blob_bytes;
    uint32_t uniform_count, uniform_size, next_uniform_reg;
    uint32_t reserved;
} fglc__file_t;

typedef struct { uint64_t key; uint32_t bytes, stamp; } fglc__entry_t;

/* One state for the whole program: every file that includes farcadegl.h
 * shares it (a weak symbol), so they do not write over each other's index. */
typedef struct fglc__state {
    int init, disabled;
    char dir[FGLC_PATH_MAX];
    uint64_t limit, limit_override;
    fglc__entry_t* e;
    uint32_t count, stamp, dirty;
    uint64_t bytes;
    fgl_shader_cache_stats_t st;
} fglc__state_t;
__attribute__((weak)) fglc__state_t fglc__g;

static uint32_t fglc__salt(void) {
    return (FGLC_COMPILER_REV << 24) ^ (FGPU_SHADER_VERSION << 16) ^ (uint32_t)sizeof(fglsl_uniform_t) ^
           ((uint32_t)FGLSL_MAX_UNIFORMS << 8) ^ (uint32_t)(sizeof(fglsl_program_t) & 0xFFFFu) << 4;
}

static uint64_t fglc__fnv(uint64_t h, const char* s, size_t n) {
    for (size_t i = 0; i < n; i++) { h ^= (uint8_t)s[i]; h *= 1099511628211ull; }
    return h;
}
static uint32_t fglc__mix(uint32_t h, const char* s, size_t n) {   // a second, independent hash
    for (size_t i = 0; i < n; i++) { h = (h ^ (uint8_t)s[i]) * 0x5bd1e995u; h ^= h >> 15; }
    return h;
}

static void fglc__path(char* out, uint64_t key) {
    static const char hex[] = "0123456789abcdef";
    size_t n = strlen(fglc__g.dir);
    memcpy(out, fglc__g.dir, n);
    out[n++] = '/';
    for (int i = 15; i >= 0; i--) out[n++] = hex[(key >> (i * 4)) & 15u];
    memcpy(out + n, ".fgc", 5);
}
static void fglc__index_path(char* out) {
    size_t n = strlen(fglc__g.dir);
    memcpy(out, fglc__g.dir, n);
    memcpy(out + n, "/index.fgi", 11);
}

// The default limit for a volume of `volume` bytes (0: unknown).
static uint64_t fglc__limit_for(uint64_t volume) {
    const uint64_t MB = 1024ull * 1024ull, GB = 1024ull * MB;
    uint64_t limit;
    if (volume < 256 * MB) limit = 20 * MB;
    else if (volume < 80 * GB) limit = 5 * GB;
    else if (volume < 300 * GB) limit = 30 * GB;
    else limit = 50 * GB;
    if (volume && limit > volume / 2) limit = volume / 2;   // never the whole disk
    return limit;
}

static int fglc__find(uint64_t key) {
    for (uint32_t i = 0; i < fglc__g.count; i++)
        if (fglc__g.e[i].key == key) return (int)i;
    return -1;
}

static int fglc__hex_key(const char* name, uint64_t* key) {        // "<16 hex>.fgc"
    uint64_t k = 0;
    for (int i = 0; i < 16; i++) {
        char c = name[i];
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (v < 0) return 0;
        k = (k << 4) | (uint64_t)v;
    }
    if (strcmp(name + 16, ".fgc") != 0) return 0;
    *key = k;
    return 1;
}

// Files the index does not know (another program wrote them, or the index
// was lost) join it as the oldest; leftover temporary files go.
static void fglc__adopt(void* ctx, const char* name) {
    (void)ctx;
    uint64_t key;
    size_t n = strlen(name);
    char path[FGLC_PATH_MAX + 24];
    size_t dn = strlen(fglc__g.dir);
    if (dn + 1 + n + 1 > sizeof(path)) return;
    memcpy(path, fglc__g.dir, dn);
    path[dn] = '/';
    memcpy(path + dn + 1, name, n + 1);
    if (n > 4 && !strcmp(name + n - 4, ".tmp")) { fglc__fs_delete(path); return; }
    if (!fglc__hex_key(name, &key) || fglc__find(key) >= 0 || fglc__g.count >= FGLC_MAX_ENTRIES) return;
    long size = fglc__fs_size(path);
    if (size <= 0) return;
    fglc__entry_t* e = &fglc__g.e[fglc__g.count++];
    e->key = key;
    e->bytes = (uint32_t)size;
    e->stamp = 0;
    fglc__g.bytes += (uint64_t)size;
    fglc__g.dirty++;
}

static void fglc__init(void) {
    if (fglc__g.init) return;
    fglc__g.init = 1;
    if (!fglc__g.dir[0]) {
#ifdef FGLC_DEFAULT_DIR
        memcpy(fglc__g.dir, FGLC_DEFAULT_DIR, sizeof(FGLC_DEFAULT_DIR));
#else
        fglc__g.disabled = 1;           // the host build names a directory or has none
        return;
#endif
    }
    fglc__g.e = (fglc__entry_t*)malloc(sizeof(fglc__entry_t) * FGLC_MAX_ENTRIES);
    if (!fglc__g.e) { fglc__g.disabled = 1; return; }
    fglc__fs_mkdirs(fglc__g.dir);
    fglc__g.limit = fglc__g.limit_override ? fglc__g.limit_override : fglc__limit_for(fglc__fs_volume_bytes());
    // The index: a header, then the entries.
    char ip[FGLC_PATH_MAX + 16];
    fglc__index_path(ip);
    uint32_t head[4];
    long size = fglc__fs_size(ip);
    if (size >= (long)sizeof(head)) {
        uint8_t* buf = (uint8_t*)malloc((size_t)size);
        if (buf && fglc__fs_read(ip, buf, (uint32_t)size) == size) {
            memcpy(head, buf, sizeof(head));
            uint32_t n = head[1];
            if (head[0] == FGLC_INDEX_MAGIC && n <= FGLC_MAX_ENTRIES &&
                (long)(sizeof(head) + n * sizeof(fglc__entry_t)) <= size) {
                memcpy(fglc__g.e, buf + sizeof(head), n * sizeof(fglc__entry_t));
                fglc__g.count = n;
                fglc__g.stamp = head[2];
            }
        }
        free(buf);
    }
    // Entries whose file is gone are dropped; the sizes are trusted.
    uint32_t keep = 0;
    for (uint32_t i = 0; i < fglc__g.count; i++) {
        char path[FGLC_PATH_MAX + 24];
        fglc__path(path, fglc__g.e[i].key);
        if (fglc__fs_size(path) <= 0) { fglc__g.dirty++; continue; }
        fglc__g.e[keep++] = fglc__g.e[i];
    }
    fglc__g.count = keep;
    fglc__g.bytes = 0;
    for (uint32_t i = 0; i < fglc__g.count; i++) fglc__g.bytes += fglc__g.e[i].bytes;
    fglc__fs_list(fglc__g.dir, fglc__adopt, 0);
}

static void fglShaderCacheFlush(void) {
    if (!fglc__g.init || fglc__g.disabled || !fglc__g.dirty) return;
    char ip[FGLC_PATH_MAX + 16];
    uint32_t head[4] = { FGLC_INDEX_MAGIC, fglc__g.count, fglc__g.stamp, 0 };
    fglc__index_path(ip);
    if (fglc__fs_write(ip, head, sizeof(head), fglc__g.e, fglc__g.count * (uint32_t)sizeof(fglc__entry_t), 0, 0))
        fglc__g.dirty = 0;
}

// Deletes the least recently used programs until the cache is under 90 %
// of its limit (or holds `room` more bytes).
static void fglc__evict(uint64_t room) {
    uint64_t target = fglc__g.limit / 10 * 9;
    while (fglc__g.count && fglc__g.bytes + room > target) {
        uint32_t old = 0;
        for (uint32_t i = 1; i < fglc__g.count; i++)
            if (fglc__g.e[i].stamp < fglc__g.e[old].stamp) old = i;
        char path[FGLC_PATH_MAX + 24];
        fglc__path(path, fglc__g.e[old].key);
        fglc__fs_delete(path);
        fglc__g.bytes -= fglc__g.e[old].bytes;
        fglc__g.e[old] = fglc__g.e[--fglc__g.count];
        fglc__g.st.evictions++;
        fglc__g.dirty++;
    }
}

static uint64_t fglc__key(const char* vs, const char* fs, uint32_t* check) {
    size_t vn = strlen(vs), fn = strlen(fs);
    uint32_t salt = fglc__salt();
    uint64_t h = fglc__fnv(1469598103934665603ull ^ salt, vs, vn);
    h = fglc__fnv(h, "\0", 1);
    h = fglc__fnv(h, fs, fn);
    *check = fglc__mix(fglc__mix(salt * 2654435761u, vs, vn), fs, fn);
    return h;
}

// Fills c from the cache; 1 on a hit.
static int fglc__load(const char* vs, const char* fs, fglsl_program_t* c) {
    fglc__init();
    if (fglc__g.disabled) return 0;
    uint32_t check;
    uint64_t key = fglc__key(vs, fs, &check);
    int i = fglc__find(key);
    if (i < 0) { fglc__g.st.misses++; return 0; }
    char path[FGLC_PATH_MAX + 24];
    fglc__path(path, key);
    uint32_t bytes = fglc__g.e[i].bytes;
    uint8_t* buf = (uint8_t*)malloc(bytes);
    int ok = 0;
    if (buf && fglc__fs_read(path, buf, bytes) == (long)bytes && bytes >= sizeof(fglc__file_t)) {
        fglc__file_t h;
        memcpy(&h, buf, sizeof(h));
        uint32_t ub = h.uniform_count * h.uniform_size;
        ok = h.magic == FGLC_FILE_MAGIC && h.salt == fglc__salt() && h.key == key && h.check == check &&
             h.vs_len == (uint32_t)strlen(vs) && h.fs_len == (uint32_t)strlen(fs) &&
             h.uniform_size == sizeof(fglsl_uniform_t) && h.uniform_count <= FGLSL_MAX_UNIFORMS &&
             h.blob_bytes <= sizeof(c->words) && !(h.blob_bytes & 3u) &&
             sizeof(h) + ub + h.blob_bytes == bytes;
        if (ok) {
            memcpy(c->uniforms, buf + sizeof(h), ub);
            memcpy(c->words, buf + sizeof(h) + ub, h.blob_bytes);
            c->uniform_count = h.uniform_count;
            c->size = h.blob_bytes;
            c->next_uniform_reg = h.next_uniform_reg;
            c->error[0] = 0;
        }
    }
    free(buf);
    if (!ok) {                          // damaged or from another compiler: forget it
        fglc__fs_delete(path);
        fglc__g.bytes -= fglc__g.e[i].bytes;
        fglc__g.e[i] = fglc__g.e[--fglc__g.count];
        fglc__g.dirty++;
        fglc__g.st.misses++;
        return 0;
    }
    fglc__g.e[i].stamp = ++fglc__g.stamp;
    if (++fglc__g.dirty >= 32) fglShaderCacheFlush();
    fglc__g.st.hits++;
    return 1;
}

// Keeps a freshly compiled program.
static void fglc__store(const char* vs, const char* fs, const fglsl_program_t* c) {
    if (!fglc__g.init || fglc__g.disabled || !c->size) return;
    uint32_t check;
    uint64_t key = fglc__key(vs, fs, &check);
    fglc__file_t h;
    memset(&h, 0, sizeof(h));
    h.magic = FGLC_FILE_MAGIC;
    h.salt = fglc__salt();
    h.key = key;
    h.check = check;
    h.vs_len = (uint32_t)strlen(vs);
    h.fs_len = (uint32_t)strlen(fs);
    h.blob_bytes = c->size;
    h.uniform_count = c->uniform_count;
    h.uniform_size = sizeof(fglsl_uniform_t);
    h.next_uniform_reg = c->next_uniform_reg;
    uint32_t ub = c->uniform_count * (uint32_t)sizeof(fglsl_uniform_t);
    uint32_t total = (uint32_t)sizeof(h) + ub + c->size;
    if (total > fglc__g.limit / 4) return;          // a program that would crowd out the rest
    int old = fglc__find(key);
    if (old >= 0) {                                 // replaced
        fglc__g.bytes -= fglc__g.e[old].bytes;
        fglc__g.e[old] = fglc__g.e[--fglc__g.count];
    }
    fglc__evict(total);
    if (fglc__g.count >= FGLC_MAX_ENTRIES) return;
    char path[FGLC_PATH_MAX + 24];
    fglc__path(path, key);
    if (!fglc__fs_write(path, &h, sizeof(h), c->uniforms, ub, c->words, c->size)) return;
    fglc__entry_t* e = &fglc__g.e[fglc__g.count++];
    e->key = key;
    e->bytes = total;
    e->stamp = ++fglc__g.stamp;
    fglc__g.bytes += total;
    fglc__g.st.stores++;
    fglc__g.dirty++;
    fglShaderCacheFlush();
}

// The kernel refused a program that came from the cache: drop it.
static void fglc__forget(const char* vs, const char* fs) {
    if (!fglc__g.init || fglc__g.disabled) return;
    uint32_t check;
    int i = fglc__find(fglc__key(vs, fs, &check));
    if (i < 0) return;
    char path[FGLC_PATH_MAX + 24];
    fglc__path(path, fglc__g.e[i].key);
    fglc__fs_delete(path);
    fglc__g.bytes -= fglc__g.e[i].bytes;
    fglc__g.e[i] = fglc__g.e[--fglc__g.count];
    fglc__g.dirty++;
    fglShaderCacheFlush();
}

/* ------------------------------------------------------------------ API */

static inline void fglShaderCacheEnable(int on) {
    if (!on) { fglShaderCacheFlush(); fglc__g.disabled = 1; }
    else if (fglc__g.disabled && fglc__g.init && fglc__g.e) fglc__g.disabled = 0;
    else if (!fglc__g.init) fglc__g.disabled = 0;
}

// Uses another directory from now on (call before the first shader).
static inline void fglShaderCacheSetDir(const char* dir) {
    fglShaderCacheFlush();
    size_t n = dir ? strlen(dir) : 0;
    if (n >= sizeof(fglc__g.dir)) n = sizeof(fglc__g.dir) - 1;
    free(fglc__g.e);
    memset(&fglc__g, 0, sizeof(fglc__g));
    if (n) memcpy(fglc__g.dir, dir, n);
    fglc__g.dir[n] = 0;
}

static inline void fglShaderCacheSetLimit(uint64_t bytes) {
    fglc__g.limit_override = bytes;
    if (fglc__g.init && !fglc__g.disabled) {
        fglc__g.limit = bytes ? bytes : fglc__limit_for(fglc__fs_volume_bytes());
        fglc__evict(0);
        fglShaderCacheFlush();
    }
}

static inline void fglShaderCacheStats(fgl_shader_cache_stats_t* out) {
    fglc__init();
    *out = fglc__g.st;
    out->bytes = fglc__g.bytes;
    out->limit = fglc__g.limit;
    out->entries = fglc__g.count;
    out->enabled = !fglc__g.disabled;
}

static inline void fglShaderCacheClear(void) {
    fglc__init();
    if (fglc__g.disabled) return;
    for (uint32_t i = 0; i < fglc__g.count; i++) {
        char path[FGLC_PATH_MAX + 24];
        fglc__path(path, fglc__g.e[i].key);
        fglc__fs_delete(path);
    }
    fglc__g.count = 0;
    fglc__g.bytes = 0;
    fglc__g.dirty++;
    fglShaderCacheFlush();
}

#endif /* FGLCACHE_H */
