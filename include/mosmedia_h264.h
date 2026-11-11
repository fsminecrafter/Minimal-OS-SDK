/*
 * mosmedia_h264.h - an H.264 (AVC) video decoder, for .vid files with the
 * h264 codec. Plain C, no libc: it runs in programs, in mosWM and on the
 * build PC, like the rest of mosmedia.
 *
 * What it decodes: the Constrained Baseline profile, which is what
 * `mosmedia vid --codec h264` writes -
 *   - I and P slices, several slices per picture, CAVLC entropy coding
 *   - all intra modes (4x4, 16x16, chroma, I_PCM), constrained intra
 *   - P macroblocks down to 4x4, quarter-sample motion, several
 *     reference frames with list reordering and memory management
 *   - the in-loop deblocking filter
 *   - 4:2:0, 8 bits, progressive frames, cropping
 * and what it refuses (h264_decode returns false, h264_error says why):
 * CABAC, B slices, the 8x8 transform (Main and High profile), interlaced
 * video, slice groups, weighted prediction, other chroma formats.
 *
 * H.264 decoding is exact: every conforming decoder produces the same
 * pictures. tools/media/h264_check.sh compares this one with ffmpeg.
 *
 * Use (one access unit = one frame, Annex B: NAL units behind 00 00 01):
 *   h264_t d;
 *   h264_init(&d, alloc, max_access_unit_bytes);
 *   if (h264_decode(&d, data, size)) h264_to_argb(&d, out, w, h, stride);
 *   h264_free(&d, free_fn);
 *
 * A hardware decoder can do the pictures instead: every h264_decode also
 * fills d->hw (a syscall_media_h264_t: parameter sets, reference pictures
 * and the slices of the picture), which is what the graphics card's video
 * decoder needs. With d->hw_only set, h264_decode stops there - headers
 * and reference bookkeeping only, no pixels. mosmedia.h uses both.
 *
 * The code tables (coefficient tokens, zero runs, deblocking thresholds)
 * are those of the standard, ITU-T H.264.
 */
#ifndef MOSMEDIA_H264_H
#define MOSMEDIA_H264_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "media_abi.h"

#ifndef H264_FN
#define H264_FN static inline
#endif

typedef void* (*h264_alloc_fn)(size_t size);
typedef void (*h264_free_fn)(void* p);

#define H264_MAX_REFS 16
#define H264_MAX_PICS (H264_MAX_REFS + 1)
#define H264_MAX_SPS 32
#define H264_MAX_PPS 64
#define H264_MAX_SIDE 4096u

typedef struct {
    bool valid;
    uint8_t profile, level;
    uint8_t log2_max_frame_num, poc_type, log2_max_poc_lsb;
    bool delta_pic_order_always_zero, direct_8x8_inference;
    uint8_t max_refs;
    uint16_t mb_w, mb_h;
    uint16_t crop_l, crop_r, crop_t, crop_b;        /* pixels */
    bool full_range;
    uint8_t matrix;                                 /* VUI matrix_coefficients, 2 = not given */
} h264_sps_t;

typedef struct {
    bool valid;
    uint8_t sps;
    bool pic_order_present, deblocking_control, constrained_intra, redundant_pic_cnt;
    uint8_t num_ref_default, num_ref_l1_default, weighted_bipred_idc;
    int8_t init_qp, chroma_qp_offset;
} h264_pps_t;

typedef struct {
    uint8_t* plane[3];
    int frame_num, pic_num, lt_idx;
    int poc;                    /* picture order count */
    bool ref, long_term;
} h264_pic_t;

typedef struct {
    const uint8_t* p;
    uint32_t bits;      /* size in bits */
    uint32_t pos;
    bool err;
} h264_bits_t;

typedef struct h264 {
    h264_alloc_fn alloc;
    const char* error;
    h264_sps_t sps[H264_MAX_SPS];
    h264_pps_t pps[H264_MAX_PPS];
    uint8_t* rbsp;              /* one NAL unit without emulation prevention */
    size_t rbsp_cap;

    /* The stream's size, fixed by the first picture. */
    bool sized;
    uint32_t mb_w, mb_h, w, h, cw, ch;
    h264_pic_t pics[H264_MAX_PICS];
    uint32_t pic_count;
    /* Per macroblock. */
    uint16_t* mb_slice;         /* slice number, 0 = not decoded in this picture */
    uint8_t* mb_intra;          /* 1 intra, 0 inter */
    uint8_t* mb_qp;
    int8_t* mb_filter;          /* 3 per macroblock: disable idc, alpha offset, beta offset */
    /* Per 4x4 luma block (4 * mb_w wide). */
    int8_t* i4mode;             /* intra 4x4 mode, -1 = not a 4x4 intra block */
    uint8_t* nz_y;              /* coefficients in the block */
    int16_t* mv;                /* 2 per block */
    int8_t* ref_idx;            /* -1 = intra */
    int8_t* ref_pic;            /* index into pics, -1 = none (for the deblocking filter) */
    /* Per 4x4 colour block (2 * mb_w wide), two planes. */
    uint8_t* nz_c[2];

    /* The picture being decoded and the one to show. */
    h264_pic_t* cur;
    h264_pic_t* out;
    const h264_sps_t* active;
    uint32_t slice_count;
    int max_frame_num;
    bool pic_is_ref, pic_idr;
    /* Reference marking of the picture, from its first slice. */
    bool mark_adaptive, mark_long_term_idr;
    uint8_t mmco_count;
    struct { uint8_t op; int a, b; } mmco[32];
    int max_lt_idx;             /* -1 = no long-term frames allowed yet */

    /* The slice being decoded. */
    const h264_pps_t* pp;
    bool slice_p;
    int qp;
    uint16_t slice_id;
    int8_t filter[3];
    h264_pic_t* list[H264_MAX_REFS + 1];
    uint32_t list_len;
    uint32_t done;              /* 4x4 blocks of this macroblock that have their motion vector */

    /* For a hardware decoder (see the top of this file). */
    bool hw_only;               /* set by the caller: no pixels, headers only */
    bool hw_valid;              /* hw describes the picture h264_decode just returned true for */
    syscall_media_h264_t hw;
    uint8_t* hw_data;           /* the picture's slice NAL units */
    /* Picture order count: what carries over from picture to picture. */
    int prev_poc_msb, prev_poc_lsb, prev_frame_num, frame_num_offset;
} h264_t;

/* ----------------------------------------------------------- tables */

static const uint8_t h264_zigzag[16] = { 0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15 };
/* 4x4 block index (the order blocks are coded in) to its x, y in blocks. */
static const uint8_t h264_blk_x[16] = { 0, 1, 0, 1, 2, 3, 2, 3, 0, 1, 0, 1, 2, 3, 2, 3 };
static const uint8_t h264_blk_y[16] = { 0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 3, 3, 2, 2, 3, 3 };
static const uint8_t h264_cbp_intra[48] = {
    47, 31, 15, 0, 23, 27, 29, 30, 7, 11, 13, 14, 39, 43, 45, 46, 16, 3, 5, 10, 12, 19, 21, 26,
    28, 35, 37, 42, 44, 1, 2, 4, 8, 17, 18, 20, 24, 6, 9, 22, 25, 32, 33, 34, 36, 40, 38, 41 };
static const uint8_t h264_cbp_inter[48] = {
    0, 16, 1, 2, 4, 8, 32, 3, 5, 10, 12, 15, 47, 7, 11, 13, 14, 6, 9, 31, 35, 37, 42, 44,
    33, 34, 36, 40, 39, 43, 45, 46, 17, 18, 20, 24, 19, 21, 26, 28, 23, 27, 29, 30, 22, 25, 38, 41 };
static const uint8_t h264_chroma_qp[22] = {
    29, 30, 31, 32, 32, 33, 34, 34, 35, 35, 36, 36, 37, 37, 37, 38, 38, 38, 39, 39, 39, 39 };
static const uint8_t h264_scale[6][3] = {
    { 10, 16, 13 }, { 11, 18, 14 }, { 13, 20, 16 }, { 14, 23, 18 }, { 16, 25, 20 }, { 18, 29, 23 } };

/* coeff_token: [table][4 * total_coeff + trailing_ones] = length, code. */
static const uint8_t h264_ct_len[3][68] = {
    { 1, 0, 0, 0, 6, 2, 0, 0, 8, 6, 3, 0, 9, 8, 7, 5, 10, 9, 8, 6, 11, 10, 9, 7, 13, 11, 10, 8,
      13, 13, 11, 9, 13, 13, 13, 10, 14, 14, 13, 11, 14, 14, 14, 13, 15, 15, 14, 14, 15, 15, 15, 14,
      16, 15, 15, 15, 16, 16, 16, 15, 16, 16, 16, 16, 16, 16, 16, 16 },
    { 2, 0, 0, 0, 6, 2, 0, 0, 6, 5, 3, 0, 7, 6, 6, 4, 8, 6, 6, 4, 8, 7, 7, 5, 9, 8, 8, 6,
      11, 9, 9, 6, 11, 11, 11, 7, 12, 11, 11, 9, 12, 12, 12, 11, 12, 12, 12, 11, 13, 13, 13, 12,
      13, 13, 13, 13, 13, 14, 13, 13, 14, 14, 14, 13, 14, 14, 14, 14 },
    { 4, 0, 0, 0, 6, 4, 0, 0, 6, 5, 4, 0, 6, 5, 5, 4, 7, 5, 5, 4, 7, 5, 5, 4, 7, 6, 6, 4,
      7, 6, 6, 4, 8, 7, 7, 5, 8, 8, 7, 6, 9, 8, 8, 7, 9, 9, 8, 8, 9, 9, 9, 8,
      10, 9, 9, 9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10 } };
static const uint8_t h264_ct_bits[3][68] = {
    { 1, 0, 0, 0, 5, 1, 0, 0, 7, 4, 1, 0, 7, 6, 5, 3, 7, 6, 5, 3, 7, 6, 5, 4, 15, 6, 5, 4,
      11, 14, 5, 4, 8, 10, 13, 4, 15, 14, 9, 4, 11, 10, 13, 12, 15, 14, 9, 12, 11, 10, 13, 8,
      15, 1, 9, 12, 11, 14, 13, 8, 7, 10, 9, 12, 4, 6, 5, 8 },
    { 3, 0, 0, 0, 11, 2, 0, 0, 7, 7, 3, 0, 7, 10, 9, 5, 7, 6, 5, 4, 4, 6, 5, 6, 7, 6, 5, 8,
      15, 6, 5, 4, 11, 14, 13, 4, 15, 10, 9, 4, 11, 14, 13, 12, 8, 10, 9, 8, 15, 14, 13, 12,
      11, 10, 9, 12, 7, 11, 6, 8, 9, 8, 10, 1, 7, 6, 5, 4 },
    { 15, 0, 0, 0, 15, 14, 0, 0, 11, 15, 13, 0, 8, 12, 14, 12, 15, 10, 11, 11, 11, 8, 9, 10, 9, 14, 13, 9,
      8, 10, 9, 8, 15, 14, 13, 13, 11, 14, 10, 12, 15, 10, 13, 12, 11, 14, 9, 12, 8, 10, 13, 8,
      13, 7, 9, 12, 9, 12, 11, 10, 5, 8, 7, 6, 1, 4, 3, 2 } };
static const uint8_t h264_ctc_len[20] = { 2, 0, 0, 0, 6, 1, 0, 0, 6, 6, 3, 0, 6, 7, 7, 6, 6, 8, 8, 7 };
static const uint8_t h264_ctc_bits[20] = { 1, 0, 0, 0, 7, 1, 0, 0, 4, 6, 1, 0, 3, 3, 2, 5, 2, 3, 2, 0 };
/* total_zeros: [total_coeff - 1][zeros] */
static const uint8_t h264_tz_len[15][16] = {
    { 1, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 9 }, { 3, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 6, 6, 6, 6 },
    { 4, 3, 3, 3, 4, 4, 3, 3, 4, 5, 5, 6, 5, 6 }, { 5, 3, 4, 4, 3, 3, 3, 4, 3, 4, 5, 5, 5 },
    { 4, 4, 4, 3, 3, 3, 3, 3, 4, 5, 4, 5 }, { 6, 5, 3, 3, 3, 3, 3, 3, 4, 3, 6 },
    { 6, 5, 3, 3, 3, 2, 3, 4, 3, 6 }, { 6, 4, 5, 3, 2, 2, 3, 3, 6 }, { 6, 6, 4, 2, 2, 3, 2, 5 },
    { 5, 5, 3, 2, 2, 2, 4 }, { 4, 4, 3, 3, 1, 3 }, { 4, 4, 2, 1, 3 }, { 3, 3, 1, 2 }, { 2, 2, 1 }, { 1, 1 } };
static const uint8_t h264_tz_bits[15][16] = {
    { 1, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 1 }, { 7, 6, 5, 4, 3, 5, 4, 3, 2, 3, 2, 3, 2, 1, 0 },
    { 5, 7, 6, 5, 4, 3, 4, 3, 2, 3, 2, 1, 1, 0 }, { 3, 7, 5, 4, 6, 5, 4, 3, 3, 2, 2, 1, 0 },
    { 5, 4, 3, 7, 6, 5, 4, 3, 2, 1, 1, 0 }, { 1, 1, 7, 6, 5, 4, 3, 2, 1, 1, 0 },
    { 1, 1, 5, 4, 3, 3, 2, 1, 1, 0 }, { 1, 1, 1, 3, 3, 2, 2, 1, 0 }, { 1, 0, 1, 3, 2, 1, 1, 1 },
    { 1, 0, 1, 3, 2, 1, 1 }, { 0, 1, 1, 2, 1, 3 }, { 0, 1, 1, 1, 1 }, { 0, 1, 1, 1 }, { 0, 1, 1 }, { 0, 1 } };
static const uint8_t h264_tzc_len[3][4] = { { 1, 2, 3, 3 }, { 1, 2, 2, 0 }, { 1, 1, 0, 0 } };
static const uint8_t h264_tzc_bits[3][4] = { { 1, 1, 1, 0 }, { 1, 1, 0, 0 }, { 1, 0, 0, 0 } };
/* run_before: [min(zeros_left, 7) - 1][run] */
static const uint8_t h264_run_len[7][16] = {
    { 1, 1 }, { 1, 2, 2 }, { 2, 2, 2, 2 }, { 2, 2, 2, 3, 3 }, { 2, 2, 3, 3, 3, 3 }, { 2, 3, 3, 3, 3, 3, 3 },
    { 3, 3, 3, 3, 3, 3, 3, 4, 5, 6, 7, 8, 9, 10, 11 } };
static const uint8_t h264_run_bits[7][16] = {
    { 1, 0 }, { 1, 1, 0 }, { 3, 2, 1, 0 }, { 3, 2, 1, 1, 0 }, { 3, 2, 3, 2, 1, 0 }, { 3, 0, 1, 3, 2, 5, 4 },
    { 7, 6, 5, 4, 3, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1 } };
/* Deblocking thresholds, from index 16 on (smaller indices: no filtering). */
static const uint8_t h264_alpha[36] = {
    4, 4, 5, 6, 7, 8, 9, 10, 12, 13, 15, 17, 20, 22, 25, 28, 32, 36, 40, 45, 50, 56, 63, 71,
    80, 90, 101, 113, 127, 144, 162, 182, 203, 226, 255, 255 };
static const uint8_t h264_beta[36] = {
    2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12,
    13, 13, 14, 14, 15, 15, 16, 16, 17, 17, 18, 18 };
static const uint8_t h264_tc0[35][3] = {     /* from index 17 on */
    { 0, 0, 1 }, { 0, 0, 1 }, { 0, 0, 1 }, { 0, 0, 1 }, { 0, 1, 1 }, { 0, 1, 1 }, { 1, 1, 1 }, { 1, 1, 1 },
    { 1, 1, 1 }, { 1, 1, 1 }, { 1, 1, 2 }, { 1, 1, 2 }, { 1, 1, 2 }, { 1, 1, 2 }, { 1, 2, 3 }, { 1, 2, 3 },
    { 2, 2, 3 }, { 2, 2, 4 }, { 2, 3, 4 }, { 2, 3, 4 }, { 3, 3, 5 }, { 3, 4, 6 }, { 3, 4, 6 }, { 4, 5, 7 },
    { 4, 5, 8 }, { 4, 6, 9 }, { 5, 7, 10 }, { 6, 8, 11 }, { 6, 8, 13 }, { 7, 10, 14 }, { 8, 11, 16 },
    { 9, 12, 18 }, { 10, 13, 20 }, { 11, 15, 23 }, { 13, 17, 25 } };

/* ------------------------------------------------------------- bits */

H264_FN int h264_clip(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }
H264_FN int h264_clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
H264_FN int h264_abs(int v) { return v < 0 ? -v : v; }
H264_FN int h264_median(int a, int b, int c) {
    int lo = a < b ? a : b, hi = a < b ? b : a;
    return c < lo ? lo : (c > hi ? hi : c);
}

H264_FN uint32_t h264_peek(const h264_bits_t* b, uint32_t n) {     /* n <= 25 */
    uint32_t v = 0, byte = b->pos >> 3, total = (b->bits + 7u) >> 3;
    for (int i = 0; i < 4; i++) v = (v << 8) | (byte + (uint32_t)i < total ? b->p[byte + (uint32_t)i] : 0u);
    return n ? (v << (b->pos & 7u)) >> (32u - n) : 0u;
}

H264_FN uint32_t h264_u(h264_bits_t* b, uint32_t n) {       /* n <= 32 */
    uint32_t v = 0;
    while (n > 16u) { v = (v << 16) | h264_peek(b, 16); b->pos += 16u; n -= 16u; }
    v = (v << n) | h264_peek(b, n);
    b->pos += n;
    if (b->pos > b->bits) b->err = true;
    return v;
}

H264_FN uint32_t h264_ue(h264_bits_t* b) {
    uint32_t zeros = 0;
    for (;;) {
        if (b->pos >= b->bits || zeros >= 32u) { b->err = true; return 0; }
        if (h264_u(b, 1)) break;
        zeros++;
    }
    return zeros ? ((1u << zeros) - 1u) + h264_u(b, zeros) : 0u;
}

H264_FN int32_t h264_se(h264_bits_t* b) {
    uint32_t v = h264_ue(b);
    return (v & 1u) ? (int32_t)((v + 1u) >> 1) : -(int32_t)(v >> 1);
}

/* Reads one code of a table given as lengths and codes; -1 if none matches. */
H264_FN int h264_vlc(h264_bits_t* b, const uint8_t* len, const uint8_t* code, int count) {
    uint32_t window = h264_peek(b, 16);
    for (int i = 0; i < count; i++) {
        if (len[i] && (window >> (16u - len[i])) == code[i]) {
            b->pos += len[i];
            if (b->pos > b->bits) b->err = true;
            return i;
        }
    }
    b->err = true;
    return -1;
}

/* ----------------------------------------------- memory and headers */

H264_FN bool h264_fail(h264_t* d, const char* why) {
    if (!d->error) d->error = why;
    return false;
}

H264_FN const char* h264_error(const h264_t* d) { return d->error ? d->error : ""; }

H264_FN void* h264_zalloc(h264_t* d, size_t size) {
    uint8_t* p = (uint8_t*)d->alloc(size ? size : 1u);
    if (p) for (size_t i = 0; i < size; i++) p[i] = 0;
    return p;
}

/* max_unit: the largest access unit that will be given to h264_decode. */
H264_FN bool h264_init(h264_t* d, h264_alloc_fn alloc, size_t max_unit) {
    for (size_t i = 0; i < sizeof(*d); i++) ((uint8_t*)d)[i] = 0;
    d->alloc = alloc;
    d->max_lt_idx = -1;
    d->rbsp_cap = max_unit + 8u;
    d->rbsp = (uint8_t*)alloc(d->rbsp_cap);
    d->hw_data = (uint8_t*)alloc(d->rbsp_cap);
    return d->rbsp != NULL && d->hw_data != NULL;
}

H264_FN void h264_free(h264_t* d, h264_free_fn free_fn) {
    if (!free_fn) return;
    void* all[] = { d->rbsp, d->hw_data, d->mb_slice, d->mb_intra, d->mb_qp, d->mb_filter, d->i4mode, d->nz_y, d->mv,
                    d->ref_idx, d->ref_pic, d->nz_c[0], d->nz_c[1] };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) if (all[i]) free_fn(all[i]);
    for (uint32_t i = 0; i < H264_MAX_PICS; i++)
        for (int p = 0; p < 3; p++) if (d->pics[i].plane[p]) free_fn(d->pics[i].plane[p]);
    d->rbsp = NULL;
    d->hw_data = NULL;
    d->sized = false;
}

H264_FN bool h264_size(h264_t* d, const h264_sps_t* s) {
    if (d->sized) {
        if (d->mb_w != s->mb_w || d->mb_h != s->mb_h) return h264_fail(d, "the picture size changes");
        if ((uint32_t)s->max_refs + 1u > d->pic_count) return h264_fail(d, "more reference frames than at the start");
        return true;
    }
    d->mb_w = s->mb_w; d->mb_h = s->mb_h;
    d->w = d->mb_w * 16u; d->h = d->mb_h * 16u; d->cw = d->w / 2u; d->ch = d->h / 2u;
    size_t mbs = (size_t)d->mb_w * d->mb_h, blocks = mbs * 16u;
    d->mb_slice = (uint16_t*)h264_zalloc(d, mbs * sizeof(uint16_t));
    d->mb_intra = (uint8_t*)h264_zalloc(d, mbs);
    d->mb_qp = (uint8_t*)h264_zalloc(d, mbs);
    d->mb_filter = (int8_t*)h264_zalloc(d, mbs * 3u);
    d->i4mode = (int8_t*)h264_zalloc(d, blocks);
    d->nz_y = (uint8_t*)h264_zalloc(d, blocks);
    d->mv = (int16_t*)h264_zalloc(d, blocks * 2u * sizeof(int16_t));
    d->ref_idx = (int8_t*)h264_zalloc(d, blocks);
    d->ref_pic = (int8_t*)h264_zalloc(d, blocks);
    d->nz_c[0] = (uint8_t*)h264_zalloc(d, mbs * 4u);
    d->nz_c[1] = (uint8_t*)h264_zalloc(d, mbs * 4u);
    bool ok = d->mb_slice && d->mb_intra && d->mb_qp && d->mb_filter && d->i4mode && d->nz_y && d->mv &&
              d->ref_idx && d->ref_pic && d->nz_c[0] && d->nz_c[1];
    d->pic_count = (uint32_t)s->max_refs + 1u;
    if (d->pic_count < 2u) d->pic_count = 2u;
    for (uint32_t i = 0; ok && i < d->pic_count; i++) {
        d->pics[i].plane[0] = (uint8_t*)h264_zalloc(d, (size_t)d->w * d->h);
        d->pics[i].plane[1] = (uint8_t*)h264_zalloc(d, (size_t)d->cw * d->ch);
        d->pics[i].plane[2] = (uint8_t*)h264_zalloc(d, (size_t)d->cw * d->ch);
        ok = d->pics[i].plane[0] && d->pics[i].plane[1] && d->pics[i].plane[2];
    }
    if (!ok) return h264_fail(d, "out of memory");
    d->sized = true;
    return true;
}

H264_FN bool h264_parse_sps(h264_t* d, h264_bits_t* b) {
    h264_sps_t s;
    for (size_t i = 0; i < sizeof(s); i++) ((uint8_t*)&s)[i] = 0;
    s.profile = (uint8_t)h264_u(b, 8);
    h264_u(b, 8);
    s.level = (uint8_t)h264_u(b, 8);
    uint32_t id = h264_ue(b);
    if (id >= H264_MAX_SPS) return h264_fail(d, "bad sequence parameter set");
    if (s.profile == 100 || s.profile == 110 || s.profile == 122 || s.profile == 244 || s.profile == 44 ||
        s.profile == 83 || s.profile == 86 || s.profile == 118 || s.profile == 128) {
        if (h264_ue(b) != 1) return h264_fail(d, "only 4:2:0 video is supported");
        if (h264_ue(b) || h264_ue(b)) return h264_fail(d, "only 8-bit video is supported");
        h264_u(b, 1);
        if (h264_u(b, 1)) return h264_fail(d, "scaling matrices are not supported");
    }
    s.log2_max_frame_num = (uint8_t)(h264_ue(b) + 4u);
    s.poc_type = (uint8_t)h264_ue(b);
    if (s.poc_type == 0) {
        s.log2_max_poc_lsb = (uint8_t)(h264_ue(b) + 4u);
    } else if (s.poc_type == 1) {
        s.delta_pic_order_always_zero = h264_u(b, 1);
        h264_se(b);
        h264_se(b);
        uint32_t n = h264_ue(b);
        if (n > 255u) return h264_fail(d, "bad sequence parameter set");
        for (uint32_t i = 0; i < n; i++) h264_se(b);
    } else if (s.poc_type != 2) {
        return h264_fail(d, "bad sequence parameter set");
    }
    uint32_t refs = h264_ue(b);
    h264_u(b, 1);
    uint32_t mw = h264_ue(b) + 1u, mh = h264_ue(b) + 1u;
    if (b->err || s.log2_max_frame_num > 16 || s.log2_max_poc_lsb > 16 || refs > H264_MAX_REFS ||
        mw * 16u > H264_MAX_SIDE || mh * 16u > H264_MAX_SIDE)
        return h264_fail(d, "bad sequence parameter set");
    if (!h264_u(b, 1)) return h264_fail(d, "interlaced video is not supported");
    s.max_refs = (uint8_t)refs;
    s.mb_w = (uint16_t)mw;
    s.mb_h = (uint16_t)mh;
    s.direct_8x8_inference = h264_u(b, 1);
    if (h264_u(b, 1)) {
        uint32_t l = h264_ue(b) * 2u, r = h264_ue(b) * 2u, t = h264_ue(b) * 2u, bo = h264_ue(b) * 2u;
        if (b->err || l + r >= mw * 16u || t + bo >= mh * 16u) return h264_fail(d, "bad cropping");
        s.crop_l = (uint16_t)l; s.crop_r = (uint16_t)r; s.crop_t = (uint16_t)t; s.crop_b = (uint16_t)bo;
    }
    s.matrix = 2;
    if (h264_u(b, 1)) {                         /* VUI: only the colour description matters here */
        if (h264_u(b, 1) && h264_u(b, 8) == 255u) h264_u(b, 32);
        if (h264_u(b, 1)) h264_u(b, 1);
        if (h264_u(b, 1)) {
            h264_u(b, 3);
            s.full_range = h264_u(b, 1);
            if (h264_u(b, 1)) {
                h264_u(b, 16);
                s.matrix = (uint8_t)h264_u(b, 8);
            }
        }
    }
    if (b->err) return h264_fail(d, "bad sequence parameter set");
    s.valid = true;
    d->sps[id] = s;
    return true;
}

H264_FN bool h264_parse_pps(h264_t* d, h264_bits_t* b) {
    h264_pps_t p;
    for (size_t i = 0; i < sizeof(p); i++) ((uint8_t*)&p)[i] = 0;
    uint32_t id = h264_ue(b), sps = h264_ue(b);
    if (b->err || id >= H264_MAX_PPS || sps >= H264_MAX_SPS) return h264_fail(d, "bad picture parameter set");
    p.sps = (uint8_t)sps;
    if (h264_u(b, 1)) return h264_fail(d, "CABAC (Main and High profile) is not supported");
    p.pic_order_present = h264_u(b, 1);
    if (h264_ue(b)) return h264_fail(d, "slice groups are not supported");
    uint32_t n0 = h264_ue(b) + 1u;
    uint32_t n1 = h264_ue(b);
    if (h264_u(b, 1)) return h264_fail(d, "weighted prediction is not supported");
    p.weighted_bipred_idc = (uint8_t)h264_u(b, 2);
    p.num_ref_l1_default = (uint8_t)(n1 < 32u ? n1 : 31u);
    int qp = 26 + h264_se(b);
    h264_se(b);
    int cqp = h264_se(b);
    p.deblocking_control = h264_u(b, 1);
    p.constrained_intra = h264_u(b, 1);
    p.redundant_pic_cnt = h264_u(b, 1);
    if (b->err || n0 > H264_MAX_REFS || qp < 0 || qp > 51 || cqp < -12 || cqp > 12)
        return h264_fail(d, "bad picture parameter set");
    /* What follows is High profile: the 8x8 transform flag comes first. */
    if (b->pos + 8u < b->bits && h264_u(b, 1)) return h264_fail(d, "the 8x8 transform (High profile) is not supported");
    p.num_ref_default = (uint8_t)n0;
    p.init_qp = (int8_t)qp;
    p.chroma_qp_offset = (int8_t)cqp;
    p.valid = true;
    d->pps[id] = p;
    return true;
}

/* -------------------------------------------------------- neighbours */

/* The macroblock holding 4x4 block (bx, by), if it belongs to the current
 * slice and was decoded before; -1 otherwise. */
H264_FN int h264_mb_at(const h264_t* d, int bx, int by) {
    if (bx < 0 || by < 0 || bx >= (int)d->mb_w * 4 || by >= (int)d->mb_h * 4) return -1;
    int mb = (by >> 2) * (int)d->mb_w + (bx >> 2);
    return d->mb_slice[mb] == d->slice_id ? mb : -1;
}

/* Number of coefficients predicted for a block from its left and top. */
H264_FN int h264_nc(const h264_t* d, const uint8_t* nz, int stride, int bx, int by, int shift) {
    /* shift: 0 for luma blocks, 1 for colour blocks (half as many per macroblock) */
    int a = -1, bb = -1;
    int unit = 4 >> shift;
    if (bx > 0 && d->mb_slice[(by / unit) * (int)d->mb_w + (bx - 1) / unit] == d->slice_id)
        a = nz[by * stride + bx - 1];
    if (by > 0 && d->mb_slice[((by - 1) / unit) * (int)d->mb_w + bx / unit] == d->slice_id)
        bb = nz[(by - 1) * stride + bx];
    if (a >= 0 && bb >= 0) return (a + bb + 1) >> 1;
    return a >= 0 ? a : (bb >= 0 ? bb : 0);
}

/* ------------------------------------------------------------ residual */

/* One block of coefficients into coef[16] (raster order). start: 0, or 1
 * for blocks without the DC coefficient; max: coefficients in the block.
 * Returns the number of coefficients, -1 on a damaged stream. */
H264_FN int h264_residual(h264_bits_t* b, int nc, int start, int max, int32_t* coef, bool chroma_dc) {
    int token;
    if (chroma_dc) {
        token = h264_vlc(b, h264_ctc_len, h264_ctc_bits, 20);
    } else if (nc >= 8) {
        uint32_t v = h264_u(b, 6);
        token = v == 3u ? 0 : (int)((((v >> 2) + 1u) << 2) | (v & 3u));
    } else {
        int t = nc < 2 ? 0 : (nc < 4 ? 1 : 2);
        token = h264_vlc(b, h264_ct_len[t], h264_ct_bits[t], 68);
    }
    if (token < 0 || b->err) return -1;
    int total = token >> 2, ones = token & 3;
    if (total == 0) return 0;
    if (total > max || ones > total) return -1;
    int level[16];
    int suffix_len = total > 10 && ones < 3 ? 1 : 0;
    for (int i = 0; i < total; i++) {
        if (i < ones) {
            level[i] = h264_u(b, 1) ? -1 : 1;
            continue;
        }
        int prefix = 0;
        for (;;) {
            if (b->pos >= b->bits || prefix > 28) return -1;
            if (h264_u(b, 1)) break;
            prefix++;
        }
        int code = (prefix < 15 ? prefix : 15) << suffix_len;
        int size = prefix == 14 && suffix_len == 0 ? 4 : (prefix >= 15 ? prefix - 3 : suffix_len);
        if (size) code += (int)h264_u(b, (uint32_t)size);
        if (prefix >= 15 && suffix_len == 0) code += 15;
        if (prefix >= 16) code += (1 << (prefix - 3)) - 4096;
        if (i == ones && ones < 3) code += 2;
        level[i] = (code & 1) ? (-code - 1) >> 1 : (code + 2) >> 1;
        if (suffix_len == 0) suffix_len = 1;
        if (h264_abs(level[i]) > (3 << (suffix_len - 1)) && suffix_len < 6) suffix_len++;
    }
    int zeros = 0;
    if (total < max) {
        int z = chroma_dc ? h264_vlc(b, h264_tzc_len[total - 1], h264_tzc_bits[total - 1], 4)
                          : h264_vlc(b, h264_tz_len[total - 1], h264_tz_bits[total - 1], 16);
        if (z < 0) return -1;
        zeros = z;
    }
    if (total + zeros > max) return -1;
    int at = total + zeros - 1;                 /* scan position of the first level */
    for (int i = 0; i < total; i++) {
        if (at < 0) return -1;
        coef[chroma_dc ? at : h264_zigzag[at + start]] = level[i];
        int run = 0;
        if (i + 1 < total && zeros > 0) {
            int t = (zeros < 7 ? zeros : 7) - 1;
            run = h264_vlc(b, h264_run_len[t], h264_run_bits[t], 16);
            if (run < 0 || run > zeros) return -1;
            zeros -= run;
        }
        at -= 1 + run;
    }
    return b->err ? -1 : total;
}

/* Scales a block's coefficients (all but the DC with `ac_only`), inverse
 * transforms and adds it to the picture. */
H264_FN void h264_add_block(uint8_t* dst, uint32_t stride, int32_t* c, int qp, bool ac_only) {
    const uint8_t* v = h264_scale[qp % 6];
    int shift = qp / 6;
    bool any = false;
    for (int i = ac_only ? 1 : 0; i < 16; i++) {
        if (!c[i]) continue;
        int x = i & 3, y = i >> 2;
        int s = (x & 1) && (y & 1) ? v[1] : ((x & 1) || (y & 1) ? v[2] : v[0]);
        c[i] = (int32_t)((uint32_t)(c[i] * s) << shift);
    }
    for (int i = 0; i < 16; i++) any = any || c[i];
    if (!any) return;
    for (int y = 0; y < 4; y++) {
        int32_t* r = c + y * 4;
        int e0 = r[0] + r[2], e1 = r[0] - r[2], e2 = (r[1] >> 1) - r[3], e3 = r[1] + (r[3] >> 1);
        r[0] = e0 + e3; r[1] = e1 + e2; r[2] = e1 - e2; r[3] = e0 - e3;
    }
    for (int x = 0; x < 4; x++) {
        int e0 = c[x] + c[8 + x], e1 = c[x] - c[8 + x];
        int e2 = (c[4 + x] >> 1) - c[12 + x], e3 = c[4 + x] + (c[12 + x] >> 1);
        int o[4] = { e0 + e3, e1 + e2, e1 - e2, e0 - e3 };
        for (int y = 0; y < 4; y++) {
            uint8_t* p = dst + (size_t)y * stride + (size_t)x;
            *p = (uint8_t)h264_clip(*p + ((o[y] + 32) >> 6));
        }
    }
}

/* ------------------------------------------------------ intra prediction */

/* Edge samples of a block: e[0] is the corner, e[1..n*2] the row above,
 * and l[0..n-1] the column to the left. */
H264_FN void h264_pred4(uint8_t* dst, uint32_t stride, int mode, const uint8_t* top8, const uint8_t* left4,
                        int corner, bool has_top, bool has_left) {
    int t[9], l[5];
    /* P(x, -1) = t[x + 1], P(-1, y) = l[y + 1], P(-1, -1) = t[0] = l[0]. */
    t[0] = l[0] = corner;
    for (int i = 0; i < 8; i++) t[i + 1] = top8[i];
    for (int i = 0; i < 4; i++) l[i + 1] = left4[i];
#define T(x) t[(x) + 1]
#define L(y) l[(y) + 1]
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            int v;
            switch (mode) {
                case 0: v = T(x); break;
                case 1: v = L(y); break;
                case 2:
                    if (has_top && has_left) v = (T(0) + T(1) + T(2) + T(3) + L(0) + L(1) + L(2) + L(3) + 4) >> 3;
                    else if (has_top) v = (T(0) + T(1) + T(2) + T(3) + 2) >> 2;
                    else if (has_left) v = (L(0) + L(1) + L(2) + L(3) + 2) >> 2;
                    else v = 128;
                    break;
                case 3:
                    v = x == 3 && y == 3 ? (T(6) + 3 * T(7) + 2) >> 2
                                         : (T(x + y) + 2 * T(x + y + 1) + T(x + y + 2) + 2) >> 2;
                    break;
                case 4:
                    if (x > y) v = (T(x - y - 2) + 2 * T(x - y - 1) + T(x - y) + 2) >> 2;
                    else if (x < y) v = (L(y - x - 2) + 2 * L(y - x - 1) + L(y - x) + 2) >> 2;
                    else v = (T(0) + 2 * T(-1) + L(0) + 2) >> 2;
                    break;
                case 5: {
                    int z = 2 * x - y, k = x - (y >> 1);
                    if (z >= 0 && !(z & 1)) v = (T(k - 1) + T(k) + 1) >> 1;
                    else if (z >= 0) v = (T(k - 2) + 2 * T(k - 1) + T(k) + 2) >> 2;
                    else if (z == -1) v = (L(0) + 2 * T(-1) + T(0) + 2) >> 2;
                    else v = (L(y - 1) + 2 * L(y - 2) + L(y - 3) + 2) >> 2;
                    break;
                }
                case 6: {
                    int z = 2 * y - x, k = y - (x >> 1);
                    if (z >= 0 && !(z & 1)) v = (L(k - 1) + L(k) + 1) >> 1;
                    else if (z >= 0) v = (L(k - 2) + 2 * L(k - 1) + L(k) + 2) >> 2;
                    else if (z == -1) v = (L(0) + 2 * T(-1) + T(0) + 2) >> 2;
                    else v = (T(x - 1) + 2 * T(x - 2) + T(x - 3) + 2) >> 2;
                    break;
                }
                case 7: {
                    int k = x + (y >> 1);
                    v = (y & 1) ? (T(k) + 2 * T(k + 1) + T(k + 2) + 2) >> 2 : (T(k) + T(k + 1) + 1) >> 1;
                    break;
                }
                default: {
                    int z = x + 2 * y, k = y + (x >> 1);
                    if (z > 5) v = L(3);
                    else if (z == 5) v = (L(2) + 3 * L(3) + 2) >> 2;
                    else if (z & 1) v = (L(k) + 2 * L(k + 1) + L(k + 2) + 2) >> 2;
                    else v = (L(k) + L(k + 1) + 1) >> 1;
                    break;
                }
            }
            dst[(size_t)y * stride + (size_t)x] = (uint8_t)v;
        }
    }
#undef T
#undef L
}

/* 16x16 luma (n = 16) and 8x8 colour (n = 8) prediction. Modes here:
 * 0 vertical, 1 horizontal, 2 DC, 3 plane. */
H264_FN void h264_pred_big(uint8_t* dst, uint32_t stride, int n, int mode, bool has_top, bool has_left) {
    const uint8_t* top = dst - stride;
    if (mode == 0) {
        for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) dst[(size_t)y * stride + x] = top[x];
    } else if (mode == 1) {
        for (int y = 0; y < n; y++) {
            int v = dst[(size_t)y * stride - 1];
            for (int x = 0; x < n; x++) dst[(size_t)y * stride + x] = (uint8_t)v;
        }
    } else if (mode == 3) {
        int half = n / 2, hsum = 0, vsum = 0;
        for (int i = 0; i < half; i++) {
            int lo = half - 2 - i;
            hsum += (i + 1) * (top[half + i] - (lo < 0 ? dst[-(ptrdiff_t)stride - 1] : top[lo]));
            vsum += (i + 1) * (dst[(size_t)(half + i) * stride - 1] -
                               (lo < 0 ? dst[-(ptrdiff_t)stride - 1] : dst[(size_t)lo * stride - 1]));
        }
        int a = 16 * (dst[(size_t)(n - 1) * stride - 1] + top[n - 1]);
        int bb = n == 16 ? (5 * hsum + 32) >> 6 : (34 * hsum + 32) >> 6;
        int c = n == 16 ? (5 * vsum + 32) >> 6 : (34 * vsum + 32) >> 6;
        /* The left column is read above before any of it is written. */
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++)
                dst[(size_t)y * stride + x] =
                    (uint8_t)h264_clip((a + bb * (x - (half - 1)) + c * (y - (half - 1)) + 16) >> 5);
    } else if (n == 16) {
        int sum = 0, count = 0;
        if (has_top) { for (int i = 0; i < 16; i++) sum += top[i]; count += 16; }
        if (has_left) { for (int i = 0; i < 16; i++) sum += dst[(size_t)i * stride - 1]; count += 16; }
        int v = count == 32 ? (sum + 16) >> 5 : (count ? (sum + 8) >> 4 : 128);
        for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) dst[(size_t)y * stride + x] = (uint8_t)v;
    } else {
        /* Colour DC: each 4x4 quarter from the edges next to it. */
        int st[2] = { 0, 0 }, sl[2] = { 0, 0 };
        for (int i = 0; i < 8; i++) {
            if (has_top) st[i >> 2] += top[i];
            if (has_left) sl[i >> 2] += dst[(size_t)i * stride - 1];
        }
        for (int q = 0; q < 4; q++) {
            int qx = q & 1, qy = q >> 1, v;
            if (qx == qy) {
                if (has_top && has_left) v = (st[qx] + sl[qy] + 4) >> 3;
                else if (has_top) v = (st[qx] + 2) >> 2;
                else if (has_left) v = (sl[qy] + 2) >> 2;
                else v = 128;
            } else if (qx) {
                v = has_top ? (st[1] + 2) >> 2 : (has_left ? (sl[0] + 2) >> 2 : 128);
            } else {
                v = has_left ? (sl[1] + 2) >> 2 : (has_top ? (st[0] + 2) >> 2 : 128);
            }
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++) dst[(size_t)(qy * 4 + y) * stride + (size_t)(qx * 4 + x)] = (uint8_t)v;
        }
    }
}

/* -------------------------------------------------- motion compensation */

H264_FN int h264_tap(int a, int b, int c, int dd, int e, int f) { return a - 5 * b + 20 * c + 20 * dd - 5 * e + f; }

/* A w x h luma block from `ref` at (x, y) plus the quarter-sample
 * fraction (fx, fy); samples outside the picture repeat its edge. */
H264_FN void h264_mc_luma(const uint8_t* ref, int pw, int ph, int x, int y, int fx, int fy, int w, int h,
                          uint8_t* dst, uint32_t stride) {
    uint8_t s[22 * 22];
    int sw = w + 6;
    for (int j = 0; j < h + 6; j++) {
        const uint8_t* row = ref + (size_t)h264_clamp(y - 2 + j, 0, ph - 1) * (size_t)pw;
        if (x - 2 >= 0 && x + w + 4 <= pw) {
            for (int i = 0; i < sw; i++) s[j * sw + i] = row[x - 2 + i];
        } else {
            for (int i = 0; i < sw; i++) s[j * sw + i] = row[h264_clamp(x - 2 + i, 0, pw - 1)];
        }
    }
#define S(i, j) ((int)s[((j) + 2) * sw + (i) + 2])
#define B1(i, j) h264_tap(S((i) - 2, j), S((i) - 1, j), S(i, j), S((i) + 1, j), S((i) + 2, j), S((i) + 3, j))
#define H1(i, j) h264_tap(S(i, (j) - 2), S(i, (j) - 1), S(i, j), S(i, (j) + 1), S(i, (j) + 2), S(i, (j) + 3))
#define HB(i, j) h264_clip((B1(i, j) + 16) >> 5)
#define HH(i, j) h264_clip((H1(i, j) + 16) >> 5)
#define HJ(i, j) h264_clip((h264_tap(B1(i, (j) - 2), B1(i, (j) - 1), B1(i, j), B1(i, (j) + 1), \
                                     B1(i, (j) + 2), B1(i, (j) + 3)) + 512) >> 10)
    int pos = fy * 4 + fx;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            int v;
            switch (pos) {
                case 0: v = S(i, j); break;
                case 1: v = (S(i, j) + HB(i, j) + 1) >> 1; break;
                case 2: v = HB(i, j); break;
                case 3: v = (S(i + 1, j) + HB(i, j) + 1) >> 1; break;
                case 4: v = (S(i, j) + HH(i, j) + 1) >> 1; break;
                case 5: v = (HB(i, j) + HH(i, j) + 1) >> 1; break;
                case 6: v = (HB(i, j) + HJ(i, j) + 1) >> 1; break;
                case 7: v = (HB(i, j) + HH(i + 1, j) + 1) >> 1; break;
                case 8: v = HH(i, j); break;
                case 9: v = (HH(i, j) + HJ(i, j) + 1) >> 1; break;
                case 10: v = HJ(i, j); break;
                case 11: v = (HJ(i, j) + HH(i + 1, j) + 1) >> 1; break;
                case 12: v = (S(i, j + 1) + HH(i, j) + 1) >> 1; break;
                case 13: v = (HH(i, j) + HB(i, j + 1) + 1) >> 1; break;
                case 14: v = (HJ(i, j) + HB(i, j + 1) + 1) >> 1; break;
                default: v = (HH(i + 1, j) + HB(i, j + 1) + 1) >> 1; break;
            }
            dst[(size_t)j * stride + (size_t)i] = (uint8_t)v;
        }
    }
#undef S
#undef B1
#undef H1
#undef HB
#undef HH
#undef HJ
}

H264_FN void h264_mc_chroma(const uint8_t* ref, int pw, int ph, int x, int y, int fx, int fy, int w, int h,
                            uint8_t* dst, uint32_t stride) {
    for (int j = 0; j < h; j++) {
        const uint8_t* r0 = ref + (size_t)h264_clamp(y + j, 0, ph - 1) * (size_t)pw;
        const uint8_t* r1 = ref + (size_t)h264_clamp(y + j + 1, 0, ph - 1) * (size_t)pw;
        for (int i = 0; i < w; i++) {
            int x0 = h264_clamp(x + i, 0, pw - 1), x1 = h264_clamp(x + i + 1, 0, pw - 1);
            dst[(size_t)j * stride + (size_t)i] = (uint8_t)(((8 - fx) * (8 - fy) * r0[x0] + fx * (8 - fy) * r0[x1] +
                                                             (8 - fx) * fy * r1[x0] + fx * fy * r1[x1] + 32) >> 6);
        }
    }
}

/* -------------------------------------------------------- motion vectors */

typedef struct {
    bool avail;
    int ref;            /* -1: intra or outside */
    int mv[2];
} h264_nb_t;

/* The motion of 4x4 block (bx, by) as a neighbour of the block being
 * decoded: blocks of this macroblock count once they have their vector. */
H264_FN h264_nb_t h264_neighbour(const h264_t* d, int mbx, int mby, int bx, int by) {
    h264_nb_t n = { false, -1, { 0, 0 } };
    if (bx < 0 || by < 0 || bx >= (int)d->mb_w * 4 || by >= (int)d->mb_h * 4) return n;
    if ((bx >> 2) == mbx && (by >> 2) == mby) {
        if (!(d->done & (1u << ((by & 3) * 4 + (bx & 3))))) return n;
    } else if (h264_mb_at(d, bx, by) < 0) {
        return n;
    }
    size_t at = (size_t)by * d->mb_w * 4u + (size_t)bx;
    n.avail = true;
    n.ref = d->ref_idx[at];
    if (n.ref >= 0) { n.mv[0] = d->mv[at * 2u]; n.mv[1] = d->mv[at * 2u + 1u]; }
    return n;
}

/* Predicts the vector of a partition at block (bx, by), bw blocks wide,
 * using reference `ref`. shape: 0 median, 1/2 upper/lower 16x8, 3/4
 * left/right 8x16. */
H264_FN void h264_predict_mv(const h264_t* d, int mbx, int mby, int bx, int by, int bw, int ref, int shape,
                             int out[2]) {
    h264_nb_t a = h264_neighbour(d, mbx, mby, bx - 1, by);
    h264_nb_t b = h264_neighbour(d, mbx, mby, bx, by - 1);
    h264_nb_t c = h264_neighbour(d, mbx, mby, bx + bw, by - 1);
    if (!c.avail) c = h264_neighbour(d, mbx, mby, bx - 1, by - 1);
    if (shape == 1 && b.ref == ref) { out[0] = b.mv[0]; out[1] = b.mv[1]; return; }
    if ((shape == 2 || shape == 3) && a.ref == ref) { out[0] = a.mv[0]; out[1] = a.mv[1]; return; }
    if (shape == 4 && c.ref == ref) { out[0] = c.mv[0]; out[1] = c.mv[1]; return; }
    if (!b.avail && !c.avail && a.avail) { out[0] = a.mv[0]; out[1] = a.mv[1]; return; }
    int same = (a.ref == ref) + (b.ref == ref) + (c.ref == ref);
    if (same == 1) {
        const h264_nb_t* n = a.ref == ref ? &a : (b.ref == ref ? &b : &c);
        out[0] = n->mv[0]; out[1] = n->mv[1];
        return;
    }
    out[0] = h264_median(a.mv[0], b.mv[0], c.mv[0]);
    out[1] = h264_median(a.mv[1], b.mv[1], c.mv[1]);
}

/* Stores a partition's motion and predicts its pixels from the reference. */
H264_FN bool h264_inter_part(h264_t* d, int mbx, int mby, int bx, int by, int bw, int bh, int ref, const int mv[2]) {
    (void)mbx; (void)mby;
    if (ref < 0 || (uint32_t)ref >= d->list_len || !d->list[ref]) return h264_fail(d, "missing reference frame");
    const h264_pic_t* rp = d->list[ref];
    int pic_index = (int)(rp - d->pics);
    for (int y = 0; y < bh; y++) {
        for (int x = 0; x < bw; x++) {
            size_t at = (size_t)(by + y) * d->mb_w * 4u + (size_t)(bx + x);
            d->mv[at * 2u] = (int16_t)mv[0];
            d->mv[at * 2u + 1u] = (int16_t)mv[1];
            d->ref_idx[at] = (int8_t)ref;
            d->ref_pic[at] = (int8_t)pic_index;
            d->done |= 1u << (((by + y) & 3) * 4 + ((bx + x) & 3));
        }
    }
    int px = bx * 4, py = by * 4, w = bw * 4, h = bh * 4;
    /* Far outside the picture every sample is an edge sample anyway. */
    int mx = h264_clamp(mv[0], -4 * ((int)d->w + 64), 4 * ((int)d->w + 64));
    int my = h264_clamp(mv[1], -4 * ((int)d->h + 64), 4 * ((int)d->h + 64));
    h264_mc_luma(rp->plane[0], (int)d->w, (int)d->h, px + (mx >> 2), py + (my >> 2), mx & 3, my & 3, w, h,
                 d->cur->plane[0] + (size_t)py * d->w + (size_t)px, d->w);
    for (int p = 1; p < 3; p++)
        h264_mc_chroma(rp->plane[p], (int)d->cw, (int)d->ch, px / 2 + (mx >> 3), py / 2 + (my >> 3), mx & 7, my & 7,
                       w / 2, h / 2, d->cur->plane[p] + (size_t)(py / 2) * d->cw + (size_t)(px / 2), d->cw);
    return true;
}

/* ----------------------------------------------------------- macroblocks */

H264_FN int h264_qpc(const h264_t* d, int qp) {
    int q = h264_clamp(qp + d->pp->chroma_qp_offset, 0, 51);
    return q < 30 ? q : h264_chroma_qp[q - 30];
}

H264_FN void h264_mb_reset(h264_t* d, int mbx, int mby, bool intra) {
    int mb = mby * (int)d->mb_w + mbx;
    d->mb_slice[mb] = d->slice_id;
    d->mb_intra[mb] = intra;
    d->mb_qp[mb] = (uint8_t)d->qp;
    for (int i = 0; i < 3; i++) d->mb_filter[mb * 3 + i] = d->filter[i];
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            size_t at = (size_t)(mby * 4 + y) * d->mb_w * 4u + (size_t)(mbx * 4 + x);
            d->i4mode[at] = -1;
            d->nz_y[at] = 0;
            d->ref_idx[at] = -1;
            d->ref_pic[at] = -1;
            d->mv[at * 2u] = d->mv[at * 2u + 1u] = 0;
        }
    }
    for (int p = 0; p < 2; p++)
        for (int y = 0; y < 2; y++)
            for (int x = 0; x < 2; x++) d->nz_c[p][(size_t)(mby * 2 + y) * d->mb_w * 2u + (size_t)(mbx * 2 + x)] = 0;
    d->done = 0;
}

/* A neighbouring macroblock's pixels may be used for intra prediction. */
H264_FN bool h264_intra_ok(const h264_t* d, int mbx, int mby) {
    if (mbx < 0 || mby < 0 || mbx >= (int)d->mb_w || mby >= (int)d->mb_h) return false;
    int mb = mby * (int)d->mb_w + mbx;
    return d->mb_slice[mb] == d->slice_id && (!d->pp->constrained_intra || d->mb_intra[mb]);
}

H264_FN bool h264_skip_mb(h264_t* d, int mbx, int mby) {
    h264_mb_reset(d, mbx, mby, false);
    int mv[2] = { 0, 0 };
    h264_nb_t a = h264_neighbour(d, mbx, mby, mbx * 4 - 1, mby * 4);
    h264_nb_t b = h264_neighbour(d, mbx, mby, mbx * 4, mby * 4 - 1);
    if (a.avail && b.avail && !(a.ref == 0 && a.mv[0] == 0 && a.mv[1] == 0) &&
        !(b.ref == 0 && b.mv[0] == 0 && b.mv[1] == 0))
        h264_predict_mv(d, mbx, mby, mbx * 4, mby * 4, 4, 0, 0, mv);
    return h264_inter_part(d, mbx, mby, mbx * 4, mby * 4, 4, 4, 0, mv);
}

H264_FN int h264_read_ref(h264_bits_t* b, uint32_t active) {
    if (active <= 1u) return 0;
    if (active == 2u) return h264_u(b, 1) ? 0 : 1;
    return (int)h264_ue(b);
}

H264_FN bool h264_macroblock(h264_t* d, h264_bits_t* b, int mbx, int mby) {
    uint32_t type = h264_ue(b);
    bool intra = !d->slice_p || type >= 5u;
    if (d->slice_p && intra) type -= 5u;
    if (b->err || (intra && type > 25u)) return h264_fail(d, "damaged macroblock");
    h264_mb_reset(d, mbx, mby, intra);
    uint8_t* Y = d->cur->plane[0] + (size_t)mby * 16u * d->w + (size_t)mbx * 16u;
    uint8_t* C[2] = { d->cur->plane[1] + (size_t)mby * 8u * d->cw + (size_t)mbx * 8u,
                      d->cur->plane[2] + (size_t)mby * 8u * d->cw + (size_t)mbx * 8u };
    int mb = mby * (int)d->mb_w + mbx;

    if (intra && type == 25u) {                 /* I_PCM: the samples themselves */
        b->pos = (b->pos + 7u) & ~7u;
        if (b->pos + 384u * 8u > b->bits) return h264_fail(d, "damaged macroblock");
        const uint8_t* src = b->p + (b->pos >> 3);
        for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) Y[(size_t)y * d->w + x] = *src++;
        for (int p = 0; p < 2; p++)
            for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) C[p][(size_t)y * d->cw + x] = *src++;
        b->pos += 384u * 8u;
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) d->nz_y[(size_t)(mby * 4 + y) * d->mb_w * 4u + (size_t)(mbx * 4 + x)] = 16;
        for (int p = 0; p < 2; p++)
            for (int y = 0; y < 2; y++)
                for (int x = 0; x < 2; x++)
                    d->nz_c[p][(size_t)(mby * 2 + y) * d->mb_w * 2u + (size_t)(mbx * 2 + x)] = 16;
        d->mb_qp[mb] = 0;
        return true;
    }

    bool i16 = intra && type >= 1u;
    int i16_mode = 0, cbp = 0, chroma_mode = 0;
    int8_t modes[16];
    if (i16) {
        i16_mode = (int)((type - 1u) & 3u);
        cbp = (int)(((type - 1u) / 4u) % 3u) << 4 | (type >= 13u ? 15 : 0);
    }
    if (intra) {
        if (!i16) {
            for (int k = 0; k < 16; k++) {
                int bx = mbx * 4 + h264_blk_x[k], by = mby * 4 + h264_blk_y[k];
                /* The more likely mode: the smaller of the left and upper
                 * block's, DC when either cannot be used. */
                int pred = 2;
                bool la = (bx & 3) ? true : h264_intra_ok(d, mbx - 1, mby);
                bool lb = (by & 3) ? true : h264_intra_ok(d, mbx, mby - 1);
                if (la && lb) {
                    int ma = d->i4mode[(size_t)by * d->mb_w * 4u + (size_t)(bx - 1)];
                    int mb2 = d->i4mode[(size_t)(by - 1) * d->mb_w * 4u + (size_t)bx];
                    if (ma < 0) ma = 2;
                    if (mb2 < 0) mb2 = 2;
                    pred = ma < mb2 ? ma : mb2;
                }
                int mode = pred;
                if (!h264_u(b, 1)) {
                    int rem = (int)h264_u(b, 3);
                    mode = rem < pred ? rem : rem + 1;
                }
                modes[k] = (int8_t)mode;
                d->i4mode[(size_t)by * d->mb_w * 4u + (size_t)bx] = (int8_t)mode;
            }
        }
        chroma_mode = (int)h264_ue(b);
        if (b->err || chroma_mode > 3) return h264_fail(d, "damaged macroblock");
    } else {
        /* Partitions: references first, then vector differences. */
        int bx0 = mbx * 4, by0 = mby * 4;
        if (type <= 2u) {
            int parts = type == 0u ? 1 : 2, ref[2], mv[2], pred[2];
            for (int i = 0; i < parts; i++) ref[i] = h264_read_ref(b, d->list_len);
            for (int i = 0; i < parts; i++) {
                int x = type == 2u ? i * 2 : 0, y = type == 1u ? i * 2 : 0;
                int w = type == 2u ? 2 : 4, h = type == 1u ? 2 : 4;
                int shape = type == 0u ? 0 : (type == 1u ? 1 + i : 3 + i);
                h264_predict_mv(d, mbx, mby, bx0 + x, by0 + y, w, ref[i], shape, pred);
                mv[0] = pred[0] + h264_se(b);
                mv[1] = pred[1] + h264_se(b);
                if (b->err) return h264_fail(d, "damaged macroblock");
                if (!h264_inter_part(d, mbx, mby, bx0 + x, by0 + y, w, h, ref[i], mv)) return false;
            }
        } else if (type <= 4u) {
            uint32_t sub[4];
            int ref[4] = { 0, 0, 0, 0 };
            for (int i = 0; i < 4; i++) {
                sub[i] = h264_ue(b);
                if (sub[i] > 3u) return h264_fail(d, "damaged macroblock");
            }
            if (type == 3u) for (int i = 0; i < 4; i++) ref[i] = h264_read_ref(b, d->list_len);
            for (int i = 0; i < 4; i++) {
                int qx = (i & 1) * 2, qy = (i >> 1) * 2;
                int w = sub[i] == 0u || sub[i] == 1u ? 2 : 1, h = sub[i] == 0u || sub[i] == 2u ? 2 : 1;
                for (int y = 0; y < 2; y += h) {
                    for (int x = 0; x < 2; x += w) {
                        int mv[2], pred[2];
                        h264_predict_mv(d, mbx, mby, bx0 + qx + x, by0 + qy + y, w, ref[i], 0, pred);
                        mv[0] = pred[0] + h264_se(b);
                        mv[1] = pred[1] + h264_se(b);
                        if (b->err) return h264_fail(d, "damaged macroblock");
                        if (!h264_inter_part(d, mbx, mby, bx0 + qx + x, by0 + qy + y, w, h, ref[i], mv))
                            return false;
                    }
                }
            }
        } else {
            return h264_fail(d, "damaged macroblock");
        }
    }
    if (!i16) {
        uint32_t code = h264_ue(b);
        if (b->err || code > 47u) return h264_fail(d, "damaged macroblock");
        cbp = intra ? h264_cbp_intra[code] : h264_cbp_inter[code];
    }
    if (cbp || i16) {
        int delta = h264_se(b);
        if (b->err || delta < -26 || delta > 25) return h264_fail(d, "damaged macroblock");
        d->qp = (d->qp + delta + 52) % 52;
    }
    d->mb_qp[mb] = (uint8_t)d->qp;
    int qp = d->qp, qpc = h264_qpc(d, qp);
    bool top_ok = h264_intra_ok(d, mbx, mby - 1), left_ok = h264_intra_ok(d, mbx - 1, mby);

    /* Luma. */
    int32_t dc[16];
    if (i16) {
        if ((i16_mode == 0 && !top_ok) || (i16_mode == 1 && !left_ok) ||
            (i16_mode == 3 && (!top_ok || !left_ok || !h264_intra_ok(d, mbx - 1, mby - 1))))
            return h264_fail(d, "damaged macroblock");
        h264_pred_big(Y, d->w, 16, i16_mode, top_ok, left_ok);
        int32_t c[16];
        for (int i = 0; i < 16; i++) c[i] = 0;
        if (h264_residual(b, h264_nc(d, d->nz_y, (int)d->mb_w * 4, mbx * 4, mby * 4, 0), 0, 16, c, false) < 0)
            return h264_fail(d, "damaged coefficients");
        /* The 16 DC coefficients have a transform of their own. */
        int32_t t[16];
        for (int i = 0; i < 4; i++) {
            int32_t* r = c + i * 4;
            int a0 = r[0] + r[1], a1 = r[2] + r[3], a2 = r[0] - r[1], a3 = r[2] - r[3];
            t[i * 4] = a0 + a1; t[i * 4 + 1] = a0 - a1; t[i * 4 + 2] = a2 - a3; t[i * 4 + 3] = a2 + a3;
        }
        int scale = h264_scale[qp % 6][0] * 16;
        for (int i = 0; i < 4; i++) {
            int a0 = t[i] + t[4 + i], a1 = t[8 + i] + t[12 + i], a2 = t[i] - t[4 + i], a3 = t[8 + i] - t[12 + i];
            int o[4] = { a0 + a1, a0 - a1, a2 - a3, a2 + a3 };
            for (int j = 0; j < 4; j++)
                dc[j * 4 + i] = qp >= 36 ? (int32_t)((uint32_t)(o[j] * scale) << (qp / 6 - 6))
                                         : (o[j] * scale + (1 << (5 - qp / 6))) >> (6 - qp / 6);
        }
    }
    for (int k = 0; k < 16; k++) {
        int x = h264_blk_x[k], y = h264_blk_y[k];
        int bx = mbx * 4 + x, by = mby * 4 + y;
        uint8_t* dst = Y + (size_t)y * 4u * d->w + (size_t)x * 4u;
        if (intra && !i16) {
            /* Predict from the pixels around the block, as they are now. */
            bool ht = y ? true : top_ok, hl = x ? true : left_ok;
            bool hc = x && y ? true : (x ? top_ok : (y ? left_ok : h264_intra_ok(d, mbx - 1, mby - 1)));
            /* Upper right: decoded already? Inside the macroblock that
             * depends on the coding order; above it, on the neighbours. */
            bool hr;
            if (y == 0) {
                hr = x < 3 ? top_ok : h264_intra_ok(d, mbx + 1, mby - 1);
            } else {
                hr = false;
                for (int j = 0; j < k && x < 3; j++)
                    if (h264_blk_x[j] == x + 1 && h264_blk_y[j] == y - 1) hr = true;
            }
            uint8_t top[8], left[4];
            int mode = modes[k];
            for (int i = 0; i < 4; i++) top[i] = ht ? dst[-(ptrdiff_t)d->w + i] : 128;
            for (int i = 0; i < 4; i++) top[4 + i] = hr ? dst[-(ptrdiff_t)d->w + 4 + i] : top[3];
            for (int i = 0; i < 4; i++) left[i] = hl ? dst[(size_t)i * d->w - 1] : 128;
            int corner = hc ? dst[-(ptrdiff_t)d->w - 1] : 128;
            bool need_top = mode == 0 || mode == 3 || mode == 7 || mode == 4 || mode == 5 || mode == 6;
            bool need_left = mode == 1 || mode == 8 || mode == 4 || mode == 5 || mode == 6;
            if (mode > 8 || (need_top && !ht) || (need_left && !hl) || ((mode >= 4 && mode <= 6) && !hc))
                return h264_fail(d, "damaged macroblock");
            h264_pred4(dst, d->w, mode, top, left, corner, ht, hl);
        }
        int32_t c[16];
        for (int i = 0; i < 16; i++) c[i] = 0;
        if (cbp & (1 << ((y >> 1) * 2 + (x >> 1)))) {
            int n = h264_residual(b, h264_nc(d, d->nz_y, (int)d->mb_w * 4, bx, by, 0), i16 ? 1 : 0, i16 ? 15 : 16,
                                  c, false);
            if (n < 0) return h264_fail(d, "damaged coefficients");
            d->nz_y[(size_t)by * d->mb_w * 4u + (size_t)bx] = (uint8_t)n;
        }
        if (i16) c[0] = dc[y * 4 + x];
        h264_add_block(dst, d->w, c, qp, i16);
    }

    /* Colour. */
    if (intra) {
        if ((chroma_mode == 2 && !top_ok) || (chroma_mode == 1 && !left_ok) ||
            (chroma_mode == 3 && (!top_ok || !left_ok || !h264_intra_ok(d, mbx - 1, mby - 1))))
            return h264_fail(d, "damaged macroblock");
        /* Colour modes are numbered DC, horizontal, vertical, plane. */
        int m = chroma_mode == 0 ? 2 : (chroma_mode == 1 ? 1 : (chroma_mode == 2 ? 0 : 3));
        for (int p = 0; p < 2; p++) h264_pred_big(C[p], d->cw, 8, m, top_ok, left_ok);
    }
    int32_t cdc[2][4] = { { 0, 0, 0, 0 }, { 0, 0, 0, 0 } };
    if (cbp & 0x30) {
        for (int p = 0; p < 2; p++) {
            int32_t c[4] = { 0, 0, 0, 0 };
            if (h264_residual(b, 0, 0, 4, c, true) < 0) return h264_fail(d, "damaged coefficients");
            int f[4] = { c[0] + c[1] + c[2] + c[3], c[0] - c[1] + c[2] - c[3], c[0] + c[1] - c[2] - c[3],
                         c[0] - c[1] - c[2] + c[3] };
            int scale = h264_scale[qpc % 6][0] * 16;
            for (int i = 0; i < 4; i++) cdc[p][i] = (int32_t)(((uint32_t)(f[i] * scale) << (qpc / 6))) >> 5;
        }
    }
    for (int p = 0; p < 2; p++) {
        for (int k = 0; k < 4; k++) {
            int x = k & 1, y = k >> 1;
            int32_t c[16];
            for (int i = 0; i < 16; i++) c[i] = 0;
            if (cbp & 0x20) {
                int bx = mbx * 2 + x, by = mby * 2 + y;
                int n = h264_residual(b, h264_nc(d, d->nz_c[p], (int)d->mb_w * 2, bx, by, 1), 1, 15, c, false);
                if (n < 0) return h264_fail(d, "damaged coefficients");
                d->nz_c[p][(size_t)by * d->mb_w * 2u + (size_t)bx] = (uint8_t)n;
            }
            c[0] = cdc[p][k];
            h264_add_block(C[p] + (size_t)y * 4u * d->cw + (size_t)x * 4u, d->cw, c, qpc, true);
        }
    }
    return !b->err || h264_fail(d, "damaged macroblock");
}

/* ----------------------------------------------------------- deblocking */

H264_FN void h264_filter_edge(uint8_t* p, ptrdiff_t step, ptrdiff_t along, int count, const uint8_t bs[4], int qp,
                              int oa, int ob, bool chroma) {
    int ia = h264_clamp(qp + oa, 0, 51), ib = h264_clamp(qp + ob, 0, 51);
    if (ia < 16 || ib < 16) return;
    int alpha = h264_alpha[ia - 16], beta = h264_beta[ib - 16];
    for (int i = 0; i < count; i++, p += along) {
        int s = bs[chroma ? i >> 1 : i >> 2];
        if (!s) continue;
        int p0 = p[-step], p1 = p[-2 * step], q0 = p[0], q1 = p[step];
        if (h264_abs(p0 - q0) >= alpha || h264_abs(p1 - p0) >= beta || h264_abs(q1 - q0) >= beta) continue;
        if (s < 4) {
            int tc0 = ia >= 17 ? h264_tc0[ia - 17][s - 1] : 0, tc;
            if (chroma) {
                tc = tc0 + 1;
            } else {
                int p2 = p[-3 * step], q2 = p[2 * step];
                bool ap = h264_abs(p2 - p0) < beta, aq = h264_abs(q2 - q0) < beta;
                tc = tc0 + ap + aq;
                if (ap) p[-2 * step] = (uint8_t)(p1 + h264_clamp((p2 + ((p0 + q0 + 1) >> 1) - 2 * p1) >> 1, -tc0, tc0));
                if (aq) p[step] = (uint8_t)(q1 + h264_clamp((q2 + ((p0 + q0 + 1) >> 1) - 2 * q1) >> 1, -tc0, tc0));
            }
            int delta = h264_clamp(((q0 - p0) * 4 + (p1 - q1) + 4) >> 3, -tc, tc);
            p[-step] = (uint8_t)h264_clip(p0 + delta);
            p[0] = (uint8_t)h264_clip(q0 - delta);
        } else if (chroma) {
            p[-step] = (uint8_t)((2 * p1 + p0 + q1 + 2) >> 2);
            p[0] = (uint8_t)((2 * q1 + q0 + p1 + 2) >> 2);
        } else {
            int p2 = p[-3 * step], q2 = p[2 * step];
            bool strong = h264_abs(p0 - q0) < (alpha >> 2) + 2;
            if (strong && h264_abs(p2 - p0) < beta) {
                int p3 = p[-4 * step];
                p[-step] = (uint8_t)((p2 + 2 * p1 + 2 * p0 + 2 * q0 + q1 + 4) >> 3);
                p[-2 * step] = (uint8_t)((p2 + p1 + p0 + q0 + 2) >> 2);
                p[-3 * step] = (uint8_t)((2 * p3 + 3 * p2 + p1 + p0 + q0 + 4) >> 3);
            } else {
                p[-step] = (uint8_t)((2 * p1 + p0 + q1 + 2) >> 2);
            }
            if (strong && h264_abs(q2 - q0) < beta) {
                int q3 = p[3 * step];
                p[0] = (uint8_t)((p1 + 2 * p0 + 2 * q0 + 2 * q1 + q2 + 4) >> 3);
                p[step] = (uint8_t)((p0 + q0 + q1 + q2 + 2) >> 2);
                p[2 * step] = (uint8_t)((2 * q3 + 3 * q2 + q1 + q0 + p0 + 4) >> 3);
            } else {
                p[0] = (uint8_t)((2 * q1 + q0 + p1 + 2) >> 2);
            }
        }
    }
}

H264_FN void h264_deblock(h264_t* d) {
    uint32_t bw = d->mb_w * 4u;
    for (int mby = 0; mby < (int)d->mb_h; mby++) {
        for (int mbx = 0; mbx < (int)d->mb_w; mbx++) {
            int mb = mby * (int)d->mb_w + mbx;
            if (!d->mb_slice[mb]) continue;
            int idc = d->mb_filter[mb * 3], oa = d->mb_filter[mb * 3 + 1], ob = d->mb_filter[mb * 3 + 2];
            if (idc == 1) continue;
            for (int dir = 0; dir < 2; dir++) {         /* vertical edges, then horizontal */
                for (int e = 0; e < 4; e++) {
                    int nmb = mb;                       /* the macroblock on the other side */
                    if (e == 0) {
                        if (dir == 0 ? mbx == 0 : mby == 0) continue;
                        nmb = dir == 0 ? mb - 1 : mb - (int)d->mb_w;
                        if (!d->mb_slice[nmb] || (idc == 2 && d->mb_slice[nmb] != d->mb_slice[mb])) continue;
                    }
                    uint8_t bs[4];
                    bool any = false;
                    for (int k = 0; k < 4; k++) {
                        int qx = mbx * 4 + (dir == 0 ? e : k), qy = mby * 4 + (dir == 0 ? k : e);
                        int px = qx - (dir == 0), py = qy - (dir == 1);
                        size_t q = (size_t)qy * bw + (size_t)qx, pp = (size_t)py * bw + (size_t)px;
                        if (d->mb_intra[mb] || d->mb_intra[nmb]) bs[k] = e == 0 ? 4 : 3;
                        else if (d->nz_y[q] || d->nz_y[pp]) bs[k] = 2;
                        else bs[k] = d->ref_pic[q] != d->ref_pic[pp] ||
                                     h264_abs(d->mv[q * 2] - d->mv[pp * 2]) >= 4 ||
                                     h264_abs(d->mv[q * 2 + 1] - d->mv[pp * 2 + 1]) >= 4;
                        any = any || bs[k];
                    }
                    if (!any) continue;
                    int qy = (d->mb_qp[mb] + d->mb_qp[nmb] + 1) >> 1;
                    uint8_t* y = d->cur->plane[0] + (size_t)(mby * 16 + (dir ? e * 4 : 0)) * d->w +
                                 (size_t)(mbx * 16 + (dir ? 0 : e * 4));
                    h264_filter_edge(y, dir ? (ptrdiff_t)d->w : 1, dir ? 1 : (ptrdiff_t)d->w, 16, bs, qy, oa, ob,
                                     false);
                    if (e & 1) continue;
                    int qc = (h264_qpc(d, d->mb_qp[mb]) + h264_qpc(d, d->mb_qp[nmb]) + 1) >> 1;
                    for (int p = 1; p < 3; p++) {
                        uint8_t* c = d->cur->plane[p] + (size_t)(mby * 8 + (dir ? e * 2 : 0)) * d->cw +
                                     (size_t)(mbx * 8 + (dir ? 0 : e * 2));
                        h264_filter_edge(c, dir ? (ptrdiff_t)d->cw : 1, dir ? 1 : (ptrdiff_t)d->cw, 8, bs, qc, oa,
                                         ob, true);
                    }
                }
            }
        }
    }
}

/* ------------------------------------------------------ reference frames */

H264_FN void h264_pic_nums(h264_t* d, int frame_num) {
    for (uint32_t i = 0; i < d->pic_count; i++) {
        h264_pic_t* p = &d->pics[i];
        if (p->ref && !p->long_term) p->pic_num = p->frame_num > frame_num ? p->frame_num - d->max_frame_num : p->frame_num;
    }
}

H264_FN void h264_build_list(h264_t* d, uint32_t active) {
    uint32_t n = 0;
    /* Short-term frames, newest first; then long-term, lowest index first. */
    for (;;) {
        h264_pic_t* best = NULL;
        for (uint32_t i = 0; i < d->pic_count; i++) {
            h264_pic_t* p = &d->pics[i];
            if (p == d->cur || !p->ref || p->long_term) continue;
            bool used = false;
            for (uint32_t k = 0; k < n; k++) used = used || d->list[k] == p;
            if (!used && (!best || p->pic_num > best->pic_num)) best = p;
        }
        if (!best || n >= H264_MAX_REFS) break;
        d->list[n++] = best;
    }
    for (;;) {
        h264_pic_t* best = NULL;
        for (uint32_t i = 0; i < d->pic_count; i++) {
            h264_pic_t* p = &d->pics[i];
            if (p == d->cur || !p->ref || !p->long_term) continue;
            bool used = false;
            for (uint32_t k = 0; k < n; k++) used = used || d->list[k] == p;
            if (!used && (!best || p->lt_idx < best->lt_idx)) best = p;
        }
        if (!best || n >= H264_MAX_REFS) break;
        d->list[n++] = best;
    }
    for (uint32_t i = n; i <= H264_MAX_REFS; i++) d->list[i] = n ? d->list[0] : NULL;
    d->list_len = active;
}

/* Moves `pic` to position `at` of the list (the reordering commands). */
H264_FN void h264_list_insert(h264_t* d, uint32_t at, h264_pic_t* pic) {
    if (at >= d->list_len) return;
    h264_pic_t* tmp[H264_MAX_REFS + 2];
    uint32_t n = 0;
    for (uint32_t i = 0; i < at; i++) tmp[n++] = d->list[i];
    tmp[n++] = pic;
    for (uint32_t i = at; i < H264_MAX_REFS && n < H264_MAX_REFS + 1u; i++)
        if (d->list[i] != pic) tmp[n++] = d->list[i];
    for (uint32_t i = 0; i <= H264_MAX_REFS; i++) d->list[i] = i < n ? tmp[i] : tmp[0];
}

/* After a reference picture is decoded: which frames stay references. */
H264_FN void h264_mark(h264_t* d, int frame_num) {
    h264_pic_t* cur = d->cur;
    int max_refs = d->active->max_refs ? d->active->max_refs : 1;
    cur->ref = true;
    cur->long_term = false;
    cur->frame_num = frame_num;
    if (d->pic_idr) {
        for (uint32_t i = 0; i < d->pic_count; i++) if (&d->pics[i] != cur) d->pics[i].ref = false;
        d->max_lt_idx = -1;
        if (d->mark_long_term_idr) { cur->long_term = true; cur->lt_idx = 0; d->max_lt_idx = 0; }
        return;
    }
    if (d->mark_adaptive) {
        h264_pic_nums(d, frame_num);
        for (uint32_t k = 0; k < d->mmco_count; k++) {
            int op = d->mmco[k].op, a = d->mmco[k].a, b = d->mmco[k].b;
            for (uint32_t i = 0; i < d->pic_count; i++) {
                h264_pic_t* p = &d->pics[i];
                if (p == cur || !p->ref) continue;
                if (op == 1 && !p->long_term && p->pic_num == frame_num - (a + 1)) p->ref = false;
                else if (op == 2 && p->long_term && p->lt_idx == a) p->ref = false;
                else if (op == 3 && p->long_term && p->lt_idx == b) p->ref = false;
                else if (op == 4 && p->long_term && p->lt_idx >= a) p->ref = false;
                else if (op == 5) p->ref = false;
                else if (op == 6 && p->long_term && p->lt_idx == a) p->ref = false;
            }
            if (op == 3) {
                for (uint32_t i = 0; i < d->pic_count; i++) {
                    h264_pic_t* p = &d->pics[i];
                    if (p != cur && p->ref && !p->long_term && p->pic_num == frame_num - (a + 1)) {
                        p->long_term = true;
                        p->lt_idx = b;
                    }
                }
            } else if (op == 4) {
                d->max_lt_idx = a - 1;
            } else if (op == 5) {
                d->max_lt_idx = -1;
                cur->frame_num = 0;
            } else if (op == 6) {
                cur->long_term = true;
                cur->lt_idx = a;
            }
        }
    }
    /* Too many: the oldest short-term frame goes (the sliding window). */
    for (;;) {
        int count = 0;
        h264_pic_t* oldest = NULL;
        h264_pic_nums(d, cur->frame_num);
        for (uint32_t i = 0; i < d->pic_count; i++) {
            h264_pic_t* p = &d->pics[i];
            if (!p->ref) continue;
            count++;
            if (p != cur && !p->long_term && (!oldest || p->pic_num < oldest->pic_num)) oldest = p;
        }
        if (count <= max_refs) break;
        if (!oldest) {              /* only long-term frames left: drop one */
            for (uint32_t i = 0; i < d->pic_count && !oldest; i++)
                if (d->pics[i].ref && &d->pics[i] != cur) oldest = &d->pics[i];
            if (!oldest) break;
        }
        oldest->ref = false;
    }
}

/* ---------------------------------------------------------------- slices */

H264_FN void h264_finish_picture(h264_t* d, int frame_num) {
    if (!d->cur) return;
    if (!d->hw_only) h264_deblock(d);
    if (d->pic_is_ref) h264_mark(d, frame_num);
    d->out = d->cur;
    d->cur = NULL;
}

H264_FN bool h264_slice(h264_t* d, h264_bits_t* b, int nal_type, int nal_ref, int* frame_num_out) {
    uint32_t first_mb = h264_ue(b);
    uint32_t type = h264_ue(b) % 5u;
    uint32_t pps_id = h264_ue(b);
    if (b->err || pps_id >= H264_MAX_PPS || !d->pps[pps_id].valid || !d->sps[d->pps[pps_id].sps].valid)
        return h264_fail(d, "slice without its parameter sets");
    if (type != 0u && type != 2u) return h264_fail(d, type == 1u ? "B frames are not supported" : "unsupported slice type");
    const h264_pps_t* pp = &d->pps[pps_id];
    const h264_sps_t* sp = &d->sps[pp->sps];
    if (!h264_size(d, sp)) return false;
    bool idr = nal_type == 5;
    int frame_num = (int)h264_u(b, sp->log2_max_frame_num);
    if (idr) h264_ue(b);
    int poc_lsb = 0;
    if (sp->poc_type == 0) {
        poc_lsb = (int)h264_u(b, sp->log2_max_poc_lsb);
        if (pp->pic_order_present) h264_se(b);
    } else if (sp->poc_type == 1 && !sp->delta_pic_order_always_zero) {
        h264_se(b);
        if (pp->pic_order_present) h264_se(b);
    }
    if (pp->redundant_pic_cnt && h264_ue(b)) return true;       /* a redundant copy: not needed */

    if (!d->cur) {
        /* A new picture: any frame that is not a reference holds it. */
        if (first_mb != 0u && !d->out) return h264_fail(d, "the picture's first slice is missing");
        h264_pic_t* pic = NULL;
        for (uint32_t i = 0; i < d->pic_count && !pic; i++) if (!d->pics[i].ref) pic = &d->pics[i];
        if (!pic) {
            /* A damaged stream filled the buffer: start over from this picture. */
            for (uint32_t i = 0; i < d->pic_count; i++) d->pics[i].ref = false;
            pic = &d->pics[0];
        }
        if (idr) for (uint32_t i = 0; i < d->pic_count; i++) d->pics[i].ref = false;
        d->cur = pic;
        d->active = sp;
        d->max_frame_num = 1 << sp->log2_max_frame_num;
        d->pic_is_ref = nal_ref != 0;
        d->pic_idr = idr;
        d->slice_count = 0;
        d->mark_adaptive = false;
        d->mark_long_term_idr = false;
        d->mmco_count = 0;
        size_t mbs = (size_t)d->mb_w * d->mb_h;
        for (size_t i = 0; i < mbs; i++) d->mb_slice[i] = 0;

        /* The picture order count (8.2.1), for types 0 and 2. */
        bool poc_known = true;
        if (sp->poc_type == 0) {
            int max_lsb = 1 << sp->log2_max_poc_lsb, msb;
            if (idr) d->prev_poc_msb = d->prev_poc_lsb = 0;
            if (poc_lsb < d->prev_poc_lsb && d->prev_poc_lsb - poc_lsb >= max_lsb / 2) msb = d->prev_poc_msb + max_lsb;
            else if (poc_lsb > d->prev_poc_lsb && poc_lsb - d->prev_poc_lsb > max_lsb / 2) msb = d->prev_poc_msb - max_lsb;
            else msb = d->prev_poc_msb;
            pic->poc = msb + poc_lsb;
            if (nal_ref) { d->prev_poc_msb = msb; d->prev_poc_lsb = poc_lsb; }
        } else if (sp->poc_type == 2) {
            if (idr) d->frame_num_offset = 0;
            else if (d->prev_frame_num > frame_num) d->frame_num_offset += d->max_frame_num;
            pic->poc = idr ? 0 : 2 * (d->frame_num_offset + frame_num) - (nal_ref ? 0 : 1);
        } else {
            poc_known = false;
            pic->poc = 0;
        }
        d->prev_frame_num = frame_num;

        /* What a hardware decoder needs to know about the picture. */
        syscall_media_h264_t* hw = &d->hw;
        hw->mb_w = sp->mb_w; hw->mb_h = sp->mb_h;
        hw->log2_max_frame_num_minus4 = (uint8_t)(sp->log2_max_frame_num - 4);
        hw->poc_type = sp->poc_type;
        hw->log2_max_poc_lsb_minus4 = (uint8_t)(sp->poc_type == 0 ? sp->log2_max_poc_lsb - 4 : 0);
        hw->delta_pic_order_always_zero = sp->delta_pic_order_always_zero;
        hw->direct_8x8_inference = sp->direct_8x8_inference;
        hw->max_refs = sp->max_refs;
        hw->entropy_coding_mode = 0;
        hw->pic_order_present = pp->pic_order_present;
        hw->num_ref_l0_default_minus1 = (uint8_t)(pp->num_ref_default - 1u);
        hw->num_ref_l1_default_minus1 = pp->num_ref_l1_default;
        hw->deblocking_control = pp->deblocking_control;
        hw->redundant_pic_cnt_present = pp->redundant_pic_cnt;
        hw->transform_8x8 = 0;
        hw->weighted_pred = 0;
        hw->weighted_bipred_idc = pp->weighted_bipred_idc;
        hw->constrained_intra = pp->constrained_intra;
        hw->pic_init_qp_minus26 = (int8_t)(pp->init_qp - 26);
        hw->chroma_qp_offset = pp->chroma_qp_offset;
        hw->second_chroma_qp_offset = pp->chroma_qp_offset;
        hw->is_reference = nal_ref != 0;
        hw->frame_num = (uint32_t)frame_num;
        hw->poc = pic->poc;
        hw->cur_index = (uint8_t)(pic - d->pics);
        hw->flags = idr ? SYS_MEDIA_H264_RESET : 0u;
        hw->ref_count = 0;
        for (uint32_t i = 0; i < d->pic_count && hw->ref_count < 16u; i++) {
            const h264_pic_t* r = &d->pics[i];
            if (r == pic || !r->ref) continue;
            syscall_media_h264_ref_t* e = &hw->refs[hw->ref_count++];
            e->index = (uint8_t)i;
            e->long_term = r->long_term;
            e->reserved = 0;
            e->frame_num = (uint32_t)(r->long_term ? r->lt_idx : r->frame_num);
            e->poc = r->poc;
        }
        d->hw_valid = poc_known;

        /* Macroblocks no slice covers keep the last picture's pixels. */
        if (!d->hw_only && d->out && d->out != pic && !idr) {
            for (int p = 0; p < 3; p++) {
                size_t n = p ? (size_t)d->cw * d->ch : (size_t)d->w * d->h;
                for (size_t i = 0; i < n; i++) pic->plane[p][i] = d->out->plane[p][i];
            }
        }
    }
    *frame_num_out = frame_num;
    d->pp = pp;
    d->slice_p = type == 0u;
    if (d->slice_count >= 0xFFFEu) return h264_fail(d, "too many slices");
    d->slice_id = (uint16_t)(++d->slice_count);

    uint32_t active = pp->num_ref_default;
    if (d->slice_p) {
        if (h264_u(b, 1)) active = h264_ue(b) + 1u;
        if (b->err || active > H264_MAX_REFS) return h264_fail(d, "damaged slice header");
        h264_pic_nums(d, frame_num);
        h264_build_list(d, active);
        if (h264_u(b, 1)) {                     /* reordering commands */
            int pred = frame_num;
            for (uint32_t at = 0;; at++) {
                uint32_t op = h264_ue(b);
                if (op == 3u) break;
                if (b->err || op > 3u || at > 64u) return h264_fail(d, "damaged slice header");
                uint32_t v = h264_ue(b);
                h264_pic_t* found = NULL;
                if (op < 2u) {
                    int diff = (int)(v + 1u);
                    pred = op == 0u ? pred - diff : pred + diff;
                    if (pred < 0) pred += d->max_frame_num;
                    if (pred >= d->max_frame_num) pred -= d->max_frame_num;
                    int num = pred > frame_num ? pred - d->max_frame_num : pred;
                    for (uint32_t i = 0; i < d->pic_count; i++) {
                        h264_pic_t* p = &d->pics[i];
                        if (p != d->cur && p->ref && !p->long_term && p->pic_num == num) found = p;
                    }
                } else {
                    for (uint32_t i = 0; i < d->pic_count; i++) {
                        h264_pic_t* p = &d->pics[i];
                        if (p != d->cur && p->ref && p->long_term && p->lt_idx == (int)v) found = p;
                    }
                }
                if (!found) return h264_fail(d, "missing reference frame");
                h264_list_insert(d, at, found);
            }
        }
        if (!d->list[0]) return h264_fail(d, "missing reference frame");
    } else {
        d->list_len = 0;
    }
    if (nal_ref) {
        if (idr) {
            h264_u(b, 1);
            d->mark_long_term_idr = h264_u(b, 1);
        } else if (h264_u(b, 1)) {
            d->mark_adaptive = true;
            uint32_t n = 0;
            for (;;) {
                uint32_t op = h264_ue(b);
                if (op == 0u) break;
                if (b->err || op > 6u || n >= 32u) return h264_fail(d, "damaged slice header");
                int a = 0, bb = 0;
                if (op == 1u || op == 3u) a = (int)h264_ue(b);
                if (op == 2u) a = (int)h264_ue(b);
                if (op == 3u) bb = (int)h264_ue(b);
                if (op == 4u || op == 6u) a = (int)h264_ue(b);
                d->mmco[n].op = (uint8_t)op; d->mmco[n].a = a; d->mmco[n].b = bb;
                n++;
            }
            d->mmco_count = (uint8_t)n;
        }
    }
    d->qp = pp->init_qp + h264_se(b);
    d->filter[0] = d->filter[1] = d->filter[2] = 0;
    if (pp->deblocking_control) {
        uint32_t idc = h264_ue(b);
        if (idc > 2u) return h264_fail(d, "damaged slice header");
        d->filter[0] = (int8_t)idc;
        if (idc != 1u) {
            int a = h264_se(b), be = h264_se(b);
            if (a < -6 || a > 6 || be < -6 || be > 6) return h264_fail(d, "damaged slice header");
            d->filter[1] = (int8_t)(a * 2);
            d->filter[2] = (int8_t)(be * 2);
        }
    }
    uint32_t total = d->mb_w * d->mb_h;
    if (b->err || d->qp < 0 || d->qp > 51 || first_mb >= total) return h264_fail(d, "damaged slice header");

    if (d->hw_only) return !b->err || h264_fail(d, "damaged slice header");

    /* Where the data ends: the last 1 bit is the stop bit. */
    uint32_t end = b->bits;
    while (end > b->pos && !((b->p[(end - 1u) >> 3] >> (7u - ((end - 1u) & 7u))) & 1u)) end--;
    if (end > b->pos) end--;
    uint32_t mb = first_mb;
    while (b->pos < end && mb < total) {
        if (d->slice_p) {
            uint32_t run = h264_ue(b);
            if (b->err || run > total - mb) return h264_fail(d, "damaged slice");
            for (uint32_t i = 0; i < run; i++, mb++)
                if (!h264_skip_mb(d, (int)(mb % d->mb_w), (int)(mb / d->mb_w))) return false;
            if (b->pos >= end || mb >= total) break;
        }
        if (!h264_macroblock(d, b, (int)(mb % d->mb_w), (int)(mb / d->mb_w))) return false;
        mb++;
    }
    return !b->err || h264_fail(d, "damaged slice");
}

/* Decodes one access unit (the NAL units of one frame, each behind a
 * 00 00 01 start code). True when it held a picture, which then is the
 * one h264_to_argb shows. Parameter sets alone return false without an
 * error (h264_error is ""). */
H264_FN bool h264_decode(h264_t* d, const uint8_t* data, size_t size) {
    d->error = NULL;
    d->hw_valid = false;
    d->hw.slice_count = 0;
    d->hw.data = d->hw_data;
    d->hw.data_bytes = 0;
    bool hw_fits = true;
    if (!d->rbsp || !d->hw_data || size > d->rbsp_cap) return h264_fail(d, "access unit too large");
    int frame_num = 0;
    bool ok = true, picture = false;
    size_t i = 0;
    while (ok) {
        /* The next start code. */
        while (i + 3 <= size && !(data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)) i++;
        if (i + 3 >= size) break;
        i += 3;
        size_t end = i;
        while (end + 3 <= size && !(data[end] == 0 && data[end + 1] == 0 && data[end + 2] <= 1)) end++;
        if (end + 3 > size) end = size;
        int header = data[i];
        int nal_type = header & 31, nal_ref = (header >> 5) & 3;
        /* Without the emulation prevention bytes (00 00 03 -> 00 00). */
        size_t n = 0;
        int zeros = 0;
        for (size_t k = i + 1; k < end; k++) {
            if (zeros >= 2 && data[k] == 3) { zeros = 0; continue; }
            zeros = data[k] == 0 ? zeros + 1 : 0;
            d->rbsp[n++] = data[k];
        }
        h264_bits_t b = { d->rbsp, (uint32_t)n * 8u, 0, false };
        if (nal_type == 7) ok = h264_parse_sps(d, &b);
        else if (nal_type == 8) ok = h264_parse_pps(d, &b);
        else if (nal_type == 1 || nal_type == 5) {
            ok = h264_slice(d, &b, nal_type, nal_ref, &frame_num);
            picture = picture || d->cur != NULL;
            /* The slice as it is, behind a start code, for a hardware decoder. */
            size_t bytes = end - i;
            if (ok && d->hw.slice_count < SYS_MEDIA_H264_MAX_SLICES && d->hw.data_bytes + 3u + bytes <= d->rbsp_cap) {
                uint8_t* to = d->hw_data + d->hw.data_bytes;
                d->hw.slice_offset[d->hw.slice_count++] = d->hw.data_bytes;
                to[0] = 0; to[1] = 0; to[2] = 1;
                for (size_t k = 0; k < bytes; k++) to[3 + k] = data[i + k];
                d->hw.data_bytes += 3u + (uint32_t)bytes;
            } else {
                hw_fits = false;
            }
        } else if (nal_type >= 2 && nal_type <= 4) {
            ok = h264_fail(d, "data partitioning is not supported");
        }
        i = end;
    }
    if (!ok) {
        /* Drop the damaged picture; references stay as they were. */
        d->cur = NULL;
        return false;
    }
    if (!picture || !d->cur) return false;
    h264_finish_picture(d, frame_num);
    d->hw_valid = d->hw_valid && hw_fits && d->hw.slice_count > 0;
    return true;
}

/* ---------------------------------------------------------------- output */

/* The size of the shown picture (after cropping). */
H264_FN uint32_t h264_width(const h264_t* d) {
    return d->active ? d->w - d->active->crop_l - d->active->crop_r : 0u;
}
H264_FN uint32_t h264_height(const h264_t* d) {
    return d->active ? d->h - d->active->crop_t - d->active->crop_b : 0u;
}

/* True when the stream is full-range BT.601, the colour space the motion
 * codec uses too (so the same conversion, on the CPU or GPU, applies). */
H264_FN bool h264_is_full_601(const h264_t* d) {
    if (!d->active || !d->active->full_range) return false;
    uint8_t m = d->active->matrix;
    return m == 5 || m == 6 || (m == 2 && h264_height(d) < 720u);
}

/* The last decoded picture as 0xFFRRGGBB pixels (stride in pixels). The
 * colour planes are upsampled as mmc_to_argb does; the matrix and range
 * are the stream's (BT.601 or BT.709, full or limited). */
H264_FN void h264_to_argb(const h264_t* d, uint32_t* out, uint32_t width, uint32_t height, uint32_t stride) {
    if (!d->out || !d->active) return;
    const h264_sps_t* s = d->active;
    if (width > h264_width(d)) width = h264_width(d);
    if (height > h264_height(d)) height = h264_height(d);
    bool bt709 = s->matrix == 1 || (s->matrix == 2 && h264_height(d) >= 720u);
    /* Fixed point, 1/1024: luma scale, then Cr->R, Cb->G, Cr->G, Cb->B. */
    static const int k[4][5] = {
        { 1024, 1436, 352, 731, 1815 },         /* BT.601 full */
        { 1192, 1634, 401, 832, 2066 },         /* BT.601 limited */
        { 1024, 1613, 192, 479, 1900 },         /* BT.709 full */
        { 1192, 1836, 218, 546, 2163 },         /* BT.709 limited */
    };
    const int* c = k[(bt709 ? 2 : 0) + (s->full_range ? 0 : 1)];
    int yoff = s->full_range ? 0 : 16;
    const uint8_t* Y = d->out->plane[0] + (size_t)s->crop_t * d->w + s->crop_l;
    const uint8_t* U = d->out->plane[1] + (size_t)(s->crop_t / 2u) * d->cw + s->crop_l / 2u;
    const uint8_t* V = d->out->plane[2] + (size_t)(s->crop_t / 2u) * d->cw + s->crop_l / 2u;
    int cw = (int)(d->cw - s->crop_l / 2u), ch = (int)(d->ch - s->crop_t / 2u);
    bool exact601 = !bt709 && s->full_range;
    for (uint32_t y = 0; y < height; y++) {
        int cy = (int)(y >> 1);
        int cy2 = h264_clamp(y & 1u ? cy + 1 : cy - 1, 0, ch - 1);
        const uint8_t *u0 = U + (size_t)cy * d->cw, *u1 = U + (size_t)cy2 * d->cw;
        const uint8_t *v0 = V + (size_t)cy * d->cw, *v1 = V + (size_t)cy2 * d->cw;
        const uint8_t* row = Y + (size_t)y * d->w;
        uint32_t* o = out + (size_t)y * stride;
        for (uint32_t x = 0; x < width; x++) {
            int cx = (int)(x >> 1);
            int cx2 = h264_clamp(x & 1u ? cx + 1 : cx - 1, 0, cw - 1);
            int u = (9 * u0[cx] + 3 * u0[cx2] + 3 * u1[cx] + u1[cx2] + 8) >> 4;
            int v = (9 * v0[cx] + 3 * v0[cx2] + 3 * v1[cx] + v1[cx2] + 8) >> 4;
            int cb = u - 128, cr = v - 128, r, g, b;
            if (exact601) {                     /* the motion codec's arithmetic, bit for bit */
                int lum = row[x];
                r = lum + ((359 * cr + 128) >> 8);
                g = lum - ((88 * cb + 183 * cr + 128) >> 8);
                b = lum + ((454 * cb + 128) >> 8);
            } else {
                int lum = (row[x] - yoff) * c[0];
                r = (lum + c[1] * cr + 512) >> 10;
                g = (lum - c[2] * cb - c[3] * cr + 512) >> 10;
                b = (lum + c[4] * cb + 512) >> 10;
            }
            o[x] = 0xFF000000u | (uint32_t)h264_clip(r) << 16 | (uint32_t)h264_clip(g) << 8 | (uint32_t)h264_clip(b);
        }
    }
}

#endif
