# Pictures and icons in your own pixels (`mosmedia.h`)

`mosmedia.h` reads and draws Minimal-OS `.img` pictures. Besides the raster
pictures (RGB or RGBA, lossless) an `.img` can be a **vector picture**:
paths with solid and gradient paints, fills and strokes. A vector picture
is drawn again at the size you ask for, so one small file gives a crisp
icon at 16 px and a crisp logo at 1024 px. This page covers the
picture half of the header; `.vid` videos are in the kernel's
[media documentation](https://github.com/fsminecrafter/Minimal-OS/blob/main/docs/media.md).

Header only, plain C, no libc: it builds in a `.run` program and on the
build PC. Pixels are `0xAARRGGBB` (not premultiplied), like windows and
easyui.

```c
#include "mosmedia.h"

/* an icon, tinted with the theme's text color, at the size of a button */
static uint32_t pixels[24 * 24];
mimg_render_opts_t o;
mimg_render_opts_init(&o);
o.current_color = 0xFFEAF2F8;                 /* "current color" paints take this */
mimg_render_ex(icon_data, icon_size, pixels, 24, 24, 24, &o);
```

Make vector pictures from SVG files with `mosmedia vimg icon.svg icon.img`
on the build PC (see the kernel's media documentation).

## Which kind is it

`mimg_info` accepts raster (version 1) and vector (version 2) files:

```c
typedef struct {
    uint32_t width, height;   /* the size of a raster picture; the natural size of a vector one */
    bool alpha;
    float vb_x, vb_y, vb_w, vb_h;   /* vector: the view box; raster: 0, 0, width, height */
    bool current_color;       /* vector: it has "current color" paints (a tintable icon) */
    bool vector;
} mimg_info_t;
bool mimg_info(const uint8_t* data, size_t size, mimg_info_t* info);
```

Returns `false` for anything that is not an `.img` this header can read,
including damaged headers.

## `mimg_decode`

```c
bool mimg_decode(const uint8_t* data, size_t size, uint32_t* out, uint32_t stride);
```

Draws the picture at `info.width` x `info.height` into `out` (`stride` is
in pixels). Raster pictures decode exactly as before; a vector picture is
drawn at its natural size with black as its "current color". Programs that
only know `mimg_info` and `mimg_decode` (Photos, Mxplorer, mosWM) show
vector files with no change.

## `mimg_render`

```c
bool mimg_render(const uint8_t* data, size_t size, uint32_t* out,
                 uint32_t width, uint32_t height, uint32_t stride);
```

Draws any `.img` into `width` x `height` pixels of `out` (1 to 16384 each,
`stride` at least `width`). The buffer is cleared first. The picture keeps
its aspect ratio and is centered in the rectangle; the rest is transparent.
A vector picture is rasterized again at that size with anti-aliasing; a
raster picture is scaled (bilinear when enlarged, an area average when
shrunk). Returns `false` for a damaged file, a bad size or when it has no
memory to work in (below). Use `mimg_render_ex` for anything else.

## `mimg_render_ex`

```c
typedef struct {
    uint32_t current_color;    /* 0xAARRGGBB for "current color" paints (default opaque black) */
    bool clear;                /* true: clear to transparent first; false: draw over the buffer */
    uint8_t fit;               /* MIMG_FIT_CONTAIN (default), MIMG_FIT_STRETCH, MIMG_FIT_COVER */
    void* scratch;             /* memory for the rasterizer ... */
    size_t scratch_size;
    void* (*alloc)(size_t);    /* ... or a way to get it */
    void (*free)(void*);
} mimg_render_opts_t;

void mimg_render_opts_init(mimg_render_opts_t* o);
bool mimg_render_ex(const uint8_t* data, size_t size, uint32_t* out,
                    uint32_t width, uint32_t height, uint32_t stride,
                    const mimg_render_opts_t* opts);   /* opts may be NULL */
```

Always start from `mimg_render_opts_init`; a zeroed struct would draw
"current color" paints transparent and composite over the buffer.

- **Tint.** "Current color" is what a drawing's `currentColor` paint
  becomes: the RGB of `current_color` colors it and its alpha multiplies
  the paint's own alpha, so `0x80FFFFFF` draws the icon half transparent.
  Paints with their own colors and gradients are not changed.
- **`clear = false`** composites with source-over onto what is in `out`
  (the result stays non-premultiplied), so icons can be drawn straight
  onto a background or a window.
- **`fit`** matters when `width`:`height` is not the picture's shape.
  `CONTAIN` shows all of it, `STRETCH` fills the rectangle and distorts,
  `COVER` fills it and crops.

### Memory

The rasterizer allocates nothing. It needs scratch memory, which you give
it in one of three ways, tried in this order:

1. `scratch` and `scratch_size`, when big enough;
2. `alloc`/`free`, called once for the recommended size and freed after;
3. a built-in 256 KiB buffer (one render at a time; a render that asks
   for it while another is running returns `false`).

```c
size_t mimg_scratch_size(const uint8_t* data, size_t size, uint32_t width, uint32_t height);
size_t mimg_scratch_min(const uint8_t* data, size_t size, uint32_t width, uint32_t height);
```

`mimg_scratch_size` is the recommended size for drawing at `width` x
`height` (0 for a file that is not an `.img`); `mimg_scratch_min` is the
least that works. A vector picture is drawn in bands of rows, so less
scratch only means more passes: about 72 KiB suffices for a small icon,
about 200 KiB for any size, and the minimum is about 15 KiB plus
4 bytes a pixel of width. A raster picture drawn at another size
needs room for itself (`width x height x 4` of its own size) and cannot
use the built-in buffer if it is bigger than 256 KiB.

## Speed

A 24-unit icon with a few shapes takes about 12 us at 16 px, 18 us at 32,
33 us at 64 and 240 us at 256 (one core of a modern PC, `-O2`); a
detailed logo takes 1.2 ms at 256 px and 16 ms at 1024 px. Render once and
keep the pixels when the size does not change.

## Damaged files

Everything in a file is checked before anything is drawn; truncated,
mutated and made-up files make `mimg_info`/`mimg_render` return `false`
(or draw something harmless), never read out of bounds. They are fuzzed
under AddressSanitizer in the kernel's host tests.
