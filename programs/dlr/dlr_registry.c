#include "dlr_registry.h"
#include "dlr_port.h"
#include "dlr_pkg.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char* g_root = DLR_PKG_STORE;

void dlr_reg_set_root(const char* root) { g_root = root; }
const char* dlr_reg_root(void) { return g_root; }

/* --- small string helpers (the SDK libc has no snprintf) ---------------- */

static void copy_str(char* dst, size_t cap, const char* src) {
    size_t i = 0;
    while (src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

// dst = a + b + c. Returns 0 (leaving dst empty) if it would not fit,
// so an over-long path is refused rather than silently truncated onto
// some other, shorter path.
static int join3(char* dst, size_t cap, const char* a, const char* b, const char* c) {
    size_t la = strlen(a), lb = strlen(b), lc = strlen(c);
    if (la + lb + lc + 1 > cap) { if (cap) dst[0] = '\0'; return 0; }
    memcpy(dst, a, la);
    memcpy(dst + la, b, lb);
    memcpy(dst + la + lb, c, lc);
    dst[la + lb + lc] = '\0';
    return 1;
}

static int path_join(char* dst, size_t cap, const char* dir, const char* leaf) {
    return join3(dst, cap, dir, "/", leaf);
}

static char lower_c(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

// Case-insensitive substring search. An empty needle matches.
static int contains_ci(const char* hay, const char* needle) {
    size_t nl = strlen(needle);
    if (nl == 0) return 1;
    size_t hl = strlen(hay);
    if (nl > hl) return 0;
    for (size_t i = 0; i + nl <= hl; i++) {
        size_t k = 0;
        while (k < nl && lower_c(hay[i + k]) == lower_c(needle[k])) k++;
        if (k == nl) return 1;
    }
    return 0;
}

static int ends_with(const char* s, const char* suffix) {
    size_t ls = strlen(s), lx = strlen(suffix);
    return ls >= lx && strcmp(s + ls - lx, suffix) == 0;
}

static void set_err(char* err, size_t cap, const char* msg) {
    if (err && cap) copy_str(err, cap, msg);
}

static int is_reserved_tree(const char* path) {
    return strcmp(path, "0:/etc") == 0 || strcmp(path, "0:/programs") == 0 ||
           strcmp(path, "0:/services") == 0;
}

/* --- names -------------------------------------------------------------- */

int dlr_reg_name_ok(const char* name) {
    if (!name || !name[0]) return 0;
    size_t n = strlen(name);
    if (n >= DLR_REG_NAME_MAX) return 0;
    if (name[0] == '.') return 0;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) return 0;
    }
    return 1;
}

/* --- manifest ----------------------------------------------------------- */

typedef struct {
    char file[DLR_REG_NAME_MAX + 8];
    int  found;
} manifest_scan;

static int manifest_cb(void* user, const char* name, int is_dir) {
    manifest_scan* m = (manifest_scan*)user;
    if (is_dir || !ends_with(name, ".pkg")) return 1;
    if (strlen(name) >= sizeof(m->file)) return 1;
    copy_str(m->file, sizeof(m->file), name);
    m->found = 1;
    return 0;       // first manifest wins
}

// Reads <dir>'s top-level .pkg manifest. Returns 1 if there is one with a name.
static int read_manifest(const char* dir, dlr_pkg* out) {
    manifest_scan m;
    m.found = 0;
    m.file[0] = '\0';
    printf("dlr: listing manifest directory...\n");
    if (dlr_dir_each(dir, manifest_cb, &m) < 0 || !m.found) return 0;
    printf("dlr: found manifest %s\n", m.file);

    char path[256];
    if (!path_join(path, sizeof(path), dir, m.file)) return 0;
    printf("dlr: opening manifest %s\n", path);

    static char text[8192];
    long n = dlr_file_slurp(path, text, sizeof(text) - 1);
    printf("dlr: manifest bytes %d\n", (int)n);
    if (n <= 0) return 0;
    text[n] = '\0';
    printf("dlr: parsing manifest...\n");
    return dlr_pkg_parse(text, (size_t)n, out);
}

/* --- listing ------------------------------------------------------------ */

typedef struct {
    const char* query;
    dlr_reg_cb  cb;
    void*       user;
    int         reported;
    int         stopped;
} each_ctx;

static int each_cb(void* user, const char* name, int is_dir) {
    each_ctx* c = (each_ctx*)user;
    if (!is_dir || !dlr_reg_name_ok(name)) return 1;

    char dir[256], archive[256];
    if (!path_join(dir, sizeof(dir), g_root, name)) return 1;
    if (!join3(archive, sizeof(archive), dir, ".mpkg", "")) return 1;
    if (!dlr_exists(archive)) return 1;          // present-time build failed or was interrupted

    dlr_reg_entry e;
    memset(&e, 0, sizeof(e));
    copy_str(e.name, sizeof(e.name), name);

    dlr_pkg pkg;
    if (read_manifest(dir, &pkg)) {
        copy_str(e.version, sizeof(e.version), pkg.version);
        copy_str(e.description, sizeof(e.description), pkg.description);
    }

    if (c->query && !contains_ci(e.name, c->query) && !contains_ci(e.description, c->query)) return 1;

    c->reported++;
    if (!c->cb(c->user, &e)) { c->stopped = 1; return 0; }
    return 1;
}

int dlr_reg_search(const char* query, dlr_reg_cb cb, void* user) {
    each_ctx c;
    c.query = query;
    c.cb = cb;
    c.user = user;
    c.reported = 0;
    c.stopped = 0;
    if (dlr_dir_each(g_root, each_cb, &c) < 0) return 0;
    return c.reported;
}

int dlr_reg_each(dlr_reg_cb cb, void* user) {
    return dlr_reg_search(NULL, cb, user);
}

int dlr_reg_find(const char* name, char* out, size_t out_size) {
    if (!dlr_reg_name_ok(name)) return 0;

    char dir[256], archive[256];
    if (!path_join(dir, sizeof(dir), g_root, name)) return 0;
    if (!join3(archive, sizeof(archive), dir, ".mpkg", "")) return 0;
    if (!dlr_is_dir(dir) || !dlr_exists(archive)) return 0;

    if (out) copy_str(out, out_size, archive);
    return 1;
}

/* --- copying a tree into the store -------------------------------------- */

#define DLR_COPY_MAX_DEPTH 12
#define DLR_COPY_MAX_BYTES (16u * 1024u * 1024u)    /* per package; keeps a bad path from filling the disk */

typedef struct {
    const char* src;
    const char* dst;
    int         depth;
    uint32_t*   total;
    int         failed;
    char        why[96];
} copy_ctx;

static int copy_tree(const char* src, const char* dst, int depth, uint32_t* total, char* why, size_t why_cap);

static int copy_entry_cb(void* user, const char* name, int is_dir) {
    copy_ctx* c = (copy_ctx*)user;

    // Never copy the archive of a package that is being re-presented
    // from inside the store.
    if (!is_dir && ends_with(name, ".mpkg") && c->depth == 0) return 1;

    char s[256], d[256];
    if (!path_join(s, sizeof(s), c->src, name) || !path_join(d, sizeof(d), c->dst, name)) {
        c->failed = 1; copy_str(c->why, sizeof(c->why), "path too long"); return 0;
    }

    if (is_dir) {
        if (!copy_tree(s, d, c->depth + 1, c->total, c->why, sizeof(c->why))) { c->failed = 1; return 0; }
        return 1;
    }

    dlr_file* in = dlr_file_open_read(s);
    if (!in) { c->failed = 1; copy_str(c->why, sizeof(c->why), "cannot read a source file"); return 0; }
    dlr_file* out = dlr_file_create(d);
    if (!out) {
        dlr_file_close(in);
        c->failed = 1; copy_str(c->why, sizeof(c->why), "cannot create a file in the store"); return 0;
    }

    static uint8_t buf[4096];
    for (;;) {
        long n = dlr_file_read(in, buf, sizeof(buf));
        if (n < 0) { c->failed = 1; copy_str(c->why, sizeof(c->why), "read error"); break; }
        if (n == 0) break;
        *c->total += (uint32_t)n;
        if (*c->total > DLR_COPY_MAX_BYTES) { c->failed = 1; copy_str(c->why, sizeof(c->why), "package is larger than 16 MiB"); break; }
        if (!dlr_file_write(out, buf, (uint32_t)n)) { c->failed = 1; copy_str(c->why, sizeof(c->why), "write error (disk full?)"); break; }
    }
    dlr_file_close(in);
    dlr_file_close(out);
    return !c->failed;
}

static int copy_tree(const char* src, const char* dst, int depth, uint32_t* total, char* why, size_t why_cap) {
    if (depth > DLR_COPY_MAX_DEPTH) { copy_str(why, why_cap, "directory nesting too deep"); return 0; }
    if (!dlr_mkdirs(dst)) { copy_str(why, why_cap, "cannot create a directory in the store"); return 0; }

    copy_ctx c;
    c.src = src; c.dst = dst; c.depth = depth; c.total = total; c.failed = 0; c.why[0] = '\0';
    int n = dlr_dir_each(src, copy_entry_cb, &c);
    if (n < 0) { copy_str(why, why_cap, "cannot read the source directory"); return 0; }
    if (c.failed) { copy_str(why, why_cap, c.why); return 0; }
    return 1;
}

/* --- removing ----------------------------------------------------------- */

typedef struct { const char* dir; int failed; int depth; } rm_ctx;

static int remove_tree(const char* dir, int depth);

static int rm_cb(void* user, const char* name, int is_dir) {
    rm_ctx* r = (rm_ctx*)user;
    char p[256];
    if (!path_join(p, sizeof(p), r->dir, name)) { r->failed = 1; return 0; }
    if (is_dir) { if (!remove_tree(p, r->depth + 1)) { r->failed = 1; return 0; } }
    else if (!dlr_remove(p)) { r->failed = 1; return 0; }
    return 1;
}

static int remove_tree(const char* dir, int depth) {
    if (depth > DLR_COPY_MAX_DEPTH + 2) return 0;
    rm_ctx r;
    r.dir = dir; r.failed = 0; r.depth = depth;
    if (dlr_dir_each(dir, rm_cb, &r) < 0) return 0;
    if (r.failed) return 0;
    return dlr_remove_dir(dir);
}

int dlr_reg_remove(const char* name) {
    if (!dlr_reg_name_ok(name)) return 0;

    char dir[256], archive[256];
    if (!path_join(dir, sizeof(dir), g_root, name)) return 0;
    if (!join3(archive, sizeof(archive), dir, ".mpkg", "")) return 0;

    int existed = 0;
    if (dlr_exists(archive)) { dlr_remove(archive); existed = 1; }
    if (dlr_is_dir(dir)) { remove_tree(dir, 0); existed = 1; }
    return existed;
}

/* --- present / rebuild -------------------------------------------------- */

static int build_archive(const char* dir, char* err, size_t err_size) {
    char produced[256];
    produced[0] = '\0';
    if (!dlr_pack_dir(dir, produced, sizeof(produced))) {
        set_err(err, err_size, "the archiver failed (out of memory or disk?)");
        return 0;
    }
    return 1;
}

int dlr_reg_rebuild(const char* name, char* err, size_t err_size) {
    if (!dlr_reg_name_ok(name)) { set_err(err, err_size, "invalid package name"); return 0; }

    char dir[256], archive[256];
    if (!path_join(dir, sizeof(dir), g_root, name) || !join3(archive, sizeof(archive), dir, ".mpkg", "")) {
        set_err(err, err_size, "path too long"); return 0;
    }
    if (!dlr_is_dir(dir)) { set_err(err, err_size, "no such package in the store"); return 0; }

    dlr_pkg pkg;
    if (!read_manifest(dir, &pkg)) { set_err(err, err_size, "package has no .pkg manifest with a name"); return 0; }

    // Delete first: the archiver refuses nothing, but a stale archive
    // left behind by a failed rebuild would look like a good one.
    if (dlr_exists(archive)) dlr_remove(archive);
    return build_archive(dir, err, err_size);
}

static const char* base_name(const char* path) {
    const char* b = path;
    for (const char* p = path; *p; p++) if ((*p == '/' || *p == ':') && p[1]) b = p + 1;
    return b;
}

int dlr_reg_present(const char* src_dir, const char* name, char* err, size_t err_size) {
    printf("dlr: checking source directory...\n");
    if (is_reserved_tree(src_dir)) {
        set_err(err, err_size, "refusing to present a system directory");
        return 0;
    }
    if (!dlr_is_dir(src_dir)) { set_err(err, err_size, "source is not a directory"); return 0; }

    dlr_pkg pkg;
    printf("dlr: reading package manifest...\n");
    if (!read_manifest(src_dir, &pkg)) {
        set_err(err, err_size, "no .pkg manifest with [Info] name in that directory - clients could not install it");
        return 0;
    }

    char chosen[DLR_REG_NAME_MAX];
    if (name && name[0]) copy_str(chosen, sizeof(chosen), name);
    else if (pkg.name[0]) copy_str(chosen, sizeof(chosen), pkg.name);
    else copy_str(chosen, sizeof(chosen), base_name(src_dir));

    if (!dlr_reg_name_ok(chosen)) {
        set_err(err, err_size, "package name must be 1-63 characters of letters, digits, '.', '_' or '-'");
        return 0;
    }

    char dst[256], archive[256];
    if (!path_join(dst, sizeof(dst), g_root, chosen) || !join3(archive, sizeof(archive), dst, ".mpkg", "")) {
        set_err(err, err_size, "path too long"); return 0;
    }
    if (dlr_exists(dst) || dlr_exists(archive)) {
        set_err(err, err_size, "already presented - remove it first");
        return 0;
    }

    printf("dlr: preparing package store...\n");
    if (!dlr_mkdirs(g_root)) { set_err(err, err_size, "cannot create the package store"); return 0; }

    char why[96];
    uint32_t total = 0;
    printf("dlr: copying package files...\n");
    if (!copy_tree(src_dir, dst, 0, &total, why, sizeof(why))) {
        dlr_reg_remove(chosen);         // leave nothing half-copied
        set_err(err, err_size, why);
        return 0;
    }

    printf("dlr: building package archive...\n");
    if (!build_archive(dst, err, err_size)) {
        dlr_reg_remove(chosen);
        return 0;
    }
    return 1;
}
