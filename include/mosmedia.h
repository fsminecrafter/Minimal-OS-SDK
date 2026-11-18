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
 * An .img is either a raster picture (version 1, the tiles above) or a
 * vector picture (version 2: paths with solid and gradient paints, fills
 * and strokes). A vector picture has no size of its own: mimg_render draws
 * it again, crisp, at any width and height, so icons and logos need one
 * file for every size. "Current color" paints let a program tint an icon
 * with its theme. The layout is below and in docs/media.md.
 *
 *   mimg_encode_bound(w, h)                 worst-case raster .img size
 *   mimg_encode(px, w, h, stride, alpha, out, cap)   -> bytes written, 0 on error
 *   mimg_info(data, size, &info)            size, alpha, vector?, view box
 *   mimg_decode(data, size, out, stride)    -> true when it decoded (a vector
 *                                              picture at its natural size)
 *   mimg_render(data, size, out, w, h, stride)       draw any .img at w x h
 *   mimg_render_ex(..., &opts)              the same with a tint ("current
 *                                           color"), compositing, fit and the
 *                                           scratch memory it needs
 *   mimg_scratch_size / mimg_scratch_min    how much scratch memory to give it
 *   mimg_vw_*                               write a vector .img (host side)
 *
 *   mvid_writer_begin / mvid_writer_frame / mvid_writer_finish   encode
 *   mvid_open / mvid_frame_at / mvid_decode                      decode
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "mosmedia_motion.h"
#include "mosmedia_h264.h"
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
/* H.264 (Constrained Baseline), decoded by mosmedia_h264.h. A frame is
 * the 4-byte frame header and one access unit in Annex B form; a
 * keyframe is an IDR picture with its parameter sets. */
#define MVID_CODEC_H264   2u
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
    uint32_t width, height;     /* the picture's size (vector: its natural size) */
    bool alpha;
    float vb_x, vb_y, vb_w, vb_h;   /* vector: the view box; raster: 0, 0, width, height */
    bool current_color;         /* vector: some paint is "current color" (a tintable icon) */
    bool vector;                /* version 2: drawn from shapes, any size */
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

/* ------------------------------------------------- .img, vector (version 2) */

/* A vector .img is the same container with version 2: icons and logos are
 * stored as shapes and drawn at whatever size is asked for. Header
 * (little-endian, 24 bytes like the raster header):
 *    0  "MIMG"
 *    4  u8 version (2)       5  u8 channels (4)
 *    6  u8 shift             7  u8 flags (bit 0: uses "current color" paints)
 *    8  u32 natural width   12  u32 natural height     (1 to 16384)
 *   16  u32 payload size    20  u32 reserved (0)
 * Payload (varint = LEB128, svarint = zigzag varint; "fixed" = a signed
 * integer counted in 1/2^shift of a drawing unit; shift is 0 to 12, the
 * converter picks it from the size of the drawing):
 *   svarint view box x, y (fixed), varint view box width, height (fixed, > 0)
 *   u16 paint count (0 to 1024), u16 shape count
 *   paints, then shapes.
 * Paint: u8 (bits 0-1 type: 0 solid, 1 current color, 2 linear, 3 radial;
 *   bits 2-3 spread: 0 pad, 1 reflect, 2 repeat; bit 4: has matrix), then
 *   solid:   u32 0xAARRGGBB
 *   current: u8 alpha (multiplies the caller's alpha)
 *   gradient: u8 stop count (1 to 8), each stop u8 offset (0..255) and u32
 *     0xAARRGGBB; then f32 geometry (linear x1 y1 x2 y2; radial cx cy r fx fy)
 *     in gradient space; then, with bit 4, f32 a b c d e f (gradient space
 *     to the shape's coordinate space: x' = a x + c y + e, y' = b x + d y + f).
 * Shape: u8 flags (bit 0 fill, 1 stroke, 2 even-odd, 3 transform; others 0), then
 *   fill:    varint paint index, u8 opacity
 *   stroke:  varint paint index, varint width (fixed), u8 (cap | join << 2;
 *            caps 0 butt, 1 round, 2 square; joins 0 miter, 1 round, 2 bevel),
 *            u8 miter limit in 1/8, u8 opacity
 *   transform: f32 a b c d e f (shape coordinates to drawing units)
 *   path: runs of u8 (op | (n - 1) << 3), then n groups of coordinates, all
 *     varint deltas (svarint, fixed) from the current point:
 *       op 1 move (dx dy), 2 line (dx dy), 3 quadratic (cx cy x y),
 *       4 cubic (c1x c1y c2x c2y x y), 5 close (none), 6 horizontal line
 *       (dx), 7 vertical line (dy); u8 0 ends the path.
 *     After a close the current point is the start of the subpath. A path
 *     starts with a move; a drawing command after a close starts a new
 *     subpath where the last one began.
 * Gradient paints (and shape transforms) hold f32 so they keep their
 * precision whatever the shift. Everything is bounds-checked; mimg_render
 * validates the whole payload before it draws anything. */

#define MIMG_VERSION_VECTOR 2u
#define MIMG_VEC_MAX_PAINTS 1024u
#define MIMG_VEC_MAX_SHAPES 65535u
#define MIMG_VEC_MAX_STOPS 8u
#define MIMG_VEC_MAX_SHIFT 12u

#define MIMG_PAINT_SOLID   0u
#define MIMG_PAINT_CURRENT 1u
#define MIMG_PAINT_LINEAR  2u
#define MIMG_PAINT_RADIAL  3u
#define MIMG_SPREAD_PAD     0u
#define MIMG_SPREAD_REFLECT 1u
#define MIMG_SPREAD_REPEAT  2u
#define MIMG_CAP_BUTT   0u
#define MIMG_CAP_ROUND  1u
#define MIMG_CAP_SQUARE 2u
#define MIMG_JOIN_MITER 0u
#define MIMG_JOIN_ROUND 1u
#define MIMG_JOIN_BEVEL 2u
#define MIMG_FIT_CONTAIN 0u   /* keep the aspect ratio, centered (default) */
#define MIMG_FIT_STRETCH 1u   /* fill width x height, distorting if needed */
#define MIMG_FIT_COVER   2u   /* keep the aspect ratio, fill and crop */

/* The built-in scratch used when the caller gives none (see mimg_render_ex). */
#define MIMG_BUILTIN_SCRATCH (256u * 1024u)

#define MVG_OP_END 0u
#define MVG_OP_M   1u
#define MVG_OP_L   2u
#define MVG_OP_Q   3u
#define MVG_OP_C   4u
#define MVG_OP_Z   5u
#define MVG_OP_H   6u
#define MVG_OP_V   7u
#define MVG_BAND_MAX 1024u              /* most rows in one accumulation band */
#define MVG_ACC_BYTES (128u * 1024u)    /* accumulation buffer budget */
#define MVG_FIXED_SCRATCH (MIMG_VEC_MAX_PAINTS * 4u + 2u * MVG_BAND_MAX * 2u + 2u * 256u * 4u + 64u)

/* --------------------------------------------------------- reading */

typedef struct { const uint8_t* p; const uint8_t* end; bool bad; } mvg_cur_t;

static inline uint32_t mvg_u8(mvg_cur_t* c) {
    if (c->p >= c->end) { c->bad = true; return 0; }
    return *c->p++;
}
static inline uint32_t mvg_uv(mvg_cur_t* c) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < 5; i++) {
        uint32_t b = mvg_u8(c);
        if (c->bad) return 0;
        if (i == 4 && (b & 0xF0u)) { c->bad = true; return 0; }
        v |= (b & 0x7Fu) << (7u * i);
        if (!(b & 0x80u)) return v;
    }
    c->bad = true;
    return 0;
}
static inline int32_t mvg_sv(mvg_cur_t* c) {
    uint32_t u = mvg_uv(c);
    return (int32_t)(u >> 1) ^ -(int32_t)(u & 1u);
}
static inline uint32_t mvg_u32(mvg_cur_t* c) {
    if ((size_t)(c->end - c->p) < 4u) { c->bad = true; c->p = c->end; return 0; }
    uint32_t v = mm_get32(c->p);
    c->p += 4;
    return v;
}
static inline float mvg_f32(mvg_cur_t* c) {
    union { uint32_t u; float f; } v;
    v.u = mvg_u32(c);
    if (((v.u >> 23) & 255u) == 255u) { c->bad = true; return 0.0f; }   /* NaN, infinity */
    return v.f;
}
static inline int32_t mvg_add(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }

typedef struct { float a, b, c, d, e, f; } mvg_mat_t;   /* x' = a x + c y + e; y' = b x + d y + f */

typedef struct {
    uint32_t type, spread, nstops;
    bool has_matrix;
    uint32_t color;                 /* solid: 0xAARRGGBB; current: alpha << 24 */
    uint32_t stop_col[MIMG_VEC_MAX_STOPS];
    uint8_t stop_off[MIMG_VEC_MAX_STOPS];
    float g[5];
    mvg_mat_t m;
} mvg_paint_t;

static bool mvg_read_mat(mvg_cur_t* c, mvg_mat_t* m) {
    m->a = mvg_f32(c); m->b = mvg_f32(c); m->c = mvg_f32(c);
    m->d = mvg_f32(c); m->e = mvg_f32(c); m->f = mvg_f32(c);
    return !c->bad;
}

static bool mvg_read_paint(mvg_cur_t* c, mvg_paint_t* p) {
    uint32_t b = mvg_u8(c);
    if (c->bad || (b >> 5) || ((b >> 2) & 3u) == 3u) return false;
    p->type = b & 3u;
    p->spread = (b >> 2) & 3u;
    p->has_matrix = (b >> 4) & 1u;
    p->nstops = 0;
    p->color = 0;
    if (p->type == MIMG_PAINT_SOLID) {
        p->color = mvg_u32(c);
    } else if (p->type == MIMG_PAINT_CURRENT) {
        p->color = mvg_u8(c) << 24;
    } else {
        uint32_t n = mvg_u8(c);
        if (n < 1 || n > MIMG_VEC_MAX_STOPS) return false;
        p->nstops = n;
        for (uint32_t i = 0; i < n; i++) { p->stop_off[i] = (uint8_t)mvg_u8(c); p->stop_col[i] = mvg_u32(c); }
        uint32_t ng = p->type == MIMG_PAINT_LINEAR ? 4u : 5u;
        for (uint32_t i = 0; i < ng; i++) p->g[i] = mvg_f32(c);
    }
    if (p->has_matrix) mvg_read_mat(c, &p->m);
    else { p->m.a = 1; p->m.b = 0; p->m.c = 0; p->m.d = 1; p->m.e = 0; p->m.f = 0; }
    return !c->bad;
}

typedef struct {
    uint32_t width, height, shift, flags;
    int32_t vbx, vby;
    uint32_t vbw, vbh;
    uint32_t npaints, nshapes;
    const uint8_t* paints;          /* first paint */
    const uint8_t* shapes;          /* first shape (after mvg_validate) */
    const uint8_t* end;             /* end of the payload */
} mvg_doc_t;

typedef struct {
    uint32_t flags, fill_paint, stroke_paint, fill_op, stroke_op, stroke_w, cap, join, miter8;
    mvg_mat_t xf;
    const uint8_t* path;
} mvg_shape_t;

/* Header and view box; the paints and shapes are not looked at. */
static bool mvg_open(const uint8_t* d, size_t size, mvg_doc_t* doc) {
    if (size < MIMG_HEADER_SIZE || !mm_same(d, MIMG_MAGIC, 4) || d[4] != MIMG_VERSION_VECTOR || d[5] != 4 ||
        d[6] > MIMG_VEC_MAX_SHIFT || (d[7] & ~1u))
        return false;
    uint32_t w = mm_get32(d + 8), h = mm_get32(d + 12), psize = mm_get32(d + 16);
    if (!w || !h || w > MMEDIA_MAX_SIDE || h > MMEDIA_MAX_SIDE || mm_get32(d + 20) != 0 ||
        psize > size - MIMG_HEADER_SIZE)
        return false;
    mvg_cur_t c = { d + MIMG_HEADER_SIZE, d + MIMG_HEADER_SIZE + psize, false };
    doc->width = w; doc->height = h; doc->shift = d[6]; doc->flags = d[7];
    doc->vbx = mvg_sv(&c); doc->vby = mvg_sv(&c);
    doc->vbw = mvg_uv(&c); doc->vbh = mvg_uv(&c);
    uint32_t np = mvg_u8(&c); np |= mvg_u8(&c) << 8;
    uint32_t ns = mvg_u8(&c); ns |= mvg_u8(&c) << 8;
    if (c.bad || !doc->vbw || !doc->vbh || doc->vbw > 0x40000000u || doc->vbh > 0x40000000u ||
        np > MIMG_VEC_MAX_PAINTS)
        return false;
    doc->npaints = np; doc->nshapes = ns;
    doc->paints = c.p; doc->shapes = NULL; doc->end = c.end;
    return true;
}

typedef struct {
    mvg_cur_t c;
    uint32_t op, left;
    int32_t cx, cy, sx, sy;
    bool started, closed;
} mvg_path_t;

static inline void mvg_path_init(mvg_path_t* P, const uint8_t* p, const uint8_t* end) {
    P->c.p = p; P->c.end = end; P->c.bad = false;
    P->op = P->left = 0; P->cx = P->cy = P->sx = P->sy = 0; P->started = P->closed = false;
}

/* Next segment with absolute fixed coordinates in q: 0 at the end of the
 * path, -1 on error, else MVG_OP_M / L / Q / C / Z (H and V come as L). A
 * move is made up before a drawing command that follows a close. */
static int mvg_path_next(mvg_path_t* P, int32_t q[6]) {
    if (P->left == 0) {
        uint32_t b = mvg_u8(&P->c);
        if (P->c.bad) return -1;
        if ((b & 7u) == MVG_OP_END) {
            if (b) { P->c.bad = true; return -1; }
            return 0;
        }
        P->op = b & 7u;
        P->left = (b >> 3) + 1u;
        if (!P->started && P->op != MVG_OP_M) { P->c.bad = true; return -1; }
    }
    if (P->closed && P->op != MVG_OP_M && P->op != MVG_OP_Z) {
        P->closed = false;
        P->cx = P->sx; P->cy = P->sy;
        q[0] = P->sx; q[1] = P->sy;
        return (int)MVG_OP_M;
    }
    P->left--;
    switch (P->op) {
    case MVG_OP_M:
        P->cx = mvg_add(P->cx, mvg_sv(&P->c)); P->cy = mvg_add(P->cy, mvg_sv(&P->c));
        P->sx = P->cx; P->sy = P->cy; P->started = true; P->closed = false;
        q[0] = P->cx; q[1] = P->cy;
        break;
    case MVG_OP_L:
        P->cx = mvg_add(P->cx, mvg_sv(&P->c)); P->cy = mvg_add(P->cy, mvg_sv(&P->c));
        q[0] = P->cx; q[1] = P->cy;
        break;
    case MVG_OP_H:
        P->cx = mvg_add(P->cx, mvg_sv(&P->c));
        q[0] = P->cx; q[1] = P->cy;
        break;
    case MVG_OP_V:
        P->cy = mvg_add(P->cy, mvg_sv(&P->c));
        q[0] = P->cx; q[1] = P->cy;
        break;
    case MVG_OP_Q:
        q[0] = mvg_add(P->cx, mvg_sv(&P->c)); q[1] = mvg_add(P->cy, mvg_sv(&P->c));
        q[2] = mvg_add(P->cx, mvg_sv(&P->c)); q[3] = mvg_add(P->cy, mvg_sv(&P->c));
        P->cx = q[2]; P->cy = q[3];
        break;
    case MVG_OP_C:
        q[0] = mvg_add(P->cx, mvg_sv(&P->c)); q[1] = mvg_add(P->cy, mvg_sv(&P->c));
        q[2] = mvg_add(P->cx, mvg_sv(&P->c)); q[3] = mvg_add(P->cy, mvg_sv(&P->c));
        q[4] = mvg_add(P->cx, mvg_sv(&P->c)); q[5] = mvg_add(P->cy, mvg_sv(&P->c));
        P->cx = q[4]; P->cy = q[5];
        break;
    default:
        P->cx = P->sx; P->cy = P->sy; P->closed = true;
        break;
    }
    if (P->c.bad) return -1;
    return P->op == MVG_OP_H || P->op == MVG_OP_V ? (int)MVG_OP_L : (int)P->op;
}

static bool mvg_read_shape(mvg_cur_t* c, const mvg_doc_t* doc, mvg_shape_t* s) {
    uint32_t f = mvg_u8(c);
    if (c->bad || (f & 0xF0u)) return false;
    s->flags = f;
    s->fill_paint = s->stroke_paint = s->fill_op = s->stroke_op = 0;
    s->stroke_w = s->cap = s->join = 0; s->miter8 = 32;
    s->xf.a = 1; s->xf.b = 0; s->xf.c = 0; s->xf.d = 1; s->xf.e = 0; s->xf.f = 0;
    if (f & 1u) {
        s->fill_paint = mvg_uv(c); s->fill_op = mvg_u8(c);
        if (s->fill_paint >= doc->npaints) return false;
    }
    if (f & 2u) {
        s->stroke_paint = mvg_uv(c); s->stroke_w = mvg_uv(c);
        uint32_t cj = mvg_u8(c);
        s->cap = cj & 3u; s->join = (cj >> 2) & 3u;
        if ((cj >> 4) || s->cap > MIMG_CAP_SQUARE || s->join > MIMG_JOIN_BEVEL) return false;
        s->miter8 = mvg_u8(c); s->stroke_op = mvg_u8(c);
        if (s->stroke_paint >= doc->npaints || s->stroke_w > 0x40000000u) return false;
    }
    if ((f & 8u) && !mvg_read_mat(c, &s->xf)) return false;
    s->path = c->p;
    return !c->bad;
}

/* Walks the whole payload: every paint and shape must parse, every index
 * must be in range and the payload must end exactly after the last shape.
 * `ptab`, when given, gets the offset of each paint from doc->paints. */
static bool mvg_validate(mvg_doc_t* doc, uint32_t* ptab) {
    mvg_cur_t c = { doc->paints, doc->end, false };
    for (uint32_t i = 0; i < doc->npaints; i++) {
        mvg_paint_t p;
        if (ptab) ptab[i] = (uint32_t)(c.p - doc->paints);
        if (!mvg_read_paint(&c, &p)) return false;
    }
    doc->shapes = c.p;
    for (uint32_t i = 0; i < doc->nshapes; i++) {
        mvg_shape_t s;
        if (!mvg_read_shape(&c, doc, &s)) return false;
        mvg_path_t P;
        mvg_path_init(&P, c.p, c.end);
        int32_t q[6];
        int r;
        while ((r = mvg_path_next(&P, q)) > 0) {}
        if (r < 0) return false;
        c.p = P.c.p;
    }
    return c.p == doc->end;
}

/* ------------------------------------------------------------ writing */

/* A host-side writer: paints first, then shapes, each built from move,
 * line, quad, cubic and close; mimg_vw_finish returns the file size.
 *
 *   mimg_vwriter_t w;
 *   mimg_vw_begin(&w, buf, cap, mimg_vw_default_shift(24, 24), 0, 0, 24, 24);
 *   int red = mimg_vw_paint_solid(&w, 0xFFFF0000);
 *   mimg_vstyle_t st = mimg_vstyle_fill(red);
 *   mimg_vw_shape_begin(&w, &st);
 *   mimg_vw_move(&w, 2, 2); mimg_vw_line(&w, 22, 2); mimg_vw_line(&w, 12, 22); mimg_vw_close(&w);
 *   mimg_vw_shape_end(&w);
 *   size_t n = mimg_vw_finish(&w);        // 0 if the buffer was too small or a call failed
 */
typedef struct {
    uint8_t* out;
    size_t cap, len;
    bool fail, in_shape, uses_current;
    uint32_t shift, nat_w, nat_h, npaints, nshapes;
    float scale;
    size_t counts_at, run_at;
    uint32_t run_op, run_n;
    int32_t cx, cy, sx, sy;
    bool have_move;
} mimg_vwriter_t;

typedef struct {
    int fill_paint;                 /* paint index, or -1 for no fill */
    bool even_odd;
    uint8_t fill_opacity;           /* 0..255 */
    int stroke_paint;               /* paint index, or -1 for no stroke */
    float stroke_width;             /* drawing units */
    uint8_t cap, join;              /* MIMG_CAP_*, MIMG_JOIN_* */
    float miter_limit;              /* default 4 */
    uint8_t stroke_opacity;
    const float* transform;         /* a b c d e f, or NULL */
} mimg_vstyle_t;

static inline mimg_vstyle_t mimg_vstyle_fill(int paint) {
    mimg_vstyle_t s = { paint, false, 255, -1, 0.0f, MIMG_CAP_BUTT, MIMG_JOIN_MITER, 4.0f, 255, NULL };
    return s;
}
static inline mimg_vstyle_t mimg_vstyle_stroke(int paint, float width) {
    mimg_vstyle_t s = { -1, false, 255, paint, width, MIMG_CAP_BUTT, MIMG_JOIN_MITER, 4.0f, 255, NULL };
    return s;
}

static inline void mvg_w_u8(mimg_vwriter_t* w, uint32_t v) {
    if (w->len >= w->cap) { w->fail = true; return; }
    w->out[w->len++] = (uint8_t)v;
}
static inline void mvg_w_uv(mimg_vwriter_t* w, uint32_t v) {
    while (v >= 0x80u) { mvg_w_u8(w, (v & 0x7Fu) | 0x80u); v >>= 7; }
    mvg_w_u8(w, v);
}
static inline void mvg_w_sv(mimg_vwriter_t* w, int32_t v) { mvg_w_uv(w, ((uint32_t)v << 1) ^ (uint32_t)(v >> 31)); }
static inline void mvg_w_u32(mimg_vwriter_t* w, uint32_t v) {
    if (w->cap - w->len < 4u || w->len > w->cap) { w->fail = true; return; }
    mm_put32(w->out + w->len, v);
    w->len += 4;
}
static inline void mvg_w_f32(mimg_vwriter_t* w, float f) {
    union { uint32_t u; float f; } v;
    v.f = f;
    if (((v.u >> 23) & 255u) == 255u) v.f = 0.0f;    /* NaN and infinity cannot be read back */
    mvg_w_u32(w, v.u);
}
static inline int32_t mvg_w_fix(const mimg_vwriter_t* w, float v) {
    float s = v * w->scale;
    if (!(s == s)) return 0;
    if (s > 1073741824.0f) s = 1073741824.0f;
    if (s < -1073741824.0f) s = -1073741824.0f;
    return (int32_t)(s + (s >= 0.0f ? 0.5f : -0.5f));
}

/* The shift that gives a drawing of this size about 14 bits of precision. */
static inline uint32_t mimg_vw_default_shift(float vbw, float vbh) {
    float m = vbw > vbh ? vbw : vbh;
    uint32_t bits = 0;
    while (m >= 1.0f && bits < 20u) { m *= 0.5f; bits++; }
    return bits >= 14u ? 0u : (14u - bits > MIMG_VEC_MAX_SHIFT ? MIMG_VEC_MAX_SHIFT : 14u - bits);
}

MM_FN bool mimg_vw_begin(mimg_vwriter_t* w, uint8_t* out, size_t cap, uint32_t shift, float vbx, float vby,
                         float vbw, float vbh) {
    w->out = out; w->cap = cap; w->len = 0; w->fail = false; w->in_shape = false; w->uses_current = false;
    w->npaints = w->nshapes = 0; w->run_n = 0; w->have_move = false;
    w->cx = w->cy = w->sx = w->sy = 0;
    if (!out || cap < 64u || shift > MIMG_VEC_MAX_SHIFT || !(vbw > 0.0f) || !(vbh > 0.0f) || vbw > 1.0e6f ||
        vbh > 1.0e6f) {
        w->fail = true;
        return false;
    }
    w->shift = shift;
    w->scale = (float)(1u << shift);
    float cw = vbw, ch = vbh;
    if (cw > (float)MMEDIA_MAX_SIDE) cw = (float)MMEDIA_MAX_SIDE;
    if (ch > (float)MMEDIA_MAX_SIDE) ch = (float)MMEDIA_MAX_SIDE;
    w->nat_w = (uint32_t)cw + ((float)(uint32_t)cw < cw ? 1u : 0u);
    w->nat_h = (uint32_t)ch + ((float)(uint32_t)ch < ch ? 1u : 0u);
    mm_copy(out, MIMG_MAGIC, 4);
    out[4] = MIMG_VERSION_VECTOR; out[5] = 4; out[6] = (uint8_t)shift; out[7] = 0;
    mm_put32(out + 8, w->nat_w); mm_put32(out + 12, w->nat_h); mm_put32(out + 16, 0); mm_put32(out + 20, 0);
    w->len = MIMG_HEADER_SIZE;
    mvg_w_sv(w, mvg_w_fix(w, vbx)); mvg_w_sv(w, mvg_w_fix(w, vby));
    int32_t fw = mvg_w_fix(w, vbw), fh = mvg_w_fix(w, vbh);
    mvg_w_uv(w, fw < 1 ? 1u : (uint32_t)fw); mvg_w_uv(w, fh < 1 ? 1u : (uint32_t)fh);
    w->counts_at = w->len;
    mvg_w_u8(w, 0); mvg_w_u8(w, 0); mvg_w_u8(w, 0); mvg_w_u8(w, 0);
    return !w->fail;
}

/* Overrides the natural size in the header (default: the view box rounded up). */
static inline void mimg_vw_set_size(mimg_vwriter_t* w, uint32_t width, uint32_t height) {
    if (!width || !height || width > MMEDIA_MAX_SIDE || height > MMEDIA_MAX_SIDE) { w->fail = true; return; }
    w->nat_w = width; w->nat_h = height;
}

static int mvg_w_paint_head(mimg_vwriter_t* w) {
    if (w->fail || w->in_shape || w->nshapes || w->npaints >= MIMG_VEC_MAX_PAINTS) { w->fail = true; return -1; }
    return (int)w->npaints++;
}

MM_FN int mimg_vw_paint_solid(mimg_vwriter_t* w, uint32_t argb) {
    int i = mvg_w_paint_head(w);
    if (i < 0) return -1;
    mvg_w_u8(w, MIMG_PAINT_SOLID);
    mvg_w_u32(w, argb);
    return w->fail ? -1 : i;
}

/* "Current color": the caller's color tints it; `alpha` multiplies the caller's alpha. */
MM_FN int mimg_vw_paint_current(mimg_vwriter_t* w, uint8_t alpha) {
    int i = mvg_w_paint_head(w);
    if (i < 0) return -1;
    mvg_w_u8(w, MIMG_PAINT_CURRENT);
    mvg_w_u8(w, alpha);
    w->uses_current = true;
    return w->fail ? -1 : i;
}

/* geom: linear x1 y1 x2 y2, radial cx cy r fx fy. matrix: a b c d e f or NULL. */
MM_FN int mimg_vw_paint_gradient(mimg_vwriter_t* w, bool radial, uint32_t spread, const uint8_t* offsets,
                                 const uint32_t* colors, uint32_t nstops, const float* geom,
                                 const float* matrix) {
    if (nstops < 1 || nstops > MIMG_VEC_MAX_STOPS || spread > MIMG_SPREAD_REPEAT) { w->fail = true; return -1; }
    int i = mvg_w_paint_head(w);
    if (i < 0) return -1;
    mvg_w_u8(w, (radial ? MIMG_PAINT_RADIAL : MIMG_PAINT_LINEAR) | spread << 2 | (matrix ? 16u : 0u));
    mvg_w_u8(w, nstops);
    for (uint32_t k = 0; k < nstops; k++) { mvg_w_u8(w, offsets[k]); mvg_w_u32(w, colors[k]); }
    for (uint32_t k = 0; k < (radial ? 5u : 4u); k++) mvg_w_f32(w, geom[k]);
    if (matrix) for (uint32_t k = 0; k < 6u; k++) mvg_w_f32(w, matrix[k]);
    return w->fail ? -1 : i;
}

MM_FN bool mimg_vw_shape_begin(mimg_vwriter_t* w, const mimg_vstyle_t* s) {
    if (w->fail || w->in_shape || w->nshapes >= MIMG_VEC_MAX_SHAPES) { w->fail = true; return false; }
    bool fill = s->fill_paint >= 0, stroke = s->stroke_paint >= 0;
    if ((fill && (uint32_t)s->fill_paint >= w->npaints) || (stroke && (uint32_t)s->stroke_paint >= w->npaints)) {
        w->fail = true;
        return false;
    }
    mvg_w_u8(w, (fill ? 1u : 0u) | (stroke ? 2u : 0u) | (s->even_odd ? 4u : 0u) | (s->transform ? 8u : 0u));
    if (fill) { mvg_w_uv(w, (uint32_t)s->fill_paint); mvg_w_u8(w, s->fill_opacity); }
    if (stroke) {
        int32_t sw = mvg_w_fix(w, s->stroke_width);
        float ml = s->miter_limit * 8.0f + 0.5f;
        mvg_w_uv(w, (uint32_t)s->stroke_paint);
        mvg_w_uv(w, sw < 0 ? 0u : (uint32_t)sw);
        mvg_w_u8(w, (s->cap & 3u) | (uint32_t)(s->join & 3u) << 2);
        mvg_w_u8(w, ml < 8.0f ? 8u : ml > 255.0f ? 255u : (uint32_t)ml);
        mvg_w_u8(w, s->stroke_opacity);
    }
    if (s->transform) for (uint32_t k = 0; k < 6u; k++) mvg_w_f32(w, s->transform[k]);
    w->in_shape = true; w->run_n = 0; w->have_move = false;
    w->cx = w->cy = w->sx = w->sy = 0;
    return !w->fail;
}

static void mvg_w_op(mimg_vwriter_t* w, uint32_t op) {
    if (w->run_n && w->run_op == op && w->run_n < 32u && op != MVG_OP_M) {
        w->run_n++;
        w->out[w->run_at] = (uint8_t)(op | (w->run_n - 1u) << 3);
        return;
    }
    w->run_at = w->len;
    w->run_op = op; w->run_n = 1;
    mvg_w_u8(w, op);
}

MM_FN void mimg_vw_move(mimg_vwriter_t* w, float x, float y) {
    if (!w->in_shape) { w->fail = true; return; }
    int32_t fx = mvg_w_fix(w, x), fy = mvg_w_fix(w, y);
    mvg_w_op(w, MVG_OP_M);
    mvg_w_sv(w, mvg_add(fx, -w->cx)); mvg_w_sv(w, mvg_add(fy, -w->cy));
    w->cx = w->sx = fx; w->cy = w->sy = fy; w->have_move = true;
}
MM_FN void mimg_vw_line(mimg_vwriter_t* w, float x, float y) {
    if (!w->in_shape || !w->have_move) { w->fail = true; return; }
    int32_t fx = mvg_w_fix(w, x), fy = mvg_w_fix(w, y);
    if (fy == w->cy) { mvg_w_op(w, MVG_OP_H); mvg_w_sv(w, mvg_add(fx, -w->cx)); }
    else if (fx == w->cx) { mvg_w_op(w, MVG_OP_V); mvg_w_sv(w, mvg_add(fy, -w->cy)); }
    else { mvg_w_op(w, MVG_OP_L); mvg_w_sv(w, mvg_add(fx, -w->cx)); mvg_w_sv(w, mvg_add(fy, -w->cy)); }
    w->cx = fx; w->cy = fy;
}
MM_FN void mimg_vw_quad(mimg_vwriter_t* w, float cx, float cy, float x, float y) {
    if (!w->in_shape || !w->have_move) { w->fail = true; return; }
    int32_t ax = mvg_w_fix(w, cx), ay = mvg_w_fix(w, cy), fx = mvg_w_fix(w, x), fy = mvg_w_fix(w, y);
    mvg_w_op(w, MVG_OP_Q);
    mvg_w_sv(w, mvg_add(ax, -w->cx)); mvg_w_sv(w, mvg_add(ay, -w->cy));
    mvg_w_sv(w, mvg_add(fx, -w->cx)); mvg_w_sv(w, mvg_add(fy, -w->cy));
    w->cx = fx; w->cy = fy;
}
MM_FN void mimg_vw_cubic(mimg_vwriter_t* w, float c1x, float c1y, float c2x, float c2y, float x, float y) {
    if (!w->in_shape || !w->have_move) { w->fail = true; return; }
    int32_t ax = mvg_w_fix(w, c1x), ay = mvg_w_fix(w, c1y), bx = mvg_w_fix(w, c2x), by = mvg_w_fix(w, c2y);
    int32_t fx = mvg_w_fix(w, x), fy = mvg_w_fix(w, y);
    mvg_w_op(w, MVG_OP_C);
    mvg_w_sv(w, mvg_add(ax, -w->cx)); mvg_w_sv(w, mvg_add(ay, -w->cy));
    mvg_w_sv(w, mvg_add(bx, -w->cx)); mvg_w_sv(w, mvg_add(by, -w->cy));
    mvg_w_sv(w, mvg_add(fx, -w->cx)); mvg_w_sv(w, mvg_add(fy, -w->cy));
    w->cx = fx; w->cy = fy;
}
MM_FN void mimg_vw_close(mimg_vwriter_t* w) {
    if (!w->in_shape || !w->have_move) { w->fail = true; return; }
    mvg_w_op(w, MVG_OP_Z);
    w->cx = w->sx; w->cy = w->sy;
}
MM_FN bool mimg_vw_shape_end(mimg_vwriter_t* w) {
    if (!w->in_shape || !w->have_move) { w->fail = true; return false; }
    mvg_w_u8(w, 0);
    w->run_n = 0; w->in_shape = false; w->nshapes++;
    return !w->fail;
}

/* Completes the header; returns the size of the file, or 0 on error. */
MM_FN size_t mimg_vw_finish(mimg_vwriter_t* w) {
    if (w->fail || w->in_shape) return 0;
    w->out[w->counts_at] = (uint8_t)w->npaints; w->out[w->counts_at + 1] = (uint8_t)(w->npaints >> 8);
    w->out[w->counts_at + 2] = (uint8_t)w->nshapes; w->out[w->counts_at + 3] = (uint8_t)(w->nshapes >> 8);
    w->out[7] = w->uses_current ? 1u : 0u;
    mm_put32(w->out + 8, w->nat_w); mm_put32(w->out + 12, w->nat_h);
    mm_put32(w->out + 16, (uint32_t)(w->len - MIMG_HEADER_SIZE));
    return w->len;
}

/* ------------------------------------------------------- rasterizer */

/* Shapes are flattened to line segments in device space (curves split by
 * how far they bend at the target scale), strokes are turned into a set of
 * convex polygons (one per segment, join and cap, all wound the same way),
 * and the segments are accumulated into a float buffer as exact signed
 * area per pixel (the method of font-rs and stb_truetype): a pixel's
 * coverage is its running sum along the row, nonzero = min(|sum|, 1),
 * even-odd = a triangle wave of |sum|. That is exact area coverage for any
 * edge crossing a pixel; where several edges cross the same pixel the fill
 * rules are approximated the way FreeType and AGG approximate them.
 * The accumulation works in bands of rows so the buffer stays small. */

static inline float mvg_absf(float v) { return v < 0.0f ? -v : v; }
static inline float mvg_sqrt(float x) {
    if (!(x > 0.0f)) return 0.0f;
#if defined(__x86_64__) && defined(__GNUC__)
    float r;
    __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x));
    return r;
#else
    union { float f; uint32_t u; } v;
    v.f = x;
    v.u = 0x1FBD1DF5u + (v.u >> 1);
    float r = v.f;
    r = 0.5f * (r + x / r);
    r = 0.5f * (r + x / r);
    r = 0.5f * (r + x / r);
    return r;
#endif
}
static inline bool mvg_finite(float v) { return v == v && v - v == 0.0f; }
static inline float mvg_clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline int mvg_ifloor(float v) { int i = (int)v; return (float)i > v ? i - 1 : i; }
static inline int mvg_iceil(float v) { int i = (int)v; return (float)i < v ? i + 1 : i; }
static inline uint32_t mvg_div255(uint32_t v) { return (v + 128u + ((v + 128u) >> 8)) >> 8; }

static inline mvg_mat_t mvg_mul(const mvg_mat_t* A, const mvg_mat_t* B) {   /* A after B */
    mvg_mat_t r;
    r.a = A->a * B->a + A->c * B->b; r.b = A->b * B->a + A->d * B->b;
    r.c = A->a * B->c + A->c * B->d; r.d = A->b * B->c + A->d * B->d;
    r.e = A->a * B->e + A->c * B->f + A->e; r.f = A->b * B->e + A->d * B->f + A->f;
    return r;
}
static bool mvg_invert(const mvg_mat_t* m, mvg_mat_t* r) {
    float det = m->a * m->d - m->b * m->c;
    if (!mvg_finite(det) || mvg_absf(det) < 1e-18f) return false;
    float id = 1.0f / det;
    r->a = m->d * id; r->b = -m->b * id; r->c = -m->c * id; r->d = m->a * id;
    r->e = (m->c * m->f - m->d * m->e) * id; r->f = (m->b * m->e - m->a * m->f) * id;
    return mvg_finite(r->a) && mvg_finite(r->b) && mvg_finite(r->c) && mvg_finite(r->d) &&
           mvg_finite(r->e) && mvg_finite(r->f);
}

typedef struct { float x, y; } mvg_pt_t;

/* A paint ready to be sampled: a solid color, or a gradient and the map from
 * device pixels to gradient space. */
typedef struct {
    int kind;                        /* 0 solid, 1 linear, 2 radial */
    uint32_t solid;
    const uint32_t* lut;
    uint32_t spread;
    mvg_mat_t inv;                   /* device -> gradient space */
    float tx, ty, t0;                /* linear: t = t0 + tx px + ty py */
    float fx, fy, dx, dy, a, r;      /* radial: focal point, centre - focal, d.d - r^2, radius */
    bool dead;                       /* nothing to paint */
} mvg_pi_t;

typedef struct {
    uint32_t* out;
    uint32_t ostride, W, H;
    float* acc;
    uint32_t acc_cells;
    int16_t* rmn;
    int16_t* rmx;
    float* edges;
    uint32_t edge_cap, edge_n;
    bool overflow, failed;
    uint32_t* lut[2];
    uint32_t current;
    const mvg_doc_t* doc;
    const uint32_t* ptab;
    float inv;
    /* what is being generated */
    mvg_mat_t in_m, out_m;
    bool has_out, stroke;
    float tol, hw, mlimit;
    uint32_t cap, join;
    float py0, py1;
    /* stroker */
    mvg_pt_t s_first, s_prev, s_first_d, s_prev_d;
    uint32_t s_nseg;
    bool s_started, s_seen;
} mvg_ctx_t;

/* ---- accumulation */

static void mvg_acc_core(float* a, uint32_t stride, int16_t* rmn, int16_t* rmx, float x0, float y0, float x1,
                         float y1, float dir, float W, uint32_t H) {
    /* y0 < y1, both in [0, H]; x in [0, W] */
    float dxdy = (x1 - x0) / (y1 - y0);
    float x = x0;
    int iy0 = mvg_ifloor(y0), iy1 = mvg_iceil(y1);
    if (iy1 > (int)H) iy1 = (int)H;
    if (iy0 < 0) iy0 = 0;
    for (int y = iy0; y < iy1; y++) {
        float top = (float)y > y0 ? (float)y : y0;
        float bot = (float)(y + 1) < y1 ? (float)(y + 1) : y1;
        float dy = bot - top;
        float xn = x + dxdy * dy;
        if (xn < 0.0f) xn = 0.0f;
        if (xn > W) xn = W;
        float d = dy * dir;
        float lo = x < xn ? x : xn, hi = x < xn ? xn : x;
        float lof = (float)mvg_ifloor(lo);
        int x0i = (int)lof;
        int x1i = mvg_iceil(hi);
        float* row = a + (size_t)y * stride;
        int hi_i;
        if (x1i <= x0i + 1) {
            float xmf = 0.5f * (x + xn) - lof;
            row[x0i] += d - d * xmf;
            row[x0i + 1] += d * xmf;
            hi_i = x0i + 1;
        } else {
            float s = 1.0f / (hi - lo);
            float x0f = lo - lof;
            float a0 = 0.5f * s * (1.0f - x0f) * (1.0f - x0f);
            float x1f = hi - (float)x1i + 1.0f;
            float am = 0.5f * s * x1f * x1f;
            row[x0i] += d * a0;
            if (x1i == x0i + 2) {
                row[x0i + 1] += d * (1.0f - a0 - am);
            } else {
                float a1 = s * (1.5f - x0f);
                row[x0i + 1] += d * (a1 - a0);
                for (int xi = x0i + 2; xi < x1i - 1; xi++) row[xi] += d * s;
                float a2 = a1 + (float)(x1i - x0i - 3) * s;
                row[x1i - 1] += d * (1.0f - a2 - am);
            }
            row[x1i] += d * am;
            hi_i = x1i;
        }
        if (x0i < rmn[y]) rmn[y] = (int16_t)x0i;
        if (hi_i > rmx[y]) rmx[y] = (int16_t)hi_i;
        x = xn;
    }
}

/* One edge into a band of H rows and W columns (device coordinates relative to it). */
static void mvg_acc_line(float* a, uint32_t stride, int16_t* rmn, int16_t* rmx, float x0, float y0, float x1,
                         float y1, float W, uint32_t H) {
    if (y0 == y1) return;
    float dir = 1.0f;
    if (y0 > y1) { float t; t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; dir = -1.0f; }
    float fh = (float)H;
    if (y1 <= 0.0f || y0 >= fh) return;
    float dxdy = (x1 - x0) / (y1 - y0);
    if (y0 < 0.0f) { x0 -= y0 * dxdy; y0 = 0.0f; }
    if (y1 > fh) { x1 = x0 + (fh - y0) * dxdy; y1 = fh; }
    if (y0 >= y1) return;
    if (x0 <= 0.0f && x1 <= 0.0f) { mvg_acc_core(a, stride, rmn, rmx, 0.0f, y0, 0.0f, y1, dir, W, H); return; }
    if (x0 >= W && x1 >= W) return;
    if (x0 >= 0.0f && x0 <= W && x1 >= 0.0f && x1 <= W) { mvg_acc_core(a, stride, rmn, rmx, x0, y0, x1, y1, dir, W, H); return; }
    /* crosses x = 0 or x = W: split into pieces that are on one side */
    float ys[4]; int n = 0;
    ys[n++] = y0;
    float cut[2]; int nc = 0;
    if ((x0 < 0.0f) != (x1 < 0.0f)) cut[nc++] = y0 + (0.0f - x0) / dxdy * 1.0f;
    if ((x0 > W) != (x1 > W)) cut[nc++] = y0 + (W - x0) / dxdy;
    if (nc == 2 && cut[1] < cut[0]) { float t = cut[0]; cut[0] = cut[1]; cut[1] = t; }
    for (int i = 0; i < nc; i++) ys[n++] = mvg_clampf(cut[i], y0, y1);
    ys[n++] = y1;
    for (int i = 0; i + 1 < n; i++) {
        float ya = ys[i], yb = ys[i + 1];
        if (yb <= ya) continue;
        float xa = x0 + (ya - y0) * dxdy, xb = x0 + (yb - y0) * dxdy;
        if (xa <= 0.0f && xb <= 0.0f) mvg_acc_core(a, stride, rmn, rmx, 0.0f, ya, 0.0f, yb, dir, W, H);
        else if (xa >= W && xb >= W) continue;
        else mvg_acc_core(a, stride, rmn, rmx, mvg_clampf(xa, 0.0f, W), ya, mvg_clampf(xb, 0.0f, W), yb, dir, W, H);
    }
}

/* ---- edge collection */

static void mvg_edge(mvg_ctx_t* c, float x0, float y0, float x1, float y1) {
    if (c->has_out) {
        const mvg_mat_t* m = &c->out_m;
        float ax = m->a * x0 + m->c * y0 + m->e, ay = m->b * x0 + m->d * y0 + m->f;
        float bx = m->a * x1 + m->c * y1 + m->e, by = m->b * x1 + m->d * y1 + m->f;
        x0 = ax; y0 = ay; x1 = bx; y1 = by;
    }
    if (!mvg_finite(x0) || !mvg_finite(y0) || !mvg_finite(x1) || !mvg_finite(y1)) return;
    x0 = mvg_clampf(x0, -1.0e6f, 1.0e6f); x1 = mvg_clampf(x1, -1.0e6f, 1.0e6f);
    y0 = mvg_clampf(y0, -1.0e6f, 1.0e6f); y1 = mvg_clampf(y1, -1.0e6f, 1.0e6f);
    if (y0 == y1) return;
    float lo = y0 < y1 ? y0 : y1, hi = y0 < y1 ? y1 : y0;
    if (hi <= c->py0 || lo >= c->py1) return;
    if (c->edge_n >= c->edge_cap) { c->overflow = true; return; }
    float* e = c->edges + (size_t)c->edge_n * 4u;
    e[0] = x0; e[1] = y0; e[2] = x1; e[3] = y1;
    c->edge_n++;
}

/* A convex piece of a stroke: wound the same way whatever the input. */
static void mvg_poly(mvg_ctx_t* c, const mvg_pt_t* p, int n) {
    float area = 0.0f;
    for (int i = 1; i + 1 < n; i++)
        area += (p[i].x - p[0].x) * (p[i + 1].y - p[0].y) - (p[i + 1].x - p[0].x) * (p[i].y - p[0].y);
    if (area == 0.0f || !mvg_finite(area)) return;
    if (area > 0.0f) for (int i = 0; i < n; i++) { int j = i + 1 == n ? 0 : i + 1; mvg_edge(c, p[i].x, p[i].y, p[j].x, p[j].y); }
    else for (int i = 0; i < n; i++) { int j = i + 1 == n ? 0 : i + 1; mvg_edge(c, p[j].x, p[j].y, p[i].x, p[i].y); }
}

/* ---- stroker */

#define MVG_ARC_DEPTH 8
#define MVG_ARC_MAX (1 << MVG_ARC_DEPTH)

/* Points on the arc of radius hw around v from unit vector u0 to u1 (both
 * excluded), appended to out. `fb` is the bisector to use when u0 and u1
 * are opposite. */
static int mvg_arc(const mvg_ctx_t* c, mvg_pt_t v, mvg_pt_t u0, mvg_pt_t u1, mvg_pt_t fb, mvg_pt_t* out, int n,
                   int depth) {
    float dt = u0.x * u1.x + u0.y * u1.y;
    float sag = c->hw * (1.0f - mvg_sqrt((1.0f + dt) * 0.5f));
    if (depth >= MVG_ARC_DEPTH || sag <= c->tol) return n;
    mvg_pt_t m = { u0.x + u1.x, u0.y + u1.y };
    float len = mvg_sqrt(m.x * m.x + m.y * m.y);
    if (len < 1e-4f) m = fb; else { m.x /= len; m.y /= len; }
    n = mvg_arc(c, v, u0, m, fb, out, n, depth + 1);
    out[n].x = v.x + m.x * c->hw; out[n].y = v.y + m.y * c->hw; n++;
    return mvg_arc(c, v, m, u1, fb, out, n, depth + 1);
}

static void mvg_join(mvg_ctx_t* c, mvg_pt_t v, mvg_pt_t d0, mvg_pt_t d1) {
    float cr = d0.x * d1.y - d0.y * d1.x, dt = d0.x * d1.x + d0.y * d1.y;
    if (mvg_absf(cr) < 1e-6f && dt > 0.0f) return;
    float o = cr > 0.0f ? -1.0f : 1.0f;
    mvg_pt_t n0 = { -d0.y * o, d0.x * o }, n1 = { -d1.y * o, d1.x * o };
    float hw = c->hw;
    mvg_pt_t a = { v.x + n0.x * hw, v.y + n0.y * hw }, b = { v.x + n1.x * hw, v.y + n1.y * hw };
    if (c->join == MIMG_JOIN_MITER) {
        /* the tip is hw / cos(turn / 2) away; 1 / cos^2 = 2 / (1 + dot) */
        if (dt > -0.9999f && 2.0f / (1.0f + dt) <= c->mlimit * c->mlimit) {
            float k = hw / (1.0f + dt);
            mvg_pt_t tip = { v.x + (n0.x + n1.x) * k, v.y + (n0.y + n1.y) * k };
            mvg_pt_t poly[4] = { v, a, tip, b };
            mvg_poly(c, poly, 4);
            return;
        }
    } else if (c->join == MIMG_JOIN_ROUND) {
        mvg_pt_t pts[MVG_ARC_MAX + 8];
        int n = 0;
        pts[n++] = v; pts[n++] = a;
        n = mvg_arc(c, v, n0, n1, d0, pts, n, 0);
        pts[n++] = b;
        mvg_poly(c, pts, n);
        return;
    }
    mvg_pt_t tri[3] = { v, a, b };
    mvg_poly(c, tri, 3);
}

/* d points away from the line. */
static void mvg_cap(mvg_ctx_t* c, mvg_pt_t p, mvg_pt_t d) {
    float hw = c->hw;
    mvg_pt_t n = { -d.y, d.x };
    if (c->cap == MIMG_CAP_SQUARE) {
        mvg_pt_t poly[4] = { { p.x + n.x * hw, p.y + n.y * hw }, { p.x + n.x * hw + d.x * hw, p.y + n.y * hw + d.y * hw },
                             { p.x - n.x * hw + d.x * hw, p.y - n.y * hw + d.y * hw }, { p.x - n.x * hw, p.y - n.y * hw } };
        mvg_poly(c, poly, 4);
    } else if (c->cap == MIMG_CAP_ROUND) {
        mvg_pt_t pts[2 * MVG_ARC_MAX + 8];
        mvg_pt_t nn = { -n.x, -n.y };
        int k = 0;
        pts[k].x = p.x + n.x * hw; pts[k].y = p.y + n.y * hw; k++;
        k = mvg_arc(c, p, n, d, d, pts, k, 1);
        pts[k].x = p.x + d.x * hw; pts[k].y = p.y + d.y * hw; k++;
        k = mvg_arc(c, p, d, nn, d, pts, k, 1);
        pts[k].x = p.x + nn.x * hw; pts[k].y = p.y + nn.y * hw; k++;
        mvg_poly(c, pts, k);
    }
}

static void mvg_dot(mvg_ctx_t* c, mvg_pt_t p) {
    float hw = c->hw;
    if (c->cap == MIMG_CAP_SQUARE) {
        mvg_pt_t poly[4] = { { p.x - hw, p.y - hw }, { p.x + hw, p.y - hw }, { p.x + hw, p.y + hw }, { p.x - hw, p.y + hw } };
        mvg_poly(c, poly, 4);
    } else if (c->cap == MIMG_CAP_ROUND) {
        mvg_pt_t pts[4 * MVG_ARC_MAX + 8];
        mvg_pt_t u[5] = { { 1, 0 }, { 0, 1 }, { -1, 0 }, { 0, -1 }, { 1, 0 } };
        int k = 0;
        for (int i = 0; i < 4; i++) {
            pts[k].x = p.x + u[i].x * hw; pts[k].y = p.y + u[i].y * hw; k++;
            k = mvg_arc(c, p, u[i], u[i + 1], u[i + 1], pts, k, 1);
        }
        mvg_poly(c, pts, k);
    }
}

static void mvg_stk_begin(mvg_ctx_t* c, float x, float y) {
    c->s_first.x = c->s_prev.x = x; c->s_first.y = c->s_prev.y = y;
    c->s_nseg = 0; c->s_started = true; c->s_seen = false;
}

static void mvg_stk_line(mvg_ctx_t* c, float x, float y) {
    if (!c->s_started) return;
    c->s_seen = true;
    float dx = x - c->s_prev.x, dy = y - c->s_prev.y;
    float len = mvg_sqrt(dx * dx + dy * dy);
    if (!(len > 1e-5f)) return;
    mvg_pt_t d = { dx / len, dy / len };
    float hw = c->hw, nx = -d.y * hw, ny = d.x * hw;
    mvg_pt_t q[4] = { { c->s_prev.x + nx, c->s_prev.y + ny }, { x + nx, y + ny }, { x - nx, y - ny },
                      { c->s_prev.x - nx, c->s_prev.y - ny } };
    mvg_poly(c, q, 4);
    if (c->s_nseg) mvg_join(c, c->s_prev, c->s_prev_d, d); else c->s_first_d = d;
    c->s_prev.x = x; c->s_prev.y = y; c->s_prev_d = d;
    c->s_nseg++;
}

static void mvg_stk_end(mvg_ctx_t* c, bool closed) {
    if (!c->s_started) return;
    c->s_started = false;
    if (closed) c->s_seen = true;
    if (!c->s_nseg) {
        if (c->s_seen && !(closed && c->s_prev.x != c->s_first.x)) mvg_dot(c, c->s_first);
        return;
    }
    if (closed) {
        c->s_started = true;
        mvg_stk_line(c, c->s_first.x, c->s_first.y);
        c->s_started = false;
        mvg_join(c, c->s_first, c->s_prev_d, c->s_first_d);
    } else {
        mvg_pt_t nd = { -c->s_first_d.x, -c->s_first_d.y };
        mvg_cap(c, c->s_first, nd);
        mvg_cap(c, c->s_prev, c->s_prev_d);
    }
}

/* ---- path walking and flattening */

static inline mvg_pt_t mvg_xf(const mvg_mat_t* m, float x, float y) {
    mvg_pt_t p = { m->a * x + m->c * y + m->e, m->b * x + m->d * y + m->f };
    return p;
}

static inline int mvg_segments(float dd, float k, float tol) {
    float n = mvg_sqrt(k * dd / tol) + 1.0f;
    return !(n < 256.0f) ? 256 : (int)n;
}

/* Generates the edges of one path for the current pass (fill or stroke). */
static void mvg_gen(mvg_ctx_t* c, const uint8_t* path, const uint8_t* end) {
    mvg_path_t P;
    mvg_path_init(&P, path, end);
    int32_t q[6];
    mvg_pt_t cur = { 0, 0 }, start = { 0, 0 };
    bool open = false;
    int op;
    while ((op = mvg_path_next(&P, q)) > 0) {
        mvg_pt_t p[3];
        int np = op == MVG_OP_Q ? 2 : op == MVG_OP_C ? 3 : op == MVG_OP_Z ? 0 : 1;
        for (int i = 0; i < np; i++) p[i] = mvg_xf(&c->in_m, (float)q[2 * i] * c->inv, (float)q[2 * i + 1] * c->inv);
        if (op == MVG_OP_M || op == MVG_OP_Z) {
            if (open) {
                if (c->stroke) mvg_stk_end(c, op == MVG_OP_Z);
                else mvg_edge(c, cur.x, cur.y, start.x, start.y);
                open = false;
            } else if (op == MVG_OP_Z && c->stroke) {
                mvg_stk_end(c, true);
            }
            if (op == MVG_OP_Z) { cur = start; continue; }
            cur = start = p[0];
            if (c->stroke) mvg_stk_begin(c, cur.x, cur.y);
            open = true;
            continue;
        }
        if (!open) continue;
        if (op == MVG_OP_L) {
            if (c->stroke) mvg_stk_line(c, p[0].x, p[0].y);
            else mvg_edge(c, cur.x, cur.y, p[0].x, p[0].y);
            cur = p[0];
        } else {
            int n;
            if (op == MVG_OP_Q) {
                float ddx = cur.x - 2.0f * p[0].x + p[1].x, ddy = cur.y - 2.0f * p[0].y + p[1].y;
                n = mvg_segments(mvg_sqrt(ddx * ddx + ddy * ddy), 0.25f, c->tol);
            } else {
                float ax = cur.x - 2.0f * p[0].x + p[1].x, ay = cur.y - 2.0f * p[0].y + p[1].y;
                float bx = p[0].x - 2.0f * p[1].x + p[2].x, by = p[0].y - 2.0f * p[1].y + p[2].y;
                float da = ax * ax + ay * ay, db = bx * bx + by * by;
                n = mvg_segments(mvg_sqrt(da > db ? da : db), 0.75f, c->tol);
            }
            mvg_pt_t last = cur;
            for (int i = 1; i <= n; i++) {
                mvg_pt_t r;
                if (i == n) r = p[np - 1];
                else {
                    float t = (float)i / (float)n, u = 1.0f - t;
                    if (op == MVG_OP_Q) {
                        r.x = u * u * cur.x + 2.0f * u * t * p[0].x + t * t * p[1].x;
                        r.y = u * u * cur.y + 2.0f * u * t * p[0].y + t * t * p[1].y;
                    } else {
                        float b0 = u * u * u, b1 = 3.0f * u * u * t, b2 = 3.0f * u * t * t, b3 = t * t * t;
                        r.x = b0 * cur.x + b1 * p[0].x + b2 * p[1].x + b3 * p[2].x;
                        r.y = b0 * cur.y + b1 * p[0].y + b2 * p[1].y + b3 * p[2].y;
                    }
                }
                if (c->stroke) mvg_stk_line(c, r.x, r.y);
                else mvg_edge(c, last.x, last.y, r.x, r.y);
                last = r;
            }
            cur = p[np - 1];
        }
    }
    if (open) {
        if (c->stroke) mvg_stk_end(c, false);
        else mvg_edge(c, cur.x, cur.y, start.x, start.y);
    }
}

/* ---- paints */

static void mvg_build_lut(const mvg_paint_t* p, uint32_t* lut) {
    uint32_t off[MIMG_VEC_MAX_STOPS];
    uint32_t n = p->nstops;
    for (uint32_t k = 0; k < n; k++) {
        off[k] = p->stop_off[k];
        if (k && off[k] < off[k - 1]) off[k] = off[k - 1];
    }
    uint32_t k = 0;
    for (uint32_t i = 0; i < 256u; i++) {
        uint32_t col;
        if (i <= off[0]) col = p->stop_col[0];
        else if (i >= off[n - 1]) col = p->stop_col[n - 1];
        else {
            while (k + 2 < n && i >= off[k + 1]) k++;
            uint32_t span = off[k + 1] - off[k];
            if (!span) col = p->stop_col[k + 1];
            else {
                uint32_t f = (i - off[k]) * 256u / span, g = 256u - f;
                col = 0;
                for (uint32_t s = 0; s < 32u; s += 8u) {
                    uint32_t a = (p->stop_col[k] >> s) & 255u, b = (p->stop_col[k + 1] >> s) & 255u;
                    col |= ((a * g + b * f + 128u) >> 8) << s;
                }
            }
        }
        lut[i] = col;
    }
}

/* `m` takes the paint's coordinates to device pixels. */
static void mvg_paint_setup(const mvg_ctx_t* c, const mvg_paint_t* p, const mvg_mat_t* shape_m, uint32_t* lut,
                            mvg_pi_t* pi) {
    pi->kind = 0; pi->dead = false; pi->lut = lut; pi->spread = p->spread;
    if (p->type == MIMG_PAINT_SOLID) { pi->solid = p->color; return; }
    if (p->type == MIMG_PAINT_CURRENT) {
        uint32_t ca = c->current >> 24, pa = p->color >> 24;
        pi->solid = (c->current & 0x00FFFFFFu) | mvg_div255(ca * pa) << 24;
        return;
    }
    mvg_build_lut(p, lut);
    pi->solid = lut[255];
    mvg_mat_t m = mvg_mul(shape_m, &p->m);
    if (!mvg_invert(&m, &pi->inv)) return;       /* degenerate: the last stop */
    const float* g = p->g;
    if (p->type == MIMG_PAINT_LINEAR) {
        float dx = g[2] - g[0], dy = g[3] - g[1], l2 = dx * dx + dy * dy;
        if (!(l2 > 1e-20f) || !mvg_finite(l2)) return;
        pi->kind = 1;
        pi->tx = (pi->inv.a * dx + pi->inv.b * dy) / l2;
        pi->ty = (pi->inv.c * dx + pi->inv.d * dy) / l2;
        pi->t0 = ((pi->inv.e - g[0]) * dx + (pi->inv.f - g[1]) * dy) / l2;
    } else {
        float r = g[2];
        if (!(r > 1e-9f) || !mvg_finite(r)) return;
        float fx = g[3], fy = g[4], dx = g[0] - fx, dy = g[1] - fy;
        float dl = mvg_sqrt(dx * dx + dy * dy);
        if (dl > r * 0.999f) {                   /* focal point on the circle at most */
            float k = r * 0.999f / dl;
            fx = g[0] - dx * k; fy = g[1] - dy * k; dx *= k; dy *= k;
        }
        pi->kind = 2;
        pi->fx = fx; pi->fy = fy; pi->dx = dx; pi->dy = dy; pi->r = r;
        pi->a = dx * dx + dy * dy - r * r;
    }
}

static inline uint32_t mvg_lut_color(const mvg_pi_t* p, float t) {
    if (!mvg_finite(t)) t = 0.0f;
    t = mvg_clampf(t, -1.0e6f, 1.0e6f);
    if (p->spread == MIMG_SPREAD_PAD) t = mvg_clampf(t, 0.0f, 1.0f);
    else if (p->spread == MIMG_SPREAD_REPEAT) { t -= (float)mvg_ifloor(t); }
    else {
        t = mvg_absf(t);
        t -= 2.0f * (float)((int)(t * 0.5f));
        if (t > 1.0f) t = 2.0f - t;
    }
    return p->lut[(int)(t * 255.0f + 0.5f)];
}

static inline uint32_t mvg_color_at(const mvg_pi_t* p, float px, float py) {
    if (p->kind == 0) return p->solid;
    if (p->kind == 1) return mvg_lut_color(p, p->t0 + p->tx * px + p->ty * py);
    float gx = p->inv.a * px + p->inv.c * py + p->inv.e - p->fx, gy = p->inv.b * px + p->inv.d * py + p->inv.f - p->fy;
    float b = gx * p->dx + gy * p->dy, cc = gx * gx + gy * gy;
    float disc = b * b - p->a * cc;
    float t = (b - mvg_sqrt(disc)) / p->a;
    return mvg_lut_color(p, t);
}

static inline uint32_t mvg_over(uint32_t dst, uint32_t rgb, uint32_t sa) {
    if (sa == 0) return dst;
    uint32_t da = dst >> 24;
    if (sa == 255u || da == 0) return sa << 24 | (rgb & 0xFFFFFFu);
    if (da == 255u) {
        uint32_t ia = 255u - sa, out = 0xFF000000u;
        for (uint32_t s = 0; s < 24u; s += 8u)
            out |= mvg_div255(((dst >> s) & 255u) * ia + ((rgb >> s) & 255u) * sa) << s;
        return out;
    }
    uint32_t wd = da * (255u - sa), ws = sa * 255u;
    uint32_t oa = sa + mvg_div255(wd);
    uint32_t den = oa * 255u, out = oa << 24;
    for (uint32_t s = 0; s < 24u; s += 8u) {
        uint32_t v = (((rgb >> s) & 255u) * ws + ((dst >> s) & 255u) * wd + den / 2u) / den;
        out |= (v > 255u ? 255u : v) << s;
    }
    return out;
}

/* ---- one band of rows: accumulate, then composite */

static void mvg_band(mvg_ctx_t* c, const mvg_pi_t* pi, uint32_t opacity, bool even_odd, int by, uint32_t bh, int X0,
                     uint32_t Wp) {
    uint32_t stride = Wp + 2u;
    for (uint32_t r = 0; r < bh; r++) { c->rmn[r] = 32767; c->rmx[r] = -1; }
    float fby = (float)by, fbe = (float)(by + (int)bh);
    for (uint32_t i = 0; i < c->edge_n; i++) {
        const float* e = c->edges + (size_t)i * 4u;
        if ((e[1] <= fby && e[3] <= fby) || (e[1] >= fbe && e[3] >= fbe)) continue;
        mvg_acc_line(c->acc, stride, c->rmn, c->rmx, e[0] - (float)X0, e[1] - fby, e[2] - (float)X0, e[3] - fby,
                     (float)Wp, bh);
    }
    for (uint32_t r = 0; r < bh; r++) {
        int lo = c->rmn[r], hi = c->rmx[r];
        if (hi < 0) continue;
        float* ar = c->acc + (size_t)r * stride;
        uint32_t* dp = c->out + (size_t)(by + (int)r) * c->ostride + (uint32_t)X0;
        float py = (float)(by + (int)r) + 0.5f;
        float sum = 0.0f;
        int lim = hi < (int)Wp ? hi : (int)Wp - 1;
        int x = lo;
        for (; x <= lim; x++) {
            sum += ar[x];
            ar[x] = 0.0f;
            float v = mvg_absf(sum);
            if (even_odd) { int iv = (int)(v > 1.0e6f ? 1.0e6f : v); v -= (float)iv; if (iv & 1) v = 1.0f - v; }
            else if (v > 1.0f) v = 1.0f;
            uint32_t c8 = (uint32_t)(v * 255.0f + 0.5f);
            if (!c8) continue;
            uint32_t col = mvg_color_at(pi, (float)(X0 + x) + 0.5f, py);
            uint32_t a = opacity == 255u ? col >> 24 : mvg_div255((col >> 24) * opacity);
            uint32_t sa = a == 255u ? c8 : mvg_div255(c8 * a);
            dp[x] = mvg_over(dp[x], col, sa);
        }
        for (; x <= hi; x++) ar[x] = 0.0f;
        if (hi + 1 < (int)Wp) {
            float v = mvg_absf(sum);
            if (even_odd) { int iv = (int)(v > 1.0e6f ? 1.0e6f : v); v -= (float)iv; if (iv & 1) v = 1.0f - v; }
            else if (v > 1.0f) v = 1.0f;
            uint32_t c8 = (uint32_t)(v * 255.0f + 0.5f);
            if (c8) {
                for (int xx = hi + 1; xx < (int)Wp; xx++) {
                    uint32_t col = mvg_color_at(pi, (float)(X0 + xx) + 0.5f, py);
                    uint32_t a = opacity == 255u ? col >> 24 : mvg_div255((col >> 24) * opacity);
                    uint32_t sa = a == 255u ? c8 : mvg_div255(c8 * a);
                    dp[xx] = mvg_over(dp[xx], col, sa);
                }
            }
        }
    }
}

/* One fill or stroke of a shape inside the device box [X0,X1) x [Y0,Y1). */
static void mvg_layer(mvg_ctx_t* c, const uint8_t* path, const uint8_t* end, const mvg_pi_t* pi, uint32_t opacity,
                      bool even_odd, int X0, int Y0, int X1, int Y1) {
    uint32_t Wp = (uint32_t)(X1 - X0);
    uint32_t band = c->acc_cells / (Wp + 2u);
    if (band > MVG_BAND_MAX) band = MVG_BAND_MAX;
    if (!band) { c->failed = true; return; }
    uint32_t per_pass = (uint32_t)(Y1 - Y0);
    int y = Y0;
    while (y < Y1) {
        uint32_t pr = per_pass;
        if (pr > (uint32_t)(Y1 - y)) pr = (uint32_t)(Y1 - y);
        c->py0 = (float)y; c->py1 = (float)(y + (int)pr);
        c->edge_n = 0; c->overflow = false;
        mvg_gen(c, path, end);
        if (c->overflow) {
            if (per_pass <= 1u) { c->failed = true; return; }
            per_pass = (per_pass + 1u) / 2u;
            continue;
        }
        for (uint32_t b = 0; b < pr; b += band) {
            uint32_t bh = pr - b < band ? pr - b : band;
            mvg_band(c, pi, opacity, even_odd, y + (int)b, bh, X0, Wp);
        }
        y += (int)pr;
    }
}

static void mvg_shape(mvg_ctx_t* c, const mvg_shape_t* s, const mvg_mat_t* view) {
    mvg_mat_t M = (s->flags & 8u) ? mvg_mul(view, &s->xf) : *view;
    float s2x = M.a * M.a + M.b * M.b, s2y = M.c * M.c + M.d * M.d;
    if (!mvg_finite(s2x) || !mvg_finite(s2y)) return;
    float det = M.a * M.d - M.b * M.c;
    if (mvg_absf(det) < 1e-12f) return;
    bool fill = (s->flags & 1u) && s->fill_op;
    bool stroke = (s->flags & 2u) && s->stroke_op && s->stroke_w;
    if (!fill && !stroke) return;
    float hwl = stroke ? (float)s->stroke_w * c->inv * 0.5f : 0.0f;
    float mlimit = (float)s->miter8 * 0.125f;
    if (mlimit < 1.0f) mlimit = 1.0f;
    /* box of the control points, grown by the stroke's reach */
    float minx = 1.0e30f, miny = 1.0e30f, maxx = -1.0e30f, maxy = -1.0e30f;
    {
        mvg_path_t P;
        mvg_path_init(&P, s->path, c->doc->end);
        int32_t q[6];
        int op;
        while ((op = mvg_path_next(&P, q)) > 0) {
            int np = op == MVG_OP_Q ? 2 : op == MVG_OP_C ? 3 : op == MVG_OP_Z ? 0 : 1;
            for (int i = 0; i < np; i++) {
                mvg_pt_t p = mvg_xf(&M, (float)q[2 * i] * c->inv, (float)q[2 * i + 1] * c->inv);
                if (!mvg_finite(p.x) || !mvg_finite(p.y)) continue;
                if (p.x < minx) minx = p.x;
                if (p.x > maxx) maxx = p.x;
                if (p.y < miny) miny = p.y;
                if (p.y > maxy) maxy = p.y;
            }
        }
    }
    if (!(minx <= maxx)) return;
    bool sim = mvg_absf(s2x - s2y) <= 1e-3f * (s2x > s2y ? s2x : s2y) &&
               mvg_absf(M.a * M.c + M.b * M.d) <= 1e-3f * (s2x > s2y ? s2x : s2y);
    float grow = 1.0f;
    if (stroke) {
        float reach = hwl * mvg_sqrt(s2x + s2y) * (mlimit > 1.5f ? mlimit : 1.5f);
        grow += reach;
    }
    float fx0 = mvg_clampf(minx - grow, 0.0f, (float)c->W), fx1 = mvg_clampf(maxx + grow, 0.0f, (float)c->W);
    float fy0 = mvg_clampf(miny - grow, 0.0f, (float)c->H), fy1 = mvg_clampf(maxy + grow, 0.0f, (float)c->H);
    int X0 = mvg_ifloor(fx0), X1 = mvg_iceil(fx1), Y0 = mvg_ifloor(fy0), Y1 = mvg_iceil(fy1);
    if (X1 <= X0 || Y1 <= Y0) return;
    const mvg_doc_t* doc = c->doc;
    if (fill) {
        mvg_cur_t pc = { doc->paints + c->ptab[s->fill_paint], doc->end, false };
        mvg_paint_t p;
        mvg_pi_t pi;
        if (mvg_read_paint(&pc, &p)) {
            mvg_paint_setup(c, &p, &M, c->lut[0], &pi);
            c->stroke = false; c->has_out = false; c->in_m = M; c->tol = 0.02f;
            mvg_layer(c, s->path, doc->end, &pi, s->fill_op, (s->flags & 4u) != 0, X0, Y0, X1, Y1);
        }
    }
    if (stroke) {
        mvg_cur_t pc = { doc->paints + c->ptab[s->stroke_paint], doc->end, false };
        mvg_paint_t p;
        mvg_pi_t pi;
        if (mvg_read_paint(&pc, &p)) {
            mvg_paint_setup(c, &p, &M, c->lut[1], &pi);
            c->stroke = true; c->cap = s->cap; c->join = s->join; c->mlimit = mlimit;
            if (sim) {
                float sc = mvg_sqrt(s2x);
                c->in_m = M; c->has_out = false; c->hw = hwl * sc; c->tol = 0.02f;
            } else {
                float sc = mvg_sqrt(s2x > s2y ? s2x : s2y);
                c->in_m.a = 1; c->in_m.b = 0; c->in_m.c = 0; c->in_m.d = 1; c->in_m.e = 0; c->in_m.f = 0;
                c->out_m = M; c->has_out = true; c->hw = hwl; c->tol = 0.02f / (sc > 1e-6f ? sc : 1e-6f);
            }
            mvg_layer(c, s->path, doc->end, &pi, s->stroke_op, false, X0, Y0, X1, Y1);
            c->has_out = false;
        }
    }
}

/* ----------------------------------------------------------- rendering */

typedef void* (*mimg_alloc_fn)(size_t size);
typedef void (*mimg_free_fn)(void* p);

/* Options of mimg_render_ex. Start from mimg_render_opts_init: a zeroed
 * struct would have a transparent "current color" and composite over the
 * picture.
 *   current_color  0xAARRGGBB for paints that use "current color": its RGB
 *                  colors them, its alpha multiplies theirs (default opaque black)
 *   clear          true: clear to transparent first; false: composite over
 *                  what is in the buffer (default true)
 *   fit            MIMG_FIT_CONTAIN (default), STRETCH or COVER when the
 *                  size has another shape than the picture
 *   scratch        memory for the rasterizer (see mimg_scratch_size); if
 *                  absent or too small, alloc is called for it and freed
 *                  after; if there is no alloc either, a built-in
 *                  256 KiB buffer is used (one render at a time; small
 *                  pictures only) */
typedef struct {
    uint32_t current_color;
    bool clear;
    uint8_t fit;
    void* scratch;
    size_t scratch_size;
    mimg_alloc_fn alloc;
    mimg_free_fn free;
} mimg_render_opts_t;

static inline void mimg_render_opts_init(mimg_render_opts_t* o) {
    o->current_color = 0xFF000000u;
    o->clear = true;
    o->fit = MIMG_FIT_CONTAIN;
    o->scratch = NULL;
    o->scratch_size = 0;
    o->alloc = NULL;
    o->free = NULL;
}

/* What the rasterizer needs, for a vector picture drawn at width x height
 * (it draws in bands, so less than `want` still works, only slower or, for
 * very dense shapes, not at all):
 *   want = 16 + paints x 4 + 2 KiB + 4 KiB + acc + edges
 *   acc   = 4 x (width + 2) x height, at most 128 KiB, at least 4 x (width + 2)
 *   edges = 32 x payload size, 64 KiB to 1 MiB (16 bytes an edge)
 *   min  = the fixed part + one row of acc + 4 KiB of edges */
static size_t mvg_scratch_parts(const mvg_doc_t* doc, uint32_t W, uint32_t H, size_t* acc, size_t* edges) {
    size_t row = ((size_t)W + 2u) * 4u, full = row * H;
    *acc = full > MVG_ACC_BYTES ? MVG_ACC_BYTES : full;
    if (*acc < row) *acc = row;
    size_t payload = (size_t)(doc->end - doc->paints);
    *edges = payload * 32u;
    if (*edges < 64u * 1024u) *edges = 64u * 1024u;
    if (*edges > 1024u * 1024u) *edges = 1024u * 1024u;
    return 16u + (size_t)doc->npaints * 4u + 2u * 256u * 4u + 2u * MVG_BAND_MAX * 2u + 16u;
}

/* Recommended scratch size for rendering this .img at width x height, or 0
 * if it is not a valid .img. (A raster picture needs room for itself:
 * width x height x 4 of its own size, unless it is drawn at that size.) */
MM_FN size_t mimg_scratch_size(const uint8_t* d, size_t size, uint32_t width, uint32_t height);
MM_FN size_t mimg_scratch_min(const uint8_t* d, size_t size, uint32_t width, uint32_t height);

static void mvg_fit(float vw, float vh, uint32_t W, uint32_t H, uint32_t fit, float* sx, float* sy,
                        float* ox, float* oy) {
    float fx = (float)W / vw, fy = (float)H / vh;
    if (fit == MIMG_FIT_STRETCH) { *sx = fx; *sy = fy; *ox = *oy = 0.0f; return; }
    float s = fit == MIMG_FIT_COVER ? (fx > fy ? fx : fy) : (fx < fy ? fx : fy);
    *sx = *sy = s;
    *ox = ((float)W - vw * s) * 0.5f; *oy = ((float)H - vh * s) * 0.5f;
}

static bool mvg_render(const uint8_t* d, size_t size, uint32_t* out, uint32_t W, uint32_t H, uint32_t stride,
                       const mimg_render_opts_t* o, uint8_t* scratch, size_t ssize) {
    mvg_doc_t doc;
    if (!mvg_open(d, size, &doc)) return false;
    size_t acc_b, edge_b;
    size_t fixed = mvg_scratch_parts(&doc, W, H, &acc_b, &edge_b);
    size_t rowb = (((size_t)W + 2u) * 4u + 15u) & ~(size_t)15u;
    uintptr_t base = ((uintptr_t)scratch + 15u) & ~(uintptr_t)15u;
    size_t slack = (size_t)(base - (uintptr_t)scratch);
    if (ssize < slack || ssize - slack < fixed + rowb + 1024u) return false;
    size_t rest = ssize - slack - fixed;           /* for the accumulation buffer and the edges */
    acc_b = (acc_b + 15u) & ~(size_t)15u;
    if (acc_b > rest / 2u) { acc_b = (rest / 2u) & ~(size_t)15u; if (acc_b < rowb) acc_b = rowb; }
    if (edge_b > rest - acc_b) edge_b = rest - acc_b;
    edge_b &= ~(size_t)15u;
    if (edge_b < 1024u) return false;

    uint8_t* p = (uint8_t*)base;
    mvg_ctx_t c;
    uint32_t* ptab = (uint32_t*)(void*)p; p += (((size_t)doc.npaints * 4u) + 15u) & ~(size_t)15u;
    c.lut[0] = (uint32_t*)(void*)p; p += 1024;
    c.lut[1] = (uint32_t*)(void*)p; p += 1024;
    c.rmn = (int16_t*)(void*)p; p += MVG_BAND_MAX * 2u;
    c.rmx = (int16_t*)(void*)p; p += MVG_BAND_MAX * 2u;
    c.acc = (float*)(void*)p; p += acc_b;
    c.edges = (float*)(void*)p;
    c.acc_cells = (uint32_t)(acc_b / 4u);
    c.edge_cap = (uint32_t)(edge_b / 16u);
    for (uint32_t i = 0; i < c.acc_cells; i++) c.acc[i] = 0.0f;
    if (!mvg_validate(&doc, ptab)) return false;

    if (o->clear)
        for (uint32_t y = 0; y < H; y++) {
            uint32_t* row = out + (size_t)y * stride;
            for (uint32_t x = 0; x < W; x++) row[x] = 0;
        }
    c.out = out; c.ostride = stride; c.W = W; c.H = H;
    c.edge_n = 0; c.overflow = false; c.failed = false;
    c.current = o->current_color;
    c.doc = &doc; c.ptab = ptab;
    c.inv = 1.0f / (float)(1u << doc.shift);
    c.has_out = false; c.stroke = false;
    c.s_started = false; c.s_nseg = 0; c.s_seen = false;

    float vw = (float)doc.vbw * c.inv, vh = (float)doc.vbh * c.inv;
    float sx, sy, ox, oy;
    mvg_fit(vw, vh, W, H, o->fit, &sx, &sy, &ox, &oy);
    mvg_mat_t view = { sx, 0.0f, 0.0f, sy, ox - (float)doc.vbx * c.inv * sx, oy - (float)doc.vby * c.inv * sy };
    mvg_cur_t cur = { doc.shapes, doc.end, false };
    for (uint32_t i = 0; i < doc.nshapes; i++) {
        mvg_shape_t s;
        if (!mvg_read_shape(&cur, &doc, &s)) return false;
        mvg_shape(&c, &s, &view);
        mvg_path_t P;
        mvg_path_init(&P, cur.p, cur.end);
        int32_t q[6];
        while (mvg_path_next(&P, q) > 0) {}
        cur.p = P.c.p;
    }
    return !c.failed;
}

/* ---- raster pictures at another size */

static bool mvg_raster_decode(const uint8_t* d, size_t size, const mimg_info_t* info, uint32_t* out, uint32_t stride);

static bool mvg_raster_render(const uint8_t* d, size_t size, const mimg_info_t* info, uint32_t* out, uint32_t W,
                              uint32_t H, uint32_t stride, const mimg_render_opts_t* o, uint32_t* nat) {
    uint32_t nw = info->width, nh = info->height;
    float sx, sy, ox, oy;
    mvg_fit((float)nw, (float)nh, W, H, o->fit, &sx, &sy, &ox, &oy);
    int rw = (int)((float)nw * sx + 0.5f), rh = (int)((float)nh * sy + 0.5f);
    if (rw < 1) rw = 1;
    if (rh < 1) rh = 1;
    int rx = (int)((float)W - (float)rw) / 2, ry = (int)((float)H - (float)rh) / 2;
    if (o->fit == MIMG_FIT_STRETCH) { rw = (int)W; rh = (int)H; rx = ry = 0; }
    if (!mvg_raster_decode(d, size, info, nat, nw)) return false;
    if (o->clear)
        for (uint32_t y = 0; y < H; y++) {
            uint32_t* row = out + (size_t)y * stride;
            for (uint32_t x = 0; x < W; x++) row[x] = 0;
        }
    float kx = (float)nw / (float)rw, ky = (float)nh / (float)rh;   /* source pixels per output pixel */
    bool box = kx > 1.0f || ky > 1.0f;
    int x0 = rx < 0 ? 0 : rx, x1 = rx + rw > (int)W ? (int)W : rx + rw;
    int y0 = ry < 0 ? 0 : ry, y1 = ry + rh > (int)H ? (int)H : ry + rh;
    for (int y = y0; y < y1; y++) {
        float u0y = (float)(y - ry) * ky, u1y = u0y + ky;
        for (int x = x0; x < x1; x++) {
            float u0x = (float)(x - rx) * kx, u1x = u0x + kx;
            float wsum = 0.0f, as = 0.0f, rs = 0.0f, gs = 0.0f, bs = 0.0f;
            int ia, ib, ja, jb;
            float cx0 = 0, cy0 = 0;
            if (box) {
                ia = mvg_ifloor(u0x); ib = mvg_iceil(u1x); ja = mvg_ifloor(u0y); jb = mvg_iceil(u1y);
            } else {                                    /* bilinear around the pixel's centre */
                float cx = (u0x + u1x) * 0.5f - 0.5f, cy = (u0y + u1y) * 0.5f - 0.5f;
                ia = mvg_ifloor(cx); ib = ia + 2; ja = mvg_ifloor(cy); jb = ja + 2;
                cx0 = cx; cy0 = cy;
            }
            for (int j = ja; j < jb; j++) {
                int sj = j < 0 ? 0 : j >= (int)nh ? (int)nh - 1 : j;
                float wy = box ? (((float)(j + 1) < u1y ? (float)(j + 1) : u1y) - ((float)j > u0y ? (float)j : u0y))
                               : 1.0f - mvg_absf((float)j - cy0);
                if (!(wy > 0.0f)) continue;
                for (int i = ia; i < ib; i++) {
                    int si = i < 0 ? 0 : i >= (int)nw ? (int)nw - 1 : i;
                    float wx = box ? (((float)(i + 1) < u1x ? (float)(i + 1) : u1x) - ((float)i > u0x ? (float)i : u0x))
                                   : 1.0f - mvg_absf((float)i - cx0);
                    if (!(wx > 0.0f)) continue;
                    uint32_t px = nat[(size_t)sj * nw + (uint32_t)si];
                    float wgt = wx * wy, a = (float)(px >> 24) * wgt;
                    wsum += wgt; as += a;
                    rs += (float)((px >> 16) & 255u) * a; gs += (float)((px >> 8) & 255u) * a; bs += (float)(px & 255u) * a;
                }
            }
            if (!(wsum > 0.0f) || !(as > 0.0f)) continue;
            uint32_t a8 = (uint32_t)(as / wsum + 0.5f);
            if (a8 > 255u) a8 = 255u;
            uint32_t r8 = (uint32_t)(rs / as + 0.5f), g8 = (uint32_t)(gs / as + 0.5f), b8 = (uint32_t)(bs / as + 0.5f);
            if (r8 > 255u) r8 = 255u;
            if (g8 > 255u) g8 = 255u;
            if (b8 > 255u) b8 = 255u;
            uint32_t rgb = r8 << 16 | g8 << 8 | b8;
            uint32_t* dp = out + (size_t)y * stride + (uint32_t)x;
            *dp = o->clear ? (a8 << 24 | rgb) : mvg_over(*dp, rgb, a8);
        }
    }
    return true;
}

/* Width, height and kind of an .img (version 1 raster or version 2 vector). */
MM_FN bool mimg_info(const uint8_t* d, size_t size, mimg_info_t* info) {
    if (size >= MIMG_HEADER_SIZE && d[4] == MIMG_VERSION_VECTOR) {
        mvg_doc_t doc;
        if (!mvg_open(d, size, &doc)) return false;
        float inv = 1.0f / (float)(1u << doc.shift);
        info->width = doc.width;
        info->height = doc.height;
        info->alpha = true;
        info->vb_x = (float)doc.vbx * inv; info->vb_y = (float)doc.vby * inv;
        info->vb_w = (float)doc.vbw * inv; info->vb_h = (float)doc.vbh * inv;
        info->current_color = (doc.flags & 1u) != 0;
        info->vector = true;
        return true;
    }
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
    info->vb_x = info->vb_y = 0.0f;
    info->vb_w = (float)w; info->vb_h = (float)h;
    info->current_color = false;
    info->vector = false;
    return true;
}

static bool mvg_raster_decode(const uint8_t* d, size_t size, const mimg_info_t* info, uint32_t* out, uint32_t stride) {
    uint32_t tiles = mm_tiles(info->width, info->height), across = mm_tiles_x(info->width);
    const uint8_t* table = d + MIMG_HEADER_SIZE;
    const uint8_t* data = table + ((size_t)tiles + 1u) * 4u;
    size_t avail = size - (size_t)(data - d);
    for (uint32_t t = 0; t < tiles; t++) {
        uint32_t a = mm_get32(table + t * 4u), b = mm_get32(table + t * 4u + 4u);
        if (a > b || b > avail) return false;
        if (!mm_tile_decode(data + a, b - a, out, info->width, info->height, stride, t % across,
                            t / across, false))
            return false;
    }
    return true;
}

MM_FN size_t mimg_scratch_size(const uint8_t* d, size_t size, uint32_t width, uint32_t height) {
    mimg_info_t info;
    if (!mimg_info(d, size, &info) || !width || !height || width > MMEDIA_MAX_SIDE || height > MMEDIA_MAX_SIDE)
        return 0;
    if (!info.vector) return (size_t)info.width * info.height * 4u + 16u;
    mvg_doc_t doc;
    size_t acc, edges;
    mvg_open(d, size, &doc);
    return mvg_scratch_parts(&doc, width, height, &acc, &edges) + acc + edges;
}

MM_FN size_t mimg_scratch_min(const uint8_t* d, size_t size, uint32_t width, uint32_t height) {
    mimg_info_t info;
    if (!mimg_info(d, size, &info) || !width || !height || width > MMEDIA_MAX_SIDE || height > MMEDIA_MAX_SIDE)
        return 0;
    if (!info.vector) return (size_t)info.width * info.height * 4u + 16u;
    mvg_doc_t doc;
    size_t acc, edges;
    mvg_open(d, size, &doc);
    return mvg_scratch_parts(&doc, width, height, &acc, &edges) + (((size_t)width + 2u) * 4u + 15u) + 4096u + 16u;
}

/* The scratch used when the caller supplies none. One user at a time. */
static uint8_t* mvg_builtin(bool acquire) {
#if defined(__GNUC__)
    static uint8_t buf[MIMG_BUILTIN_SCRATCH] __attribute__((aligned(16)));
#else
    static uint8_t buf[MIMG_BUILTIN_SCRATCH];
#endif
    static volatile int busy;
#if defined(__GNUC__)
    if (!acquire) { __sync_lock_release(&busy); return NULL; }
    if (__sync_lock_test_and_set(&busy, 1)) return NULL;
#else
    if (!acquire) { busy = 0; return NULL; }
    if (busy) return NULL;
    busy = 1;
#endif
    return buf;
}

/* Draws the picture, vector or raster, into width x height pixels of `out`
 * (stride in pixels), 0xAARRGGBB, not premultiplied. A vector picture is
 * rasterized again at that size (crisp at any size); a raster one is scaled
 * (bilinear when enlarging, area average when shrinking). Both keep their
 * aspect ratio and are centered unless o->fit says otherwise (the rest of
 * the buffer is cleared, or left alone with clear = false). Returns false
 * for a damaged file, a bad size or too little scratch memory; the buffer
 * may then be partly drawn. */
MM_FN bool mimg_render_ex(const uint8_t* d, size_t size, uint32_t* out, uint32_t width, uint32_t height,
                          uint32_t stride, const mimg_render_opts_t* o) {
    mimg_render_opts_t def;
    if (!o) { mimg_render_opts_init(&def); o = &def; }
    mimg_info_t info;
    if (!d || !out || !width || !height || width > MMEDIA_MAX_SIDE || height > MMEDIA_MAX_SIDE || stride < width ||
        !mimg_info(d, size, &info) || o->fit > MIMG_FIT_COVER)
        return false;
    if (!info.vector && width == info.width && height == info.height && o->clear)
        return mvg_raster_decode(d, size, &info, out, stride);
    size_t need = mimg_scratch_min(d, size, width, height), want = mimg_scratch_size(d, size, width, height);
    uint8_t* buf = (uint8_t*)o->scratch;
    size_t have = o->scratch_size;
    void* owned = NULL;
    bool builtin = false;
    if (!buf || have < need) {
        buf = NULL;
        if (o->alloc) {
            owned = o->alloc(want);
            buf = (uint8_t*)owned;
            have = want;
        } else if (need <= MIMG_BUILTIN_SCRATCH) {
            buf = mvg_builtin(true);
            builtin = buf != NULL;
            have = MIMG_BUILTIN_SCRATCH;
        }
        if (!buf) return false;
    }
    bool ok;
    if (info.vector) {
        ok = mvg_render(d, size, out, width, height, stride, o, buf, have);
    } else {
        uintptr_t base = ((uintptr_t)buf + 15u) & ~(uintptr_t)15u;
        ok = have >= (size_t)(base - (uintptr_t)buf) + (size_t)info.width * info.height * 4u &&
             mvg_raster_render(d, size, &info, out, width, height, stride, o, (uint32_t*)(void*)base);
    }
    if (owned && o->free) o->free(owned);
    if (builtin) mvg_builtin(false);
    return ok;
}

/* Draws the picture into width x height pixels (see mimg_render_ex): black
 * for "current color", cleared first, aspect ratio kept and centered. */
MM_FN bool mimg_render(const uint8_t* d, size_t size, uint32_t* out, uint32_t width, uint32_t height,
                       uint32_t stride) {
    return mimg_render_ex(d, size, out, width, height, stride, NULL);
}

/* Decodes into `out` (stride in pixels, room for width x height). A vector
 * picture is drawn at its natural size. */
MM_FN bool mimg_decode(const uint8_t* d, size_t size, uint32_t* out, uint32_t stride) {
    mimg_info_t info;
    if (!mimg_info(d, size, &info) || stride < info.width) return false;
    if (info.vector) return mimg_render_ex(d, size, out, info.width, info.height, stride, NULL);
    return mvg_raster_decode(d, size, &info, out, stride);
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
    uint32_t codec;             /* MVID_CODEC_TILES, MVID_CODEC_MOTION or MVID_CODEC_H264 */
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

/* Adds a frame that is already encoded as H.264: one access unit in
 * Annex B form (`key`: an IDR picture with its parameter sets). The
 * writer has no H.264 encoder; tools/media/mosmedia.c gets these from
 * ffmpeg. Do not mix with mvid_writer_frame. */
MM_FN bool mvid_writer_h264_frame(mvid_writer_t* v, const uint8_t* unit, size_t size, bool key) {
    if (v->failed) return false;
    uint32_t n = v->info.frame_count;
    if (n == 0) v->info.codec = MVID_CODEC_H264;
    if (v->info.codec != MVID_CODEC_H264 || !size || size > (64u << 20) - 4u) { v->failed = true; return false; }
    if (n == v->index_cap) {
        uint8_t* bigger = (uint8_t*)v->alloc((size_t)v->index_cap * 2u * MVID_INDEX_ENTRY);
        if (!bigger) { v->failed = true; return false; }
        mm_copy(bigger, v->index, (size_t)v->index_cap * MVID_INDEX_ENTRY);
        v->free(v->index);
        v->index = bigger;
        v->index_cap *= 2u;
    }
    uint8_t head[4] = { key ? MVID_FRAME_KEY : 0, 0, 0, 0 };
    if (!v->write(v->ctx, v->at, head, 4) || !v->write(v->ctx, v->at + 4u, unit, size)) {
        v->failed = true;
        return false;
    }
    uint8_t* e = v->index + (size_t)n * MVID_INDEX_ENTRY;
    mm_put64(e, v->at); mm_put32(e + 8, (uint32_t)size + 4u); mm_put32(e + 12, key ? MVID_FRAME_KEY : 0);
    v->at += size + 4u;
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
        bool motion = in->codec != MVID_CODEC_TILES;     /* a version 2 file: the codec is in the header */
        if (motion) in->alpha = false;
        mm_copy(hd, MVID_MAGIC, 4);
        hd[4] = motion ? MVID_VERSION_CODEC : MMEDIA_VERSION; hd[5] = in->alpha ? 4 : 3; hd[6] = MMEDIA_TILE_LOG2;
        hd[7] = in->has_audio ? 1 : 0;
        mm_put32(hd + 8, in->width); mm_put32(hd + 12, in->height);
        mm_put32(hd + 16, in->fps_num); mm_put32(hd + 20, in->fps_den);
        mm_put32(hd + 24, in->frame_count); mm_put32(hd + 28, in->duration_ms);
        mm_put64(hd + 32, in->index_offset); mm_put64(hd + 40, in->audio_offset);
        mm_put64(hd + 48, in->audio_size); mm_put32(hd + 56, in->keyframe_interval);
        if (motion) mm_put32(hd + 60, in->codec);
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

/* A hardware H.264 decoder: decodes the picture `pic` describes into
 * w x h pixels (0xFFRRGGBB, rows of w). False = cannot, or not now. */
typedef bool (*mvid_h264_hw_fn)(void* ctx, const syscall_media_h264_t* pic, uint32_t* out, uint32_t w, uint32_t h);
/* Told when the hardware decoder's pictures differ from the CPU's. */
typedef void (*mvid_h264_report_fn)(void* ctx);

/* A file starts on the CPU decoder. From its first keyframe the hardware
 * decoder decodes every picture too and the two are compared pixel for
 * pixel; after MVID_HW_CHECK_FRAMES equal pictures it is trusted and
 * takes over alone at the next keyframe. One different picture, or a
 * refusal, and the file stays on the CPU. */
#define MVID_HW_NEW      0u
#define MVID_HW_CHECKING 1u
#define MVID_HW_TRUSTED  2u
#define MVID_HW_ONLY     3u
#define MVID_HW_OFF      4u
#define MVID_HW_CHECK_FRAMES 12u

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
    bool no_gpu;                /* motion, h264: colour conversion stays on the CPU */
    h264_t h264;                /* h264: the decoder */
    bool h264_ready;
    /* h264 on a hardware decoder (see mvid_decode_one). */
    mvid_h264_hw_fn hw_decode;  /* NULL = none; set by mvid_open on Minimal-OS */
    mvid_h264_report_fn hw_mismatch;
    void* hw_ctx;
    uint32_t hw_state;          /* MVID_HW_* */
    uint32_t hw_checked;        /* pictures that came out the same on both */
    uint32_t* hw_check;         /* a picture from the hardware decoder, to compare */
} mvid_reader_t;

MM_FN bool mvid_parse_header(const uint8_t* hd, uint64_t file_size, mvid_info_t* in) {
    if (!mm_same(hd, MVID_MAGIC, 4) || (hd[4] != MMEDIA_VERSION && hd[4] != MVID_VERSION_CODEC) ||
        (hd[5] != 3 && hd[5] != 4) || hd[6] != MMEDIA_TILE_LOG2)
        return false;
    in->codec = hd[4] == MVID_VERSION_CODEC ? mm_get32(hd + 60) : MVID_CODEC_TILES;
    if (in->codec > MVID_CODEC_H264 || (in->codec != MVID_CODEC_TILES && hd[5] != 3)) return false;
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

#if defined(MINIMALOS_TARGET)
/* The kernel's hardware H.264 decoder (the graphics card's NVDEC). */
MM_FN bool mvid_h264_syscall(void* ctx, const syscall_media_h264_t* pic, uint32_t* out, uint32_t w, uint32_t h) {
    (void)ctx;
    syscall_media_request_t request = {
        .size = sizeof(request),
        .version = SYS_MEDIA_ABI_VERSION,
        .op = SYS_MEDIA_DECODE_H264,
        .width = w,
        .height = h,
        .frame_bytes = sizeof(*pic),
        .frame = (const uint8_t*)pic,
        .pixels = out
    };
    return mos_syscall(SYS_MEDIA, (long)(uintptr_t)&request, 0, 0) == SYS_SUCCESS;
}

MM_FN void mvid_h264_mismatch_syscall(void* ctx) {
    (void)ctx;
    syscall_media_request_t request = {
        .size = sizeof(request), .version = SYS_MEDIA_ABI_VERSION, .op = SYS_MEDIA_H264_MISMATCH
    };
    mos_syscall(SYS_MEDIA, (long)(uintptr_t)&request, 0, 0);
}
#endif

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
    /* An H.264 frame has no bound from the picture size alone. */
    if (r->info.codec == MVID_CODEC_H264) r->frame_cap = largest < (64u << 20) ? largest : (64u << 20);
    r->frame = (uint8_t*)alloc(r->frame_cap ? r->frame_cap : 1);
    if (!r->frame) return false;
    if (r->info.codec == MVID_CODEC_MOTION) {
        r->mc_ready = true;   /* mmc_init clears it on failure; mmc_free copes */
        if (!mmc_init(&r->mc, r->info.width, r->info.height, false, alloc)) return false;
    }
    if (r->info.codec == MVID_CODEC_H264) {
        r->h264_ready = true;
        if (!h264_init(&r->h264, alloc, r->frame_cap)) return false;
        r->hw_check = (uint32_t*)alloc((size_t)r->info.width * r->info.height * 4u);
        if (!r->hw_check) return false;
#if defined(MINIMALOS_TARGET)
        r->hw_decode = mvid_h264_syscall;
        r->hw_mismatch = mvid_h264_mismatch_syscall;
#endif
    }
    return true;
}

MM_FN void mvid_close(mvid_reader_t* r, mvid_free_fn free_fn) {
    if (r->index) free_fn(r->index);
    if (r->frame) free_fn(r->frame);
    r->index = NULL; r->frame = NULL;
    if (r->mc_ready) mmc_free(&r->mc, free_fn);
    r->mc_ready = false;
    if (r->h264_ready) h264_free(&r->h264, free_fn);
    r->h264_ready = false;
    if (r->hw_check) free_fn(r->hw_check);
    r->hw_check = NULL;
}

/* The frame shown at `ms`. */
MM_FN uint32_t mvid_frame_at(const mvid_info_t* in, uint32_t ms) {
    uint64_t f = (uint64_t)ms * in->fps_num / (1000u * (uint64_t)in->fps_den);
    return f >= in->frame_count ? in->frame_count - 1u : (uint32_t)f;
}

/* Colour conversion (4:2:0 full-range BT.601 planes to pixels) on the
 * graphics card, when the kernel's GPU backend offers it: the same pixels
 * as mmc_to_argb. False = convert here. */
MM_FN bool mvid_gpu_convert(mvid_reader_t* r, const uint8_t* y, const uint8_t* cb, const uint8_t* cr, uint32_t pw,
                            uint32_t ph, uint32_t cw, uint32_t ch, uint32_t* out, uint32_t w, uint32_t h) {
#if defined(MINIMALOS_TARGET)
    if (!r->no_gpu) {
        syscall_media_planes_t planes = { .y = y, .cb = cb, .cr = cr, .w = pw, .h = ph, .cw = cw, .ch = ch };
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
        if (mos_syscall(SYS_MEDIA, (long)(uintptr_t)&request, 0, 0) == SYS_SUCCESS) return true;
        /* No GPU for it (or an older kernel): stop asking for this file. */
        r->no_gpu = true;
    }
#else
    (void)r; (void)y; (void)cb; (void)cr; (void)pw; (void)ph; (void)cw; (void)ch; (void)out; (void)w; (void)h;
#endif
    return false;
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
        if (!mvid_gpu_convert(r, r->mc.ref[0], r->mc.ref[1], r->mc.ref[2], r->mc.w, r->mc.h, r->mc.cw, r->mc.ch,
                              out, w, h))
            mmc_to_argb(&r->mc, out, w, h, w);
        r->decoded = n;
        return true;
    }
    if (r->info.codec == MVID_CODEC_H264) {
        if (size < 5u || size > r->frame_cap || offset + size > r->size || !r->read(r->ctx, offset, r->frame, size))
            return false;
        bool key = (r->frame[0] & MVID_FRAME_KEY) != 0;
        if (!key && r->decoded != (int64_t)n - 1) return false;
        h264_t* d = &r->h264;
        if (r->hw_state == MVID_HW_TRUSTED && key) r->hw_state = MVID_HW_ONLY;
        d->hw_only = r->hw_state == MVID_HW_ONLY;
        if (d->hw_only) {
            /* The hardware decoder alone: here only the headers are read. */
            if (h264_decode(d, r->frame + 4, size - 4u) && d->hw_valid && h264_width(d) >= w && h264_height(d) >= h &&
                r->hw_decode(r->hw_ctx, &d->hw, out, w, h)) {
                r->decoded = n;
                return true;
            }
            /* It gave up: the file goes on with the CPU decoder, which
             * has to start again from the last keyframe. */
            r->hw_state = MVID_HW_OFF;
            d->hw_only = false;
            uint32_t from = n;
            while (from > 0 && !(mm_get32(r->index + (size_t)from * MVID_INDEX_ENTRY + 12) & MVID_FRAME_KEY)) from--;
            r->decoded = -1;
            for (uint32_t k = from; k <= n; k++)
                if (!mvid_decode_one(r, k, out)) return false;
            return true;
        }
        if (!h264_decode(d, r->frame + 4, size - 4u) || h264_width(d) < w || h264_height(d) < h) return false;
        const h264_sps_t* s = d->active;
        if (!(h264_is_full_601(d) && !s->crop_l && !s->crop_t &&
              mvid_gpu_convert(r, d->out->plane[0], d->out->plane[1], d->out->plane[2], d->w, d->h, d->cw, d->ch,
                               out, w, h)))
            h264_to_argb(d, out, w, h, w);
        if (r->hw_decode && r->hw_state < MVID_HW_TRUSTED) {
            /* The hardware decoder writes full-range BT.601 pictures
             * from the top left corner; other streams stay here. */
            bool can = h264_is_full_601(d) && !s->crop_l && !s->crop_t && d->hw_valid && r->hw_check;
            if (r->hw_state == MVID_HW_NEW && key) r->hw_state = can ? MVID_HW_CHECKING : MVID_HW_OFF;
            if (r->hw_state == MVID_HW_CHECKING) {
                if (!can || !r->hw_decode(r->hw_ctx, &d->hw, r->hw_check, w, h)) {
                    r->hw_state = MVID_HW_OFF;
                } else {
                    bool same = true;
                    for (size_t i = 0; i < (size_t)w * h && same; i++) same = r->hw_check[i] == out[i];
                    if (!same) {
                        r->hw_state = MVID_HW_OFF;
                        if (r->hw_mismatch) r->hw_mismatch(r->hw_ctx);
                    } else if (++r->hw_checked >= MVID_HW_CHECK_FRAMES) {
                        r->hw_state = MVID_HW_TRUSTED;
                    }
                }
            }
        }
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
