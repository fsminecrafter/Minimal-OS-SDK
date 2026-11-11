#ifndef MINIMALOS_MOSMEDIA_H
#define MINIMALOS_MOSMEDIA_H

/*
 * Minimal-OS images (.img) and videos (.vid): encode and decode.
 *
 * Both are lossless and share one codec. A picture is cut into square
 * tiles (64x64) and each tile is compressed on its own, row by row:
 *
 *   - each row picks a filter: the row as it is, the row minus the one
 *     above it, or (in videos) minus the same row of the previous frame;
 *   - what is left is coded with QOI's operations (repeat the last value,
 *     a recently seen value, a small difference, or the value itself).
 *
 * Flat areas and vertical structure (user interfaces, drawings,
 * screenshots) shrink to almost nothing, and the tiles are independent,
 * so they can be decoded in any order (or in parallel).
 *
 * .img keeps 8-bit RGB or RGBA (transparency, like a PNG). A .vid uses
 * one of two codecs: "tiles" (this one: only the tiles that changed since
 * the last frame are stored; lossless, or near-lossless with `loss`) or
 * "motion" (lossy and built like H.264, for camera footage; see
 * mosmedia_motion.h). Both have keyframes for seeking and an .adi
 * soundtrack that SYS_AUDIO plays straight from the file
 * (mos_audio_open_at). The full layout is in docs/media.md.
 *
 * Pixels are 0xAARRGGBB, the format windows and EasyUI use. Everything
 * here is plain C without libc, so it also builds on a host (tools/media).
 *
 *   mimg_encode_bound(w, h)                 worst-case .img size
 *   mimg_encode(px, w, h, stride, alpha, out, cap)   -> bytes written, 0 on error
 *   mimg_info(data, size, &info)            width, height, alpha
 *   mimg_decode(data, size, out, stride)    -> true when it decoded
 *
 *   mvid_writer_begin / mvid_writer_frame / mvid_writer_finish   encode
 *   mvid_open / mvid_frame_at / mvid_decode                      decode
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "mosmedia_motion.h"
#if defined(MINIMALOS_TARGET)
#include "minimalos.h"
#include "media_abi.h"
#endif

#define MIMG_MAGIC "MIMG"
#define MVID_MAGIC "MVID"
#if defined(__GNUC__)
#define MM_FN static __attribute__((unused))
#else
#define MM_FN static
#endif

#define MMEDIA_VERSION 1u
#define MMEDIA_TILE_LOG2 6u
#define MMEDIA_TILE (1u << MMEDIA_TILE_LOG2)
#define MMEDIA_MAX_SIDE 16384u
#define MIMG_HEADER_SIZE 24u
#define MVID_HEADER_SIZE 64u
#define MVID_INDEX_ENTRY 16u
#define MVID_FRAME_KEY 1u
/* .vid codecs. Tiles files are version 1 (readable by older players),
 * motion files version 2 with the codec at offset 60. */
#define MVID_CODEC_TILES  0u
#define MVID_CODEC_MOTION 1u
#define MVID_VERSION_CODEC 2u

/* Row filters. */
#define MMEDIA_FILTER_NONE 0u
#define MMEDIA_FILTER_UP   1u   /* minus the pixel above (in the same tile) */
#define MMEDIA_FILTER_PREV 2u   /* minus the same pixel in the previous frame */

#define MMEDIA_OP_INDEX 0x00u
#define MMEDIA_OP_DIFF  0x40u
#define MMEDIA_OP_LUMA  0x80u
#define MMEDIA_OP_RUN   0xC0u
#define MMEDIA_OP_RGB   0xFEu
#define MMEDIA_OP_RGBA  0xFFu

/* ------------------------------------------------------------ helpers */

static inline void mm_put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline void mm_put64(uint8_t* p, uint64_t v) { mm_put32(p, (uint32_t)v); mm_put32(p + 4, (uint32_t)(v >> 32)); }
static inline uint32_t mm_get32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline uint64_t mm_get64(const uint8_t* p) { return mm_get32(p) | (uint64_t)mm_get32(p + 4) << 32; }
static inline void mm_copy(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst; const uint8_t* s = (const uint8_t*)src;
    while (n--) *d++ = *s++;
}
static inline int mm_same(const void* a, const void* b, size_t n) {
    const uint8_t* x = (const uint8_t*)a; const uint8_t* y = (const uint8_t*)b;
    while (n--) if (*x++ != *y++) return 0;
    return 1;
}

/* Per channel, modulo 256: a - b and a + b. Alpha is byte 3. */
static inline uint32_t mm_sub(uint32_t a, uint32_t b) {
    return (((a | 0x80808080u) - (b & 0x7F7F7F7Fu)) ^ ((a ^ ~b) & 0x80808080u));
}
static inline uint32_t mm_add(uint32_t a, uint32_t b) {
    return ((a & 0x7F7F7F7Fu) + (b & 0x7F7F7F7Fu)) ^ ((a ^ b) & 0x80808080u);
}
/* The value a row is coded against with no filter: opaque black, so an
 * opaque picture's alpha residual is always 0. */
#define MM_BASE 0xFF000000u

/* Each colour of a and b within `loss` of each other, alpha the same. */
static inline bool mm_close(uint32_t a, uint32_t b, uint32_t loss) {
    if ((a >> 24) != (b >> 24)) return false;
    for (int sh = 0; sh < 24; sh += 8) {
        int d = (int)((a >> sh) & 255u) - (int)((b >> sh) & 255u);
        if ((uint32_t)(d < 0 ? -d : d) > loss) return false;
    }
    return true;
}

static inline uint32_t mm_hash(uint32_t v) {
    return (((v >> 16) & 255u) * 3u + ((v >> 8) & 255u) * 5u + (v & 255u) * 7u + (v >> 24) * 11u) & 63u;
}

static inline uint32_t mm_tiles_x(uint32_t w) { return (w + MMEDIA_TILE - 1u) >> MMEDIA_TILE_LOG2; }
static inline uint32_t mm_tiles(uint32_t w, uint32_t h) { return mm_tiles_x(w) * mm_tiles_x(h); }

/* Worst case for one tile: the filters and 5 bytes a pixel. */
static inline size_t mm_tile_bound(void) { return (size_t)MMEDIA_TILE * MMEDIA_TILE * 5u + MMEDIA_TILE; }

/* ------------------------------------------------------- tile coding */

typedef struct {
    uint32_t index[64];
    uint32_t prev;
    uint32_t run;               /* repeats of prev not written yet */
} mm_state_t;

/* Codes `n` residual values; returns the bytes written. A run still open
 * at the end stays in s->run: runs continue into the next row. */
MM_FN size_t mm_code_row(mm_state_t* s, const uint32_t* v, uint32_t n, uint8_t* out) {
    size_t at = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t px = v[i];
        if (px == s->prev) {
            if (++s->run == 62u) { out[at++] = (uint8_t)(MMEDIA_OP_RUN | 61u); s->run = 0; }
            continue;
        }
        if (s->run) { out[at++] = (uint8_t)(MMEDIA_OP_RUN | (s->run - 1u)); s->run = 0; }
        uint32_t h = mm_hash(px);
        if (s->index[h] == px) {
            out[at++] = (uint8_t)(MMEDIA_OP_INDEX | h);
        } else {
            s->index[h] = px;
            if ((px >> 24) == (s->prev >> 24)) {
                int dr = (int8_t)(((px >> 16) & 255u) - ((s->prev >> 16) & 255u));
                int dg = (int8_t)(((px >> 8) & 255u) - ((s->prev >> 8) & 255u));
                int db = (int8_t)((px & 255u) - (s->prev & 255u));
                int dr_dg = dr - dg, db_dg = db - dg;
                if (dr >= -2 && dr <= 1 && dg >= -2 && dg <= 1 && db >= -2 && db <= 1) {
                    out[at++] = (uint8_t)(MMEDIA_OP_DIFF | (dr + 2) << 4 | (dg + 2) << 2 | (db + 2));
                } else if (dg >= -32 && dg <= 31 && dr_dg >= -8 && dr_dg <= 7 && db_dg >= -8 && db_dg <= 7) {
                    out[at++] = (uint8_t)(MMEDIA_OP_LUMA | (dg + 32));
                    out[at++] = (uint8_t)((dr_dg + 8) << 4 | (db_dg + 8));
                } else {
                    out[at++] = MMEDIA_OP_RGB;
                    out[at++] = (uint8_t)(px >> 16); out[at++] = (uint8_t)(px >> 8); out[at++] = (uint8_t)px;
                }
            } else {
                out[at++] = MMEDIA_OP_RGBA;
                out[at++] = (uint8_t)(px >> 16); out[at++] = (uint8_t)(px >> 8);
                out[at++] = (uint8_t)px; out[at++] = (uint8_t)(px >> 24);
            }
        }
        s->prev = px;
    }
    return at;
}

/* A tile: its rows' filters first, 2 bits each (row 0 in the low bits of
 * the first byte), then one stream of operations for all its pixels, row
 * by row; a run may continue into the next row. */
static inline uint32_t mm_filter_bytes(uint32_t rows) { return (rows + 3u) / 4u; }

/* Encodes the tile at (tx, ty) of `px` (strides in pixels). `prev` is the
 * previous frame (NULL for a still picture or a keyframe). Without alpha
 * the pixels are coded as opaque. `fast` tries fewer filters. */
MM_FN size_t mm_tile_encode(const uint32_t* px, const uint32_t* prev, uint32_t w, uint32_t h,
                            uint32_t stride, uint32_t prev_stride, uint32_t tx, uint32_t ty,
                            bool alpha, bool fast, uint8_t* out) {
    uint32_t x0 = tx * MMEDIA_TILE, y0 = ty * MMEDIA_TILE;
    uint32_t tw = w - x0 < MMEDIA_TILE ? w - x0 : MMEDIA_TILE;
    uint32_t th = h - y0 < MMEDIA_TILE ? h - y0 : MMEDIA_TILE;
    mm_state_t s, trial, best_state;
    for (int i = 0; i < 64; i++) s.index[i] = 0;
    s.prev = 0;
    s.run = 0;
    best_state = s;
    uint32_t row[MMEDIA_TILE], above[MMEDIA_TILE], cur[MMEDIA_TILE];
    uint8_t scratch[MMEDIA_TILE * 5u + 2u];   /* a row, plus a run left open by the last */
    uint32_t nf = mm_filter_bytes(th);
    for (uint32_t i = 0; i < nf; i++) out[i] = 0;
    size_t at = nf;
    for (uint32_t y = 0; y < th; y++) {
        const uint32_t* src = px + (size_t)(y0 + y) * stride + x0;
        for (uint32_t x = 0; x < tw; x++) cur[x] = alpha ? src[x] : (src[x] | 0xFF000000u);
        uint32_t best_filter = 0;
        size_t best_cost = (size_t)-1, best_bytes = 0;
        for (uint32_t f = 0; f < 3u; f++) {
            if (f == MMEDIA_FILTER_UP && y == 0) continue;
            if (f == MMEDIA_FILTER_PREV && !prev) continue;
            if (fast && prev && f != MMEDIA_FILTER_PREV) continue;
            if (fast && !prev && y > 0 && f == MMEDIA_FILTER_NONE) continue;
            const uint32_t* ref_row = prev ? prev + (size_t)(y0 + y) * prev_stride + x0 : NULL;
            for (uint32_t x = 0; x < tw; x++) {
                uint32_t ref = f == MMEDIA_FILTER_NONE ? MM_BASE
                             : f == MMEDIA_FILTER_UP ? above[x]
                             : (alpha ? ref_row[x] : (ref_row[x] | 0xFF000000u));
                row[x] = mm_sub(cur[x], ref);
            }
            trial = s;
            size_t n = mm_code_row(&trial, row, tw, scratch);
            /* An open run will cost about a byte per 62 pixels. */
            size_t cost = n * 62u + trial.run;
            if (cost < best_cost) {
                best_cost = cost;
                best_bytes = n;
                best_filter = f;
                best_state = trial;
                mm_copy(out + at, scratch, n);
            }
        }
        out[y / 4u] |= (uint8_t)(best_filter << (2u * (y % 4u)));
        at += best_bytes;
        s = best_state;
        mm_copy(above, cur, tw * sizeof(uint32_t));
    }
    if (s.run) out[at++] = (uint8_t)(MMEDIA_OP_RUN | (s.run - 1u));
    return at;
}

/* Decodes one tile into `px` (stride in pixels). For PREV rows `px` must
 * hold the previous frame: the tile is decoded in place over it. Returns
 * false on corrupt data. */
MM_FN bool mm_tile_decode(const uint8_t* in, size_t size, uint32_t* px, uint32_t w, uint32_t h,
                          uint32_t stride, uint32_t tx, uint32_t ty, bool has_prev) {
    uint32_t x0 = tx * MMEDIA_TILE, y0 = ty * MMEDIA_TILE;
    if (x0 >= w || y0 >= h) return false;
    uint32_t tw = w - x0 < MMEDIA_TILE ? w - x0 : MMEDIA_TILE;
    uint32_t th = h - y0 < MMEDIA_TILE ? h - y0 : MMEDIA_TILE;
    uint32_t nf = mm_filter_bytes(th);
    if (size < nf) return false;
    uint32_t index[64];
    for (int i = 0; i < 64; i++) index[i] = 0;
    uint32_t prev = 0, run = 0;
    size_t at = nf;
    for (uint32_t y = 0; y < th; y++) {
        uint32_t f = (in[y / 4u] >> (2u * (y % 4u))) & 3u;
        if (f > 2u || (f == MMEDIA_FILTER_UP && y == 0) || (f == MMEDIA_FILTER_PREV && !has_prev))
            return false;
        uint32_t* dst = px + (size_t)(y0 + y) * stride + x0;
        const uint32_t* up = y ? dst - stride : NULL;
        for (uint32_t x = 0; x < tw; x++) {
            if (run) {
                run--;
            } else {
                if (at >= size) return false;
                uint32_t b = in[at++];
                if (b == MMEDIA_OP_RGB) {
                    if (at + 3 > size) return false;
                    prev = (prev & 0xFF000000u) | (uint32_t)in[at] << 16 | (uint32_t)in[at + 1] << 8 | in[at + 2];
                    at += 3;
                    index[mm_hash(prev)] = prev;
                } else if (b == MMEDIA_OP_RGBA) {
                    if (at + 4 > size) return false;
                    prev = (uint32_t)in[at + 3] << 24 | (uint32_t)in[at] << 16 | (uint32_t)in[at + 1] << 8 | in[at + 2];
                    at += 4;
                    index[mm_hash(prev)] = prev;
                } else if ((b & 0xC0u) == MMEDIA_OP_INDEX) {
                    prev = index[b];
                } else if ((b & 0xC0u) == MMEDIA_OP_DIFF) {
                    uint32_t r = ((prev >> 16) + ((b >> 4) & 3u) - 2u) & 255u;
                    uint32_t g = ((prev >> 8) + ((b >> 2) & 3u) - 2u) & 255u;
                    uint32_t bl = (prev + (b & 3u) - 2u) & 255u;
                    prev = (prev & 0xFF000000u) | r << 16 | g << 8 | bl;
                    index[mm_hash(prev)] = prev;
                } else if ((b & 0xC0u) == MMEDIA_OP_LUMA) {
                    if (at >= size) return false;
                    uint32_t b2 = in[at++];
                    int dg = (int)(b & 63u) - 32;
                    int dr = dg + (int)(b2 >> 4) - 8, db = dg + (int)(b2 & 15u) - 8;
                    uint32_t r = ((prev >> 16) + (uint32_t)dr) & 255u;
                    uint32_t g = ((prev >> 8) + (uint32_t)dg) & 255u;
                    uint32_t bl = (prev + (uint32_t)db) & 255u;
                    prev = (prev & 0xFF000000u) | r << 16 | g << 8 | bl;
                    index[mm_hash(prev)] = prev;
                } else {
                    run = b & 63u;      /* this pixel, then `run` more */
                }
            }
            uint32_t ref = f == MMEDIA_FILTER_NONE ? MM_BASE : f == MMEDIA_FILTER_UP ? up[x] : dst[x];
            dst[x] = mm_add(prev, ref);
        }
    }
    return run == 0;
}

/* ------------------------------------------------------------- .img */

/* Header (little-endian):
 *    0  "MIMG"
 *    4  u8 version (1)      5  u8 channels (3 or 4)
 *    6  u8 tile size log2   7  u8 flags (0)
 *    8  u32 width          12  u32 height
 *   16  u32 tile count     20  u32 reserved (0)
 *   24  u32 tile offsets[count + 1], from the end of this table
 *       tile data, left to right, top to bottom */

typedef struct {
    uint32_t width, height;
    bool alpha;
} mimg_info_t;

static inline size_t mimg_encode_bound(uint32_t w, uint32_t h) {
    uint32_t tiles = mm_tiles(w, h);
    return MIMG_HEADER_SIZE + ((size_t)tiles + 1u) * 4u + (size_t)w * h * 5u + (size_t)tiles * MMEDIA_TILE;
}

MM_FN size_t mimg_encode(const uint32_t* px, uint32_t w, uint32_t h, uint32_t stride, bool alpha,
                          uint8_t* out, size_t cap) {
    if (!px || !w || !h || w > MMEDIA_MAX_SIDE || h > MMEDIA_MAX_SIDE || cap < mimg_encode_bound(w, h))
        return 0;
    uint32_t tiles = mm_tiles(w, h), across = mm_tiles_x(w);
    mm_copy(out, MIMG_MAGIC, 4);
    out[4] = MMEDIA_VERSION; out[5] = alpha ? 4 : 3; out[6] = MMEDIA_TILE_LOG2; out[7] = 0;
    mm_put32(out + 8, w); mm_put32(out + 12, h); mm_put32(out + 16, tiles); mm_put32(out + 20, 0);
    uint8_t* table = out + MIMG_HEADER_SIZE;
    uint8_t* data = table + ((size_t)tiles + 1u) * 4u;
    size_t at = 0;
    for (uint32_t t = 0; t < tiles; t++) {
        mm_put32(table + t * 4u, (uint32_t)at);
        at += mm_tile_encode(px, NULL, w, h, stride, 0, t % across, t / across, alpha, false, data + at);
    }
    mm_put32(table + (size_t)tiles * 4u, (uint32_t)at);
    return (size_t)(data - out) + at;
}

MM_FN bool mimg_info(const uint8_t* d, size_t size, mimg_info_t* info) {
    if (size < MIMG_HEADER_SIZE || !mm_same(d, MIMG_MAGIC, 4) || d[4] != MMEDIA_VERSION ||
        (d[5] != 3 && d[5] != 4) || d[6] != MMEDIA_TILE_LOG2)
        return false;
    uint32_t w = mm_get32(d + 8), h = mm_get32(d + 12);
    if (!w || !h || w > MMEDIA_MAX_SIDE || h > MMEDIA_MAX_SIDE || mm_get32(d + 16) != mm_tiles(w, h))
        return false;
    if (MIMG_HEADER_SIZE + ((uint64_t)mm_tiles(w, h) + 1u) * 4u > size) return false;
    info->width = w;
    info->height = h;
    info->alpha = d[5] == 4;
    return true;
}

/* Decodes into `out` (stride in pixels, room for width x height). */
MM_FN bool mimg_decode(const uint8_t* d, size_t size, uint32_t* out, uint32_t stride) {
    mimg_info_t info;
    if (!mimg_info(d, size, &info) || stride < info.width) return false;
    uint32_t tiles = mm_tiles(info.width, info.height), across = mm_tiles_x(info.width);
    const uint8_t* table = d + MIMG_HEADER_SIZE;
    const uint8_t* data = table + ((size_t)tiles + 1u) * 4u;
    size_t avail = size - (size_t)(data - d);
    for (uint32_t t = 0; t < tiles; t++) {
        uint32_t a = mm_get32(table + t * 4u), b = mm_get32(table + t * 4u + 4u);
        if (a > b || b > avail) return false;
        if (!mm_tile_decode(data + a, b - a, out, info.width, info.height, stride, t % across,
                            t / across, false))
            return false;
    }
    return true;
}

/* ------------------------------------------------------------- .vid */

/* Header (64 bytes, little-endian):
 *    0  "MVID"
 *    4  u8 version (1 tiles, 2 codec at 60)   5  u8 channels (motion: 3)
 *    6  u8 tile size log2   7  u8 flags (bit 0: audio)
 *    8  u32 width       12  u32 height
 *   16  u32 fps numerator   20  u32 fps denominator
 *   24  u32 frame count     28  u32 duration in ms
 *   32  u64 index offset    40  u64 audio offset   48  u64 audio size
 *   56  u32 keyframe interval   60  u32 version 2: codec (0 tiles, 1 motion)
 * Frames follow the header. A tiles frame: u8 flags (bit 0 keyframe), 3
 * zero bytes, a bitmap with one bit per tile (set = coded, clear = same as
 * the previous frame), a u32 size for each coded tile, then their data.
 * A motion frame: u8 flags (bit 0 keyframe), u8 QP, 2 zero bytes, then
 * the range-coded frame (mmc_decode_frame).
 * The index has 16 bytes a frame: u64 offset, u32 size, u32 flags.
 * The soundtrack is an .adi file, stored unchanged. */

typedef bool (*mvid_write_fn)(void* ctx, uint64_t offset, const void* data, size_t size);
typedef bool (*mvid_read_fn)(void* ctx, uint64_t offset, void* data, size_t size);
typedef void* (*mvid_alloc_fn)(size_t size);
typedef void (*mvid_free_fn)(void* p);

typedef struct {
    uint32_t width, height, fps_num, fps_den, frame_count, duration_ms, keyframe_interval;
    uint32_t codec;             /* MVID_CODEC_TILES or MVID_CODEC_MOTION */
    bool alpha, has_audio;
    uint64_t index_offset, audio_offset, audio_size;
} mvid_info_t;

typedef struct {
    mvid_info_t info;
    mvid_write_fn write;
    void* ctx;
    mvid_alloc_fn alloc;
    mvid_free_fn free;
    bool fast, failed;
    uint64_t at;                /* where the next frame goes */
    uint32_t* prev;             /* the last frame */
    uint8_t* frame;             /* one encoded frame */
    size_t frame_cap;
    uint8_t* index;             /* MVID_INDEX_ENTRY a frame */
    uint32_t index_cap;
    /* Settings, before the first frame (all optional): */
    uint32_t codec;             /* MVID_CODEC_TILES (default) or MVID_CODEC_MOTION */
    uint32_t qp;                /* motion: quality, 0 (best) to 51, default 26 */
    uint32_t loss;              /* tiles: colours may move up to this (0 = lossless) */
    /* motion: decides a frame's macroblocks (mmc_encode_mb for every one, a
     * row at most two macroblocks ahead of the row below); NULL = in order */
    void (*parallel)(void* ctx, mmc_t* m, const mmc_job_t* job);
    void* parallel_ctx;
    mmc_t mc;                   /* motion: the encoder (its reference frame) */
    bool mc_ready;
    uint32_t* work;             /* tiles with loss: the frame as stored */
} mvid_writer_t;

MM_FN size_t mvid_frame_bound(uint32_t w, uint32_t h) {
    uint32_t tiles = mm_tiles(w, h);
    return 4u + (tiles + 7u) / 8u + (size_t)tiles * (4u + mm_tile_bound());
}

/* Starts a video. `fast` codes delta frames against the previous frame
 * only (for recording live); otherwise every filter is tried. */
MM_FN bool mvid_writer_begin(mvid_writer_t* v, uint32_t w, uint32_t h, uint32_t fps_num,
                              uint32_t fps_den, bool alpha, uint32_t keyframe_interval, bool fast,
                              mvid_write_fn write, void* ctx, mvid_alloc_fn alloc, mvid_free_fn free_fn) {
    for (size_t i = 0; i < sizeof(*v); i++) ((uint8_t*)v)[i] = 0;
    if (!w || !h || w > MMEDIA_MAX_SIDE || h > MMEDIA_MAX_SIDE || !fps_num || !fps_den) return false;
    v->info.width = w; v->info.height = h;
    v->info.fps_num = fps_num; v->info.fps_den = fps_den;
    v->info.alpha = alpha;
    v->info.keyframe_interval = keyframe_interval ? keyframe_interval : 1u;
    v->write = write; v->ctx = ctx; v->alloc = alloc; v->free = free_fn; v->fast = fast;
    v->qp = 26;
    v->at = MVID_HEADER_SIZE;
    v->frame_cap = mvid_frame_bound(w, h);
    v->prev = (uint32_t*)alloc((size_t)w * h * 4u);
    v->frame = (uint8_t*)alloc(v->frame_cap);
    v->index_cap = 1024;
    v->index = (uint8_t*)alloc((size_t)v->index_cap * MVID_INDEX_ENTRY);
    if (!v->prev || !v->frame || !v->index) { v->failed = true; return false; }
    return true;
}

/* A motion frame: decided (maybe on several threads), then written; a
 * frame that does not fit the buffer is coded again at a higher QP. */
MM_FN size_t mvid_motion_frame(mvid_writer_t* v, const uint32_t* px, uint32_t stride, bool key) {
    mmc_t* m = &v->mc;
    if (!v->mc_ready) {
        if (!mmc_init(m, v->info.width, v->info.height, true, v->alloc)) {
            mmc_free(m, v->free);
            return 0;
        }
        v->mc_ready = true;
    }
    mmc_load_frame(m, px, stride);
    int qp = (int)(v->qp > MMC_MAX_QP ? MMC_MAX_QP : v->qp);
    if (key) qp = qp >= 2 ? qp - 2 : 0;
    mmc_job_t job;
    for (;;) {
        mmc_begin_frame(m, &job, key, qp, v->fast);
        if (v->parallel) {
            v->parallel(v->parallel_ctx, m, &job);
        } else {
            for (uint32_t my = 0; my < m->mbh; my++)
                for (uint32_t mx = 0; mx < m->mbw; mx++) mmc_encode_mb(m, &job, mx, my);
        }
        size_t n = mmc_write_frame(m, &job, v->frame + 4, v->frame_cap - 4u);
        if (n) {
            mmc_finish_frame(m, job.qp);
            v->frame[0] = key ? MVID_FRAME_KEY : 0;
            v->frame[1] = (uint8_t)job.qp;
            v->frame[2] = v->frame[3] = 0;
            return n + 4u;
        }
        if (qp >= MMC_MAX_QP) return 0;
        qp = qp + 6 > MMC_MAX_QP ? MMC_MAX_QP : qp + 6;
        /* the vectors were overwritten: start the search from the last frame's */
        size_t mbs = (size_t)m->mbw * m->mbh;
        for (size_t i = 0; i < mbs * 2u; i++) m->mv[i] = m->prev_mv[i];
    }
}

/* Near-lossless tiles: each pixel takes the colour of the last frame's,
 * the left or the above pixel when that is within `loss` (its own alpha
 * stays), so more tiles stay unchanged and residuals repeat. Returns the
 * frame to store. */
MM_FN const uint32_t* mvid_lossy_frame(mvid_writer_t* v, const uint32_t* px, uint32_t stride, bool first) {
    uint32_t w = v->info.width, h = v->info.height, loss = v->loss;
    uint32_t* o = v->work;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint32_t s = px[(size_t)y * stride + x];
            if (!v->info.alpha) s |= 0xFF000000u;
            size_t i = (size_t)y * w + x;
            uint32_t a = s & 0xFF000000u, c = s;
            if (!first && mm_close(s, a | (v->prev[i] & 0xFFFFFFu), loss)) c = v->prev[i];
            else if (x > 0 && mm_close(s, a | (o[i - 1] & 0xFFFFFFu), loss)) c = o[i - 1];
            else if (y > 0 && mm_close(s, a | (o[i - w] & 0xFFFFFFu), loss)) c = o[i - w];
            o[i] = a | (c & 0xFFFFFFu);
        }
    return o;
}

/* Adds a frame (stride in pixels). */
MM_FN bool mvid_writer_frame(mvid_writer_t* v, const uint32_t* px, uint32_t stride) {
    if (v->failed) return false;
    uint32_t w = v->info.width, h = v->info.height;
    uint32_t n = v->info.frame_count;
    bool key = n % v->info.keyframe_interval == 0;
    if (n == v->index_cap) {
        uint8_t* bigger = (uint8_t*)v->alloc((size_t)v->index_cap * 2u * MVID_INDEX_ENTRY);
        if (!bigger) { v->failed = true; return false; }
        mm_copy(bigger, v->index, (size_t)v->index_cap * MVID_INDEX_ENTRY);
        v->free(v->index);
        v->index = bigger;
        v->index_cap *= 2u;
    }
    if (n == 0) v->info.codec = v->codec == MVID_CODEC_MOTION ? MVID_CODEC_MOTION : MVID_CODEC_TILES;
    if (v->info.codec == MVID_CODEC_MOTION) {
        size_t size = mvid_motion_frame(v, px, stride, key);
        if (!size || !v->write(v->ctx, v->at, v->frame, size)) { v->failed = true; return false; }
        uint8_t* e = v->index + (size_t)n * MVID_INDEX_ENTRY;
        mm_put64(e, v->at); mm_put32(e + 8, (uint32_t)size); mm_put32(e + 12, key ? MVID_FRAME_KEY : 0);
        v->at += size;
        v->info.frame_count++;
        return true;
    }
    if (v->loss) {
        if (!v->work && !(v->work = (uint32_t*)v->alloc((size_t)w * h * 4u))) { v->failed = true; return false; }
        px = mvid_lossy_frame(v, px, stride, n == 0);
        stride = w;
    }
    uint32_t tiles = mm_tiles(w, h), across = mm_tiles_x(w);
    uint8_t* f = v->frame;
    uint32_t bitmap_bytes = (tiles + 7u) / 8u;
    f[0] = key ? MVID_FRAME_KEY : 0; f[1] = f[2] = f[3] = 0;
    uint8_t* bitmap = f + 4;
    for (uint32_t i = 0; i < bitmap_bytes; i++) bitmap[i] = 0;
    /* Which tiles changed. */
    uint32_t coded = 0;
    for (uint32_t t = 0; t < tiles; t++) {
        uint32_t x0 = (t % across) * MMEDIA_TILE, y0 = (t / across) * MMEDIA_TILE;
        uint32_t tw = w - x0 < MMEDIA_TILE ? w - x0 : MMEDIA_TILE;
        uint32_t th = h - y0 < MMEDIA_TILE ? h - y0 : MMEDIA_TILE;
        bool changed = key;
        for (uint32_t y = 0; y < th && !changed; y++)
            changed = !mm_same(px + (size_t)(y0 + y) * stride + x0, v->prev + (size_t)(y0 + y) * w + x0,
                               tw * 4u);
        if (changed) { bitmap[t / 8u] |= (uint8_t)(1u << (t % 8u)); coded++; }
    }
    uint8_t* sizes = bitmap + bitmap_bytes;
    uint8_t* data = sizes + (size_t)coded * 4u;
    size_t at = 0;
    uint32_t k = 0;
    for (uint32_t t = 0; t < tiles; t++) {
        if (!(bitmap[t / 8u] & (1u << (t % 8u)))) continue;
        size_t n2 = mm_tile_encode(px, key ? NULL : v->prev, w, h, stride, w, t % across,
                                   t / across, v->info.alpha, v->fast, data + at);
        mm_put32(sizes + k * 4u, (uint32_t)n2);
        at += n2;
        k++;
    }
    size_t size = (size_t)(data - f) + at;
    if (!v->write(v->ctx, v->at, f, size)) { v->failed = true; return false; }
    uint8_t* e = v->index + (size_t)n * MVID_INDEX_ENTRY;
    mm_put64(e, v->at); mm_put32(e + 8, (uint32_t)size); mm_put32(e + 12, key ? MVID_FRAME_KEY : 0);
    v->at += size;
    for (uint32_t y = 0; y < h; y++) mm_copy(v->prev + (size_t)y * w, px + (size_t)y * stride, w * 4u);
    v->info.frame_count++;
    return true;
}

/* Writes the index, the soundtrack (an .adi file's bytes, or NULL) and
 * the header, and frees the writer's buffers. */
MM_FN bool mvid_writer_finish(mvid_writer_t* v, const void* adi, size_t adi_size) {
    bool ok = !v->failed;
    if (ok) {
        v->info.index_offset = v->at;
        size_t index_size = (size_t)v->info.frame_count * MVID_INDEX_ENTRY;
        ok = v->write(v->ctx, v->at, v->index, index_size);
        v->at += index_size;
    }
    if (ok && adi && adi_size) {
        v->info.has_audio = true;
        v->info.audio_offset = v->at;
        v->info.audio_size = adi_size;
        ok = v->write(v->ctx, v->at, adi, adi_size);
        v->at += adi_size;
    }
    if (ok) {
        uint8_t hd[MVID_HEADER_SIZE];
        for (uint32_t i = 0; i < MVID_HEADER_SIZE; i++) hd[i] = 0;
        mvid_info_t* in = &v->info;
        in->duration_ms = (uint32_t)((uint64_t)in->frame_count * 1000u * in->fps_den / in->fps_num);
        bool motion = in->codec == MVID_CODEC_MOTION;
        if (motion) in->alpha = false;
        mm_copy(hd, MVID_MAGIC, 4);
        hd[4] = motion ? MVID_VERSION_CODEC : MMEDIA_VERSION; hd[5] = in->alpha ? 4 : 3; hd[6] = MMEDIA_TILE_LOG2;
        hd[7] = in->has_audio ? 1 : 0;
        mm_put32(hd + 8, in->width); mm_put32(hd + 12, in->height);
        mm_put32(hd + 16, in->fps_num); mm_put32(hd + 20, in->fps_den);
        mm_put32(hd + 24, in->frame_count); mm_put32(hd + 28, in->duration_ms);
        mm_put64(hd + 32, in->index_offset); mm_put64(hd + 40, in->audio_offset);
        mm_put64(hd + 48, in->audio_size); mm_put32(hd + 56, in->keyframe_interval);
        if (motion) mm_put32(hd + 60, MVID_CODEC_MOTION);
        ok = v->write(v->ctx, 0, hd, MVID_HEADER_SIZE);
    }
    if (v->mc_ready) mmc_free(&v->mc, v->free);
    v->mc_ready = false;
    if (v->work) v->free(v->work);
    v->work = NULL;
    if (v->prev) v->free(v->prev);
    if (v->frame) v->free(v->frame);
    if (v->index) v->free(v->index);
    v->prev = NULL; v->frame = NULL; v->index = NULL;
    return ok;
}

typedef struct {
    mvid_info_t info;
    mvid_read_fn read;
    void* ctx;
    uint64_t size;
    uint8_t* index;             /* frame_count entries */
    uint8_t* frame;             /* one encoded frame */
    size_t frame_cap;
    int64_t decoded;            /* frame in the output buffer, -1 = none */
    mmc_t mc;                   /* motion: the decoder */
    bool mc_ready;
    bool no_gpu;                /* motion: colour conversion stays on the CPU */
} mvid_reader_t;

MM_FN bool mvid_parse_header(const uint8_t* hd, uint64_t file_size, mvid_info_t* in) {
    if (!mm_same(hd, MVID_MAGIC, 4) || (hd[4] != MMEDIA_VERSION && hd[4] != MVID_VERSION_CODEC) ||
        (hd[5] != 3 && hd[5] != 4) || hd[6] != MMEDIA_TILE_LOG2)
        return false;
    in->codec = hd[4] == MVID_VERSION_CODEC ? mm_get32(hd + 60) : MVID_CODEC_TILES;
    if (in->codec > MVID_CODEC_MOTION || (in->codec == MVID_CODEC_MOTION && hd[5] != 3)) return false;
    in->alpha = hd[5] == 4;
    in->has_audio = (hd[7] & 1u) != 0;
    in->width = mm_get32(hd + 8); in->height = mm_get32(hd + 12);
    in->fps_num = mm_get32(hd + 16); in->fps_den = mm_get32(hd + 20);
    in->frame_count = mm_get32(hd + 24); in->duration_ms = mm_get32(hd + 28);
    in->index_offset = mm_get64(hd + 32); in->audio_offset = mm_get64(hd + 40);
    in->audio_size = mm_get64(hd + 48); in->keyframe_interval = mm_get32(hd + 56);
    if (!in->width || !in->height || in->width > MMEDIA_MAX_SIDE || in->height > MMEDIA_MAX_SIDE ||
        !in->fps_num || !in->fps_den || !in->frame_count)
        return false;
    if (in->index_offset + (uint64_t)in->frame_count * MVID_INDEX_ENTRY > file_size) return false;
    if (in->has_audio && in->audio_offset + in->audio_size > file_size) return false;
    return true;
}

/* Opens a video: reads the header and the index. */
MM_FN bool mvid_open(mvid_reader_t* r, mvid_read_fn read, void* ctx, uint64_t size,
                      mvid_alloc_fn alloc) {
    for (size_t i = 0; i < sizeof(*r); i++) ((uint8_t*)r)[i] = 0;
    r->read = read; r->ctx = ctx; r->size = size; r->decoded = -1;
    uint8_t hd[MVID_HEADER_SIZE];
    if (size < MVID_HEADER_SIZE || !read(ctx, 0, hd, MVID_HEADER_SIZE) ||
        !mvid_parse_header(hd, size, &r->info))
        return false;
    size_t index_size = (size_t)r->info.frame_count * MVID_INDEX_ENTRY;
    r->index = (uint8_t*)alloc(index_size);
    if (!r->index || !read(ctx, r->info.index_offset, r->index, index_size)) return false;
    /* Room for the largest frame (no more than a frame can need). */
    size_t bound = mvid_frame_bound(r->info.width, r->info.height), largest = 0;
    for (uint32_t i = 0; i < r->info.frame_count; i++) {
        uint32_t n = mm_get32(r->index + (size_t)i * MVID_INDEX_ENTRY + 8);
        if (n > largest) largest = n;
    }
    r->frame_cap = largest < bound ? largest : bound;
    r->frame = (uint8_t*)alloc(r->frame_cap ? r->frame_cap : 1);
    if (!r->frame) return false;
    if (r->info.codec == MVID_CODEC_MOTION) {
        r->mc_ready = true;   /* mmc_init clears it on failure; mmc_free copes */
        if (!mmc_init(&r->mc, r->info.width, r->info.height, false, alloc)) return false;
    }
    return true;
}

MM_FN void mvid_close(mvid_reader_t* r, mvid_free_fn free_fn) {
    if (r->index) free_fn(r->index);
    if (r->frame) free_fn(r->frame);
    r->index = NULL; r->frame = NULL;
    if (r->mc_ready) mmc_free(&r->mc, free_fn);
    r->mc_ready = false;
}

/* The frame shown at `ms`. */
MM_FN uint32_t mvid_frame_at(const mvid_info_t* in, uint32_t ms) {
    uint64_t f = (uint64_t)ms * in->fps_num / (1000u * (uint64_t)in->fps_den);
    return f >= in->frame_count ? in->frame_count - 1u : (uint32_t)f;
}

MM_FN bool mvid_decode_one(mvid_reader_t* r, uint32_t n, uint32_t* out) {
    const uint8_t* e = r->index + (size_t)n * MVID_INDEX_ENTRY;
    uint64_t offset = mm_get64(e);
    uint32_t size = mm_get32(e + 8);
    uint32_t w = r->info.width, h = r->info.height;
    if (r->info.codec == MVID_CODEC_MOTION) {
        if (size < 5u || size > r->frame_cap || offset + size > r->size || !r->read(r->ctx, offset, r->frame, size))
            return false;
        bool key = (r->frame[0] & MVID_FRAME_KEY) != 0;
        if (!key && r->decoded != (int64_t)n - 1) return false;
        if (!mmc_decode_frame(&r->mc, r->frame + 4, size - 4u, key, r->frame[1])) return false;
        /* Colour conversion: on the graphics card when the kernel's GPU
         * backend offers it (the same pixels), otherwise here. */
        bool converted = false;
#if defined(MINIMALOS_TARGET)
        if (!r->no_gpu) {
            syscall_media_planes_t planes = {
                .y = r->mc.ref[0], .cb = r->mc.ref[1], .cr = r->mc.ref[2],
                .w = r->mc.w, .h = r->mc.h, .cw = r->mc.cw, .ch = r->mc.ch
            };
            syscall_media_request_t request = {
                .size = sizeof(request),
                .version = SYS_MEDIA_ABI_VERSION,
                .op = SYS_MEDIA_CONVERT_YCBCR,
                .width = w,
                .height = h,
                .frame_bytes = sizeof(planes),
                .frame = (const uint8_t*)&planes,
                .pixels = out
            };
            long result = mos_syscall(SYS_MEDIA, (long)(uintptr_t)&request, 0, 0);
            converted = result == SYS_SUCCESS;
            /* No GPU for it (or an older kernel): stop asking for this file. */
            if (!converted) r->no_gpu = true;
        }
#endif
        if (!converted) mmc_to_argb(&r->mc, out, w, h, w);
        r->decoded = n;
        return true;
    }
    uint32_t tiles = mm_tiles(w, h), across = mm_tiles_x(w), bitmap_bytes = (tiles + 7u) / 8u;
    if (size < 4u + bitmap_bytes || size > r->frame_cap || offset + size > r->size ||
        !r->read(r->ctx, offset, r->frame, size))
        return false;
    const uint8_t* f = r->frame;
    bool key = (f[0] & MVID_FRAME_KEY) != 0;
    if (!key && r->decoded != (int64_t)n - 1) return false;
    const uint8_t* bitmap = f + 4;
    uint32_t coded = 0;
    for (uint32_t t = 0; t < tiles; t++) coded += (bitmap[t / 8u] >> (t % 8u)) & 1u;
    const uint8_t* sizes = bitmap + bitmap_bytes;
    if (4u + bitmap_bytes + (uint64_t)coded * 4u > size) return false;
    const uint8_t* data = sizes + (size_t)coded * 4u;
    size_t avail = size - (size_t)(data - f), at = 0;
#if defined(MINIMALOS_TARGET)
    if (coded > 1u) {
        syscall_media_request_t request = {
            .size = sizeof(request),
            .version = SYS_MEDIA_ABI_VERSION,
            .op = SYS_MEDIA_DECODE_TILES,
            .flags = key ? 0u : SYS_MEDIA_HAS_PREVIOUS_FRAME,
            .width = w,
            .height = h,
            .frame_bytes = size,
            .frame = f,
            .pixels = out
        };
        long result = mos_syscall(SYS_MEDIA, (long)(uintptr_t)&request, 0, 0);
        if (result == SYS_SUCCESS) {
            r->decoded = n;
            return true;
        }
        if (result != SYS_ERR_AGAIN && result != SYS_ERR_GENERIC) return false;
    }
#endif
    uint32_t k = 0;
    for (uint32_t t = 0; t < tiles; t++) {
        if (!((bitmap[t / 8u] >> (t % 8u)) & 1u)) {
            if (key) return false;
            continue;
        }
        uint32_t ts = mm_get32(sizes + k * 4u);
        if (ts > avail - at) return false;
        if (!mm_tile_decode(data + at, ts, out, w, h, w, t % across, t / across, !key)) return false;
        at += ts;
        k++;
    }
    r->decoded = n;
    return true;
}

/* Decodes frame `n` into `out` (width x height pixels, stride = width).
 * `out` must be the buffer of earlier calls: the next frame is decoded
 * over the last one; any other frame starts again from its keyframe. */
MM_FN bool mvid_decode(mvid_reader_t* r, uint32_t n, uint32_t* out) {
    if (n >= r->info.frame_count) return false;
    if ((int64_t)n == r->decoded) return true;
    uint32_t from;
    if (r->decoded >= 0 && (int64_t)n > r->decoded) {
        from = (uint32_t)r->decoded + 1u;
        /* A keyframe in between is a shorter way there. */
        for (uint32_t k = n; k > from; k--)
            if (mm_get32(r->index + (size_t)k * MVID_INDEX_ENTRY + 12) & MVID_FRAME_KEY) { from = k; break; }
    } else {
        from = n;
        while (from > 0 && !(mm_get32(r->index + (size_t)from * MVID_INDEX_ENTRY + 12) & MVID_FRAME_KEY))
            from--;
    }
    for (uint32_t i = from; i <= n; i++)
        if (!mvid_decode_one(r, i, out)) { r->decoded = -1; return false; }
    return true;
}

#endif
