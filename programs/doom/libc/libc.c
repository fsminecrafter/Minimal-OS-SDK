/*
 * Doom for Minimal-OS: a small C library for the engine, on top of the
 * Minimal-OS SDK (files through SYS_OPEN/SYS_READ/SYS_FWRITE, memory
 * through SYS_HEAP). Only what Doom needs, but complete enough for it:
 * printf-style formatting with widths and precisions, scanf with
 * character sets, stdio files with seeking.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <assert.h>
#include "minimalos.h"

int errno;

/* ------------------------------------------------------------- strings */

char* strncpy(char* dst, const char* src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

char* strcat(char* dst, const char* src) {
    strcpy(dst + strlen(dst), src);
    return dst;
}

char* strncat(char* dst, const char* src, size_t n) {
    char* d = dst + strlen(dst);
    size_t i = 0;
    for (; i < n && src[i]; i++) d[i] = src[i];
    d[i] = '\0';
    return dst;
}

char* strndup(const char* s, size_t n) {
    size_t len = 0;
    while (len < n && s[len]) len++;
    char* out = malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

char* strdup(const char* s) { return strndup(s, (size_t)-1); }

char* strerror(int err) {
    switch (err) {
        case ENOENT: return "No such file or directory";
        case EEXIST: return "File exists";
        case ENOMEM: return "Out of memory";
        case EINVAL: return "Invalid argument";
        default: return "I/O error";
    }
}

size_t strspn(const char* s, const char* accept) {
    size_t n = 0;
    while (s[n] && strchr(accept, s[n])) n++;
    return n;
}

size_t strcspn(const char* s, const char* reject) {
    size_t n = 0;
    while (s[n] && !strchr(reject, s[n])) n++;
    return n;
}

int strncasecmp(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int x = tolower((unsigned char)a[i]), y = tolower((unsigned char)b[i]);
        if (x != y) return x - y;
        if (!x) return 0;
    }
    return 0;
}

int strcasecmp(const char* a, const char* b) { return strncasecmp(a, b, (size_t)-1); }

void* memchr(const void* s, int c, size_t n) {
    const unsigned char* p = s;
    for (size_t i = 0; i < n; i++)
        if (p[i] == (unsigned char)c) return (void*)(p + i);
    return NULL;
}

/* -------------------------------------------------------------- numbers */

unsigned long strtoul(const char* s, char** end, int base) {
    const char* p = s;
    while (isspace((unsigned char)*p)) p++;
    bool neg = false;
    if (*p == '+' || *p == '-') neg = *p++ == '-';
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2])) {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = p[0] == '0' ? 8 : 10;
    }
    unsigned long v = 0;
    const char* start = p;
    for (;; p++) {
        int c = (unsigned char)*p, d;
        if (isdigit(c)) d = c - '0';
        else if (isalpha(c)) d = tolower(c) - 'a' + 10;
        else break;
        if (d >= base) break;
        v = v * (unsigned long)base + (unsigned long)d;
    }
    if (end) *end = (char*)(p == start ? s : p);
    return neg ? (unsigned long)-(long)v : v;
}

long strtol(const char* s, char** end, int base) { return (long)strtoul(s, end, base); }

double strtod(const char* s, char** end) {
    const char* p = s;
    while (isspace((unsigned char)*p)) p++;
    double sign = 1.0, v = 0.0;
    if (*p == '+' || *p == '-') sign = *p++ == '-' ? -1.0 : 1.0;
    const char* start = p;
    while (isdigit((unsigned char)*p)) v = v * 10.0 + (*p++ - '0');
    if (*p == '.') {
        double scale = 0.1;
        for (p++; isdigit((unsigned char)*p); p++, scale *= 0.1) v += (*p - '0') * scale;
    }
    if (p != start && (*p == 'e' || *p == 'E')) {
        char* q;
        long e = strtol(p + 1, &q, 10);
        if (q != p + 1) {
            p = q;
            for (; e > 0; e--) v *= 10.0;
            for (; e < 0; e++) v *= 0.1;
        }
    }
    if (end) *end = (char*)(p == start ? s : p);
    return sign * v;
}

double atof(const char* s) { return strtod(s, NULL); }

static unsigned long g_rand = 1;
int rand(void) {
    g_rand = g_rand * 1103515245u + 12345u;
    return (int)((g_rand >> 16) & RAND_MAX);
}
void srand(unsigned seed) { g_rand = seed; }

void qsort(void* base, size_t count, size_t size, int (*cmp)(const void*, const void*)) {
    /* Shell sort: small arrays only in Doom. */
    unsigned char* b = base;
    unsigned char tmp[256];
    if (size > sizeof(tmp)) return;
    for (size_t gap = count / 2; gap > 0; gap /= 2) {
        for (size_t i = gap; i < count; i++) {
            memcpy(tmp, b + i * size, size);
            size_t j = i;
            while (j >= gap && cmp(b + (j - gap) * size, tmp) > 0) {
                memcpy(b + j * size, b + (j - gap) * size, size);
                j -= gap;
            }
            memcpy(b + j * size, tmp, size);
        }
    }
}

/* ------------------------------------------------------------- process */

#define MAX_ATEXIT 16
static void (*g_atexit[MAX_ATEXIT])(void);
static int g_atexit_count;

int atexit(void (*fn)(void)) {
    if (g_atexit_count >= MAX_ATEXIT) return -1;
    g_atexit[g_atexit_count++] = fn;
    return 0;
}

void doom_platform_exit(int code);     /* main.c: closes the window */

void exit(int code) {
    while (g_atexit_count > 0) g_atexit[--g_atexit_count]();
    fflush(stdout);
    fflush(stderr);
    doom_platform_exit(code);
    mos_exit_with(code);
}

void abort(void) { exit(134); }

char* getenv(const char* name) { (void)name; return NULL; }
int system(const char* command) { (void)command; return -1; }

void doom_assert_failed(const char* expr, const char* file, int line) {
    fprintf(stderr, "assertion failed: %s (%s:%d)\n", expr, file, line);
    abort();
}

/* ----------------------------------------------------------------- math */

double fabs(double x) { return x < 0 ? -x : x; }

double floor(double x) {
    if (x >= 9.2e18 || x <= -9.2e18 || x != x) return x;
    long long i = (long long)x;
    double f = (double)i;
    return f > x ? f - 1.0 : f;
}

double ceil(double x) { return -floor(-x); }

double sqrt(double x) {
    double r;
    __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
    return r;
}

double fmod(double x, double y) {
    if (y == 0.0) return 0.0;
    double q = x / y;
    q = q < 0 ? ceil(q) : floor(q);
    return x - q * y;
}

double exp(double x) {
    /* e^x = 2^k * e^r, |r| <= ln2/2 */
    if (x > 700) return 1e308;
    if (x < -700) return 0.0;
    double k = floor(x / 0.6931471805599453 + 0.5);
    double r = x - k * 0.6931471805599453, term = 1.0, sum = 1.0;
    for (int n = 1; n < 20; n++) { term *= r / n; sum += term; }
    long long ik = (long long)k;
    while (ik > 0) { sum *= 2.0; ik--; }
    while (ik < 0) { sum *= 0.5; ik++; }
    return sum;
}

double log(double x) {
    if (x <= 0.0) return -1e308;
    int k = 0;
    while (x > 2.0) { x *= 0.5; k++; }
    while (x < 1.0) { x *= 2.0; k--; }
    /* ln(x) = 2 atanh((x-1)/(x+1)) */
    double y = (x - 1.0) / (x + 1.0), y2 = y * y, term = y, sum = 0.0;
    for (int n = 1; n < 40; n += 2) { sum += term / n; term *= y2; }
    return 2.0 * sum + k * 0.6931471805599453;
}

double pow(double x, double y) {
    if (y == 0.0) return 1.0;
    if (x == 0.0) return 0.0;
    double iy = floor(y);
    if (iy == y && fabs(y) < 64) {
        double r = 1.0, b = x;
        long long n = (long long)fabs(y);
        while (n) { if (n & 1) r *= b; b *= b; n >>= 1; }
        return y < 0 ? 1.0 / r : r;
    }
    if (x < 0.0) return 0.0;
    return exp(y * log(x));
}

double sin(double x) {
    x = fmod(x, 2 * M_PI);
    if (x > M_PI) x -= 2 * M_PI;
    if (x < -M_PI) x += 2 * M_PI;
    double term = x, sum = x, x2 = x * x;
    for (int n = 1; n < 12; n++) { term *= -x2 / ((2 * n) * (2 * n + 1)); sum += term; }
    return sum;
}

double cos(double x) { return sin(x + M_PI / 2); }
double tan(double x) { double c = cos(x); return c == 0.0 ? 1e308 : sin(x) / c; }

double atan(double x) {
    if (x > 1.0) return M_PI / 2 - atan(1.0 / x);
    if (x < -1.0) return -M_PI / 2 - atan(1.0 / x);
    /* atan(x) = 2 atan(x / (1 + sqrt(1 + x^2))) brings |x| below 0.42 */
    double t = x / (1.0 + sqrt(1.0 + x * x));
    double t2 = t * t, term = t, sum = 0.0;
    for (int n = 1; n < 40; n += 2) { sum += term / n; term *= -t2; }
    return 2.0 * sum;
}

double atan2(double y, double x) {
    if (x > 0) return atan(y / x);
    if (x < 0) return y >= 0 ? atan(y / x) + M_PI : atan(y / x) - M_PI;
    return y > 0 ? M_PI / 2 : (y < 0 ? -M_PI / 2 : 0.0);
}

/* --------------------------------------------------------------- format */

typedef struct {
    char* out;
    size_t size, len;
    FILE* file;
} sink_t;

static void put(sink_t* s, char c) {
    if (s->file) { fputc(c, s->file); s->len++; return; }
    if (s->len + 1 < s->size) s->out[s->len] = c;
    s->len++;
}

static void put_padded(sink_t* s, const char* text, size_t len, int width, bool left, char pad,
                       const char* prefix) {
    size_t plen = prefix ? strlen(prefix) : 0;
    int fill = width - (int)(len + plen);
    if (pad == '0' && !left) {
        for (size_t i = 0; i < plen; i++) put(s, prefix[i]);
        for (; fill > 0; fill--) put(s, '0');
    } else {
        if (!left) for (; fill > 0; fill--) put(s, ' ');
        for (size_t i = 0; i < plen; i++) put(s, prefix[i]);
    }
    for (size_t i = 0; i < len; i++) put(s, text[i]);
    if (left) for (; fill > 0; fill--) put(s, ' ');
}

static int format(sink_t* s, const char* fmt, va_list ap) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') { put(s, *fmt); continue; }
        fmt++;
        bool left = false, plus = false, space = false, alt = false;
        char pad = ' ';
        for (;; fmt++) {
            if (*fmt == '-') left = true;
            else if (*fmt == '+') plus = true;
            else if (*fmt == ' ') space = true;
            else if (*fmt == '#') alt = true;
            else if (*fmt == '0') pad = '0';
            else break;
        }
        int width = 0, prec = -1;
        if (*fmt == '*') { width = va_arg(ap, int); if (width < 0) { left = true; width = -width; } fmt++; }
        else while (isdigit((unsigned char)*fmt)) width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (isdigit((unsigned char)*fmt)) prec = prec * 10 + (*fmt++ - '0');
        }
        int size = 0;           /* -2 hh, -1 h, 1 l, 2 ll, 3 z */
        for (;; fmt++) {
            if (*fmt == 'h') size = size == -1 ? -2 : -1;
            else if (*fmt == 'l') size = size == 1 ? 2 : 1;
            else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't') size = 3;
            else break;
        }
        char c = *fmt, buf[72];
        if (!c) break;
        if (c == '%') { put(s, '%'); continue; }
        if (c == 'c') { buf[0] = (char)va_arg(ap, int); put_padded(s, buf, 1, width, left, ' ', NULL); continue; }
        if (c == 's') {
            const char* t = va_arg(ap, const char*);
            if (!t) t = "(null)";
            size_t len = 0;
            while (t[len] && (prec < 0 || (int)len < prec)) len++;
            put_padded(s, t, len, width, left, ' ', NULL);
            continue;
        }
        if (c == 'f' || c == 'g' || c == 'e') {
            double v = va_arg(ap, double);
            if (prec < 0) prec = 6;
            if (prec > 15) prec = 15;
            const char* sign = v < 0 ? "-" : (plus ? "+" : (space ? " " : NULL));
            if (v < 0) v = -v;
            double scale = 1.0;
            for (int i = 0; i < prec; i++) scale *= 10.0;
            unsigned long long whole = (unsigned long long)v;
            unsigned long long frac = (unsigned long long)((v - (double)whole) * scale + 0.5);
            if ((double)frac >= scale) { whole++; frac = 0; }
            int n = 0;
            char tmp[48];
            do { tmp[n++] = (char)('0' + whole % 10); whole /= 10; } while (whole && n < 30);
            int len = 0;
            while (n) buf[len++] = tmp[--n];
            if (prec > 0) {
                buf[len++] = '.';
                for (int i = prec - 1; i >= 0; i--) { buf[len + i] = (char)('0' + frac % 10); frac /= 10; }
                len += prec;
            }
            put_padded(s, buf, (size_t)len, width, left, pad, sign);
            continue;
        }
        unsigned long long u;
        bool neg = false;
        int base = 10;
        if (c == 'd' || c == 'i') {
            long long v = size >= 2 ? va_arg(ap, long long) : size == 1 || size == 3 ? va_arg(ap, long) : va_arg(ap, int);
            if (size == -1) v = (short)v;
            if (size == -2) v = (signed char)v;
            neg = v < 0;
            u = neg ? (unsigned long long)-v : (unsigned long long)v;
        } else if (c == 'u' || c == 'x' || c == 'X' || c == 'o' || c == 'p') {
            if (c == 'p') { u = (uintptr_t)va_arg(ap, void*); alt = true; }
            else u = size >= 2 ? va_arg(ap, unsigned long long) : size == 1 || size == 3 ? va_arg(ap, unsigned long)
                                                                   : va_arg(ap, unsigned int);
            if (size == -1) u = (unsigned short)u;
            if (size == -2) u = (unsigned char)u;
            base = c == 'o' ? 8 : (c == 'u' ? 10 : 16);
        } else {
            put(s, '%');
            put(s, c);
            continue;
        }
        const char* digits = c == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
        char tmp[40];
        int n = 0;
        if (u || prec != 0) do { tmp[n++] = digits[u % (unsigned)base]; u /= (unsigned)base; } while (u);
        int len = 0;
        for (int i = n; i < prec; i++) buf[len++] = '0';
        while (n) buf[len++] = tmp[--n];
        const char* prefix = neg ? "-" : (plus && base == 10 && c != 'u') ? "+" :
                             (space && base == 10 && c != 'u') ? " " : NULL;
        if (alt && base == 16) prefix = c == 'X' ? "0X" : "0x";
        if (prec >= 0) pad = ' ';
        put_padded(s, buf, (size_t)len, width, left, pad, prefix);
    }
    if (!s->file && s->size) s->out[s->len < s->size ? s->len : s->size - 1] = '\0';
    return (int)s->len;
}

int vsnprintf(char* out, size_t size, const char* fmt, va_list ap) {
    sink_t s = { out, size, 0, NULL };
    va_list copy;
    va_copy(copy, ap);
    int n = format(&s, fmt, copy);
    va_end(copy);
    return n;
}

int snprintf(char* out, size_t size, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, size, fmt, ap);
    va_end(ap);
    return n;
}

int vsprintf(char* out, const char* fmt, va_list ap) { return vsnprintf(out, (size_t)1 << 30, fmt, ap); }

int sprintf(char* out, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsprintf(out, fmt, ap);
    va_end(ap);
    return n;
}

int vfprintf(FILE* f, const char* fmt, va_list ap) {
    sink_t s = { NULL, 0, 0, f };
    va_list copy;
    va_copy(copy, ap);
    int n = format(&s, fmt, copy);
    va_end(copy);
    return n;
}

int fprintf(FILE* f, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int doom_vprintf(const char* fmt, va_list ap) { return vfprintf(stdout, fmt, ap); }

int doom_printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return n;
}

/* ---------------------------------------------------------------- files */

#define F_READ  1
#define F_WRITE 2
#define F_EOF   4
#define F_ERR   8
#define F_CONSOLE 16

struct doom_file {
    long fd;
    int flags;
    long pos;
    int unget;              /* a character pushed back, -1 = none */
    char buf[4096];         /* console: one line at a time; files: writes not yet made */
    size_t buf_len;
};

static FILE g_stdin = { 0, F_READ | F_CONSOLE, 0, -1, {0}, 0 };
static FILE g_stdout = { 1, F_WRITE | F_CONSOLE, 0, -1, {0}, 0 };
static FILE g_stderr = { 2, F_WRITE | F_CONSOLE, 0, -1, {0}, 0 };
FILE* stdin = &g_stdin;
FILE* stdout = &g_stdout;
FILE* stderr = &g_stderr;

void doom_console_text(const char* text, size_t len);     /* main.c: keeps the last lines for errors */

FILE* fopen(const char* path, const char* mode) {
    bool write = mode[0] == 'w' || mode[0] == 'a' || strchr(mode, '+');
    if (mode[0] == 'w') {
        if (mos_exists(path) > 0) mos_delete(path);
        if (mos_create(path, NULL, NULL) < 0) { errno = EIO; return NULL; }
    } else if (mode[0] == 'a' && mos_exists(path) <= 0) {
        if (mos_create(path, NULL, NULL) < 0) { errno = EIO; return NULL; }
    }
    long fd = mos_open(path, write ? SYS_O_RDWR : SYS_O_RDONLY);
    if (fd < 0) { errno = ENOENT; return NULL; }
    FILE* f = calloc(1, sizeof(FILE));
    if (!f) { mos_close(fd); errno = ENOMEM; return NULL; }
    f->fd = fd;
    f->flags = (mode[0] == 'r' || strchr(mode, '+') ? F_READ : 0) | (write ? F_WRITE : 0);
    f->unget = -1;
    if (mode[0] == 'a') {
        long size = mos_size(fd);
        if (size > 0) { mos_seek(fd, size); f->pos = size; }
    }
    return f;
}

/* File writes are gathered in f->buf and written in one go: each
 * mos_fwrite is a disk write, and printf hands over one character at a
 * time (saving the config on quit was thousands of writes). */
static int flush_writes(FILE* f) {
    if ((f->flags & F_CONSOLE) || !f->buf_len) return 0;
    long n = mos_fwrite(f->fd, f->buf, f->buf_len);
    f->buf_len = 0;
    if (n < 0) { f->flags |= F_ERR; return EOF; }
    return 0;
}

int fflush(FILE* f) {
    if (!f) { fflush(stdout); fflush(stderr); return 0; }
    if (!(f->flags & F_CONSOLE)) return flush_writes(f);
    if ((f->flags & F_CONSOLE) && f->buf_len) {
        mos_write(f == stderr ? 2 : 1, f->buf, f->buf_len);
        doom_console_text(f->buf, f->buf_len);
        f->buf_len = 0;
    }
    return 0;
}

int fclose(FILE* f) {
    if (!f) return EOF;
    fflush(f);
    if (f->flags & F_CONSOLE) return 0;
    int r = flush_writes(f);
    mos_close(f->fd);
    free(f);
    return r;
}

size_t fread(void* buf, size_t size, size_t count, FILE* f) {
    size_t want = size * count, got = 0;
    if (!f || !want || !(f->flags & F_READ) || (f->flags & F_CONSOLE)) return 0;
    unsigned char* out = buf;
    flush_writes(f);
    if (f->unget >= 0) { out[got++] = (unsigned char)f->unget; f->unget = -1; f->pos++; }
    while (got < want) {
        long n = mos_read(f->fd, out + got, want - got);
        if (n < 0) { f->flags |= F_ERR; break; }
        if (n == 0) { f->flags |= F_EOF; break; }
        got += (size_t)n;
        f->pos += n;
    }
    return size ? got / size : 0;
}

size_t fwrite(const void* buf, size_t size, size_t count, FILE* f) {
    size_t want = size * count;
    if (!f || !want || !(f->flags & F_WRITE)) return 0;
    if (f->flags & F_CONSOLE) {
        const char* p = buf;
        for (size_t i = 0; i < want; i++) fputc(p[i], f);
        return count;
    }
    if (f->buf_len + want <= sizeof(f->buf)) {
        memcpy(f->buf + f->buf_len, buf, want);
        f->buf_len += want;
        f->pos += (long)want;
        if (f->buf_len == sizeof(f->buf) && flush_writes(f) == EOF) return 0;
        return count;
    }
    if (flush_writes(f) == EOF) return 0;
    long n = mos_fwrite(f->fd, buf, want);
    if (n < 0) { f->flags |= F_ERR; return 0; }
    f->pos += n;
    return size ? (size_t)n / size : 0;
}

int fseek(FILE* f, long offset, int whence) {
    if (!f || (f->flags & F_CONSOLE)) return -1;
    if (flush_writes(f) == EOF) return -1;     /* before SEEK_END reads the size */
    long target = offset;
    if (whence == SEEK_CUR) target = f->pos + offset;
    else if (whence == SEEK_END) target = mos_size(f->fd) + offset;
    if (target < 0 || mos_seek(f->fd, target) < 0) return -1;
    f->pos = target;
    f->unget = -1;
    f->flags &= ~F_EOF;
    return 0;
}

long ftell(FILE* f) { return f ? f->pos : -1; }
int feof(FILE* f) { return f && (f->flags & F_EOF); }
int ferror(FILE* f) { return f && (f->flags & F_ERR); }

int fgetc(FILE* f) {
    unsigned char c;
    return fread(&c, 1, 1, f) == 1 ? c : EOF;
}

static int ungetc_(int c, FILE* f) {
    if (c == EOF || !f) return EOF;
    f->unget = c;
    f->pos--;
    f->flags &= ~F_EOF;
    return c;
}

char* fgets(char* s, int n, FILE* f) {
    int i = 0;
    while (i + 1 < n) {
        int c = fgetc(f);
        if (c == EOF) break;
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    if (!i) return NULL;
    s[i] = '\0';
    return s;
}

int fputc(int c, FILE* f) {
    if (!f || !(f->flags & F_WRITE)) return EOF;
    if (f->flags & F_CONSOLE) {
        f->buf[f->buf_len++] = (char)c;
        if (c == '\n' || f->buf_len == sizeof(f->buf)) fflush(f);
        return c;
    }
    unsigned char b = (unsigned char)c;
    return fwrite(&b, 1, 1, f) == 1 ? c : EOF;
}

int fputs(const char* s, FILE* f) {
    while (*s) if (fputc(*s++, f) == EOF) return EOF;
    return 0;
}

int puts(const char* s) { fputs(s, stdout); return fputc('\n', stdout) == EOF ? EOF : 0; }
int putchar(int c) { return fputc(c, stdout); }

int remove(const char* path) { return mos_delete(path) < 0 ? -1 : 0; }

/* No rename in MinimaFS: copy, then delete. Used for savegames. */
int rename(const char* from, const char* to) {
    FILE* in = fopen(from, "rb");
    if (!in) return -1;
    FILE* out = fopen(to, "wb");
    if (!out) { fclose(in); return -1; }
    char buf[4096];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
    fclose(in);
    fclose(out);
    if (!ok) return -1;
    remove(from);
    return 0;
}

int access(const char* path, int mode) { (void)mode; return mos_exists(path) > 0 ? 0 : -1; }
int unlink(const char* path) { return remove(path); }
int isatty(int fd) { (void)fd; return 0; }
unsigned sleep(unsigned seconds) { mos_sleep((long)seconds * 1000); return 0; }
int usleep(unsigned usec) { mos_sleep((long)(usec / 1000)); return 0; }

int stat(const char* path, struct stat* st) {
    if (mos_exists(path) <= 0) { errno = ENOENT; return -1; }
    st->st_mode = mos_is_dir(path) > 0 ? S_IFDIR : S_IFREG;
    st->st_size = 0;
    if (!S_ISDIR(st->st_mode)) {
        long fd = mos_open(path, SYS_O_RDONLY);
        if (fd >= 0) { st->st_size = mos_size(fd); mos_close(fd); }
    }
    return 0;
}

int mkdir(const char* path, mode_t mode) {
    (void)mode;
    if (mos_is_dir(path) > 0) { errno = EEXIST; return -1; }
    return mos_mkdir(path) < 0 ? -1 : 0;
}

int gettimeofday(struct timeval* tv, void* tz) {
    (void)tz;
    long ms = mos_uptime();
    tv->tv_sec = ms / 1000;
    tv->tv_usec = (ms % 1000) * 1000;
    return 0;
}

/* ----------------------------------------------------------------- scan */

typedef struct {
    const char* str;        /* string source, or */
    FILE* file;             /* file source */
    int count;              /* characters consumed */
} source_t;

static int peek(source_t* s) {
    if (s->str) return *s->str ? (unsigned char)*s->str : EOF;
    int c = fgetc(s->file);
    if (c != EOF) ungetc_(c, s->file);
    return c;
}

static void advance(source_t* s) {
    if (s->str) { if (*s->str) s->str++; }
    else fgetc(s->file);
    s->count++;
}

static int scan(source_t* s, const char* fmt, va_list ap) {
    int assigned = 0;
    for (; *fmt; fmt++) {
        if (isspace((unsigned char)*fmt)) {
            while (isspace(peek(s))) advance(s);
            continue;
        }
        if (*fmt != '%' || fmt[1] == '%') {
            if (*fmt == '%') fmt++;
            if (peek(s) != (unsigned char)*fmt) return assigned ? assigned : (peek(s) == EOF ? EOF : 0);
            advance(s);
            continue;
        }
        fmt++;
        bool skip = false;
        if (*fmt == '*') { skip = true; fmt++; }
        int width = 0;
        while (isdigit((unsigned char)*fmt)) width = width * 10 + (*fmt++ - '0');
        int size = 0;
        while (*fmt == 'h' || *fmt == 'l') { size += *fmt == 'l' ? 1 : -1; fmt++; }
        char c = *fmt;
        if (c == 'n') { if (!skip) *va_arg(ap, int*) = s->count; continue; }
        if (c != 'c' && c != '[') while (isspace(peek(s))) advance(s);
        if (peek(s) == EOF) return assigned ? assigned : EOF;
        if (c == 's' || c == 'c' || c == '[') {
            bool set[256] = { false }, invert = false;
            if (c == '[') {
                fmt++;
                if (*fmt == '^') { invert = true; fmt++; }
                if (*fmt == ']') { set[']'] = true; fmt++; }
                for (; *fmt && *fmt != ']'; fmt++) {
                    if (fmt[1] == '-' && fmt[2] && fmt[2] != ']') {
                        for (int k = (unsigned char)fmt[0]; k <= (unsigned char)fmt[2]; k++) set[k] = true;
                        fmt += 2;
                    } else {
                        set[(unsigned char)*fmt] = true;
                    }
                }
            }
            if (!width) width = c == 'c' ? 1 : 0x7FFFFFFF;
            char* out = skip ? NULL : va_arg(ap, char*);
            int n = 0;
            for (; n < width; n++) {
                int ch = peek(s);
                if (ch == EOF) break;
                if (c == 's' && isspace(ch)) break;
                if (c == '[' && set[ch] == invert) break;
                if (out) out[n] = (char)ch;
                advance(s);
            }
            if (!n) return assigned;
            if (out && c != 'c') out[n] = '\0';
            if (out) assigned++;
            continue;
        }
        /* Numbers: read the characters, then convert. */
        char num[64];
        int n = 0, base = c == 'x' || c == 'X' ? 16 : (c == 'o' ? 8 : (c == 'i' ? 0 : 10));
        if (!width || width > 63) width = 63;
        int ch = peek(s);
        if (ch == '+' || ch == '-') { num[n++] = (char)ch; advance(s); }
        while (n < width) {
            ch = peek(s);
            bool ok = isdigit(ch) || (base != 10 && base != 8 && isxdigit(ch)) ||
                      ((base == 0 || base == 16) && (ch == 'x' || ch == 'X') && n > 0 && num[n - 1] == '0') ||
                      (c == 'f' && (ch == '.' || ch == 'e' || ch == 'E'));
            if (!ok || ch == EOF) break;
            if (base == 8 && ch > '7') break;
            num[n++] = (char)ch;
            advance(s);
        }
        num[n] = '\0';
        if (!n || (n == 1 && (num[0] == '+' || num[0] == '-'))) return assigned;
        if (skip) continue;
        if (c == 'f' || c == 'g' || c == 'e') {
            double v = strtod(num, NULL);
            if (size > 0) *va_arg(ap, double*) = v;
            else *va_arg(ap, float*) = (float)v;
        } else {
            long v = strtol(num, NULL, base);
            if (size > 0) *va_arg(ap, long*) = v;
            else if (size < 0) *va_arg(ap, short*) = (short)v;
            else *va_arg(ap, int*) = (int)v;
        }
        assigned++;
    }
    return assigned;
}

int sscanf(const char* str, const char* fmt, ...) {
    source_t s = { str, NULL, 0 };
    va_list ap;
    va_start(ap, fmt);
    int n = scan(&s, fmt, ap);
    va_end(ap);
    return n;
}

int fscanf(FILE* f, const char* fmt, ...);
int fscanf(FILE* f, const char* fmt, ...) {
    source_t s = { NULL, f, 0 };
    va_list ap;
    va_start(ap, fmt);
    int n = scan(&s, fmt, ap);
    va_end(ap);
    return n;
}
