/*
 * fglcube - a rotating, lit cube drawn with FarcadeGL.
 *
 *     ./build.sh programs/fglcube        -> fglcube.run
 *     fglcube [seconds] [--gouraud|--flat|--wire]
 *
 * Under MOSdesk it opens a window. From the terminal it takes the whole
 * screen (page flipping in video memory when the GPU can) and stops at
 * the first key press, or after `seconds`.
 */
#include "farcadegl.h"
#include "window.h"
#include "stdio.h"
#include "x86_64/user_common.h"

static volatile int g_quit;

static void on_close(uint32_t id) { (void)id; g_quit = 1; }

static long screen_owner(void) {
    syscall_graphics_request_t r;
    memset(&r, 0, sizeof(r));
    r.op = SYS_GRAPHICS_OWNER;
    return mos_graphics_call(&r);
}

static void number(char* out, long v) {
    char t[24];
    int n = 0;
    if (v < 0) { *out++ = '-'; v = -v; }
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *out++ = t[--n];
    *out = '\0';
}

int main(int argc, char** argv) {
    long seconds = 0;
    uint32_t program = FGL_PROGRAM_PHONG;
    int wire = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--gouraud") == 0) program = FGL_PROGRAM_GOURAUD;
        else if (strcmp(argv[i], "--flat") == 0) program = FGL_PROGRAM_COLOR;
        else if (strcmp(argv[i], "--wire") == 0) wire = 1;
        else seconds = atoi(argv[i]);
    }

    syscall_gpu_info_t info;
    if (fglGetDeviceInfo(&info) != 0) {
        printf("fglcube: this kernel has no GPU interface\n");
        return 1;
    }
    printf("GPU: %s (%s), backend: %s\n", info.name, info.driver, info.backend);

    // A window manager owns the screen under MOSdesk: use a window.
    long owner = screen_owner();
    int windowed = owner > 0 && owner != mos_getpid();
    uint32_t window = 0;
    FGLContextDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.depth = 1;
    if (windowed) {
        desc.width = 640;
        desc.height = 480;
        window = mos_window_create(0, 120, 90, desc.width, desc.height, "FarcadeGL cube");
        if (!window) { printf("fglcube: could not open a window\n"); return 1; }
        mos_window_set_resizable(window, false);
        desc.window_id = window;
    } else {
        if (mos_graphics_acquire() != 0) { printf("fglcube: screen is busy\n"); return 1; }
        desc.width = info.screen_width;
        desc.height = info.screen_height;
        desc.page_flip = 1;
    }

    FGLContext* gl = fglCreateContext(&desc);
    if (!gl) {
        printf("fglcube: could not create GPU surfaces\n");
        if (!windowed) mos_graphics_release();
        return 1;
    }

    FGLVertex cube[36];
    uint32_t count = fglMakeCube(cube, 1.0f, NULL);
    FGLBuffer vb = fglCreateBuffer(gl, sizeof(cube));
    fglBufferData(gl, vb, 0, cube, sizeof(cube));

    // A floor under the cube so the lighting has something to show.
    FGLVertex floor_quad[6];
    const float fy = -1.6f, fs = 4.0f;
    const float corners[6][2] = { {-1,-1}, {1,1}, {1,-1}, {-1,-1}, {-1,1}, {1,1} };
    for (int i = 0; i < 6; i++) {
        FGLVertex v = { corners[i][0] * fs, fy, corners[i][1] * fs, 0, 1, 0, 0.55f, 0.58f, 0.65f, 1 };
        floor_quad[i] = v;
    }
    FGLBuffer fb = fglCreateBuffer(gl, sizeof(floor_quad));
    fglBufferData(gl, fb, 0, floor_quad, sizeof(floor_quad));

    mos_window_callbacks_t callbacks;
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.onwindowclose = on_close;

    float aspect = (float)desc.width / (float)desc.height;
    long start = mos_uptime(), last_title = start;
    long frames = 0, fps_frames = 0;
    while (!g_quit) {
        long now = mos_uptime();
        float t = (float)(now - start) / 1000.0f;

        fglClearColor(gl, 0.06f, 0.08f, 0.13f);
        fglClear(gl, FGL_COLOR_BUFFER_BIT | FGL_DEPTH_BUFFER_BIT);
        fglEnable(gl, FGL_DEPTH_TEST | FGL_CULL_FACE);
        if (wire) fglEnable(gl, FGL_WIREFRAME);
        fglUseProgram(gl, program);
        fglSetProjection(gl, fglPerspective(0.9f, aspect, 0.1f, 50.0f));
        fglSetView(gl, fglLookAt(fglVec3(0, 1.6f, 5.5f), fglVec3(0, 0, 0), fglVec3(0, 1, 0)));
        fglSetLight(gl, fglVec3(-0.5f, -1.0f, -0.6f), fglVec3(1.0f, 0.97f, 0.9f),
                    fglVec3(0.16f, 0.17f, 0.22f));

        fglSetModel(gl, fglIdentity());
        fglSetMaterial(gl, 0.1f, 8);
        fglDraw(gl, fb, 0, 6);

        FGLMat4 spin = fglMul(fglRotate(t * 0.9f, fglVec3(0, 1, 0)),
                              fglRotate(t * 0.6f, fglVec3(1, 0, 0)));
        fglSetModel(gl, fglMul(fglTranslate(fglVec3(0, 0.2f * fglSin(t * 2.0f), 0)), spin));
        fglSetMaterial(gl, 0.6f, 32);
        fglDraw(gl, vb, 0, count);

        if (fglPresent(gl) != 0) {
            printf("fglcube: GPU error %d\n", gl->error);
            break;
        }
        frames++;
        fps_frames++;

        if (now - last_title >= 1000) {
            char text[64] = "FarcadeGL cube - ";
            char* end = text + strlen(text);
            number(end, fps_frames * 1000 / (now - last_title));
            end += strlen(end);
            memcpy(end, " fps", 5);
            if (windowed) mos_window_set_title(window, text);
            else printf("%s\n", text);
            fps_frames = 0;
            last_title = now;
        }

        if (windowed) {
            mos_window_dispatch_events(window, &callbacks);
        } else {
            syscall_input_event_t ev[8];
            long n = (long)mos_input_read(ev, 8, 0, NULL);
            for (long i = 0; i < n && n <= 8; i++) {
                if (ev[i].type == SYSCALL_INPUT_KEY_DOWN) g_quit = 1;
            }
        }
        if (seconds && now - start >= seconds * 1000) g_quit = 1;
    }

    fglDestroyBuffer(gl, vb);
    fglDestroyBuffer(gl, fb);
    fglDestroyContext(gl);
    if (windowed) mos_window_close(window);
    else mos_graphics_release();
    printf("fglcube: %ld frames\n", frames);
    return 0;
}
