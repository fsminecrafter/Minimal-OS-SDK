/*
 * Host test for the Doom port: the real engine (src/) and the port's C
 * library (libc/) as a freestanding Linux program, in place of the
 * Minimal-OS platform layer (main.c). No host C library at all: files,
 * memory and output go through raw Linux system calls (sdk/minimalos.h).
 *
 * The clock is simulated (each sleep advances it), keys come from a
 * script, and frames are written as PPM pictures. One run:
 *   title -> menu -> New Game -> episode 1 -> skill 3 -> walk forward and
 *   fire -> open the automap -> quit (which saves the config)
 * The second run (argument "again") starts with that config and quits.
 *
 * Usage: doom_host <iwad> <work dir> [again]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "doomgeneric.h"
#include "doomkeys.h"

void I_Quit(void);

/* ---------------------------------------------------- memory (no libc) */

void* memcpy(void* d, const void* s, size_t n) { unsigned char* a = d; const unsigned char* b = s; while (n--) *a++ = *b++; return d; }
void* memmove(void* d, const void* s, size_t n) {
    unsigned char* a = d; const unsigned char* b = s;
    if (a < b) while (n--) *a++ = *b++;
    else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}
void* memset(void* d, int v, size_t n) { unsigned char* a = d; while (n--) *a++ = (unsigned char)v; return d; }
int memcmp(const void* x, const void* y, size_t n) {
    const unsigned char* a = x; const unsigned char* b = y;
    for (; n; n--, a++, b++) if (*a != *b) return *a - *b;
    return 0;
}

static unsigned char* g_heap;
static size_t g_heap_used, g_heap_size = (size_t)512 << 20;

void* malloc(size_t size) {
    size = (size + 15) & ~(size_t)15;
    if (!g_heap || g_heap_used + size + 16 > g_heap_size) return NULL;
    unsigned char* p = g_heap + g_heap_used;
    *(size_t*)p = size;
    g_heap_used += size + 16;
    return p + 16;
}
void* calloc(size_t c, size_t s) { void* p = malloc(c * s); if (p) memset(p, 0, c * s); return p; }
void free(void* p) { (void)p; }
void* realloc(void* p, size_t size) {
    if (!p) return malloc(size);
    size_t old = *(size_t*)((unsigned char*)p - 16);
    if (old >= size) return p;
    void* q = malloc(size);
    if (q) memcpy(q, p, old);
    return q;
}

static long mmap6(long len) {
    register long r10 __asm__("r10") = 0x22;     /* MAP_PRIVATE | MAP_ANONYMOUS */
    register long r8 __asm__("r8") = -1;
    register long r9 __asm__("r9") = 0;
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(9L), "D"(0L), "S"(len), "d"(3L), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return r;
}

/* ------------------------------------------------------------ platform */

static long g_now;          /* simulated milliseconds */
long test_uptime_ms(void) { return g_now; }
uint32_t DG_GetTicksMs(void) { return (uint32_t)g_now; }
void DG_SleepMs(uint32_t ms) { g_now += ms ? ms : 1; }
void DG_Init(void) {}
void DG_SetWindowTitle(const char* t) { printf("title: %s\n", t); }

static char g_dir[256];
const char* doom_data_dir(void) { return g_dir; }
void doom_console_text(const char* t, size_t n) { (void)t; (void)n; }
void doom_platform_exit(int code) { printf("exit code %d after %ld simulated ms\n", code, g_now); }

static int g_again;
typedef struct { long at; unsigned char key; int pressed; } step_t;
static const step_t k_script[] = {
    { 3000, KEY_ESCAPE, 1 }, { 3050, KEY_ESCAPE, 0 },       /* menu */
    { 3600, KEY_ENTER, 1 }, { 3650, KEY_ENTER, 0 },         /* New Game */
    { 4200, KEY_ENTER, 1 }, { 4250, KEY_ENTER, 0 },         /* episode 1 */
    { 4800, KEY_ENTER, 1 }, { 4850, KEY_ENTER, 0 },         /* skill */
    { 6500, KEY_UPARROW, 1 }, { 8000, KEY_UPARROW, 0 },     /* walk */
    { 8100, KEY_RIGHTARROW, 1 }, { 8400, KEY_RIGHTARROW, 0 },
    { 8500, KEY_FIRE, 1 }, { 8800, KEY_FIRE, 0 },           /* fire */
    { 9500, KEY_TAB, 1 }, { 9550, KEY_TAB, 0 },             /* automap */
};
static unsigned g_step;

int DG_GetKey(int* pressed, unsigned char* key) {
    if (g_again || g_step >= sizeof(k_script) / sizeof(k_script[0]) || k_script[g_step].at > g_now) return 0;
    *key = k_script[g_step].key;
    *pressed = k_script[g_step].pressed;
    g_step++;
    return 1;
}

static void shot(const char* name) {
    char path[320];
    snprintf(path, sizeof(path), "%s%s.ppm", g_dir, name);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", DOOMGENERIC_RESX, DOOMGENERIC_RESY);
    static unsigned char row[DOOMGENERIC_RESX * 3];
    for (int y = 0; y < DOOMGENERIC_RESY; y++) {
        for (int x = 0; x < DOOMGENERIC_RESX; x++) {
            uint32_t p = DG_ScreenBuffer[y * DOOMGENERIC_RESX + x];
            row[x * 3] = (unsigned char)(p >> 16);
            row[x * 3 + 1] = (unsigned char)(p >> 8);
            row[x * 3 + 2] = (unsigned char)p;
        }
        fwrite(row, 1, sizeof(row), f);
    }
    fclose(f);
    printf("frame %s at %ld ms\n", name, g_now);
}

static unsigned g_frames;
void DG_DrawFrame(void) {
    g_frames++;
    static const struct { long at; const char* name; } shots[] = {
        { 2000, "1-title" }, { 3300, "2-menu" }, { 4500, "3-skill" }, { 7900, "4-walking" },
        { 8700, "5-firing" }, { 9900, "6-automap" },
    };
    static unsigned next;
    if (!g_again && next < sizeof(shots) / sizeof(shots[0]) && g_now >= shots[next].at) shot(shots[next++].name);
    long end = g_again ? 1500 : 10500;
    if (g_now >= end) {
        printf("%u frames drawn\n", g_frames);
        I_Quit();
    }
}

__attribute__((force_align_arg_pointer)) void _start(void) {
    g_heap = (unsigned char*)mmap6((long)g_heap_size);
    /* The arguments, from /proc/self/cmdline. */
    static char cmd[4096];
    long fd = mos_open("/proc/self/cmdline", 0), n = fd >= 0 ? mos_read(fd, cmd, sizeof(cmd) - 1) : 0;
    static char* argv[8];
    int argc = 0;
    for (long i = 0; i < n && argc < 8; i += (long)strlen(cmd + i) + 1) argv[argc++] = cmd + i;
    if (argc < 3) { printf("usage: doom_host <iwad> <work dir> [again]\n"); exit(2); }
    snprintf(g_dir, sizeof(g_dir), "%s/", argv[2]);
    g_again = argc > 3;
    static char* args[] = { "doom", "-iwad", NULL, NULL };
    args[2] = argv[1];
    doomgeneric_Create(3, args);
    for (;;) doomgeneric_Tick();
}
