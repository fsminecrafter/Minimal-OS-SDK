#ifndef MINIMALOS_MOSMEDIA_MOTION_H
#define MINIMALOS_MOSMEDIA_MOTION_H

/*
 * The "motion" .vid codec: lossy, built like H.264, for camera footage.
 * mosmedia.h includes this; programs use it through mvid_writer_* and
 * mvid_open / mvid_decode (set `codec = MVID_CODEC_MOTION` on a writer).
 *
 *   - YCbCr 4:2:0 (full range, BT.601), the coded size rounded up to 16;
 *   - 16x16 macroblocks: intra (4x4 luma blocks predicted DC / vertical /
 *     horizontal / TrueMotion, 8x8 colour the same), moved by a quarter
 *     pixel vector (bilinear) plus a residual, or skipped;
 *   - H.264's 4x4 integer transform and quantizer, then a deblocking
 *     filter whose output is the next frame's reference;
 *   - a binary range coder (LZMA's) with CABAC-like contexts.
 *
 * Everything is integer arithmetic, so every decoder (this one and
 * tools/media/mosplayer.py) shows exactly the encoder's reference
 * pixels. docs/media.md describes the bitstream.
 *
 * The encoder decides each macroblock from its left, above and
 * above-right neighbours only (mmc_encode_mb), so macroblocks may be
 * decided on several threads as a wavefront; the bits are then written
 * in order (mmc_write_frame). The output does not depend on the order.
 *
 * Plain C without libc: it runs in programs, in mosWM and on the build PC.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#if defined(__GNUC__)
#define MMC_FN static __attribute__((unused))
#define MMC_DATA static const __attribute__((unused))
#else
#define MMC_FN static
#define MMC_DATA static const
#endif

#define MMC_MAX_QP 51
#define MMC_UBINS 14
#define MMC_MV_MAX 16383
#define MMC_PROB_INIT 1024u

typedef void* (*mmc_alloc_fn)(size_t size);
typedef void (*mmc_free_fn)(void* p);

/* ------------------------------------------------------------ tables */

MMC_DATA uint8_t mmc_zigzag[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
/* H.264's dequantisation (V) and quantisation (MF) factors, by QP % 6 and
 * coefficient class. */
MMC_DATA uint8_t mmc_v[6][3] = {{10, 16, 13}, {11, 18, 14}, {13, 20, 16},
                                {14, 23, 18}, {16, 25, 20}, {18, 29, 23}};
MMC_DATA uint16_t mmc_mf[6][3] = {{13107, 5243, 8066}, {11916, 4660, 7490}, {10082, 4194, 6554},
                                  {9362, 3647, 5825},  {8192, 3355, 5243},  {7282, 2893, 4559}};
MMC_DATA uint8_t mmc_chroma_qp[52] = {
    0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,
    26, 27, 28, 29, 29, 30, 31, 32, 32, 33, 34, 34, 35, 35, 36, 36, 37, 37, 37, 38, 38, 38, 39, 39, 39, 39};
MMC_DATA uint8_t mmc_alpha[52] = {
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   0,   0,   0,   0,   4,   4,   5,   6,   7,   8,   9,   10, 12, 13,
    15, 17, 20, 22, 25, 28, 32, 36, 40, 45, 50, 56, 63, 71, 80, 90, 101, 113, 127, 144, 162, 182, 203, 226, 255, 255};
MMC_DATA uint8_t mmc_beta[52] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  0,  0,  0,  0,  2,  2,  2,  3,  3,  3,  3,  4,  4,  4,
    6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14, 15, 15, 16, 16, 17, 17, 18, 18};
MMC_DATA uint8_t mmc_tc0[52] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 6, 6, 7, 8, 9, 10, 13};
/* Encoder: lambda for mode decisions (0.85 x 2^((QP - 12) / 3)) and for the
 * motion search (its square root), both x16. */
MMC_DATA uint32_t mmc_lambda[52] = {
    1,    1,    1,    2,    2,    3,     3,     4,     5,     7,     9,     11,    14,    17,    22,    27,   34,   43,
    54,   69,   86,   109,  137,  173,   218,   274,   345,   435,   548,   691,   870,   1097,  1382,  1741, 2193, 2763,
    3482, 4387, 5527, 6963, 8773, 11053, 13926, 17546, 22107, 27853, 35092, 44214, 55706, 70185, 88427, 111411};
MMC_DATA uint32_t mmc_lambda_me[52] = {
    4,  4,  5,  5,  6,  7,  7,   8,   9,   10,  12,  13,  15,  17,  19,  21,  23,  26,  30,  33,  37,  42,  47,  53,  59,  66,
    74, 83, 94, 105, 118, 132, 149, 167, 187, 210, 236, 265, 297, 334, 375, 421, 472, 530, 595, 668, 749, 841, 944, 1060, 1189, 1335};

/* Coefficient class of raster position i: 0 both even, 1 both odd, 2 mixed. */
static inline int mmc_class(int i) {
    int r = (i >> 2) & 1, c = i & 1;
    return !r && !c ? 0 : r && c ? 1 : 2;
}

/* Shifts that round towards minus infinity, also for negative numbers. */
static inline int32_t mmc_asr(int32_t v, int s) { return v >= 0 ? v >> s : ~((~v) >> s); }
static inline int mmc_clip(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static inline int mmc_clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline int mmc_abs(int v) { return v < 0 ? -v : v; }
static inline int mmc_min(int a, int b) { return a < b ? a : b; }

/* ------------------------------------------------------- range coder */

typedef struct {
    const uint8_t* data;
    size_t size, pos;
    uint32_t range, code, over;
} mmc_rd_t;

static inline uint32_t mmc_rd_byte(mmc_rd_t* d) {
    if (d->pos < d->size) return d->data[d->pos++];
    d->over++;
    return 0;
}

static inline void mmc_rd_init(mmc_rd_t* d, const uint8_t* data, size_t size) {
    d->data = data; d->size = size; d->pos = 0; d->range = 0xFFFFFFFFu; d->code = 0; d->over = 0;
    for (int i = 0; i < 5; i++) d->code = d->code << 8 | mmc_rd_byte(d);
}

static inline void mmc_rd_norm(mmc_rd_t* d) {
    while (d->range < 0x1000000u) {
        d->range <<= 8;
        d->code = d->code << 8 | mmc_rd_byte(d);
    }
}

static inline int mmc_rd_bit(mmc_rd_t* d, uint16_t* p) {
    uint32_t bound = (d->range >> 11) * *p;
    int b;
    if (d->code < bound) {
        d->range = bound;
        *p = (uint16_t)(*p + ((2048u - *p) >> 5));
        b = 0;
    } else {
        d->code -= bound;
        d->range -= bound;
        *p = (uint16_t)(*p - (*p >> 5));
        b = 1;
    }
    mmc_rd_norm(d);
    return b;
}

static inline int mmc_rd_direct(mmc_rd_t* d) {
    d->range >>= 1;
    int b = 0;
    if (d->code >= d->range) { d->code -= d->range; b = 1; }
    mmc_rd_norm(d);
    return b;
}

static inline uint32_t mmc_rd_eg(mmc_rd_t* d) {
    int n = 0;
    while (mmc_rd_direct(d)) {
        if (++n > 24) { d->over += 1000; return 0; }
    }
    uint32_t v = 1;
    for (int i = 0; i < n; i++) v = v << 1 | (uint32_t)mmc_rd_direct(d);
    return v - 1;
}

static inline uint32_t mmc_rd_uint(mmc_rd_t* d, uint16_t* bins) {
    uint32_t i = 0;
    while (i < MMC_UBINS && mmc_rd_bit(d, &bins[i])) i++;
    return i < MMC_UBINS ? i : MMC_UBINS + mmc_rd_eg(d);
}

typedef struct {
    uint8_t* out;
    size_t cap, pos;
    uint64_t low, cache_size;
    uint32_t range;
    uint8_t cache;
    bool full;
} mmc_re_t;

static inline void mmc_re_init(mmc_re_t* e, uint8_t* out, size_t cap) {
    e->out = out; e->cap = cap; e->pos = 0; e->low = 0; e->cache_size = 1;
    e->range = 0xFFFFFFFFu; e->cache = 0; e->full = false;
}

static inline void mmc_re_emit(mmc_re_t* e, uint8_t b) {
    if (e->pos < e->cap) e->out[e->pos++] = b;
    else e->full = true;
}

static inline void mmc_re_shift(mmc_re_t* e) {
    if ((uint32_t)e->low < 0xFF000000u || (e->low >> 32) != 0) {
        uint8_t carry = (uint8_t)(e->low >> 32), temp = e->cache;
        do {
            mmc_re_emit(e, (uint8_t)(temp + carry));
            temp = 0xFF;
        } while (--e->cache_size != 0);
        e->cache = (uint8_t)(e->low >> 24);
    }
    e->cache_size++;
    e->low = (e->low & 0x00FFFFFFu) << 8;
}

static inline void mmc_re_norm(mmc_re_t* e) {
    while (e->range < 0x1000000u) {
        e->range <<= 8;
        mmc_re_shift(e);
    }
}

static inline void mmc_re_bit(mmc_re_t* e, uint16_t* p, int b) {
    uint32_t bound = (e->range >> 11) * *p;
    if (!b) {
        e->range = bound;
        *p = (uint16_t)(*p + ((2048u - *p) >> 5));
    } else {
        e->low += bound;
        e->range -= bound;
        *p = (uint16_t)(*p - (*p >> 5));
    }
    mmc_re_norm(e);
}

static inline void mmc_re_direct(mmc_re_t* e, int b) {
    e->range >>= 1;
    if (b) e->low += e->range;
    mmc_re_norm(e);
}

static inline void mmc_re_eg(mmc_re_t* e, uint32_t v) {
    uint32_t x = v + 1u;
    int n = 0;
    while ((x >> (n + 1)) != 0) n++;
    for (int i = 0; i < n; i++) mmc_re_direct(e, 1);
    mmc_re_direct(e, 0);
    for (int i = n - 1; i >= 0; i--) mmc_re_direct(e, (int)((x >> i) & 1u));
}

static inline void mmc_re_uint(mmc_re_t* e, uint16_t* bins, uint32_t v) {
    uint32_t i = 0;
    for (; i < v && i < MMC_UBINS; i++) mmc_re_bit(e, &bins[i], 1);
    if (v < MMC_UBINS) mmc_re_bit(e, &bins[v], 0);
    else mmc_re_eg(e, v - MMC_UBINS);
}

static inline void mmc_re_flush(mmc_re_t* e) {
    for (int i = 0; i < 5; i++) mmc_re_shift(e);
}

/* Probabilities, all starting at 1/2 in every frame. */
typedef struct {
    uint16_t skip[3], intra[3], mpm[1], rem[2], cmode[3], mv_zero[2];
    uint16_t mv_bins[2][MMC_UBINS];
    uint16_t cbf[2][3], sig[2][15], last[2][15], gt1[2][5], lev[2][MMC_UBINS];
} mmc_ctx_t;

static inline void mmc_ctx_init(mmc_ctx_t* c) {
    uint16_t* p = (uint16_t*)c;
    for (size_t i = 0; i < sizeof(*c) / sizeof(uint16_t); i++) p[i] = MMC_PROB_INIT;
}

/* ------------------------------------------------------------- state */

typedef struct mmc {
    uint32_t width, height;     /* the picture */
    uint32_t w, h, cw, ch;      /* coded luma and colour planes */
    uint32_t mbw, mbh;          /* macroblocks across and down */
    uint8_t* cur[3];            /* the frame being made (Y, Cb, Cr) */
    uint8_t* ref[3];            /* the last frame, after deblocking */
    uint8_t* mode;              /* per 4x4 luma block: intra mode (0 when moved) */
    uint8_t* nz_y;              /* per 4x4 luma block: has coefficients */
    uint8_t* nz_c[2];           /* per 4x4 colour block */
    uint8_t* mb_intra;
    uint8_t* mb_skip;
    int32_t* mv;                /* per macroblock, x then y, quarter pixels */
    /* encoder only */
    uint8_t* src[3];            /* the frame to code */
    uint8_t* cmode;             /* per macroblock: colour intra mode */
    int32_t* prev_mv;           /* the last frame's vectors (search candidates) */
    int32_t* levels;            /* per macroblock: 24 blocks of 16 levels (raster order) */
    bool encoder;
} mmc_t;

/* One frame's encoding parameters. */
typedef struct {
    bool key, fast;
    int qp, qpc;
    uint32_t lambda, lambda_me;
} mmc_job_t;

MMC_FN void mmc_free(mmc_t* m, mmc_free_fn free_fn) {
    void* all[] = {m->cur[0], m->cur[1], m->cur[2], m->ref[0], m->ref[1], m->ref[2], m->mode, m->nz_y,
                   m->nz_c[0], m->nz_c[1], m->mb_intra, m->mb_skip, m->mv, m->src[0], m->src[1], m->src[2],
                   m->cmode, m->prev_mv, m->levels};
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++)
        if (all[i]) free_fn(all[i]);
    for (size_t i = 0; i < sizeof(*m); i++) ((uint8_t*)m)[i] = 0;
}

static inline void* mmc_zalloc(mmc_alloc_fn alloc, size_t size, bool* ok) {
    uint8_t* p = (uint8_t*)alloc(size ? size : 1);
    if (!p) { *ok = false; return NULL; }
    for (size_t i = 0; i < size; i++) p[i] = 0;
    return p;
}

/* Sets up a decoder (or an encoder) for width x height. */
MMC_FN bool mmc_init(mmc_t* m, uint32_t width, uint32_t height, bool encoder, mmc_alloc_fn alloc) {
    for (size_t i = 0; i < sizeof(*m); i++) ((uint8_t*)m)[i] = 0;
    if (!width || !height || width > 16384u || height > 16384u) return false;
    m->width = width; m->height = height;
    m->w = (width + 15u) & ~15u; m->h = (height + 15u) & ~15u;
    m->cw = m->w / 2u; m->ch = m->h / 2u;
    m->mbw = m->w / 16u; m->mbh = m->h / 16u;
    m->encoder = encoder;
    size_t sy = (size_t)m->w * m->h, sc = (size_t)m->cw * m->ch, mbs = (size_t)m->mbw * m->mbh;
    bool ok = true;
    for (int p = 0; p < 3; p++) {
        m->cur[p] = (uint8_t*)mmc_zalloc(alloc, p ? sc : sy, &ok);
        m->ref[p] = (uint8_t*)mmc_zalloc(alloc, p ? sc : sy, &ok);
    }
    m->mode = (uint8_t*)mmc_zalloc(alloc, sy / 16u, &ok);
    m->nz_y = (uint8_t*)mmc_zalloc(alloc, sy / 16u, &ok);
    m->nz_c[0] = (uint8_t*)mmc_zalloc(alloc, sc / 16u, &ok);
    m->nz_c[1] = (uint8_t*)mmc_zalloc(alloc, sc / 16u, &ok);
    m->mb_intra = (uint8_t*)mmc_zalloc(alloc, mbs, &ok);
    m->mb_skip = (uint8_t*)mmc_zalloc(alloc, mbs, &ok);
    m->mv = (int32_t*)mmc_zalloc(alloc, mbs * 2u * sizeof(int32_t), &ok);
    if (encoder) {
        for (int p = 0; p < 3; p++) m->src[p] = (uint8_t*)mmc_zalloc(alloc, p ? sc : sy, &ok);
        m->cmode = (uint8_t*)mmc_zalloc(alloc, mbs, &ok);
        m->prev_mv = (int32_t*)mmc_zalloc(alloc, mbs * 2u * sizeof(int32_t), &ok);
        m->levels = (int32_t*)mmc_zalloc(alloc, mbs * 24u * 16u * sizeof(int32_t), &ok);
    }
    return ok;
}

/* ------------------------------------------ prediction, reconstruction */

/* Predicts an n x n block (n = 4 or 8) at (x, y) from its neighbours:
 * mode 0 DC, 1 vertical, 2 horizontal, 3 TrueMotion. */
static inline void mmc_intra_pred(const uint8_t* plane, uint32_t stride, int x, int y, int n, int mode,
                                  uint8_t* out) {
    int above[8], left[8];
    bool ha = y > 0, hl = x > 0;
    int sa = 0, sl = 0;
    for (int i = 0; i < n; i++) {
        above[i] = ha ? plane[(size_t)(y - 1) * stride + (size_t)(x + i)] : 128;
        left[i] = hl ? plane[(size_t)(y + i) * stride + (size_t)(x - 1)] : 128;
        sa += above[i];
        sl += left[i];
    }
    int corner = ha && hl ? plane[(size_t)(y - 1) * stride + (size_t)(x - 1)] : 128;
    int shift = n == 4 ? 2 : 3;
    int dc = 128;
    if (ha && hl) dc = (sa + sl + n) >> (shift + 1);
    else if (ha) dc = (sa + n / 2) >> shift;
    else if (hl) dc = (sl + n / 2) >> shift;
    for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++) {
            int v = mode == 0 ? dc : mode == 1 ? above[i] : mode == 2 ? left[j]
                  : mmc_clip(left[j] + above[i] - corner);
            out[j * n + i] = (uint8_t)v;
        }
}

/* Motion compensation: the n x n block at (x, y) moved by (mvx, mvy) in
 * units of 1 / 2^frac pixels, bilinear, edges repeated. */
static inline void mmc_mc(const uint8_t* plane, int pw, int ph, int x, int y, int n, int mvx, int mvy,
                          int frac, uint8_t* out) {
    int one = 1 << frac;
    int ix0 = x + mmc_asr(mvx, frac), iy0 = y + mmc_asr(mvy, frac);
    int fx = mvx & (one - 1), fy = mvy & (one - 1);
    int w00 = (one - fx) * (one - fy), w01 = fx * (one - fy), w10 = (one - fx) * fy, w11 = fx * fy;
    int sh = 2 * frac, rnd = 1 << (sh - 1);
    int xs0[16], xs1[16];
    for (int i = 0; i < n; i++) {
        xs0[i] = mmc_clamp(ix0 + i, 0, pw - 1);
        xs1[i] = mmc_clamp(ix0 + i + 1, 0, pw - 1);
    }
    for (int j = 0; j < n; j++) {
        const uint8_t* r0 = plane + (size_t)mmc_clamp(iy0 + j, 0, ph - 1) * (size_t)pw;
        const uint8_t* r1 = plane + (size_t)mmc_clamp(iy0 + j + 1, 0, ph - 1) * (size_t)pw;
        uint8_t* o = out + j * n;
        if (!fx && !fy) {
            for (int i = 0; i < n; i++) o[i] = r0[xs0[i]];
        } else {
            for (int i = 0; i < n; i++)
                o[i] = (uint8_t)((w00 * r0[xs0[i]] + w01 * r0[xs1[i]] + w10 * r1[xs0[i]] + w11 * r1[xs1[i]] + rnd) >> sh);
        }
    }
}

static inline void mmc_put(uint8_t* plane, uint32_t stride, int x, int y, int n, const uint8_t* block) {
    for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++) plane[(size_t)(y + j) * stride + (size_t)(x + i)] = block[j * n + i];
}

/* Adds the residual of levels `z` (raster order) to the 4x4 block at `at`. */
static inline void mmc_add_residual(const int32_t* z, int qp, uint8_t* at, uint32_t stride) {
    int sh = qp / 6;
    const uint8_t* v = mmc_v[qp % 6];
    int32_t c[16], t[16];
    for (int i = 0; i < 16; i++) {
        int64_t x = (int64_t)z[i] * v[mmc_class(i)] * ((int64_t)1 << sh);
        c[i] = x < -(1 << 20) ? -(1 << 20) : x > (1 << 20) ? (1 << 20) : (int32_t)x;
    }
    for (int i = 0; i < 4; i++) {
        int32_t d0 = c[i * 4], d1 = c[i * 4 + 1], d2 = c[i * 4 + 2], d3 = c[i * 4 + 3];
        int32_t e0 = d0 + d2, e1 = d0 - d2, e2 = mmc_asr(d1, 1) - d3, e3 = d1 + mmc_asr(d3, 1);
        t[i * 4] = e0 + e3; t[i * 4 + 1] = e1 + e2; t[i * 4 + 2] = e1 - e2; t[i * 4 + 3] = e0 - e3;
    }
    for (int i = 0; i < 4; i++) {
        int32_t d0 = t[i], d1 = t[4 + i], d2 = t[8 + i], d3 = t[12 + i];
        int32_t e0 = d0 + d2, e1 = d0 - d2, e2 = mmc_asr(d1, 1) - d3, e3 = d1 + mmc_asr(d3, 1);
        int32_t r[4] = {e0 + e3, e1 + e2, e1 - e2, e0 - e3};
        for (int y = 0; y < 4; y++) {
            uint8_t* q = at + (size_t)y * stride + (size_t)i;
            *q = (uint8_t)mmc_clip(*q + mmc_asr(r[y] + 32, 6));
        }
    }
}

/* The predicted vector: the median of the left, above and above-right
 * (above-left at the right edge) macroblocks' vectors; intra ones count
 * as zero. The top row uses the left one. */
static inline void mmc_mvp(const mmc_t* m, uint32_t mx, uint32_t my, int* px, int* py) {
    uint32_t w = m->mbw;
    int a[2] = {0, 0}, b[2] = {0, 0}, c[2] = {0, 0};
    if (mx > 0) {
        size_t i = (size_t)my * w + mx - 1u;
        if (!m->mb_intra[i]) { a[0] = m->mv[i * 2]; a[1] = m->mv[i * 2 + 1]; }
    }
    if (my == 0) { *px = a[0]; *py = a[1]; return; }
    size_t bi = (size_t)(my - 1u) * w + mx;
    if (!m->mb_intra[bi]) { b[0] = m->mv[bi * 2]; b[1] = m->mv[bi * 2 + 1]; }
    size_t ci = mx + 1u < w ? bi + 1u : mx > 0 ? bi - 1u : bi;
    if (ci != bi && !m->mb_intra[ci]) { c[0] = m->mv[ci * 2]; c[1] = m->mv[ci * 2 + 1]; }
    int r[2];
    for (int k = 0; k < 2; k++) {
        int lo = a[k] < b[k] ? a[k] : b[k], hi = a[k] < b[k] ? b[k] : a[k];
        r[k] = c[k] < lo ? lo : c[k] > hi ? hi : c[k];
    }
    *px = r[0]; *py = r[1];
}

/* The most probable intra mode of 4x4 block (bx, by). */
static inline int mmc_mpm(const mmc_t* m, uint32_t bx, uint32_t by) {
    uint32_t bw = m->w / 4u;
    int a = bx > 0 ? m->mode[(size_t)by * bw + bx - 1u] : 0;
    int b = by > 0 ? m->mode[(size_t)(by - 1u) * bw + bx] : 0;
    return a < b ? a : b;
}

static inline int mmc_nz_ctx(const uint8_t* nz, uint32_t bw, uint32_t bx, uint32_t by) {
    return (bx > 0 && nz[(size_t)by * bw + bx - 1u]) + (by > 0 && nz[(size_t)(by - 1u) * bw + bx]);
}

/* The deblocking filter, after a whole frame: vertical edges, then
 * horizontal ones. `mbsh`: 4x4 blocks per macroblock side, as a shift. */
static inline void mmc_deblock(const mmc_t* m, uint8_t* plane, uint32_t pw, uint32_t ph, const uint8_t* nz,
                               int mbsh, int qp) {
    int alpha = mmc_alpha[qp], beta = mmc_beta[qp];
    if (!alpha) return;
    uint32_t bw = pw / 4u, bh = ph / 4u, mbw = m->mbw;
    for (int dir = 0; dir < 2; dir++) {
        for (uint32_t by = (uint32_t)dir; by < bh; by++) {
            for (uint32_t bx = 1u - (uint32_t)dir; bx < bw; bx++) {
                uint32_t ax = dir ? bx : bx - 1u, ay = dir ? by - 1u : by;
                size_t mba = (size_t)(ay >> mbsh) * mbw + (ax >> mbsh);
                size_t mbb = (size_t)(by >> mbsh) * mbw + (bx >> mbsh);
                int s;
                if (m->mb_intra[mba] || m->mb_intra[mbb]) s = 2;
                else if (nz[(size_t)ay * bw + ax] || nz[(size_t)by * bw + bx]) s = 1;
                else if (mba != mbb && (mmc_abs(m->mv[mba * 2] - m->mv[mbb * 2]) >= 4 ||
                                        mmc_abs(m->mv[mba * 2 + 1] - m->mv[mbb * 2 + 1]) >= 4)) s = 1;
                else continue;
                int tc = mmc_tc0[qp] + (s == 2);
                size_t base = (size_t)by * 4u * pw + (size_t)bx * 4u;
                size_t step = dir ? pw : 1u;
                for (uint32_t k = 0; k < 4; k++) {
                    size_t p = dir ? base + k : base + (size_t)k * pw;
                    int p1 = plane[p - 2 * step], p0 = plane[p - step], q0 = plane[p], q1 = plane[p + step];
                    if (mmc_abs(p0 - q0) < alpha && mmc_abs(p1 - p0) < beta && mmc_abs(q1 - q0) < beta) {
                        int dl = mmc_asr((q0 - p0) * 4 + (p1 - q1) + 4, 3);
                        dl = dl < -tc ? -tc : dl > tc ? tc : dl;
                        plane[p - step] = (uint8_t)mmc_clip(p0 + dl);
                        plane[p] = (uint8_t)mmc_clip(q0 - dl);
                    }
                }
            }
        }
    }
}

/* Deblocks the frame just made and makes it the reference. */
static inline void mmc_finish_frame(mmc_t* m, int qp) {
    int qpc = mmc_chroma_qp[qp];
    mmc_deblock(m, m->cur[0], m->w, m->h, m->nz_y, 2, qp);
    mmc_deblock(m, m->cur[1], m->cw, m->ch, m->nz_c[0], 1, qpc);
    mmc_deblock(m, m->cur[2], m->cw, m->ch, m->nz_c[1], 1, qpc);
    for (int p = 0; p < 3; p++) {
        uint8_t* t = m->cur[p];
        m->cur[p] = m->ref[p];
        m->ref[p] = t;
    }
}

/* The last frame (the reference) as 0xFFRRGGBB pixels (stride in pixels). */
MMC_FN void mmc_to_argb(const mmc_t* m, uint32_t* out, uint32_t width, uint32_t height, uint32_t stride) {
    const uint8_t *Y = m->ref[0], *U = m->ref[1], *V = m->ref[2];
    int cw = (int)m->cw, ch = (int)m->ch;
    if (width > m->width) width = m->width;
    if (height > m->height) height = m->height;
    for (uint32_t y = 0; y < height; y++) {
        int cy = (int)(y >> 1);
        int cy2 = mmc_clamp(y & 1u ? cy + 1 : cy - 1, 0, ch - 1);
        const uint8_t *u0 = U + (size_t)cy * cw, *u1 = U + (size_t)cy2 * cw;
        const uint8_t *v0 = V + (size_t)cy * cw, *v1 = V + (size_t)cy2 * cw;
        const uint8_t* row = Y + (size_t)y * m->w;
        uint32_t* o = out + (size_t)y * stride;
        for (uint32_t x = 0; x < width; x++) {
            int cx = (int)(x >> 1);
            int cx2 = mmc_clamp(x & 1u ? cx + 1 : cx - 1, 0, cw - 1);
            int u = (9 * u0[cx] + 3 * u0[cx2] + 3 * u1[cx] + u1[cx2] + 8) >> 4;
            int v = (9 * v0[cx] + 3 * v0[cx2] + 3 * v1[cx] + v1[cx2] + 8) >> 4;
            int lum = row[x], cb = u - 128, cr = v - 128;
            int r = mmc_clip(lum + mmc_asr(359 * cr + 128, 8));
            int g = mmc_clip(lum - mmc_asr(88 * cb + 183 * cr + 128, 8));
            int b = mmc_clip(lum + mmc_asr(454 * cb + 128, 8));
            o[x] = 0xFF000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
        }
    }
}

/* ----------------------------------------------------------- decoding */

/* Reads one block's levels into z (raster order). Returns false when the
 * block has none (z untouched). */
static inline bool mmc_rd_coeffs(mmc_rd_t* d, mmc_ctx_t* c, int typ, int ctx, int32_t* z) {
    if (!mmc_rd_bit(d, &c->cbf[typ][ctx])) return false;
    for (int i = 0; i < 16; i++) z[i] = 0;
    int pos[16], count = 0, last = 15;
    for (int i = 0; i < 15; i++) {
        if (mmc_rd_bit(d, &c->sig[typ][i])) {
            pos[count++] = i;
            if (mmc_rd_bit(d, &c->last[typ][i])) { last = i; break; }
        }
    }
    if (last == 15) pos[count++] = 15;
    int gt1 = 0, eq1 = 0;
    for (int k = count - 1; k >= 0; k--) {
        int inc = gt1 ? 0 : mmc_min(eq1 + 1, 4);
        uint32_t a = 1;
        if (mmc_rd_bit(d, &c->gt1[typ][inc])) {
            a = 2u + mmc_rd_uint(d, c->lev[typ]);
            if (a > 65535u) a = 65535u;
            gt1++;
        } else {
            eq1++;
        }
        z[mmc_zigzag[pos[k]]] = mmc_rd_direct(d) ? -(int32_t)a : (int32_t)a;
    }
    return true;
}

static inline int mmc_rd_mvd(mmc_rd_t* d, mmc_ctx_t* c, int k) {
    if (!mmc_rd_bit(d, &c->mv_zero[k])) return 0;
    int s = mmc_rd_direct(d);
    uint32_t a = 1u + mmc_rd_uint(d, c->mv_bins[k]);
    if (a > 2u * MMC_MV_MAX) a = 2u * MMC_MV_MAX;
    return s ? -(int)a : (int)a;
}

static inline void mmc_rd_chroma(mmc_t* m, mmc_rd_t* d, mmc_ctx_t* c, uint32_t mx, uint32_t my, int qp) {
    int qpc = mmc_chroma_qp[qp];
    uint32_t cw = m->cw, cbw = m->w / 8u;
    int32_t z[16];
    for (int pl = 0; pl < 2; pl++) {
        uint8_t* plane = m->cur[1 + pl];
        uint8_t* nz = m->nz_c[pl];
        for (uint32_t k = 0; k < 4; k++) {
            uint32_t bx = mx * 2u + (k & 1u), by = my * 2u + (k >> 1);
            int ctx = mmc_nz_ctx(nz, cbw, bx, by);
            bool coded = mmc_rd_coeffs(d, c, 1, ctx, z);
            nz[(size_t)by * cbw + bx] = coded;
            if (coded) mmc_add_residual(z, qpc, plane + (size_t)by * 4u * cw + bx * 4u, cw);
        }
    }
}

/* Decodes one frame's range-coded data; the result becomes the reference
 * (mmc_to_argb shows it). Returns false on damaged data. */
MMC_FN bool mmc_decode_frame(mmc_t* m, const uint8_t* data, size_t size, bool key, int qp) {
    if (qp < 0 || qp > MMC_MAX_QP) return false;
    mmc_rd_t d;
    mmc_rd_init(&d, data, size);
    mmc_ctx_t c;
    mmc_ctx_init(&c);
    uint32_t w = m->w, cw = m->cw, bw = m->w / 4u, cbw = m->w / 8u;
    uint8_t *Y = m->cur[0], *U = m->cur[1], *V = m->cur[2];
    uint8_t blk[256];
    int32_t z[16];
    for (uint32_t my = 0; my < m->mbh; my++) {
        for (uint32_t mx = 0; mx < m->mbw; mx++) {
            size_t mb = (size_t)my * m->mbw + mx;
            int nskip = (mx > 0 && m->mb_skip[mb - 1]) + (my > 0 && m->mb_skip[mb - m->mbw]);
            int nintra = (mx > 0 && m->mb_intra[mb - 1]) + (my > 0 && m->mb_intra[mb - m->mbw]);
            bool skip = !key && mmc_rd_bit(&d, &c.skip[nskip]);
            bool intra = key || (!skip && mmc_rd_bit(&d, &c.intra[nintra]));
            m->mb_skip[mb] = skip;
            m->mb_intra[mb] = intra;
            if (intra) {
                m->mv[mb * 2] = m->mv[mb * 2 + 1] = 0;
                int modes[16];
                for (uint32_t k = 0; k < 16; k++) {
                    uint32_t bx = mx * 4u + (k & 3u), by = my * 4u + (k >> 2);
                    int mpm = mmc_mpm(m, bx, by), mode = mpm;
                    if (mmc_rd_bit(&d, &c.mpm[0])) {
                        int rem = mmc_rd_bit(&d, &c.rem[0]) ? 1 + mmc_rd_bit(&d, &c.rem[1]) : 0;
                        mode = rem < mpm ? rem : rem + 1;
                    }
                    m->mode[(size_t)by * bw + bx] = (uint8_t)mode;
                    modes[k] = mode;
                }
                int b0 = mmc_rd_bit(&d, &c.cmode[0]);
                int cmode = b0 * 2 + mmc_rd_bit(&d, &c.cmode[1 + b0]);
                for (uint32_t k = 0; k < 16; k++) {
                    uint32_t bx = mx * 4u + (k & 3u), by = my * 4u + (k >> 2);
                    mmc_intra_pred(Y, w, (int)bx * 4, (int)by * 4, 4, modes[k], blk);
                    mmc_put(Y, w, (int)bx * 4, (int)by * 4, 4, blk);
                    int ctx = mmc_nz_ctx(m->nz_y, bw, bx, by);
                    bool coded = mmc_rd_coeffs(&d, &c, 0, ctx, z);
                    m->nz_y[(size_t)by * bw + bx] = coded;
                    if (coded) mmc_add_residual(z, qp, Y + (size_t)by * 4u * w + bx * 4u, w);
                }
                for (int p = 1; p < 3; p++) {
                    uint8_t* plane = p == 1 ? U : V;
                    mmc_intra_pred(plane, cw, (int)mx * 8, (int)my * 8, 8, cmode, blk);
                    mmc_put(plane, cw, (int)mx * 8, (int)my * 8, 8, blk);
                }
                mmc_rd_chroma(m, &d, &c, mx, my, qp);
            } else {
                int px, py;
                mmc_mvp(m, mx, my, &px, &py);
                if (!skip) {
                    px = mmc_clamp(px + mmc_rd_mvd(&d, &c, 0), -MMC_MV_MAX, MMC_MV_MAX);
                    py = mmc_clamp(py + mmc_rd_mvd(&d, &c, 1), -MMC_MV_MAX, MMC_MV_MAX);
                }
                m->mv[mb * 2] = px;
                m->mv[mb * 2 + 1] = py;
                mmc_mc(m->ref[0], (int)w, (int)m->h, (int)mx * 16, (int)my * 16, 16, px, py, 2, blk);
                mmc_put(Y, w, (int)mx * 16, (int)my * 16, 16, blk);
                mmc_mc(m->ref[1], (int)cw, (int)m->ch, (int)mx * 8, (int)my * 8, 8, px, py, 3, blk);
                mmc_put(U, cw, (int)mx * 8, (int)my * 8, 8, blk);
                mmc_mc(m->ref[2], (int)cw, (int)m->ch, (int)mx * 8, (int)my * 8, 8, px, py, 3, blk);
                mmc_put(V, cw, (int)mx * 8, (int)my * 8, 8, blk);
                for (uint32_t k = 0; k < 16; k++) {
                    size_t i = (size_t)(my * 4u + (k >> 2)) * bw + mx * 4u + (k & 3u);
                    m->mode[i] = 0;
                    m->nz_y[i] = 0;
                }
                if (skip) {
                    for (uint32_t k = 0; k < 4; k++) {
                        size_t i = (size_t)(my * 2u + (k >> 1)) * cbw + mx * 2u + (k & 1u);
                        m->nz_c[0][i] = m->nz_c[1][i] = 0;
                    }
                } else {
                    for (uint32_t k = 0; k < 16; k++) {
                        uint32_t bx = mx * 4u + (k & 3u), by = my * 4u + (k >> 2);
                        int ctx = mmc_nz_ctx(m->nz_y, bw, bx, by);
                        bool coded = mmc_rd_coeffs(&d, &c, 0, ctx, z);
                        m->nz_y[(size_t)by * bw + bx] = coded;
                        if (coded) mmc_add_residual(z, qp, Y + (size_t)by * 4u * w + bx * 4u, w);
                    }
                    mmc_rd_chroma(m, &d, &c, mx, my, qp);
                }
            }
            if (d.over > 16u) return false;
        }
    }
    mmc_finish_frame(m, qp);
    return true;
}

/* ----------------------------------------------------------- encoding */

/* Estimated costs in quarter bits (the coder adapts; these only steer
 * decisions, so they need not be exact). */
static inline uint32_t mmc_ubits(uint32_t v) {
    if (v < MMC_UBINS) return 3u * (v + 1u);
    uint32_t x = v - MMC_UBINS + 1u, n = 0;
    while (x >> (n + 1)) n++;
    return 3u * MMC_UBINS + 4u * (2u * n + 1u);
}

static inline uint32_t mmc_coeff_bits(const int32_t* lev) {
    int last = -1;
    for (int i = 15; i >= 0; i--)
        if (lev[mmc_zigzag[i]]) { last = i; break; }
    if (last < 0) return 3;
    uint32_t bits = 4;
    for (int i = 0; i <= last && i < 15; i++) {
        bits += 3;
        if (lev[mmc_zigzag[i]]) bits += 2;
    }
    for (int i = 0; i <= last; i++) {
        int32_t a = lev[mmc_zigzag[i]];
        if (!a) continue;
        if (a < 0) a = -a;
        bits += 4u + 3u;
        if (a > 1) bits += mmc_ubits((uint32_t)a - 2u);
    }
    return bits;
}

static inline uint32_t mmc_mvd_bits(int d) {
    if (!d) return 2;
    return 8u + mmc_ubits((uint32_t)mmc_abs(d) - 1u);
}

/* H.264's forward 4x4 transform and quantizer: residual (raster) -> levels. */
static inline bool mmc_quant(const int* res, int qp, bool intra, int32_t* lev) {
    int t[16], wv[16];
    for (int j = 0; j < 4; j++) {
        int x0 = res[j * 4], x1 = res[j * 4 + 1], x2 = res[j * 4 + 2], x3 = res[j * 4 + 3];
        int s03 = x0 + x3, d03 = x0 - x3, s12 = x1 + x2, d12 = x1 - x2;
        t[j * 4] = s03 + s12; t[j * 4 + 1] = 2 * d03 + d12;
        t[j * 4 + 2] = s03 - s12; t[j * 4 + 3] = d03 - 2 * d12;
    }
    for (int i = 0; i < 4; i++) {
        int x0 = t[i], x1 = t[4 + i], x2 = t[8 + i], x3 = t[12 + i];
        int s03 = x0 + x3, d03 = x0 - x3, s12 = x1 + x2, d12 = x1 - x2;
        wv[i] = s03 + s12; wv[4 + i] = 2 * d03 + d12;
        wv[8 + i] = s03 - s12; wv[12 + i] = d03 - 2 * d12;
    }
    int qbits = 15 + qp / 6;
    int64_t f = ((int64_t)1 << qbits) / (intra ? 3 : 6);
    const uint16_t* mf = mmc_mf[qp % 6];
    bool any = false;
    for (int i = 0; i < 16; i++) {
        int a = wv[i] < 0 ? -wv[i] : wv[i];
        int64_t l = ((int64_t)a * mf[mmc_class(i)] + f) >> qbits;
        if (l > 65535) l = 65535;
        lev[i] = wv[i] < 0 ? -(int32_t)l : (int32_t)l;
        any |= l != 0;
    }
    return any;
}

/* Codes one 4x4 block of `src` predicted by `pred` (4x4, contiguous):
 * picks levels or none, by distortion + lambda x bits. Writes the
 * reconstruction to `rec` (4x4) and the levels to `lev`; returns the cost
 * (ssd x 64 + lambda x quarter bits). */
static inline uint64_t mmc_code_block(const uint8_t* src, uint32_t sstride, const uint8_t* pred, int qp,
                                      bool intra, uint32_t lambda, int32_t* lev, uint8_t* rec, bool* nz) {
    int res[16];
    uint64_t ssd0 = 0;
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++) {
            int d = src[(size_t)j * sstride + (size_t)i] - pred[j * 4 + i];
            res[j * 4 + i] = d;
            ssd0 += (uint64_t)(d * d);
        }
    uint64_t cost0 = ssd0 * 64u + (uint64_t)lambda * 3u;
    if (mmc_quant(res, qp, intra, lev)) {
        uint8_t r[16];
        for (int i = 0; i < 16; i++) r[i] = pred[i];
        mmc_add_residual(lev, qp, r, 4);
        uint64_t ssd = 0;
        for (int j = 0; j < 4; j++)
            for (int i = 0; i < 4; i++) {
                int d = src[(size_t)j * sstride + (size_t)i] - r[j * 4 + i];
                ssd += (uint64_t)(d * d);
            }
        uint64_t cost = ssd * 64u + (uint64_t)lambda * mmc_coeff_bits(lev);
        if (cost < cost0) {
            for (int i = 0; i < 16; i++) rec[i] = r[i];
            *nz = true;
            return cost;
        }
    }
    for (int i = 0; i < 16; i++) { lev[i] = 0; rec[i] = pred[i]; }
    *nz = false;
    return cost0;
}

/* Codes the colour of a macroblock predicted by pu / pv (8x8 each).
 * Writes the reconstruction to ru / rv, levels to lev (8 blocks) and
 * flags to nz (8). */
static inline uint64_t mmc_code_chroma(const mmc_t* m, uint32_t mx, uint32_t my, const uint8_t* pu,
                                       const uint8_t* pv, int qpc, bool intra, uint32_t lambda, int32_t* lev,
                                       uint8_t* ru, uint8_t* rv, bool* nz) {
    uint64_t cost = 0;
    uint32_t cw = m->cw;
    for (int pl = 0; pl < 2; pl++) {
        const uint8_t* pred = pl ? pv : pu;
        uint8_t* rec = pl ? rv : ru;
        const uint8_t* src = m->src[1 + pl] + (size_t)my * 8u * cw + mx * 8u;
        for (int k = 0; k < 4; k++) {
            int ox = (k & 1) * 4, oy = (k >> 1) * 4;
            uint8_t p4[16], r4[16];
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++) p4[j * 4 + i] = pred[(oy + j) * 8 + ox + i];
            cost += mmc_code_block(src + (size_t)oy * cw + (size_t)ox, cw, p4, qpc, intra, lambda,
                                   lev + (pl * 4 + k) * 16, r4, &nz[pl * 4 + k]);
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++) rec[(oy + j) * 8 + ox + i] = r4[j * 4 + i];
        }
    }
    return cost;
}

/* Turns a frame (0xAARRGGBB, stride in pixels) into the encoder's source
 * planes, repeating the last column and row up to the coded size. */
MMC_FN void mmc_load_frame(mmc_t* m, const uint32_t* px, uint32_t stride) {
    uint32_t W = m->width, H = m->height;
    for (uint32_t y = 0; y < m->h; y++) {
        const uint32_t* row = px + (size_t)(y < H ? y : H - 1u) * stride;
        uint8_t* o = m->src[0] + (size_t)y * m->w;
        for (uint32_t x = 0; x < m->w; x++) {
            uint32_t c = row[x < W ? x : W - 1u];
            int r = (int)(c >> 16 & 255u), g = (int)(c >> 8 & 255u), b = (int)(c & 255u);
            o[x] = (uint8_t)((77 * r + 150 * g + 29 * b + 128) >> 8);
        }
    }
    for (uint32_t y = 0; y < m->ch; y++)
        for (uint32_t x = 0; x < m->cw; x++) {
            int r = 0, g = 0, b = 0;
            for (uint32_t k = 0; k < 4; k++) {
                uint32_t sx = x * 2u + (k & 1u), sy = y * 2u + (k >> 1);
                uint32_t c = px[(size_t)(sy < H ? sy : H - 1u) * stride + (sx < W ? sx : W - 1u)];
                r += (int)(c >> 16 & 255u); g += (int)(c >> 8 & 255u); b += (int)(c & 255u);
            }
            m->src[1][(size_t)y * m->cw + x] = (uint8_t)mmc_clip(mmc_asr(-43 * r - 85 * g + 128 * b + 512, 10) + 128);
            m->src[2][(size_t)y * m->cw + x] = (uint8_t)mmc_clip(mmc_asr(128 * r - 107 * g - 21 * b + 512, 10) + 128);
        }
}

/* Starts coding a frame: keyframe or not, at `qp`. */
MMC_FN void mmc_begin_frame(mmc_t* m, mmc_job_t* job, bool key, int qp, bool fast) {
    job->key = key;
    job->fast = fast;
    job->qp = qp < 0 ? 0 : qp > MMC_MAX_QP ? MMC_MAX_QP : qp;
    job->qpc = mmc_chroma_qp[job->qp];
    job->lambda = mmc_lambda[job->qp];
    job->lambda_me = mmc_lambda_me[job->qp];
    size_t mbs = (size_t)m->mbw * m->mbh;
    for (size_t i = 0; i < mbs * 2u; i++) m->prev_mv[i] = m->mv[i];
}

/* Codes the macroblock as intra straight into the frame; returns its cost. */
static inline uint64_t mmc_try_intra(mmc_t* m, const mmc_job_t* job, uint32_t mx, uint32_t my) {
    uint32_t w = m->w, bw = m->w / 4u, cw = m->cw, cbw = m->w / 8u;
    size_t mb = (size_t)my * m->mbw + mx;
    int32_t* levels = m->levels + mb * 384u;
    uint64_t total = 0;
    uint8_t pred[16], rec[16], best_rec[16];
    int32_t lev[16];
    for (uint32_t k = 0; k < 16; k++) {
        uint32_t bx = mx * 4u + (k & 3u), by = my * 4u + (k >> 2);
        int mpm = mmc_mpm(m, bx, by);
        const uint8_t* src = m->src[0] + (size_t)by * 4u * w + bx * 4u;
        uint64_t best = UINT64_MAX;
        int best_mode = 0;
        bool best_nz = false;
        for (int mode = 0; mode < 4; mode++) {
            mmc_intra_pred(m->cur[0], w, (int)bx * 4, (int)by * 4, 4, mode, pred);
            bool nz;
            uint64_t cost = mmc_code_block(src, w, pred, job->qp, true, job->lambda, lev, rec, &nz);
            cost += (uint64_t)job->lambda * (mode == mpm ? 2u : 10u);
            if (cost < best) {
                best = cost;
                best_mode = mode;
                best_nz = nz;
                for (int i = 0; i < 16; i++) { best_rec[i] = rec[i]; levels[k * 16u + (uint32_t)i] = lev[i]; }
            }
        }
        mmc_put(m->cur[0], w, (int)bx * 4, (int)by * 4, 4, best_rec);
        m->mode[(size_t)by * bw + bx] = (uint8_t)best_mode;
        m->nz_y[(size_t)by * bw + bx] = best_nz;
        total += best;
    }
    /* colour: the mode with the smallest difference, then its residual */
    uint8_t pu[64], pv[64], ru[64], rv[64];
    uint64_t best = UINT64_MAX;
    int cmode = 0;
    for (int mode = 0; mode < 4; mode++) {
        mmc_intra_pred(m->cur[1], cw, (int)mx * 8, (int)my * 8, 8, mode, pu);
        mmc_intra_pred(m->cur[2], cw, (int)mx * 8, (int)my * 8, 8, mode, pv);
        uint64_t sad = 0;
        for (int j = 0; j < 8; j++)
            for (int i = 0; i < 8; i++) {
                size_t at = (size_t)(my * 8u + (uint32_t)j) * cw + mx * 8u + (uint32_t)i;
                sad += (uint64_t)mmc_abs(m->src[1][at] - pu[j * 8 + i]) + (uint64_t)mmc_abs(m->src[2][at] - pv[j * 8 + i]);
            }
        uint64_t cost = sad * 64u + (uint64_t)job->lambda_me * (mode ? 8u : 4u);
        if (cost < best) { best = cost; cmode = mode; }
    }
    mmc_intra_pred(m->cur[1], cw, (int)mx * 8, (int)my * 8, 8, cmode, pu);
    mmc_intra_pred(m->cur[2], cw, (int)mx * 8, (int)my * 8, 8, cmode, pv);
    bool nz[8];
    total += mmc_code_chroma(m, mx, my, pu, pv, job->qpc, true, job->lambda, levels + 256, ru, rv, nz);
    total += (uint64_t)job->lambda * 8u;
    mmc_put(m->cur[1], cw, (int)mx * 8, (int)my * 8, 8, ru);
    mmc_put(m->cur[2], cw, (int)mx * 8, (int)my * 8, 8, rv);
    for (uint32_t k = 0; k < 4; k++) {
        size_t i = (size_t)(my * 2u + (k >> 1)) * cbw + mx * 2u + (k & 1u);
        m->nz_c[0][i] = nz[k];
        m->nz_c[1][i] = nz[4 + k];
    }
    m->cmode[mb] = (uint8_t)cmode;
    return total;
}

/* Luma SAD of the macroblock moved by (mvx, mvy), as a search cost. */
static inline uint64_t mmc_me_cost(const mmc_t* m, const mmc_job_t* job, uint32_t mx, uint32_t my, int mvx,
                                   int mvy, int px, int py) {
    const uint8_t* src = m->src[0] + (size_t)my * 16u * m->w + mx * 16u;
    int x0 = (int)mx * 16 + mmc_asr(mvx, 2), y0 = (int)my * 16 + mmc_asr(mvy, 2);
    uint32_t sad = 0;
    if (!(mvx & 3) && !(mvy & 3) && x0 >= 0 && y0 >= 0 && x0 + 16 <= (int)m->w && y0 + 16 <= (int)m->h) {
        /* a whole-pixel vector inside the picture: no interpolation */
        const uint8_t* ref = m->ref[0] + (size_t)y0 * m->w + (size_t)x0;
        for (int j = 0; j < 16; j++) {
            const uint8_t *a = src + (size_t)j * m->w, *b = ref + (size_t)j * m->w;
            for (int i = 0; i < 16; i++) sad += (uint32_t)(a[i] > b[i] ? a[i] - b[i] : b[i] - a[i]);
        }
    } else if (x0 >= 0 && y0 >= 0 && x0 + 17 <= (int)m->w && y0 + 17 <= (int)m->h) {
        /* inside the picture: mmc_mc's bilinear without the edge clamps */
        int fx = mvx & 3, fy = mvy & 3;
        uint32_t w00 = (uint32_t)((4 - fx) * (4 - fy)), w01 = (uint32_t)(fx * (4 - fy));
        uint32_t w10 = (uint32_t)((4 - fx) * fy), w11 = (uint32_t)(fx * fy);
        const uint8_t* ref = m->ref[0] + (size_t)y0 * m->w + (size_t)x0;
        for (int j = 0; j < 16; j++) {
            const uint8_t *a = src + (size_t)j * m->w, *r0 = ref + (size_t)j * m->w, *r1 = r0 + m->w;
            for (int i = 0; i < 16; i++) {
                uint32_t p = (w00 * r0[i] + w01 * r0[i + 1] + w10 * r1[i] + w11 * r1[i + 1] + 8u) >> 4;
                sad += a[i] > p ? a[i] - p : p - a[i];
            }
        }
    } else {
        uint8_t blk[256];
        mmc_mc(m->ref[0], (int)m->w, (int)m->h, (int)mx * 16, (int)my * 16, 16, mvx, mvy, 2, blk);
        for (int j = 0; j < 16; j++) {
            const uint8_t *a = src + (size_t)j * m->w, *b = blk + j * 16;
            for (int i = 0; i < 16; i++) sad += (uint32_t)(a[i] > b[i] ? a[i] - b[i] : b[i] - a[i]);
        }
    }
    return sad * 64u + (uint64_t)job->lambda_me * (mmc_mvd_bits(mvx - px) + mmc_mvd_bits(mvy - py));
}

/* Decides and reconstructs macroblock (mx, my). It reads only this
 * macroblock and its left, above, above-left and above-right neighbours,
 * so a wavefront of threads may call it (a row two macroblocks behind the
 * one above). */
MMC_FN void mmc_encode_mb(mmc_t* m, const mmc_job_t* job, uint32_t mx, uint32_t my) {
    size_t mb = (size_t)my * m->mbw + mx;
    uint32_t w = m->w, bw = m->w / 4u, cw = m->cw, cbw = m->w / 8u;
    if (job->key) {
        mmc_try_intra(m, job, mx, my);
        m->mb_intra[mb] = 1;
        m->mb_skip[mb] = 0;
        m->mv[mb * 2] = m->mv[mb * 2 + 1] = 0;
        return;
    }
    int px, py;
    mmc_mvp(m, mx, my, &px, &py);
    /* Motion search: candidates, then a shrinking square pattern. Vectors
     * stay within 64 pixels of the picture. */
    int lox = -((int)mx * 16 + 64) * 4, hix = ((int)w - (int)mx * 16 + 48) * 4;
    int loy = -((int)my * 16 + 64) * 4, hiy = ((int)m->h - (int)my * 16 + 48) * 4;
    if (lox < -MMC_MV_MAX) lox = -MMC_MV_MAX;
    if (loy < -MMC_MV_MAX) loy = -MMC_MV_MAX;
    if (hix > MMC_MV_MAX) hix = MMC_MV_MAX;
    if (hiy > MMC_MV_MAX) hiy = MMC_MV_MAX;
    int cand[6][2];
    int nc = 0;
    cand[nc][0] = 0; cand[nc][1] = 0; nc++;
    cand[nc][0] = px; cand[nc][1] = py; nc++;
    if (mx > 0 && !m->mb_intra[mb - 1]) { cand[nc][0] = m->mv[(mb - 1) * 2]; cand[nc][1] = m->mv[(mb - 1) * 2 + 1]; nc++; }
    if (my > 0) {
        size_t a = mb - m->mbw;
        if (!m->mb_intra[a]) { cand[nc][0] = m->mv[a * 2]; cand[nc][1] = m->mv[a * 2 + 1]; nc++; }
        if (mx + 1u < m->mbw && !m->mb_intra[a + 1]) {
            cand[nc][0] = m->mv[(a + 1) * 2]; cand[nc][1] = m->mv[(a + 1) * 2 + 1]; nc++;
        }
    }
    cand[nc][0] = m->prev_mv[mb * 2]; cand[nc][1] = m->prev_mv[mb * 2 + 1]; nc++;
    int bx = 0, by = 0;
    uint64_t best = UINT64_MAX;
    for (int i = 0; i < nc; i++) {
        int cx = mmc_clamp(cand[i][0], lox, hix), cy = mmc_clamp(cand[i][1], loy, hiy);
        bool seen = false;
        for (int k = 0; k < i && !seen; k++) seen = cand[k][0] == cand[i][0] && cand[k][1] == cand[i][1];
        if (seen) continue;
        uint64_t cost = mmc_me_cost(m, job, mx, my, cx, cy, px, py);
        if (cost < best) { best = cost; bx = cx; by = cy; }
    }
    static const int steps[5] = {16, 8, 4, 2, 1};
    for (int s = 0; s < 5; s++) {
        if (job->fast && steps[s] == 1) break;
        /* big steps may walk a while; the fine ones settle at once */
        int iters = steps[s] >= 4 ? 8 : 2;
        for (int iter = 0; iter < iters; iter++) {
            int nbx = bx, nby = by;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    if (!dx && !dy) continue;
                    if (steps[s] >= 4 && dx && dy) continue;   /* a diamond while the steps are big */
                    int cx = bx + dx * steps[s], cy = by + dy * steps[s];
                    if (cx < lox || cx > hix || cy < loy || cy > hiy) continue;
                    uint64_t cost = mmc_me_cost(m, job, mx, my, cx, cy, px, py);
                    if (cost < best) { best = cost; nbx = cx; nby = cy; }
                }
            if (nbx == bx && nby == by) break;
            bx = nbx; by = nby;
        }
    }
    uint64_t sad_best = best;

    /* skip: the predicted vector, no residual */
    uint8_t sy[256], su[64], sv[64];
    mmc_mc(m->ref[0], (int)w, (int)m->h, (int)mx * 16, (int)my * 16, 16, px, py, 2, sy);
    mmc_mc(m->ref[1], (int)cw, (int)m->ch, (int)mx * 8, (int)my * 8, 8, px, py, 3, su);
    mmc_mc(m->ref[2], (int)cw, (int)m->ch, (int)mx * 8, (int)my * 8, 8, px, py, 3, sv);
    uint64_t ssd = 0;
    for (int j = 0; j < 16; j++)
        for (int i = 0; i < 16; i++) {
            int d = m->src[0][(size_t)(my * 16u + (uint32_t)j) * w + mx * 16u + (uint32_t)i] - sy[j * 16 + i];
            ssd += (uint64_t)(d * d);
        }
    for (int j = 0; j < 8; j++)
        for (int i = 0; i < 8; i++) {
            size_t at = (size_t)(my * 8u + (uint32_t)j) * cw + mx * 8u + (uint32_t)i;
            int du = m->src[1][at] - su[j * 8 + i], dv = m->src[2][at] - sv[j * 8 + i];
            ssd += (uint64_t)(du * du + dv * dv);
        }
    uint64_t cost_skip = ssd * 64u + (uint64_t)job->lambda * 2u;
    /* A skip that is about as good as the residual could make it: done. */
    bool early_skip = ssd * 64u <= (uint64_t)job->lambda * 8u;

    /* moved: the found vector plus a residual */
    uint8_t py_[256], pu[64], pv[64], ry[256], ru[64], rv[64];
    int32_t lev[384];
    bool nzy[16], nzc[8];
    mmc_mc(m->ref[0], (int)w, (int)m->h, (int)mx * 16, (int)my * 16, 16, bx, by, 2, py_);
    mmc_mc(m->ref[1], (int)cw, (int)m->ch, (int)mx * 8, (int)my * 8, 8, bx, by, 3, pu);
    mmc_mc(m->ref[2], (int)cw, (int)m->ch, (int)mx * 8, (int)my * 8, 8, bx, by, 3, pv);
    uint64_t cost_inter = (uint64_t)job->lambda * (8u + mmc_mvd_bits(bx - px) + mmc_mvd_bits(by - py));
    bool any = false;
    for (uint32_t k = 0; k < 16 && !early_skip; k++) {
        uint32_t ox = (k & 3u) * 4u, oy = (k >> 2) * 4u;
        uint8_t p4[16], r4[16];
        for (uint32_t j = 0; j < 4; j++)
            for (uint32_t i = 0; i < 4; i++) p4[j * 4 + i] = py_[(oy + j) * 16 + ox + i];
        const uint8_t* src = m->src[0] + (size_t)(my * 16u + oy) * w + mx * 16u + ox;
        cost_inter += mmc_code_block(src, w, p4, job->qp, false, job->lambda, lev + k * 16u, r4, &nzy[k]);
        any |= nzy[k];
        for (uint32_t j = 0; j < 4; j++)
            for (uint32_t i = 0; i < 4; i++) ry[(oy + j) * 16 + ox + i] = r4[j * 4 + i];
    }
    if (!early_skip) {
        cost_inter += mmc_code_chroma(m, mx, my, pu, pv, job->qpc, false, job->lambda, lev + 256, ru, rv, nzc);
        for (int k = 0; k < 8; k++) any |= nzc[k];
    }
    if (early_skip || (!any && bx == px && by == py)) cost_inter = UINT64_MAX;   /* that is a skip */

    /* intra (written straight into the frame; undone below if it loses) */
    /* intra only where moving the last frame predicts badly: more than
     * an average difference of 3 (6 with --fast) a pixel */
    uint64_t cost_intra = UINT64_MAX;
    uint64_t sad_only = sad_best - (uint64_t)job->lambda_me * (mmc_mvd_bits(bx - px) + mmc_mvd_bits(by - py));
    if (!early_skip && sad_only > (uint64_t)256 * (job->fast ? 6u : 3u) * 64u)
        cost_intra = mmc_try_intra(m, job, mx, my) + (uint64_t)job->lambda * 8u;

    if (cost_intra < cost_skip && cost_intra < cost_inter) {
        m->mb_intra[mb] = 1;
        m->mb_skip[mb] = 0;
        m->mv[mb * 2] = m->mv[mb * 2 + 1] = 0;
        return;
    }
    bool skip = cost_skip <= cost_inter;
    int32_t* levels = m->levels + mb * 384u;
    m->mb_intra[mb] = 0;
    m->mb_skip[mb] = skip;
    m->mv[mb * 2] = skip ? px : bx;
    m->mv[mb * 2 + 1] = skip ? py : by;
    mmc_put(m->cur[0], w, (int)mx * 16, (int)my * 16, 16, skip ? sy : ry);
    mmc_put(m->cur[1], cw, (int)mx * 8, (int)my * 8, 8, skip ? su : ru);
    mmc_put(m->cur[2], cw, (int)mx * 8, (int)my * 8, 8, skip ? sv : rv);
    for (uint32_t k = 0; k < 16; k++) {
        size_t i = (size_t)(my * 4u + (k >> 2)) * bw + mx * 4u + (k & 3u);
        m->mode[i] = 0;
        m->nz_y[i] = skip ? 0 : nzy[k];
    }
    for (uint32_t k = 0; k < 4; k++) {
        size_t i = (size_t)(my * 2u + (k >> 1)) * cbw + mx * 2u + (k & 1u);
        m->nz_c[0][i] = skip ? 0 : nzc[k];
        m->nz_c[1][i] = skip ? 0 : nzc[4 + k];
    }
    for (int i = 0; i < 384; i++) levels[i] = skip ? 0 : lev[i];
}

static inline void mmc_put_coeffs(mmc_re_t* e, mmc_ctx_t* c, int typ, int ctx, const int32_t* lev) {
    int32_t s[16];
    int last = -1;
    for (int i = 0; i < 16; i++) {
        s[i] = lev[mmc_zigzag[i]];
        if (s[i]) last = i;
    }
    mmc_re_bit(e, &c->cbf[typ][ctx], last >= 0);
    if (last < 0) return;
    for (int i = 0; i < 15 && i <= last; i++) {
        mmc_re_bit(e, &c->sig[typ][i], s[i] != 0);
        if (s[i]) mmc_re_bit(e, &c->last[typ][i], i == last);
    }
    int gt1 = 0, eq1 = 0;
    for (int i = last; i >= 0; i--) {
        if (!s[i]) continue;
        uint32_t a = (uint32_t)(s[i] < 0 ? -s[i] : s[i]);
        int inc = gt1 ? 0 : mmc_min(eq1 + 1, 4);
        mmc_re_bit(e, &c->gt1[typ][inc], a > 1);
        if (a > 1) {
            mmc_re_uint(e, c->lev[typ], a - 2u);
            gt1++;
        } else {
            eq1++;
        }
        mmc_re_direct(e, s[i] < 0);
    }
}

static inline void mmc_put_mvd(mmc_re_t* e, mmc_ctx_t* c, int k, int d) {
    mmc_re_bit(e, &c->mv_zero[k], d != 0);
    if (!d) return;
    mmc_re_direct(e, d < 0);
    mmc_re_uint(e, c->mv_bins[k], (uint32_t)mmc_abs(d) - 1u);
}

/* Writes the decided frame's bits to `out`. Returns the size, or 0 when it
 * does not fit in `cap`. Call mmc_finish_frame afterwards. */
MMC_FN size_t mmc_write_frame(mmc_t* m, const mmc_job_t* job, uint8_t* out, size_t cap) {
    mmc_re_t e;
    mmc_re_init(&e, out, cap);
    mmc_ctx_t c;
    mmc_ctx_init(&c);
    uint32_t bw = m->w / 4u, cbw = m->w / 8u;
    for (uint32_t my = 0; my < m->mbh; my++) {
        for (uint32_t mx = 0; mx < m->mbw; mx++) {
            size_t mb = (size_t)my * m->mbw + mx;
            const int32_t* levels = m->levels + mb * 384u;
            bool skip = m->mb_skip[mb], intra = m->mb_intra[mb];
            if (!job->key) {
                int nskip = (mx > 0 && m->mb_skip[mb - 1]) + (my > 0 && m->mb_skip[mb - m->mbw]);
                int nintra = (mx > 0 && m->mb_intra[mb - 1]) + (my > 0 && m->mb_intra[mb - m->mbw]);
                mmc_re_bit(&e, &c.skip[nskip], skip);
                if (!skip) mmc_re_bit(&e, &c.intra[nintra], intra);
            }
            if (skip) continue;
            if (intra) {
                for (uint32_t k = 0; k < 16; k++) {
                    uint32_t bx = mx * 4u + (k & 3u), by = my * 4u + (k >> 2);
                    int mpm = mmc_mpm(m, bx, by), mode = m->mode[(size_t)by * bw + bx];
                    mmc_re_bit(&e, &c.mpm[0], mode != mpm);
                    if (mode != mpm) {
                        int rem = mode < mpm ? mode : mode - 1;
                        mmc_re_bit(&e, &c.rem[0], rem > 0);
                        if (rem > 0) mmc_re_bit(&e, &c.rem[1], rem - 1);
                    }
                }
                int cmode = m->cmode[mb], b0 = cmode >> 1;
                mmc_re_bit(&e, &c.cmode[0], b0);
                mmc_re_bit(&e, &c.cmode[1 + b0], cmode & 1);
            } else {
                int px, py;
                mmc_mvp(m, mx, my, &px, &py);
                mmc_put_mvd(&e, &c, 0, m->mv[mb * 2] - px);
                mmc_put_mvd(&e, &c, 1, m->mv[mb * 2 + 1] - py);
            }
            for (uint32_t k = 0; k < 16; k++) {
                uint32_t bx = mx * 4u + (k & 3u), by = my * 4u + (k >> 2);
                mmc_put_coeffs(&e, &c, 0, mmc_nz_ctx(m->nz_y, bw, bx, by), levels + k * 16u);
            }
            for (uint32_t pl = 0; pl < 2; pl++)
                for (uint32_t k = 0; k < 4; k++) {
                    uint32_t bx = mx * 2u + (k & 1u), by = my * 2u + (k >> 1);
                    mmc_put_coeffs(&e, &c, 1, mmc_nz_ctx(m->nz_c[pl], cbw, bx, by), levels + 256u + (pl * 4u + k) * 16u);
                }
            if (e.full) return 0;
        }
    }
    mmc_re_flush(&e);
    return e.full ? 0 : e.pos;
}

#endif
