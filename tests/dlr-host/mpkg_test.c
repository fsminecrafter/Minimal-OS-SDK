/*
 * Tests for programs/dlr/dlr_mpkg.c - the client-side .mpkg reader.
 *
 *  1. The LZSS decoder against the KERNEL's compressor (lzss.c, compiled
 *     unmodified from the kernel repo). This is the check that matters:
 *     the reader is a copy of kernel code, and copies drift.
 *  2. Hand-built archives, valid and hostile. The archive comes from the
 *     network; every malformed shape must be refused, never trusted and
 *     never crash. Run under ASan/UBSan (build script does).
 *  3. With arguments `<archive.mpkg> <dir>`: an archive made by the host
 *     tool mkpkg.py extracts to exactly the files in <dir>.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>

#include "dlr_mpkg.h"
#include "x86_64/lzss.h"

/* The kernel's compressor wants these three from its allocator. */
void* alloc_unzeroed(size_t n) { return malloc(n); }
void* alloc_resize(void* p, size_t n) { return realloc(p, n); }
void  free_mem(void* p) { free(p); }

static int g_pass = 0, g_fail = 0;
#define CHECK(c, ...) do { if (c) g_pass++; else { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- 1. decoder vs kernel compressor -------------------------------- */

static uint32_t g_rng = 12345;
static uint32_t rnd(void) { g_rng = g_rng * 1664525u + 1013904223u; return g_rng >> 8; }

static void roundtrip(const uint8_t* data, uint32_t len, const char* label) {
    uint32_t clen = 0;
    uint8_t* comp = lzss_compress(data, len, &clen);
    if (len == 0) { CHECK(comp == NULL, "%s: empty input", label); return; }
    CHECK(comp != NULL, "%s: compress failed", label);
    if (!comp) return;

    uint8_t* out = malloc(len + 1);
    uint32_t n = dlr_lzss_decode(comp, clen, out, len);
    CHECK(n == len && memcmp(out, data, len) == 0, "%s: %u bytes -> %u, mismatch", label, len, n);

    /* Same answer as the kernel's own decoder. */
    uint8_t* kout = malloc(len + 1);
    uint32_t kn = lzss_decompress(comp, clen, kout, len);
    CHECK(kn == n && memcmp(kout, out, len) == 0, "%s: differs from the kernel decoder", label);

    /* One byte of room too few: refuse, do not overflow (ASan watches). */
    if (len > 1) CHECK(dlr_lzss_decode(comp, clen, out, len - 1) == 0, "%s: undersized output accepted", label);

    free(out); free(kout); free(comp);
}

static void test_lzss_roundtrip(void) {
    printf("LZSS decoder vs the kernel compressor\n");
    uint8_t* buf = malloc(300000);
    char label[64];

    for (uint32_t len = 1; len <= 70; len++) {
        for (uint32_t i = 0; i < len; i++) buf[i] = (uint8_t)rnd();
        snprintf(label, sizeof(label), "random/%u", len);
        roundtrip(buf, len, label);
        for (uint32_t i = 0; i < len; i++) buf[i] = (uint8_t)"abcab"[i % 5];
        snprintf(label, sizeof(label), "periodic/%u", len);
        roundtrip(buf, len, label);
    }
    memset(buf, 0, 65536);                              roundtrip(buf, 65536, "zeros 64K");
    for (int i = 0; i < 131072; i++) buf[i] = (uint8_t)"the quick brown fox "[i % 20];
                                                        roundtrip(buf, 131072, "text 128K");
    for (int i = 0; i < 200000; i++) buf[i] = (i & 1) ? (uint8_t)rnd() : (uint8_t)"ab"[(i >> 3) & 1];
                                                        roundtrip(buf, 200000, "mixed 200K");
    /* Matches that reach exactly across the 4096-byte window edge. */
    for (int i = 0; i < 20000; i++) buf[i] = (uint8_t)((i % 4096) * 7);
                                                        roundtrip(buf, 20000, "window-sized period");
    for (int i = 0; i < 20000; i++) buf[i] = (uint8_t)((i % 4097) * 7);
                                                        roundtrip(buf, 20000, "window+1 period");
    free(buf);
}

/* ---- 2. building archives by hand ----------------------------------- */

#define HDR 16
#define ENT 148

typedef struct {
    char name[128];
    uint32_t flags, method, usize, csize, offset;
} ent_t;

static void w32(uint8_t* p, uint32_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }

/* Lays out header + table + payloads exactly like the kernel does. */
typedef struct { uint8_t* data; size_t len; } blob_t;

static blob_t make_archive(const ent_t* e, const uint8_t* const* payloads, int n) {
    size_t total = HDR + (size_t)n * ENT;
    for (int i = 0; i < n; i++) total += e[i].csize;
    uint8_t* a = calloc(1, total + 1);
    memcpy(a, "MPKG0001", 8); w32(a + 8, 1); w32(a + 12, (uint32_t)n);
    size_t off = HDR + (size_t)n * ENT;
    for (int i = 0; i < n; i++) {
        uint8_t* t = a + HDR + (size_t)i * ENT;
        memcpy(t, e[i].name, 128);
        w32(t + 128, e[i].flags); w32(t + 132, e[i].method);
        w32(t + 136, e[i].usize); w32(t + 140, e[i].csize);
        w32(t + 144, e[i].offset ? e[i].offset : (uint32_t)off);
        if (payloads[i] && e[i].csize) memcpy(a + off, payloads[i], e[i].csize);
        off += e[i].csize;
    }
    blob_t b = { a, total };
    return b;
}

static ent_t mk(const char* name, uint32_t flags, uint32_t method, uint32_t usize, uint32_t csize) {
    ent_t e; memset(&e, 0, sizeof(e));
    strncpy(e.name, name, 127);
    e.flags = flags; e.method = method; e.usize = usize; e.csize = csize;
    return e;
}

/* Sink that records what it was given. */
typedef struct {
    int files, dirs, abort_after;
    char names[8][128];
    uint8_t* data[8]; uint32_t len[8];
} rec_t;

static int on_file(void* u, const char* name, const uint8_t* d, uint32_t n) {
    rec_t* r = u;
    if (r->abort_after && r->files >= r->abort_after) return 0;
    if (r->files < 8) {
        snprintf(r->names[r->files], 128, "%s", name);
        r->data[r->files] = malloc(n ? n : 1); if (n) memcpy(r->data[r->files], d, n);
        r->len[r->files] = n;
    }
    r->files++;
    return 1;
}
static int on_dir(void* u, const char* name) { rec_t* r = u; (void)name; r->dirs++; return 1; }
static void rec_free(rec_t* r) { for (int i = 0; i < 8 && i < r->files; i++) free(r->data[i]); }

static long extract(blob_t b, rec_t* r) {
    memset(r, 0, sizeof(*r));
    dlr_tar_sink sink = { on_file, on_dir, r };
    return dlr_mpkg_extract_mem(b.data, b.len, &sink);
}

static void test_valid_archive(void) {
    printf("valid archive: store + lzss + empty file + directory\n");
    const char* text = "hello hello hello hello hello hello hello hello, world of packages";
    uint32_t tlen = (uint32_t)strlen(text);
    uint32_t clen; uint8_t* comp = lzss_compress((const uint8_t*)text, tlen, &clen);
    uint8_t raw[5] = { 1, 2, 3, 4, 5 };

    ent_t e[4] = {
        mk("bin/",        1, 0, 0, 0),
        mk("bin/raw.dat", 0, 0, 5, 5),
        mk("bin/text.txt",0, 1, tlen, clen),
        mk("empty",       0, 0, 0, 0),
    };
    const uint8_t* p[4] = { NULL, raw, comp, NULL };
    blob_t b = make_archive(e, p, 4);
    rec_t r;
    long n = extract(b, &r);
    CHECK(n == 4, "handled %ld entries", n);
    CHECK(r.dirs == 1 && r.files == 3, "dirs %d files %d", r.dirs, r.files);
    CHECK(r.files == 3 && r.len[0] == 5 && memcmp(r.data[0], raw, 5) == 0, "stored file");
    CHECK(r.files == 3 && r.len[1] == tlen && memcmp(r.data[1], text, tlen) == 0, "compressed file");
    CHECK(r.files == 3 && r.len[2] == 0 && strcmp(r.names[2], "empty") == 0, "empty file");
    CHECK(dlr_mpkg_is_archive(b.data, b.len), "recognised");
    rec_free(&r); free(b.data); free(comp);
}

static void expect_refused(const char* what, blob_t b) {
    rec_t r;
    long n = extract(b, &r);
    CHECK(n < 0, "%s: accepted (%ld)", what, n);
    rec_free(&r);
    free(b.data);
}

static void test_hostile(void) {
    printf("malformed and hostile archives are refused\n");
    uint8_t raw[64] = { 0 };       /* big enough for the largest csize below */
    const uint8_t* p1[1] = { raw };

    /* names */
    const char* bad[] = { "../evil", "/abs/path", "a/../../b", "C:/x", "dir\\file", "..", "a/.." };
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
        ent_t e[1] = { mk(bad[i], 0, 0, 8, 8) };
        char what[64]; snprintf(what, sizeof(what), "unsafe name '%s'", bad[i]);
        expect_refused(what, make_archive(e, p1, 1));
    }
    { /* name with no terminator in all 128 bytes */
        ent_t e[1]; memset(&e[0], 0, sizeof(e[0])); memset(e[0].name, '/', 128);
        e[0].usize = e[0].csize = 8;
        expect_refused("128-byte name, no NUL", make_archive(e, p1, 1));
    }

    /* structure */
    { ent_t e[1] = { mk("f", 0, 0, 8, 8) }; blob_t b = make_archive(e, p1, 1);
      memcpy(b.data, "MPKG0002", 8);  expect_refused("bad magic", b); }
    { ent_t e[1] = { mk("f", 0, 0, 8, 8) }; blob_t b = make_archive(e, p1, 1);
      w32(b.data + 8, 2);             expect_refused("unsupported version", b); }
    { ent_t e[1] = { mk("f", 0, 0, 8, 8) }; blob_t b = make_archive(e, p1, 1);
      w32(b.data + 12, 1000000);      expect_refused("entry count beyond the limit", b); }
    { ent_t e[1] = { mk("f", 0, 0, 8, 8) }; blob_t b = make_archive(e, p1, 1);
      w32(b.data + 12, 50);           expect_refused("entry table longer than the file", b); }
    { ent_t e[1] = { mk("f", 0, 0, 8, 8) }; blob_t b = make_archive(e, p1, 1);
      b.len = 10;                     expect_refused("truncated header", b); }
    { ent_t e[1] = { mk("f", 0, 0, 8, 8) }; blob_t b = make_archive(e, p1, 1);
      b.len -= 4;                     expect_refused("payload cut short", b); }

    /* payload ranges */
    { ent_t e[1] = { mk("f", 0, 0, 8, 8) }; e[0].offset = 0xFFFFFFF0u;
      expect_refused("offset near 2^32 with a 32-bit wrap", make_archive(e, p1, 1)); }
    { ent_t e[1] = { mk("f", 0, 0, 0x30, 0x30) }; e[0].offset = 0xFFFFFFF0u;
      expect_refused("offset+size wraps 32 bits", make_archive(e, p1, 1)); }
    { ent_t e[1] = { mk("f", 0, 0, 8, 8) }; e[0].offset = 4;
      expect_refused("payload inside the header/table", make_archive(e, p1, 1)); }

    /* sizes and methods */
    { ent_t e[1] = { mk("f", 0, 0, 9, 8) };
      expect_refused("STORE with usize != csize", make_archive(e, p1, 1)); }
    { ent_t e[1] = { mk("f", 0, 7, 8, 8) };
      expect_refused("unknown method", make_archive(e, p1, 1)); }
    {   uint8_t d[64]; memset(d, 'a', sizeof(d)); uint32_t cl; uint8_t* c = lzss_compress(d, sizeof(d), &cl);
        const uint8_t* p[1] = { c };
        ent_t big[1]  = { mk("f", 0, 1, 1000, cl) };
        expect_refused("LZSS claims more output than it has", make_archive(big, p, 1));
        ent_t small[1] = { mk("f", 0, 1, 10, cl) };
        expect_refused("LZSS overruns the declared size", make_archive(small, p, 1));
        ent_t huge[1] = { mk("f", 0, 1, 0x7FFFFFFFu, cl) };
        expect_refused("LZSS claims a 2 GiB output", make_archive(huge, p, 1));
        free(c); }
}

static void test_sink_abort(void) {
    printf("a sink that says stop stops the extraction\n");
    uint8_t raw[4] = { 1, 2, 3, 4 };
    ent_t e[3] = { mk("a", 0, 0, 4, 4), mk("b", 0, 0, 4, 4), mk("c", 0, 0, 4, 4) };
    const uint8_t* p[3] = { raw, raw, raw };
    blob_t b = make_archive(e, p, 3);
    rec_t r; memset(&r, 0, sizeof(r)); r.abort_after = 1;
    dlr_tar_sink sink = { on_file, on_dir, &r };
    long n = dlr_mpkg_extract_mem(b.data, b.len, &sink);
    CHECK(n < 0, "abort reported as failure (%ld)", n);
    CHECK(r.files == 1, "no entries after the abort (%d)", r.files);
    rec_free(&r);
    CHECK(dlr_mpkg_extract_mem(b.data, b.len, NULL) < 0, "NULL sink refused");
    free(b.data);
}

static void test_fuzz(void) {
    printf("random corruption of a valid archive never crashes\n");
    char text[4000]; for (int i = 0; i < 4000; i++) text[i] = "lorem ipsum dolor "[i % 18];
    uint32_t cl; uint8_t* comp = lzss_compress((const uint8_t*)text, 4000, &cl);
    uint8_t raw[16] = { 9 };
    ent_t e[3] = { mk("d/", 1, 0, 0, 0), mk("d/a", 0, 1, 4000, cl), mk("d/b", 0, 0, 16, 16) };
    const uint8_t* p[3] = { NULL, comp, raw };
    blob_t good = make_archive(e, p, 3);

    int accepted = 0, refused = 0;
    for (int iter = 0; iter < 20000; iter++) {
        blob_t b = { malloc(good.len), good.len };
        memcpy(b.data, good.data, good.len);
        int flips = 1 + (int)(rnd() % 6);
        for (int k = 0; k < flips; k++) b.data[rnd() % b.len] ^= (uint8_t)(1u << (rnd() % 8));
        if (rnd() % 4 == 0) b.len = rnd() % (b.len + 1);       /* also truncate */
        rec_t r;
        long n = extract(b, &r);
        if (n >= 0) accepted++; else refused++;
        rec_free(&r);
        free(b.data);
    }
    printf("    (%d accepted, %d refused)\n", accepted, refused);
    CHECK(refused > 0, "corruption was detected at least sometimes");
    free(good.data); free(comp);
}

/* ---- 3. an archive made by mkpkg.py --------------------------------- */

static int read_file(const char* path, uint8_t** out, uint32_t* len) {
    FILE* f = fopen(path, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    *out = malloc(n ? (size_t)n : 1); *len = (uint32_t)fread(*out, 1, (size_t)n, f); fclose(f);
    return 1;
}

typedef struct { const char* dir; int files, mismatched, dirs; } cmp_t;
static int cmp_file(void* u, const char* name, const uint8_t* d, uint32_t n) {
    cmp_t* c = u; char path[1024]; snprintf(path, sizeof(path), "%s/%s", c->dir, name);
    uint8_t* want; uint32_t wl;
    c->files++;
    if (!read_file(path, &want, &wl) || wl != n || (n && memcmp(want, d, n) != 0)) {
        c->mismatched++; printf("    mismatch: %s\n", name);
    }
    if (want) free(want);
    return 1;
}
static int cmp_dir(void* u, const char* name) { (void)name; ((cmp_t*)u)->dirs++; return 1; }

static int count_files(const char* dir) {
    int n = 0; DIR* d = opendir(dir); if (!d) return 0; struct dirent* e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[1024]; snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        struct stat st; stat(p, &st);
        n += S_ISDIR(st.st_mode) ? count_files(p) : 1;
    }
    closedir(d); return n;
}

static void test_mkpkg_archive(const char* archive, const char* dir) {
    printf("archive from mkpkg.py: %s\n", archive);
    uint8_t* a; uint32_t len;
    if (!read_file(archive, &a, &len)) { CHECK(0, "cannot read archive"); return; }
    cmp_t c = { dir, 0, 0, 0 };
    dlr_tar_sink sink = { cmp_file, cmp_dir, &c };
    long n = dlr_mpkg_extract_mem(a, len, &sink);
    CHECK(n >= 0, "extraction failed");
    CHECK(c.mismatched == 0, "%d files differ from the source tree", c.mismatched);
    CHECK(c.files == count_files(dir), "%d files extracted, %d in the tree", c.files, count_files(dir));
    free(a);
}

int main(int argc, char** argv) {
    test_lzss_roundtrip();
    test_valid_archive();
    test_hostile();
    test_sink_abort();
    test_fuzz();
    if (argc >= 3) test_mkpkg_archive(argv[1], argv[2]);
    printf("\n%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
