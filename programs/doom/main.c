/*
 * Doom (1993) for Minimal-OS: the platform layer.
 *
 * The game is id Software's Doom engine as released under the GNU GPL,
 * through doomgeneric (Chocolate Doom made portable; src/, see
 * README.md). This file is everything Minimal-OS-specific: a mosWM
 * window, the keyboard, the clock, and the picture.
 *
 * The picture: Doom renders its 320x200 frame in software, exactly as in
 * 1993. Each frame becomes a FarcadeGL texture and is drawn as one quad
 * with an FGSL shader, scaled to the window and letterboxed to 4:3 (the
 * shape Doom was drawn for: 320x200 on a 4:3 screen). On a GPU that runs
 * FarcadeGL in hardware the scaling happens on the card.
 *
 * The game data (an IWAD) is looked for next to the program, in
 * 0:/programs/Doom/: doom.wad (the registered or Ultimate game),
 * doom1.wad (shareware), freedoom1.wad (Freedoom, which make mosdesk-doom
 * bundles), and the Doom II family after them. A .wad given on the
 * command line wins: run 0:/programs/Doom.run 0:/users/me/doom.wad
 *
 * Keys: arrows move and turn, Ctrl fires, Space opens doors, Shift runs,
 * Alt strafes, 1-7 pick weapons, Escape is the menu, Tab the map.
 * Alt+Enter switches full screen.
 *
 * The mouse: while a level is being played (not in the menu, not paused)
 * the window asks mosWM for pointer lock: the cursor is hidden and kept
 * by the window, and mosWM sends how far the mouse moved
 * (SYSCALL_WINDOW_EVENT_MOUSE_RELATIVE). As in 1993, sideways turns,
 * forward and back walks, the left button fires, the right one strafes
 * and the middle one walks forward. The menu (Escape), pausing or
 * switching to another window gives the cursor back.
 *
 * Sound effects are in sound.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "farcadegl.h"
#include "window.h"
#include "minimalos.h"
#include "src/doomgeneric.h"
#include "src/doomkeys.h"
#include "src/doomstat.h"
#include "src/d_event.h"

void I_Quit(void);

#define TITLE "Doom"
#define START_W 960u
#define START_H 720u

static uint32_t g_window;
static FGLContext* g_gl;
static FGLTexture g_tex;
static FGLBuffer g_quad;
static FGLShader* g_shader;
static int g_tex_loc = -1;
static uint32_t g_width = START_W, g_height = START_H;
static int g_fullscreen;
static int g_alt_down;
static int g_focused = 1;
static int g_locked;                    /* pointer lock asked for */
static int g_mouse_dx, g_mouse_dy;      /* motion since the last tic */
static int g_mouse_buttons, g_mouse_sent_buttons;
static int g_mouse_moved;
static char g_data_dir[256] = "0:/programs/Doom/";

/* --------------------------------------------------------- the picture */

static const char* k_vertex =
    "attribute vec2 position;\n"
    "layout(location = 1) attribute vec2 uv;\n"
    "varying vec2 v_uv;\n"
    "void main() { v_uv = uv; gl_Position = vec4(position, 0.0, 1.0); }\n";

// Doom's pixels are 0x00RRGGBB: the alpha byte is not used.
static const char* k_fragment =
    "uniform sampler2D frame;\n"
    "varying vec2 v_uv;\n"
    "void main() { gl_FragColor = vec4(texture(frame, v_uv).rgb, 1.0); }\n";

typedef struct { float x, y, u, v; } quad_vertex_t;

static void destroy_gl(void) {
    if (!g_gl) return;
    if (g_shader) fglDestroyShader(g_gl, g_shader);
    if (g_quad) fglDestroyBuffer(g_gl, g_quad);
    if (g_tex) fglDestroyTexture(g_gl, g_tex);
    fglDestroyContext(g_gl);
    g_gl = NULL;
    g_shader = NULL;
    g_quad = 0;
    g_tex = 0;
}

static int create_gl(void) {
    FGLContextDesc desc = { .width = g_width, .height = g_height, .window_id = g_window };
    g_gl = fglCreateContext(&desc);
    if (!g_gl) return -1;
    g_tex = fglCreateTexture(g_gl, DOOMGENERIC_RESX, DOOMGENERIC_RESY, NULL);
    static const quad_vertex_t quad[6] = {
        { -1, -1, 0, 1 }, { 1, -1, 1, 1 }, { 1, 1, 1, 0 },
        { -1, -1, 0, 1 }, { 1, 1, 1, 0 }, { -1, 1, 0, 0 },
    };
    g_quad = fglCreateBuffer(g_gl, sizeof(quad));
    if (g_quad) fglBufferData(g_gl, g_quad, 0, quad, sizeof(quad));
    g_shader = fglCreateShader(g_gl, k_vertex, k_fragment);
    if (!g_tex || !g_quad || !g_shader) {
        if (!g_shader) mos_log(fglShaderLog(g_gl));
        destroy_gl();
        return -1;
    }
    g_tex_loc = fglUniformLocation(g_shader, "frame");
    static const FGLAttribute attrs[2] = { { 0, 2 }, { 8, 2 } };
    fglVertexLayout(g_gl, sizeof(quad_vertex_t), 2, attrs);
    return 0;
}

void DG_DrawFrame(void) {
    if (!g_gl && create_gl() != 0) return;
    fglTextureData(g_gl, g_tex, DG_ScreenBuffer, DOOMGENERIC_RESX * 4);
    // The largest 4:3 rectangle that fits, in the middle.
    uint32_t w = g_width, h = g_width * 3u / 4u;
    if (h > g_height) { h = g_height; w = g_height * 4u / 3u; }
    fglViewport(g_gl, 0, 0, (int32_t)g_width, (int32_t)g_height);
    fglClearColor(g_gl, 0, 0, 0);
    fglClear(g_gl, FGL_COLOR_BUFFER_BIT);
    fglViewport(g_gl, (int32_t)(g_width - w) / 2, (int32_t)(g_height - h) / 2, (int32_t)w, (int32_t)h);
    fglUseShader(g_gl, g_shader);
    // Nearest texels: Doom's chunky pixels, as on a 1993 screen.
    fglUniformTexture(g_gl, g_shader, g_tex_loc, g_tex, FGL_TEXTURE_CLAMP);
    fglDraw(g_gl, g_quad, 0, 6);
    if (fglPresent(g_gl) != 0) {
        // The surfaces are gone (a GPU reset, a mode change): start over.
        destroy_gl();
    }
}

/* ------------------------------------------------------------- input */

// HID usage codes (what the window events carry) to Doom's keys.
static unsigned char doom_key(uint32_t hid) {
    if (hid >= 0x04 && hid <= 0x1D) return (unsigned char)('a' + (hid - 0x04));
    if (hid >= 0x1E && hid <= 0x26) return (unsigned char)('1' + (hid - 0x1E));
    if (hid >= 0x3A && hid <= 0x43) return (unsigned char)(KEY_F1 + (hid - 0x3A));
    switch (hid) {
        case 0x27: return '0';
        case 0x28: return KEY_ENTER;
        case 0x29: return KEY_ESCAPE;
        case 0x2A: return KEY_BACKSPACE;
        case 0x2B: return KEY_TAB;
        case 0x2C: return KEY_USE;          // Space: open doors, flip switches
        case 0x2D: return KEY_MINUS;
        case 0x2E: return KEY_EQUALS;
        case 0x2F: return '[';
        case 0x30: return ']';
        case 0x31: return '\\';
        case 0x33: return ';';
        case 0x34: return '\'';
        case 0x35: return '`';
        case 0x36: return ',';              // also strafe left
        case 0x37: return '.';              // also strafe right
        case 0x38: return '/';
        case 0x39: return KEY_CAPSLOCK;
        case 0x44: return KEY_F11;
        case 0x45: return KEY_F12;
        case 0x46: return KEY_PRTSCR;
        case 0x47: return KEY_SCRLCK;
        case 0x48: return KEY_PAUSE;
        case 0x49: return KEY_INS;
        case 0x4A: return KEY_HOME;
        case 0x4B: return KEY_PGUP;
        case 0x4C: return KEY_DEL;
        case 0x4D: return KEY_END;
        case 0x4E: return KEY_PGDN;
        case 0x4F: return KEY_RIGHTARROW;
        case 0x50: return KEY_LEFTARROW;
        case 0x51: return KEY_DOWNARROW;
        case 0x52: return KEY_UPARROW;
        case 0x53: return KEY_NUMLOCK;
        case 0x54: return KEYP_DIVIDE;
        case 0x55: return KEYP_MULTIPLY;
        case 0x56: return KEYP_MINUS;
        case 0x57: return KEYP_PLUS;
        case 0x58: return KEYP_ENTER;
        case 0x59: return KEYP_1;
        case 0x5A: return KEYP_2;
        case 0x5B: return KEYP_3;
        case 0x5C: return KEYP_4;
        case 0x5D: return KEYP_5;
        case 0x5E: return KEYP_6;
        case 0x5F: return KEYP_7;
        case 0x60: return KEYP_8;
        case 0x61: return KEYP_9;
        case 0xE0: case 0xE4: return KEY_FIRE;      // Ctrl
        case 0xE1: case 0xE5: return KEY_RSHIFT;    // Shift: run
        case 0xE2: case 0xE6: return KEY_LALT;      // Alt: strafe
        default: return 0;
    }
}

#define KEY_QUEUE 64
static struct { unsigned char key, pressed; } g_keys[KEY_QUEUE];
static unsigned g_key_head, g_key_tail;

static void queue_key(unsigned char key, int pressed) {
    if (!key || g_key_tail - g_key_head >= KEY_QUEUE) return;
    g_keys[g_key_tail % KEY_QUEUE].key = key;
    g_keys[g_key_tail % KEY_QUEUE].pressed = (unsigned char)pressed;
    g_key_tail++;
}

static void toggle_fullscreen(void) {
    g_fullscreen = !g_fullscreen;
    mos_window_set_fullscreen(g_window, g_fullscreen);
}

static void release_all_keys(void) {
    // A key held when the window loses focus never sends its release.
    static const unsigned char held[] = { KEY_FIRE, KEY_RSHIFT, KEY_LALT, KEY_UPARROW, KEY_DOWNARROW,
                                          KEY_LEFTARROW, KEY_RIGHTARROW, KEY_USE, ',', '.' };
    for (unsigned i = 0; i < sizeof(held); i++) queue_key(held[i], 0);
    g_alt_down = 0;
}

/* Pointer lock while a level is played; the cursor back otherwise. */
static void update_pointer_lock(void) {
    int want = g_window && g_focused && gamestate == GS_LEVEL && !menuactive && !paused && !demoplayback;
    if (want == g_locked) return;
    if (mos_window_set_pointer_lock(g_window, want != 0) != SYS_SUCCESS) return;
    g_locked = want;
    g_mouse_dx = g_mouse_dy = 0;
    g_mouse_moved = 0;
    g_mouse_buttons = 0;
}

/* Doom reads the mouse once per tic (G_Responder sets, not adds, the
 * motion), so the motion of all events since the last tic goes in one. */
static void post_mouse(void) {
    if (!g_mouse_moved && g_mouse_buttons == g_mouse_sent_buttons) return;
    event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = ev_mouse;
    ev.data1 = g_mouse_buttons;         /* bit 0 fire, 1 strafe, 2 forward: the same bits */
    ev.data2 = g_mouse_dx;
    ev.data3 = -g_mouse_dy;             /* away from you (up) walks forward */
    D_PostEvent(&ev);
    g_mouse_sent_buttons = g_mouse_buttons;
    g_mouse_dx = g_mouse_dy = 0;
    g_mouse_moved = 0;
}

static void pump_events(void) {
    syscall_window_event_t ev;
    while (mos_window_next_event(g_window, &ev) == SYS_SUCCESS) {
        switch (ev.type) {
            case SYSCALL_WINDOW_EVENT_CLOSE_REQUEST:
                I_Quit();           // saves the settings, then exits
                break;
            case SYSCALL_WINDOW_EVENT_RESIZED:
                if (ev.width > 0 && ev.height > 0 &&
                    ((uint32_t)ev.width != g_width || (uint32_t)ev.height != g_height)) {
                    g_width = (uint32_t)ev.width;
                    g_height = (uint32_t)ev.height;
                    destroy_gl();       // made again at the next frame, at the new size
                }
                break;
            case SYSCALL_WINDOW_EVENT_FOCUS_LOST:
                release_all_keys();
                g_focused = 0;
                g_mouse_buttons = 0;
                update_pointer_lock();
                break;
            case SYSCALL_WINDOW_EVENT_FOCUS_GAINED:
                g_focused = 1;
                break;
            case SYSCALL_WINDOW_EVENT_MOUSE_RELATIVE:
                if (!g_locked) break;
                g_mouse_dx += ev.x;
                g_mouse_dy += ev.y;
                g_mouse_moved = 1;
                g_mouse_buttons = ev.buttons & 7;
                break;
            case SYSCALL_WINDOW_EVENT_MOUSE:
                // Clicks count only while locked: without the lock the
                // cursor is free, and a click is just a click.
                if (g_locked) g_mouse_buttons = ev.buttons & 7;
                break;
            case SYSCALL_WINDOW_EVENT_KEY: {
                int pressed = ev.buttons != 0;
                if (ev.key == 0xE2 || ev.key == 0xE6) g_alt_down = pressed;
                if (pressed && ev.key == 0x28 && g_alt_down) { toggle_fullscreen(); break; }
                queue_key(doom_key(ev.key), pressed);
                break;
            }
            default:
                break;
        }
    }
}

int DG_GetKey(int* pressed, unsigned char* key) {
    if (g_key_head == g_key_tail) pump_events();
    if (g_key_head == g_key_tail) {
        // Keys first, then the mouse: I_GetEvent calls this until it
        // returns 0, once per tic.
        update_pointer_lock();
        post_mouse();
        return 0;
    }
    *key = g_keys[g_key_head % KEY_QUEUE].key;
    *pressed = g_keys[g_key_head % KEY_QUEUE].pressed;
    g_key_head++;
    return 1;
}

/* ------------------------------------------------------- the rest */

void DG_Init(void) {
    g_window = mos_window_create(0, 80, 60, g_width, g_height, TITLE);
    if (!g_window) {
        printf("Doom: no window (is MOSdesk running?)\n");
        mos_exit_with(1);
    }
    mos_window_set_resizable(g_window, true);
    if (create_gl() != 0) {
        printf("Doom: FarcadeGL is not available\n");
        mos_exit_with(1);
    }
}

void DG_SleepMs(uint32_t ms) {
    pump_events();
    mos_sleep((long)ms);
}

uint32_t DG_GetTicksMs(void) { return (uint32_t)mos_uptime(); }

void DG_SetWindowTitle(const char* title) {
    char t[SYSCALL_WINDOW_MAX_TITLE];
    snprintf(t, sizeof(t), "%s", title && *title ? title : TITLE);
    if (g_window) mos_window_set_title(g_window, t);
}

const char* doom_data_dir(void) { return g_data_dir; }

/* The last lines Doom printed: what an error looks like when Doom was
 * started from the desktop and no terminal shows them. */
static char g_console[512];
static size_t g_console_len;

void doom_console_text(const char* text, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (g_console_len == sizeof(g_console) - 1) {
            memmove(g_console, g_console + 128, g_console_len - 128);
            g_console_len -= 128;
        }
        g_console[g_console_len++] = text[i];
    }
    g_console[g_console_len] = '\0';
}

void doom_platform_exit(int code) {
    if (code != 0 && g_console_len) {
        // Doom stopped with an error (I_Error): keep it in the kernel log.
        mos_log("Doom stopped:");
        char line[160];
        size_t n = 0;
        for (size_t i = 0; i <= g_console_len; i++) {
            char c = g_console[i];
            if (c == '\n' || c == '\0' || n + 1 == sizeof(line)) {
                line[n] = '\0';
                if (n) mos_log(line);
                n = 0;
            } else {
                line[n++] = c;
            }
        }
    }
    destroy_gl();
    if (g_window && g_locked) mos_window_set_pointer_lock(g_window, false);
    if (g_window) mos_window_close(g_window);
    g_window = 0;
}

static int ends_with_wad(const char* s) {
    size_t n = strlen(s);
    return n > 4 && !strcasecmp(s + n - 4, ".wad");
}

int main(int argc, char** argv) {
    // The data folder: next to the program, <dir>/Doom/ for <dir>/Doom.run.
    if (argc > 0 && argv[0] && strlen(argv[0]) + 2 < sizeof(g_data_dir)) {
        const char* slash = strrchr(argv[0], '/');
        size_t dir = slash ? (size_t)(slash - argv[0]) + 1 : 0;
        if (dir) {
            memcpy(g_data_dir, argv[0], dir);
            g_data_dir[dir] = '\0';
            strncat(g_data_dir, "Doom/", sizeof(g_data_dir) - strlen(g_data_dir) - 1);
        }
    }

    static char iwad[320];
    iwad[0] = '\0';
    static char* args[64];
    int n = 0;
    args[n++] = argc > 0 ? argv[0] : "Doom.run";
    for (int i = 1; i < argc && n < 60; i++) {
        if (!iwad[0] && ends_with_wad(argv[i]) && argv[i - 1][0] != '-') snprintf(iwad, sizeof(iwad), "%s", argv[i]);
        else args[n++] = argv[i];
    }
    if (!iwad[0]) {
        static const char* const names[] = {
            "doom.wad", "doomu.wad", "doom1.wad", "freedoom1.wad",
            "doom2.wad", "plutonia.wad", "tnt.wad", "freedoom2.wad",
        };
        for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]) && !iwad[0]; i++) {
            char path[320];
            snprintf(path, sizeof(path), "%s%s", g_data_dir, names[i]);
            if (mos_exists(path) > 0) snprintf(iwad, sizeof(iwad), "%s", path);
        }
    }
    if (!iwad[0]) {
        printf("Doom: no game data. Put doom.wad, doom1.wad or freedoom1.wad in %s\n", g_data_dir);
        return 1;
    }
    args[n++] = "-iwad";
    args[n++] = iwad;
    args[n] = NULL;

    doomgeneric_Create(n, args);
    for (;;) doomgeneric_Tick();
}
