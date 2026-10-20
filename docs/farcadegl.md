# FarcadeGL (`farcadegl.h`)

FarcadeGL is the 3D library of Minimal-OS. It sits on the kernel's FGPU
interface (`SYS_GPU`, described in `fgpu_abi.h`) and borrows from both
OpenGL and Vulkan:

- **Like Vulkan**, drawing is recorded into a command buffer and sent to
  the GPU in one call (`fglSubmit` / `fglPresent`). GPU memory is explicit:
  you create and destroy buffers, and the library creates the color and
  depth surfaces for a context.
- **Like OpenGL**, state is a handful of simple calls (`fglEnable`,
  `fglUseProgram`, `fglSetModel` ...) and matrices are column-major.

It is header only: include `farcadegl.h` and build with `./build.sh`.
`programs/fglcube` is a complete example (shipped in the OS as
`0:/programs/fglcube.run`; MOSdesk has a windowed version, *Cube*).

## How it runs

The kernel executes command buffers through its FGPU command processor.
Today its only backend renders on the CPU (vertex transform, clipping,
perspective-correct rasterization, depth test, lighting). Surfaces and
buffers are placed in the graphics card's video memory when the GPU driver
can hand some out (`gpu` in the terminal shows the VRAM heap). Programs
do not change when a hardware backend is added.

Rendering speed: about 75 frames per second for a full-screen 1024x768
scene on one core of a desktop CPU. Under QEMU without KVM it is around
50 times slower.

## Contexts

```c
typedef struct {
    uint32_t width, height;     // render size
    uint32_t window_id;         // present into this window; 0 = the screen
    int32_t x, y;               // screen position when window_id == 0
    int depth;                  // create a depth buffer
    int page_flip;              // screen only: flip between two VRAM buffers
} FGLContextDesc;

FGLContext* fglCreateContext(const FGLContextDesc* desc);
void fglDestroyContext(FGLContext* gl);
```

For a window, create it with `mos_window_create` and pass its id; frames
are copied straight into the window. For the whole screen, first take the
screen with `mos_graphics_acquire()`. With `page_flip` set and a GPU that
can flip (the QEMU/Bochs adapter can), frames are shown by switching the
display to the buffer just drawn instead of copying it. Returns `NULL`
when the surfaces cannot be created.

## Buffers

```c
FGLBuffer fglCreateBuffer(FGLContext* gl, uint32_t bytes);
int fglBufferData(FGLContext* gl, FGLBuffer b, uint32_t offset, const void* data, uint32_t bytes);
void fglDestroyBuffer(FGLContext* gl, FGLBuffer b);
```

Vertex buffers hold `FGLVertex` (position, normal, RGBA color, all
floats). Index buffers hold `uint32_t` indices. A buffer may be up to
16 MiB. Everything a process creates is freed when it exits.

## State and drawing

| Call | Effect |
| --- | --- |
| `fglClearColor(gl, r, g, b)` / `fglClear(gl, bits)` | Clear color and/or depth (`FGL_COLOR_BUFFER_BIT`, `FGL_DEPTH_BUFFER_BIT`). |
| `fglEnable` / `fglDisable` | `FGL_DEPTH_TEST`, `FGL_CULL_FACE` (back faces; counter-clockwise is front), `FGL_WIREFRAME`. |
| `fglUseProgram(gl, p)` | `FGL_PROGRAM_COLOR` (vertex colors), `FGL_PROGRAM_GOURAUD` (lighting per vertex), `FGL_PROGRAM_PHONG` (lighting per pixel). |
| `fglSetModel`, `fglSetView`, `fglSetProjection` | The usual three matrices. |
| `fglSetLight(gl, direction, color, ambient)` | One directional light; `direction` is where the light travels. |
| `fglSetMaterial(gl, specular, shininess)` | Highlight strength (0-1) and sharpness (1-128). |
| `fglViewport(gl, x, y, w, h)` | Area of the surface to draw into (defaults to all of it). |
| `fglDraw(gl, vertices, first, count)` | Triangles from consecutive vertices. |
| `fglDrawIndexed(gl, vertices, indices, first_index, count)` | Triangles from an index buffer. |
| `fglSubmit(gl)` | Run the recorded commands. |
| `fglPresent(gl)` | Submit and show the frame. Returns 0, or -1 with `gl->error` set. |
| `fglReadPixels(gl, out, pitch)` | Copy the frame into your memory. |

## Math

`FGLVec3`, `FGLMat4` and: `fglVec3`, `fglAdd`, `fglSub`, `fglScale3`,
`fglDot`, `fglCross`, `fglNormalize`, `fglIdentity`, `fglMul`,
`fglTranslate`, `fglScale`, `fglRotate(angle, axis)`,
`fglPerspective(fovy, aspect, near, far)`, `fglLookAt(eye, center, up)`,
`fglNormalMatrix`, and `fglSin`, `fglCos`, `fglTan`, `fglSqrt` (the SDK
has no libm).

`fglMakeCube(out, half_size, face_colors)` writes the 36 vertices of a
cube with one color per face (`NULL` for the default colors).

## Example

```c
#include "farcadegl.h"
#include "window.h"

int main(void) {
    uint32_t win = mos_window_create(0, 100, 100, 640, 480, "Hello 3D");
    FGLContext* gl = fglCreateContext(&(FGLContextDesc){
        .width = 640, .height = 480, .window_id = win, .depth = 1 });
    FGLVertex cube[36];
    fglMakeCube(cube, 1.0f, NULL);
    FGLBuffer vb = fglCreateBuffer(gl, sizeof cube);
    fglBufferData(gl, vb, 0, cube, sizeof cube);

    for (int frame = 0; frame < 600; frame++) {
        fglClearColor(gl, 0.05f, 0.07f, 0.12f);
        fglClear(gl, FGL_COLOR_BUFFER_BIT | FGL_DEPTH_BUFFER_BIT);
        fglEnable(gl, FGL_DEPTH_TEST | FGL_CULL_FACE);
        fglUseProgram(gl, FGL_PROGRAM_PHONG);
        fglSetProjection(gl, fglPerspective(0.9f, 640.0f / 480.0f, 0.1f, 50));
        fglSetView(gl, fglLookAt(fglVec3(0, 1, 4), fglVec3(0, 0, 0), fglVec3(0, 1, 0)));
        fglSetModel(gl, fglRotate(frame * 0.02f, fglVec3(0, 1, 0)));
        fglDraw(gl, vb, 0, 36);
        fglPresent(gl);
    }
    fglDestroyContext(gl);
    mos_window_close(win);
    return 0;
}
```

## The FGPU interface underneath

`SYS_GPU` (40) takes a `syscall_gpu_request_t`. Operations: `INFO`,
`CREATE_SURFACE`, `DESTROY_SURFACE`, `CREATE_BUFFER`, `UPLOAD_BUFFER`,
`DESTROY_BUFFER`, `SUBMIT`, `READBACK`, `PRESENT`, `RELEASE_DISPLAY`.
A command buffer is a list of 32-bit words; each command starts with
`FGPU_CMD(opcode, length_in_words)`. See `fgpu_abi.h` for the opcodes and
payload structures (`fgpu_uniforms_t`, `fgpu_draw_t`). Programs are
built in for now; the format has room for shader code later.
