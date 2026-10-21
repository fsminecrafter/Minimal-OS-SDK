#ifndef FARCADEGL_H
#define FARCADEGL_H

/*
 * FarcadeGL - 3D graphics for Minimal-OS.
 *
 * A small library in the spirit of OpenGL and Vulkan, on top of the
 * kernel's FGPU interface (SYS_GPU, see fgpu_abi.h):
 *
 *   - Like Vulkan, drawing is *recorded* into a command buffer and sent to
 *     the GPU in one go when you call fglPresent() (or fglSubmit()). GPU
 *     memory is explicit: buffers and surfaces are created and destroyed.
 *   - Like OpenGL, state is set with simple calls (fglEnable, fglUseProgram,
 *     fglSetModel, ...) and matrices are column-major.
 *
 * Header only: #include "farcadegl.h" and build with ./build.sh.
 *
 * A frame:
 *
 *     FGLContext* gl = fglCreateContext(&(FGLContextDesc){
 *         .width = 640, .height = 480, .window_id = win, .depth = 1 });
 *     FGLBuffer cube = fglCreateBuffer(gl, sizeof(vertices));
 *     fglBufferData(gl, cube, 0, vertices, sizeof(vertices));
 *
 *     fglClearColor(gl, 0.05f, 0.07f, 0.12f);
 *     fglClear(gl, FGL_COLOR_BUFFER_BIT | FGL_DEPTH_BUFFER_BIT);
 *     fglEnable(gl, FGL_DEPTH_TEST | FGL_CULL_FACE);
 *     fglUseProgram(gl, FGL_PROGRAM_PHONG);
 *     fglSetProjection(gl, fglPerspective(1.0f, 640.0f / 480, 0.1f, 100));
 *     fglSetView(gl, fglLookAt(eye, center, up));
 *     fglSetModel(gl, fglRotate(angle, fglVec3(0, 1, 0)));
 *     fglDraw(gl, cube, 0, 36);
 *     fglPresent(gl);
 *
 * Shaders (FGSL, a GLSL-style language) replace the built-in programs:
 *
 *     FGLShader* sh = fglCreateShader(gl, vertex_src, fragment_src);
 *     if (!sh) print(fglShaderLog(gl));
 *     fglUseShader(gl, sh);
 *     fglUniform1f(sh, fglUniformLocation(sh, "time"), t);
 *
 * Rendering currently runs on the CPU inside the kernel (the FGPU
 * software backend), drawing into video memory when the GPU driver has
 * some. Programs do not change when a hardware backend arrives.
 *
 * Full guide: docs/farcadegl.md; shading language: docs/fgsl.md.
 */

#include <stdint.h>
#include <stddef.h>
#include "syscall.h"
#include "stdlib.h"
#include "string.h"
#include "x86_64/user_syscalls.h"
#include "fglsl.h"

/* ================================================================ math */

typedef struct { float x, y, z; } FGLVec3;
typedef struct { float m[16]; } FGLMat4;      // column-major: m[col * 4 + row]

#define FGL_PI 3.14159265358979323846f

static inline FGLVec3 fglVec3(float x, float y, float z) {
    FGLVec3 v = { x, y, z };
    return v;
}

static inline float fglSqrt(float x) {
    if (x <= 0.0f) return 0.0f;
    float r;
    __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x));
    return r;
}

// sin/cos: reduce to [-pi, pi], then a degree-11 Taylor polynomial on
// [-pi/2, pi/2] (error below 1e-6 there).
static inline float fglSin(float x) {
    const float two_pi = 2.0f * FGL_PI;
    long k = (long)(x / two_pi);
    x -= (float)k * two_pi;
    if (x > FGL_PI) x -= two_pi;
    if (x < -FGL_PI) x += two_pi;
    if (x > FGL_PI / 2) x = FGL_PI - x;
    if (x < -FGL_PI / 2) x = -FGL_PI - x;
    float x2 = x * x;
    return x * (1.0f - x2 / 6.0f * (1.0f - x2 / 20.0f * (1.0f - x2 / 42.0f *
           (1.0f - x2 / 72.0f * (1.0f - x2 / 110.0f)))));
}

static inline float fglCos(float x) { return fglSin(x + FGL_PI / 2); }
static inline float fglTan(float x) { return fglSin(x) / fglCos(x); }

static inline FGLVec3 fglAdd(FGLVec3 a, FGLVec3 b) { return fglVec3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline FGLVec3 fglSub(FGLVec3 a, FGLVec3 b) { return fglVec3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline FGLVec3 fglScale3(FGLVec3 a, float s) { return fglVec3(a.x * s, a.y * s, a.z * s); }
static inline float fglDot(FGLVec3 a, FGLVec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline FGLVec3 fglCross(FGLVec3 a, FGLVec3 b) {
    return fglVec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline FGLVec3 fglNormalize(FGLVec3 a) {
    float l = fglSqrt(fglDot(a, a));
    return l > 0.0f ? fglScale3(a, 1.0f / l) : a;
}

static inline FGLMat4 fglIdentity(void) {
    FGLMat4 r;
    memset(&r, 0, sizeof(r));
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

static inline FGLMat4 fglMul(FGLMat4 a, FGLMat4 b) {
    FGLMat4 r;
    for (int c = 0; c < 4; c++) {
        for (int row = 0; row < 4; row++) {
            r.m[c * 4 + row] = a.m[0 * 4 + row] * b.m[c * 4 + 0] +
                               a.m[1 * 4 + row] * b.m[c * 4 + 1] +
                               a.m[2 * 4 + row] * b.m[c * 4 + 2] +
                               a.m[3 * 4 + row] * b.m[c * 4 + 3];
        }
    }
    return r;
}

static inline FGLMat4 fglTranslate(FGLVec3 t) {
    FGLMat4 r = fglIdentity();
    r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
    return r;
}

static inline FGLMat4 fglScale(FGLVec3 s) {
    FGLMat4 r = fglIdentity();
    r.m[0] = s.x; r.m[5] = s.y; r.m[10] = s.z;
    return r;
}

// Rotation of `angle` radians around `axis` (right-handed).
static inline FGLMat4 fglRotate(float angle, FGLVec3 axis) {
    FGLVec3 a = fglNormalize(axis);
    float c = fglCos(angle), s = fglSin(angle), t = 1.0f - c;
    FGLMat4 r = fglIdentity();
    r.m[0] = t * a.x * a.x + c;       r.m[4] = t * a.x * a.y - s * a.z; r.m[8]  = t * a.x * a.z + s * a.y;
    r.m[1] = t * a.x * a.y + s * a.z; r.m[5] = t * a.y * a.y + c;       r.m[9]  = t * a.y * a.z - s * a.x;
    r.m[2] = t * a.x * a.z - s * a.y; r.m[6] = t * a.y * a.z + s * a.x; r.m[10] = t * a.z * a.z + c;
    return r;
}

// OpenGL-style perspective projection (fovy in radians).
static inline FGLMat4 fglPerspective(float fovy, float aspect, float near_z, float far_z) {
    FGLMat4 r;
    memset(&r, 0, sizeof(r));
    float f = 1.0f / fglTan(fovy * 0.5f);
    r.m[0] = f / aspect;
    r.m[5] = f;
    r.m[10] = (far_z + near_z) / (near_z - far_z);
    r.m[11] = -1.0f;
    r.m[14] = 2.0f * far_z * near_z / (near_z - far_z);
    return r;
}

static inline FGLMat4 fglLookAt(FGLVec3 eye, FGLVec3 center, FGLVec3 up) {
    FGLVec3 f = fglNormalize(fglSub(center, eye));
    FGLVec3 s = fglNormalize(fglCross(f, up));
    FGLVec3 u = fglCross(s, f);
    FGLMat4 r = fglIdentity();
    r.m[0] = s.x; r.m[4] = s.y; r.m[8] = s.z;
    r.m[1] = u.x; r.m[5] = u.y; r.m[9] = u.z;
    r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
    r.m[12] = -fglDot(s, eye);
    r.m[13] = -fglDot(u, eye);
    r.m[14] = fglDot(f, eye);
    return r;
}

// Inverse transpose of the upper 3x3 (for normals), as a 4x4.
static inline FGLMat4 fglNormalMatrix(FGLMat4 m) {
    float a = m.m[0], b = m.m[4], c = m.m[8];
    float d = m.m[1], e = m.m[5], f = m.m[9];
    float g = m.m[2], h = m.m[6], i = m.m[10];
    float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    float det = a * A + b * B + c * C;
    FGLMat4 r = fglIdentity();
    if (det == 0.0f) return r;
    float inv = 1.0f / det;
    // inverse = adj / det; transpose of inverse = cofactor matrix / det
    r.m[0] = A * inv;                 r.m[4] = B * inv;                 r.m[8]  = C * inv;
    r.m[1] = -(b * i - c * h) * inv;  r.m[5] = (a * i - c * g) * inv;   r.m[9]  = -(a * h - b * g) * inv;
    r.m[2] = (b * f - c * e) * inv;   r.m[6] = -(a * f - c * d) * inv;  r.m[10] = (a * e - b * d) * inv;
    return r;
}

/* ============================================================= objects */

typedef uint32_t FGLBuffer;     // 0 = none
typedef uint32_t FGLSurface;

#define FGL_COLOR_BUFFER_BIT FGPU_CLEAR_COLOR
#define FGL_DEPTH_BUFFER_BIT FGPU_CLEAR_DEPTH

#define FGL_DEPTH_TEST  (1u << 0)
#define FGL_CULL_FACE   (1u << 1)   // cull back faces (counter-clockwise = front)
#define FGL_WIREFRAME   (1u << 2)
#define FGL_BLEND       (1u << 3)   // color = src * src.a + dst * (1 - src.a)

#define FGL_PROGRAM_COLOR   FGPU_PROGRAM_COLOR
#define FGL_PROGRAM_GOURAUD FGPU_PROGRAM_GOURAUD
#define FGL_PROGRAM_PHONG   FGPU_PROGRAM_PHONG

// One vertex: position, normal, color (rgba 0-1). Shader attribute
// locations 0, 1, 2 (see docs/fgsl.md, "Attributes").
typedef struct {
    float px, py, pz;
    float nx, ny, nz;
    float r, g, b, a;
} FGLVertex;

// FGLVertex plus texture coordinates (location 3). Use it after
// fglVertexLayoutUV(gl).
typedef struct {
    float px, py, pz;
    float nx, ny, nz;
    float r, g, b, a;
    float u, v;
} FGLVertexUV;

// One attribute of a custom vertex layout (fglVertexLayout).
typedef struct {
    uint32_t offset;            // bytes from the start of the vertex
    uint32_t components;        // 1-4 floats; 0 = not present
} FGLAttribute;

typedef uint32_t FGLTexture;    // a color surface; 0 = none

#define FGL_TEXTURE_LINEAR FGPU_TEXTURE_LINEAR   // bilinear filtering (default: nearest)
#define FGL_TEXTURE_CLAMP  FGPU_TEXTURE_CLAMP    // clamp to the edge (default: repeat)
#define FGL_MAX_TEXTURE_UNITS FGPU_MAX_TEXTURE_UNITS

// The fgl_* uniforms FarcadeGL fills in for every shader that declares them.
enum {
    FGL__U_MVP, FGL__U_MODEL, FGL__U_VIEW, FGL__U_PROJECTION, FGL__U_MODELVIEW,
    FGL__U_NORMAL, FGL__U_LIGHT_DIR, FGL__U_LIGHT_COLOR, FGL__U_AMBIENT, FGL__U_CAMERA,
    FGL__U_MATERIAL, FGL__U_COUNT
};

typedef struct {
    uint32_t handle;            // kernel program
    fglsl_uniform_t uniforms[FGLSL_MAX_UNIFORMS];
    uint32_t uniform_count;
    uint32_t registers;         // uniform registers in use
    float values[FGPU_SHADER_MAX_UNIFORMS][4];
    int builtin[FGL__U_COUNT];  // index into uniforms, or -1
    int dirty;
} FGLShader;

typedef struct {
    uint32_t width, height;     // render size
    uint32_t window_id;         // present into this window; 0 = the screen
    int32_t x, y;               // screen position when window_id == 0
    int depth;                  // create a depth buffer
    int page_flip;              // screen only: show frames by flipping VRAM
} FGLContextDesc;

typedef struct {
    FGLContextDesc desc;
    FGLSurface color, color2, depth;    // color2: second buffer when flipping
    int back;                           // which color surface is drawn to
    uint32_t* cmd;
    uint32_t cmd_len, cmd_cap;
    int error;                          // first failed SYS_GPU result, 0 if none
    uint32_t enabled;
    uint32_t program;
    float clear_color[3];
    FGLMat4 model, view, projection;
    FGLVec3 light_dir, light_color, ambient;
    float specular, shininess;
    int state_dirty, uniforms_dirty;
    uint32_t last_triangles;
    // shaders, layouts, textures (ABI 2)
    FGLShader* shader;                  // NULL = built-in program
    FGLShader* uploaded;                // whose uniforms the GPU has now
    uint32_t layout_stride;             // floats; 0 = FGLVertex
    uint32_t layout_count;
    uint32_t layout[FGPU_MAX_ATTRIBS];
    int layout_dirty;
    FGLTexture textures[FGPU_MAX_TEXTURE_UNITS];
    uint32_t texture_flags[FGPU_MAX_TEXTURE_UNITS];
    int textures_dirty;
    char shader_log[192];               // why the last fglCreateShader failed
} FGLContext;

/* ============================================================ syscalls */

static inline uint64_t fgl__call(syscall_gpu_request_t* r) {
    r->size = sizeof(*r);
    r->version = SYS_GPU_ABI_VERSION;
    return mos_syscall1(SYS_GPU, (uint64_t)(uintptr_t)r);
}

// Fills *info about the GPU. Returns 0 on success.
static inline int fglGetDeviceInfo(syscall_gpu_info_t* info) {
    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
    syscall_gpu_request_t r;
    memset(&r, 0, sizeof(r));
    r.op = SYS_GPU_INFO;
    r.out = info;
    return fgl__call(&r) == SYS_SUCCESS ? 0 : -1;
}

static inline FGLSurface fgl__surface(uint32_t w, uint32_t h, uint32_t format, uint32_t flags) {
    syscall_gpu_request_t r;
    memset(&r, 0, sizeof(r));
    r.op = SYS_GPU_CREATE_SURFACE;
    r.width = w; r.height = h; r.format = format; r.flags = flags;
    return fgl__call(&r) == SYS_SUCCESS ? r.handle : 0;
}

static inline void fgl__destroy(uint32_t op, uint32_t handle) {
    if (!handle) return;
    syscall_gpu_request_t r;
    memset(&r, 0, sizeof(r));
    r.op = op;
    r.handle = handle;
    fgl__call(&r);
}

/* ============================================================= context */

static inline void fgl__emit(FGLContext* gl, uint32_t op, const void* payload, uint32_t bytes) {
    uint32_t words = 1 + (bytes + 3) / 4;
    if (gl->cmd_len + words > gl->cmd_cap) {
        uint32_t cap = gl->cmd_cap ? gl->cmd_cap * 2 : 1024;
        while (cap < gl->cmd_len + words) cap *= 2;
        uint32_t* grown = (uint32_t*)realloc(gl->cmd, cap * 4);
        if (!grown) { gl->error = -1; return; }
        gl->cmd = grown;
        gl->cmd_cap = cap;
    }
    gl->cmd[gl->cmd_len] = FGPU_CMD(op, words);
    memset(gl->cmd + gl->cmd_len + 1, 0, (words - 1) * 4);
    if (bytes) memcpy(gl->cmd + gl->cmd_len + 1, payload, bytes);
    gl->cmd_len += words;
}

static inline void fgl__bind(FGLContext* gl) {
    uint32_t t[2] = { gl->back ? gl->color2 : gl->color, gl->depth };
    fgl__emit(gl, FGPU_OP_BIND_TARGET, t, sizeof(t));
    // Every command buffer starts from the defaults: send state again.
    gl->state_dirty = gl->uniforms_dirty = 1;
    gl->layout_dirty = gl->textures_dirty = 1;
    gl->uploaded = NULL;
}

static inline void fglDestroyContext(FGLContext* gl) {
    if (!gl) return;
    if (gl->desc.page_flip) {
        syscall_gpu_request_t r;
        memset(&r, 0, sizeof(r));
        r.op = SYS_GPU_RELEASE_DISPLAY;
        fgl__call(&r);
    }
    fgl__destroy(SYS_GPU_DESTROY_SURFACE, gl->color);
    fgl__destroy(SYS_GPU_DESTROY_SURFACE, gl->color2);
    fgl__destroy(SYS_GPU_DESTROY_SURFACE, gl->depth);
    free(gl->cmd);
    free(gl);
}

// Creates the render surfaces. Returns NULL if the GPU refused.
static inline FGLContext* fglCreateContext(const FGLContextDesc* desc) {
    FGLContext* gl = (FGLContext*)calloc(1, sizeof(FGLContext));
    if (!gl) return NULL;
    gl->desc = *desc;
    if (desc->page_flip && !desc->window_id) {
        gl->color = fgl__surface(desc->width, desc->height, FGPU_FORMAT_XRGB8888, FGPU_SURFACE_SCANOUT);
        gl->color2 = gl->color ? fgl__surface(desc->width, desc->height, FGPU_FORMAT_XRGB8888,
                                              FGPU_SURFACE_SCANOUT) : 0;
        if (!gl->color2) {          // no flipping here: fall back to copies
            fgl__destroy(SYS_GPU_DESTROY_SURFACE, gl->color);
            gl->color = 0;
            gl->desc.page_flip = 0;
        }
    }
    if (!gl->color) gl->color = fgl__surface(desc->width, desc->height, FGPU_FORMAT_XRGB8888, 0);
    if (desc->depth) gl->depth = fgl__surface(desc->width, desc->height, FGPU_FORMAT_DEPTH32F, 0);
    if (!gl->color || (desc->depth && !gl->depth)) {
        fglDestroyContext(gl);
        return NULL;
    }
    gl->model = gl->view = gl->projection = fglIdentity();
    gl->program = FGL_PROGRAM_COLOR;
    gl->light_dir = fglNormalize(fglVec3(-0.4f, -0.7f, -0.6f));
    gl->light_color = fglVec3(1, 1, 1);
    gl->ambient = fglVec3(0.15f, 0.15f, 0.18f);
    gl->specular = 0.5f;
    gl->shininess = 32.0f;
    fgl__bind(gl);
    return gl;
}

/* ============================================================= buffers */

static inline FGLBuffer fglCreateBuffer(FGLContext* gl, uint32_t bytes) {
    (void)gl;
    syscall_gpu_request_t r;
    memset(&r, 0, sizeof(r));
    r.op = SYS_GPU_CREATE_BUFFER;
    r.bytes = bytes;
    return fgl__call(&r) == SYS_SUCCESS ? r.handle : 0;
}

static inline int fglBufferData(FGLContext* gl, FGLBuffer buffer, uint32_t offset,
                                const void* data, uint32_t bytes) {
    (void)gl;
    syscall_gpu_request_t r;
    memset(&r, 0, sizeof(r));
    r.op = SYS_GPU_UPLOAD_BUFFER;
    r.handle = buffer;
    r.offset = offset;
    r.bytes = bytes;
    r.data = data;
    return fgl__call(&r) == SYS_SUCCESS ? 0 : -1;
}

static inline void fglDestroyBuffer(FGLContext* gl, FGLBuffer buffer) {
    (void)gl;
    fgl__destroy(SYS_GPU_DESTROY_BUFFER, buffer);
}

/* =============================================================== state */

static inline void fglEnable(FGLContext* gl, uint32_t caps) { gl->enabled |= caps; gl->state_dirty = 1; }
static inline void fglDisable(FGLContext* gl, uint32_t caps) { gl->enabled &= ~caps; gl->state_dirty = 1; }
// Selects a built-in program (and stops using a shader).
static inline void fglUseProgram(FGLContext* gl, uint32_t program) {
    gl->program = program;
    gl->shader = NULL;
    gl->state_dirty = 1;
}
static inline void fglSetModel(FGLContext* gl, FGLMat4 m) { gl->model = m; gl->uniforms_dirty = 1; }
static inline void fglSetView(FGLContext* gl, FGLMat4 m) { gl->view = m; gl->uniforms_dirty = 1; }
static inline void fglSetProjection(FGLContext* gl, FGLMat4 m) { gl->projection = m; gl->uniforms_dirty = 1; }

// Directional light: `direction` is the way the light travels.
static inline void fglSetLight(FGLContext* gl, FGLVec3 direction, FGLVec3 color, FGLVec3 ambient) {
    gl->light_dir = fglNormalize(direction);
    gl->light_color = color;
    gl->ambient = ambient;
    gl->uniforms_dirty = 1;
}

static inline void fglSetMaterial(FGLContext* gl, float specular, float shininess) {
    gl->specular = specular;
    gl->shininess = shininess;
    gl->uniforms_dirty = 1;
}

static inline void fglViewport(FGLContext* gl, int32_t x, int32_t y, int32_t w, int32_t h) {
    int32_t v[4] = { x, y, w, h };
    fgl__emit(gl, FGPU_OP_VIEWPORT, v, sizeof(v));
}

static inline void fglClearColor(FGLContext* gl, float r, float g, float b) {
    gl->clear_color[0] = r; gl->clear_color[1] = g; gl->clear_color[2] = b;
}

static inline void fglClear(FGLContext* gl, uint32_t bits) {
    uint32_t r = (uint32_t)(gl->clear_color[0] * 255.0f + 0.5f) & 0xFF;
    uint32_t g = (uint32_t)(gl->clear_color[1] * 255.0f + 0.5f) & 0xFF;
    uint32_t b = (uint32_t)(gl->clear_color[2] * 255.0f + 0.5f) & 0xFF;
    float depth = 1.0f;
    uint32_t p[3] = { bits, (r << 16) | (g << 8) | b, 0 };
    memcpy(&p[2], &depth, 4);
    fgl__emit(gl, FGPU_OP_CLEAR, p, sizeof(p));
}

// Writes a value into a shader's uniform registers (count elements of the
// uniform's type, packed tightly in `v`).
static inline void fgl__uniform_set(FGLShader* sh, int loc, uint32_t first, const float* v, uint32_t count) {
    if (!sh || loc < 0 || (uint32_t)loc >= sh->uniform_count) return;
    const fglsl_uniform_t* u = &sh->uniforms[loc];
    if (u->type == FGLSL_SAMPLER) return;
    uint32_t elems = u->array ? u->array : 1;
    uint32_t floats = u->type == FGLSL_MAT4 ? 16u : (uint32_t)u->type;
    uint32_t stride = u->type == FGLSL_MAT4 ? 4u : 1u;      // registers per element
    for (uint32_t e = 0; e < count && first + e < elems; e++) {
        float* dst = sh->values[u->reg + (first + e) * stride];
        const float* src = v + e * floats;
        if (u->type == FGLSL_MAT4) memcpy(dst, src, 64);
        else for (uint32_t k = 0; k < floats; k++) dst[k] = src[k];
    }
    sh->dirty = 1;
}

static inline void fgl__builtin_uniforms(FGLContext* gl, FGLShader* sh, const fgpu_uniforms_t* u) {
    FGLMat4 mv = fglMul(gl->view, gl->model);
    for (int i = 0; i < FGL__U_COUNT; i++) {
        int loc = sh->builtin[i];
        if (loc < 0) continue;
        float tmp[4] = { 0, 0, 0, 0 };
        switch (i) {
            case FGL__U_MVP:        fgl__uniform_set(sh, loc, 0, u->mvp, 1); break;
            case FGL__U_MODEL:      fgl__uniform_set(sh, loc, 0, gl->model.m, 1); break;
            case FGL__U_VIEW:       fgl__uniform_set(sh, loc, 0, gl->view.m, 1); break;
            case FGL__U_PROJECTION: fgl__uniform_set(sh, loc, 0, gl->projection.m, 1); break;
            case FGL__U_MODELVIEW:  fgl__uniform_set(sh, loc, 0, mv.m, 1); break;
            case FGL__U_NORMAL:     fgl__uniform_set(sh, loc, 0, u->normal, 1); break;
            case FGL__U_LIGHT_DIR:  fgl__uniform_set(sh, loc, 0, u->light_dir, 1); break;
            case FGL__U_LIGHT_COLOR:fgl__uniform_set(sh, loc, 0, u->light_color, 1); break;
            case FGL__U_AMBIENT:    fgl__uniform_set(sh, loc, 0, u->ambient, 1); break;
            case FGL__U_CAMERA:     fgl__uniform_set(sh, loc, 0, u->camera_pos, 1); break;
            case FGL__U_MATERIAL:
                tmp[0] = gl->specular; tmp[1] = gl->shininess;
                fgl__uniform_set(sh, loc, 0, tmp, 1);
                break;
        }
    }
}

static inline void fgl__flush_state(FGLContext* gl) {
    if (gl->state_dirty) {
        uint32_t s = 0;
        if (gl->enabled & FGL_DEPTH_TEST) s |= FGPU_STATE_DEPTH_TEST | FGPU_STATE_DEPTH_WRITE;
        if (gl->enabled & FGL_CULL_FACE) s |= FGPU_STATE_CULL_BACK;
        if (gl->enabled & FGL_WIREFRAME) s |= FGPU_STATE_WIREFRAME;
        if (gl->enabled & FGL_BLEND) s |= FGPU_STATE_BLEND;
        fgl__emit(gl, FGPU_OP_SET_STATE, &s, 4);
        fgl__emit(gl, FGPU_OP_SET_PROGRAM, &gl->program, 4);
        uint32_t handle = gl->shader ? gl->shader->handle : 0;
        fgl__emit(gl, FGPU_OP_SET_SHADER, &handle, 4);
        gl->state_dirty = 0;
    }
    if (gl->layout_dirty) {
        if (gl->layout_stride) {
            uint32_t w[2 + FGPU_MAX_ATTRIBS];
            w[0] = gl->layout_stride;
            w[1] = gl->layout_count;
            for (uint32_t i = 0; i < gl->layout_count; i++) w[2 + i] = gl->layout[i];
            fgl__emit(gl, FGPU_OP_SET_VERTEX_LAYOUT, w, (2 + gl->layout_count) * 4);
        }
        gl->layout_dirty = 0;
    }
    if (gl->textures_dirty) {
        for (uint32_t i = 0; i < FGPU_MAX_TEXTURE_UNITS; i++) {
            if (!gl->textures[i]) continue;
            uint32_t w[3] = { i, gl->textures[i], gl->texture_flags[i] };
            fgl__emit(gl, FGPU_OP_BIND_TEXTURE, w, sizeof(w));
        }
        gl->textures_dirty = 0;
    }
    if (gl->uniforms_dirty) {
        fgpu_uniforms_t u;
        memset(&u, 0, sizeof(u));
        FGLMat4 mvp = fglMul(gl->projection, fglMul(gl->view, gl->model));
        FGLMat4 nm = fglNormalMatrix(gl->model);
        memcpy(u.mvp, mvp.m, sizeof(u.mvp));
        memcpy(u.model, gl->model.m, sizeof(u.model));
        memcpy(u.normal, nm.m, sizeof(u.normal));
        u.light_dir[0] = gl->light_dir.x; u.light_dir[1] = gl->light_dir.y; u.light_dir[2] = gl->light_dir.z;
        u.light_color[0] = gl->light_color.x; u.light_color[1] = gl->light_color.y; u.light_color[2] = gl->light_color.z;
        u.ambient[0] = gl->ambient.x; u.ambient[1] = gl->ambient.y; u.ambient[2] = gl->ambient.z;
        // The camera sits at the view matrix's inverse translation.
        FGLMat4 v = gl->view;
        FGLVec3 t = fglVec3(v.m[12], v.m[13], v.m[14]);
        u.camera_pos[0] = -(v.m[0] * t.x + v.m[1] * t.y + v.m[2] * t.z);
        u.camera_pos[1] = -(v.m[4] * t.x + v.m[5] * t.y + v.m[6] * t.z);
        u.camera_pos[2] = -(v.m[8] * t.x + v.m[9] * t.y + v.m[10] * t.z);
        u.material[0] = gl->specular;
        u.material[1] = gl->shininess;
        fgl__emit(gl, FGPU_OP_SET_UNIFORMS, &u, sizeof(u));
        if (gl->shader) fgl__builtin_uniforms(gl, gl->shader, &u);
        gl->uniforms_dirty = 0;
    }
    FGLShader* sh = gl->shader;
    if (sh && sh->registers && (sh->dirty || gl->uploaded != sh)) {
        uint32_t buf[2 + FGPU_SHADER_MAX_UNIFORMS * 4];
        buf[0] = 0;                     // first register
        buf[1] = sh->registers;         // count
        memcpy(buf + 2, sh->values, sh->registers * 16);
        fgl__emit(gl, FGPU_OP_SET_CONSTANTS, buf, (2 + sh->registers * 4) * 4);
        sh->dirty = 0;
        gl->uploaded = sh;
    } else if (sh && !sh->registers) {
        gl->uploaded = sh;
    }
}

/* ============================================================= shaders */

static inline int fglSubmit(FGLContext* gl);

// Compiles an FGSL vertex + fragment shader (docs/fgsl.md) and loads it
// onto the GPU. Returns NULL on failure; fglShaderLog(gl) says why.
static inline FGLShader* fglCreateShader(FGLContext* gl, const char* vertex_src, const char* fragment_src) {
    gl->shader_log[0] = '\0';
    fglsl_program_t* c = (fglsl_program_t*)malloc(sizeof(fglsl_program_t));
    FGLShader* sh = (FGLShader*)calloc(1, sizeof(FGLShader));
    if (!c || !sh) {
        free(c);
        free(sh);
        strcpy(gl->shader_log, "out of memory");
        return NULL;
    }
    if (fglsl_compile(c, vertex_src, fragment_src) != 0) {
        memcpy(gl->shader_log, c->error, sizeof(gl->shader_log));
        gl->shader_log[sizeof(gl->shader_log) - 1] = '\0';
        free(c);
        free(sh);
        return NULL;
    }
    syscall_gpu_request_t r;
    memset(&r, 0, sizeof(r));
    r.op = SYS_GPU_CREATE_PROGRAM;
    r.data = c->words;
    r.bytes = c->size;
    if (fgl__call(&r) != SYS_SUCCESS) {
        strcpy(gl->shader_log, "the GPU rejected the program (see the serial log)");
        free(c);
        free(sh);
        return NULL;
    }
    sh->handle = r.handle;
    sh->uniform_count = c->uniform_count;
    memcpy(sh->uniforms, c->uniforms, sizeof(sh->uniforms));
    sh->registers = c->next_uniform_reg;
    static const char* const names[FGL__U_COUNT] = {
        "fgl_ModelViewProjection", "fgl_Model", "fgl_View", "fgl_Projection", "fgl_ModelView",
        "fgl_NormalMatrix", "fgl_LightDirection", "fgl_LightColor", "fgl_Ambient",
        "fgl_CameraPosition", "fgl_Material" };
    for (int i = 0; i < FGL__U_COUNT; i++) sh->builtin[i] = fglsl_find_uniform(c, names[i]);
    sh->dirty = 1;
    free(c);
    return sh;
}

// Why the last fglCreateShader failed ("" if it did not).
static inline const char* fglShaderLog(FGLContext* gl) { return gl->shader_log; }

static inline void fglUseShader(FGLContext* gl, FGLShader* sh) {
    gl->shader = sh;
    gl->state_dirty = 1;
    gl->uniforms_dirty = 1;     // refresh its fgl_* uniforms
}

static inline void fglDestroyShader(FGLContext* gl, FGLShader* sh) {
    if (!sh) return;
    fglSubmit(gl);      // recorded commands may still use it
    if (gl->shader == sh) fglUseProgram(gl, gl->program);
    if (gl->uploaded == sh) gl->uploaded = NULL;
    fgl__destroy(SYS_GPU_DESTROY_PROGRAM, sh->handle);
    free(sh);
}

// Location of a uniform (an index for fglUniform*), or -1 if the shaders
// do not declare it (or the compiler found it unused).
static inline int fglUniformLocation(FGLShader* sh, const char* name) {
    if (!sh) return -1;
    for (uint32_t i = 0; i < sh->uniform_count; i++)
        if (strcmp(sh->uniforms[i].name, name) == 0) return (int)i;
    return -1;
}

// Uniform values belong to the shader and stay set until changed.
static inline void fglUniform1f(FGLShader* sh, int loc, float x) {
    float v[4] = { x, 0, 0, 0 };
    fgl__uniform_set(sh, loc, 0, v, 1);
}
static inline void fglUniform2f(FGLShader* sh, int loc, float x, float y) {
    float v[4] = { x, y, 0, 0 };
    fgl__uniform_set(sh, loc, 0, v, 1);
}
static inline void fglUniform3f(FGLShader* sh, int loc, float x, float y, float z) {
    float v[4] = { x, y, z, 0 };
    fgl__uniform_set(sh, loc, 0, v, 1);
}
static inline void fglUniform4f(FGLShader* sh, int loc, float x, float y, float z, float w) {
    float v[4] = { x, y, z, w };
    fgl__uniform_set(sh, loc, 0, v, 1);
}
static inline void fglUniformVec3(FGLShader* sh, int loc, FGLVec3 v) { fglUniform3f(sh, loc, v.x, v.y, v.z); }
static inline void fglUniformMatrix4(FGLShader* sh, int loc, FGLMat4 m) { fgl__uniform_set(sh, loc, 0, m.m, 1); }
// `count` elements of the uniform's type (an array), tightly packed
// (vec3 = 3 floats, mat4 = 16), starting at element `first`.
static inline void fglUniformArray(FGLShader* sh, int loc, uint32_t first, const float* values, uint32_t count) {
    fgl__uniform_set(sh, loc, first, values, count);
}

/* ============================================================ textures */

// Creates a texture from width x height ARGB8888 pixels (rows packed);
// pixels may be NULL (black). Up to 4096 x 4096. Returns 0 on failure.
static inline FGLTexture fglCreateTexture(FGLContext* gl, uint32_t width, uint32_t height, const uint32_t* pixels) {
    (void)gl;
    FGLTexture t = fgl__surface(width, height, FGPU_FORMAT_XRGB8888, 0);
    if (t && pixels) {
        syscall_gpu_request_t r;
        memset(&r, 0, sizeof(r));
        r.op = SYS_GPU_WRITE_SURFACE;
        r.handle = t;
        r.data = pixels;
        r.pitch = width * 4;
        if (fgl__call(&r) != SYS_SUCCESS) { fgl__destroy(SYS_GPU_DESTROY_SURFACE, t); return 0; }
    }
    return t;
}

// Replaces a texture's pixels (pitch = bytes per row).
static inline int fglTextureData(FGLContext* gl, FGLTexture t, const uint32_t* pixels, uint32_t pitch) {
    fglSubmit(gl);      // draws recorded so far see the old pixels
    syscall_gpu_request_t r;
    memset(&r, 0, sizeof(r));
    r.op = SYS_GPU_WRITE_SURFACE;
    r.handle = t;
    r.data = pixels;
    r.pitch = pitch;
    return fgl__call(&r) == SYS_SUCCESS ? 0 : -1;
}

static inline void fglDestroyTexture(FGLContext* gl, FGLTexture t) {
    fglSubmit(gl);
    for (uint32_t i = 0; i < FGPU_MAX_TEXTURE_UNITS; i++)
        if (gl->textures[i] == t) gl->textures[i] = 0;
    fgl__destroy(SYS_GPU_DESTROY_SURFACE, t);
}

// Binds a texture to a unit (0-3); flags: FGL_TEXTURE_LINEAR, FGL_TEXTURE_CLAMP.
static inline void fglBindTexture(FGLContext* gl, uint32_t unit, FGLTexture t, uint32_t flags) {
    if (unit >= FGPU_MAX_TEXTURE_UNITS) return;
    gl->textures[unit] = t;
    gl->texture_flags[unit] = flags;
    gl->textures_dirty = 1;
    if (!t) {   // unbinding must reach the GPU too
        uint32_t w[3] = { unit, 0, 0 };
        fgl__emit(gl, FGPU_OP_BIND_TEXTURE, w, sizeof(w));
    }
}

// Binds a texture to the unit of a sampler2D uniform.
static inline void fglUniformTexture(FGLContext* gl, FGLShader* sh, int loc, FGLTexture t, uint32_t flags) {
    if (!sh || loc < 0 || (uint32_t)loc >= sh->uniform_count || sh->uniforms[loc].type != FGLSL_SAMPLER) return;
    fglBindTexture(gl, sh->uniforms[loc].reg, t, flags);
}

/* ====================================================== vertex layouts */

// Describes your own vertex struct: `stride` bytes per vertex, and for
// each attribute location 0..count-1 its byte offset and float count.
// Offsets and the stride must be multiples of 4. Returns 0, or -1 if the
// layout is invalid (it is then not changed).
static inline int fglVertexLayout(FGLContext* gl, uint32_t stride, uint32_t count, const FGLAttribute* attribs) {
    if (!stride || (stride & 3) || stride / 4 > FGPU_MAX_VERTEX_STRIDE || count > FGPU_MAX_ATTRIBS) return -1;
    uint32_t words[FGPU_MAX_ATTRIBS];
    for (uint32_t i = 0; i < count; i++) {
        uint32_t off = attribs[i].offset, n = attribs[i].components;
        if (n > 4 || (off & 3) || (n && off / 4 + n > stride / 4)) return -1;
        words[i] = FGPU_ATTRIB(off / 4, n);
    }
    gl->layout_stride = stride / 4;
    gl->layout_count = count;
    memcpy(gl->layout, words, count * 4);
    gl->layout_dirty = 1;
    return 0;
}

// Back to FGLVertex.
static inline void fglVertexLayoutDefault(FGLContext* gl) {
    gl->layout_stride = 0;
    gl->layout_count = 0;
    gl->layout_dirty = 1;
}

// FGLVertexUV: position, normal, color, uv at locations 0-3.
static inline void fglVertexLayoutUV(FGLContext* gl) {
    static const FGLAttribute a[4] = { { 0, 3 }, { 12, 3 }, { 24, 4 }, { 40, 2 } };
    fglVertexLayout(gl, sizeof(FGLVertexUV), 4, a);
}

/* ============================================================= drawing */

// Draws `count` vertices (triangles) starting at `first`.
static inline void fglDraw(FGLContext* gl, FGLBuffer vertices, uint32_t first, uint32_t count) {
    fgl__flush_state(gl);
    uint32_t format = gl->layout_stride ? FGPU_VERTEX_LAYOUT : FGPU_VERTEX_P3N3C4;
    fgpu_draw_t d = { vertices, format, first, count, 0, 0, 0 };
    fgl__emit(gl, FGPU_OP_DRAW, &d, sizeof(d));
}

// Draws triangles from a uint32 index buffer.
static inline void fglDrawIndexed(FGLContext* gl, FGLBuffer vertices, FGLBuffer indices,
                                  uint32_t first_index, uint32_t count) {
    fgl__flush_state(gl);
    uint32_t format = gl->layout_stride ? FGPU_VERTEX_LAYOUT : FGPU_VERTEX_P3N3C4;
    fgpu_draw_t d = { vertices, format, 0, 0, indices, first_index, count };
    fgl__emit(gl, FGPU_OP_DRAW, &d, sizeof(d));
}

// Sends the recorded commands to the GPU and waits for them.
static inline int fglSubmit(FGLContext* gl) {
    if (!gl->cmd_len) return 0;
    syscall_gpu_request_t r;
    memset(&r, 0, sizeof(r));
    r.op = SYS_GPU_SUBMIT;
    r.data = gl->cmd;
    r.bytes = gl->cmd_len * 4;
    uint64_t res = fgl__call(&r);
    gl->cmd_len = 0;
    gl->last_triangles = r.handle;
    fgl__bind(gl);     // the next command buffer starts from scratch
    if (res != SYS_SUCCESS) { gl->error = (int)res; return -1; }
    return 0;
}

// Submits and shows the frame (in the window, or on the screen).
static inline int fglPresent(FGLContext* gl) {
    if (fglSubmit(gl) != 0) return -1;
    syscall_gpu_request_t r;
    memset(&r, 0, sizeof(r));
    r.op = SYS_GPU_PRESENT;
    r.handle = gl->back ? gl->color2 : gl->color;
    r.window_id = gl->desc.window_id;
    r.x = gl->desc.x;
    r.y = gl->desc.y;
    if (gl->desc.page_flip) r.flags = FGPU_PRESENT_FLIP;
    uint64_t res = fgl__call(&r);
    if (gl->desc.page_flip) {          // draw the next frame into the other buffer
        gl->back ^= 1;
        gl->cmd_len = 0;
        fgl__bind(gl);
    }
    return res == SYS_SUCCESS ? 0 : -1;
}

// Copies the last frame into memory (pitch = bytes per row).
static inline int fglReadPixels(FGLContext* gl, uint32_t* out, uint32_t pitch) {
    syscall_gpu_request_t r;
    memset(&r, 0, sizeof(r));
    r.op = SYS_GPU_READBACK;
    r.handle = gl->back ? gl->color2 : gl->color;
    r.out = out;
    r.pitch = pitch;
    return fgl__call(&r) == SYS_SUCCESS ? 0 : -1;
}

/* ============================================================= helpers */

// Writes 36 vertices (12 triangles) of an axis-aligned cube of half-size
// `s` centered on the origin, one color per face. Returns 36.
static inline uint32_t fglMakeCube(FGLVertex out[36], float s, const float face_colors[6][3]) {
    static const float normals[6][3] = { {0,0,1}, {0,0,-1}, {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0} };
    static const float default_colors[6][3] = {
        {0.95f, 0.30f, 0.25f}, {0.25f, 0.80f, 0.35f}, {0.25f, 0.45f, 0.95f},
        {0.95f, 0.80f, 0.20f}, {0.20f, 0.85f, 0.90f}, {0.85f, 0.35f, 0.90f} };
    if (!face_colors) face_colors = default_colors;
    uint32_t n = 0;
    for (int f = 0; f < 6; f++) {
        FGLVec3 N = fglVec3(normals[f][0], normals[f][1], normals[f][2]);
        // Two in-face axes so the corners run counter-clockwise from outside.
        FGLVec3 U, V;
        if (N.x != 0) { U = fglVec3(0, 0, -N.x); V = fglVec3(0, 1, 0); }
        else if (N.y != 0) { U = fglVec3(1, 0, 0); V = fglVec3(0, 0, -N.y); }
        else { U = fglVec3(N.z, 0, 0); V = fglVec3(0, 1, 0); }
        static const float su[4] = { -1, 1, 1, -1 }, sv[4] = { -1, -1, 1, 1 };
        static const int order[6] = { 0, 1, 2, 0, 2, 3 };
        for (int k = 0; k < 6; k++) {
            int c = order[k];
            FGLVec3 p = fglAdd(N, fglAdd(fglScale3(U, su[c]), fglScale3(V, sv[c])));
            FGLVertex* v = &out[n++];
            v->px = p.x * s; v->py = p.y * s; v->pz = p.z * s;
            v->nx = N.x; v->ny = N.y; v->nz = N.z;
            v->r = face_colors[f][0]; v->g = face_colors[f][1]; v->b = face_colors[f][2];
            v->a = 1.0f;
        }
    }
    return n;
}

// The same cube as fglMakeCube with texture coordinates: each face shows
// the whole texture, upright. Use with fglVertexLayoutUV(gl). Returns 36.
static inline uint32_t fglMakeCubeUV(FGLVertexUV out[36], float s, const float face_colors[6][3]) {
    FGLVertex v[36];
    uint32_t n = fglMakeCube(v, s, face_colors);
    // v = 0 is the top row of a texture: the bottom corners get v = 1.
    static const float su[6] = { 0, 1, 1, 0, 1, 0 }, sv[6] = { 1, 1, 0, 1, 0, 0 };
    for (uint32_t i = 0; i < n; i++) {
        memcpy(&out[i], &v[i], sizeof(FGLVertex));
        out[i].u = su[i % 6];
        out[i].v = sv[i % 6];
    }
    return n;
}

#endif // FARCADEGL_H
