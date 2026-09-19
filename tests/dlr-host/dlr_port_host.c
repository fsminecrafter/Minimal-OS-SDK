/*
 * Host backend for the dlr port layer.
 *
 * This exists so the protocol, tar and install code in programs/dlr/
 * can be run against a real dlr_server on Linux - same sources, same
 * bytes on the wire - instead of being debugged through a QEMU serial
 * log. It is test scaffolding and is never compiled into dlr.run:
 * build.sh compiles every .c under programs/dlr/, which is exactly
 * why this file lives outside it.
 *
 * Files are sandboxed under $DLR_ROOT (default ./sandbox) so a test
 * cannot scribble outside its own directory, and MinimaFS-style
 * "0:/..." paths are mapped into it.
 */

#define _GNU_SOURCE
#include "dlr_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/wait.h>

static char g_root[512];

/* --- path mapping ------------------------------------------------------- */

// "0:/etc/dlr/x" -> "<root>/etc/dlr/x"
static const char* mapped(const char* path, char* out, size_t cap) {
    const char* rel = path;
    if (rel[0] && rel[1] == ':') rel += 2;
    while (*rel == '/') rel++;
    snprintf(out, cap, "%s/%s", g_root, rel);
    return out;
}

static int mkdir_p(const char* path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char* p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return 0;
        *p = '/';
    }
    return (mkdir(tmp, 0755) == 0 || errno == EEXIST);
}

int dlr_port_init(void) {
    const char* root = getenv("DLR_ROOT");
    snprintf(g_root, sizeof(g_root), "%s", root ? root : "./sandbox");
    return mkdir_p(g_root) ? 0 : -1;
}

/* --- time --------------------------------------------------------------- */

uint64_t dlr_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

void dlr_sleep_ms(uint32_t ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* --- TCP ---------------------------------------------------------------- */

long dlr_tcp_open(uint32_t ip, uint16_t port, uint32_t timeout_ms) {
    (void)timeout_ms;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return DLR_INVALID;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(ip);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(fd);
        return DLR_INVALID;
    }
    return fd;
}

int dlr_tcp_write(long handle, const void* buf, uint32_t len) {
    const uint8_t* p = (const uint8_t*)buf;
    uint32_t sent = 0;
    while (sent < len) {
        ssize_t n = send((int)handle, p + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return 0;
        sent += (uint32_t)n;
    }
    return 1;
}

int dlr_tcp_read_exact(long handle, void* buf, uint32_t len, uint32_t idle_timeout_ms) {
    uint8_t* p = (uint8_t*)buf;
    uint32_t got = 0;
    while (got < len) {
        struct pollfd pfd = { (int)handle, POLLIN, 0 };
        int r = poll(&pfd, 1, (int)idle_timeout_ms);
        if (r <= 0) return 0;
        ssize_t n = recv((int)handle, p + got, len - got, 0);
        if (n <= 0) return 0;
        got += (uint32_t)n;
    }
    return 1;
}

void dlr_tcp_close(long handle) {
    if (handle >= 0) close((int)handle);
}

/* --- UDP ---------------------------------------------------------------- */

long dlr_udp_open(uint16_t local_port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return DLR_INVALID;

    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(local_port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(fd);
        return DLR_INVALID;
    }
    return fd;
}

int dlr_udp_broadcast(long handle, uint16_t dst_port, const void* buf, uint32_t len) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(dst_port);
    addr.sin_addr.s_addr = INADDR_BROADCAST;
    return sendto((int)handle, buf, len, 0, (struct sockaddr*)&addr, sizeof(addr)) > 0;
}

long dlr_udp_read(long handle, void* buf, uint32_t len,
                  uint32_t* out_from_ip, uint32_t timeout_ms) {
    struct pollfd pfd = { (int)handle, POLLIN, 0 };
    if (poll(&pfd, 1, (int)timeout_ms) <= 0) return 0;

    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    ssize_t n = recvfrom((int)handle, buf, len, 0, (struct sockaddr*)&from, &from_len);
    if (n <= 0) return 0;
    if (out_from_ip) *out_from_ip = ntohl(from.sin_addr.s_addr);
    return n;
}

void dlr_udp_close(long handle) {
    if (handle >= 0) close((int)handle);
}

uint32_t dlr_resolve(const char* host) {
    struct in_addr addr;
    if (inet_pton(AF_INET, host, &addr) == 1) return ntohl(addr.s_addr);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return 0;

    uint32_t ip = ntohl(((struct sockaddr_in*)res->ai_addr)->sin_addr.s_addr);
    freeaddrinfo(res);
    return ip;
}

/* --- files -------------------------------------------------------------- */

struct dlr_file { FILE* fp; };
static struct dlr_file g_slot;
static int g_slot_used = 0;

dlr_file* dlr_file_create(const char* path) {
    if (g_slot_used) return NULL;
    char real[1024];
    mapped(path, real, sizeof(real));

    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", real);
    char* slash = strrchr(dir, '/');
    if (slash) { *slash = '\0'; mkdir_p(dir); }

    g_slot.fp = fopen(real, "wb");
    if (!g_slot.fp) return NULL;
    g_slot_used = 1;
    return &g_slot;
}

int dlr_file_write(dlr_file* f, const void* buf, uint32_t len) {
    return f && fwrite(buf, 1, len, f->fp) == len;
}

void dlr_file_close(dlr_file* f) {
    if (!f) return;
    fclose(f->fp);
    g_slot_used = 0;
}

long dlr_file_size(const char* path) {
    char real[1024];
    struct stat st;
    if (stat(mapped(path, real, sizeof(real)), &st) != 0) return -1;
    return (long)st.st_size;
}

long dlr_file_slurp(const char* path, void* buf, uint32_t max) {
    char real[1024];
    FILE* fp = fopen(mapped(path, real, sizeof(real)), "rb");
    if (!fp) return -1;
    size_t n = fread(buf, 1, max, fp);
    fclose(fp);
    return (long)n;
}

int dlr_file_put(const char* path, const void* buf, uint32_t len) {
    dlr_file* f = dlr_file_create(path);
    if (!f) return 0;
    int ok = dlr_file_write(f, buf, len);
    dlr_file_close(f);
    return ok;
}

int dlr_mkdirs(const char* path) {
    char real[1024];
    return mkdir_p(mapped(path, real, sizeof(real)));
}

int dlr_exists(const char* path) {
    char real[1024];
    struct stat st;
    return stat(mapped(path, real, sizeof(real)), &st) == 0;
}

int dlr_remove(const char* path) {
    char real[1024];
    return unlink(mapped(path, real, sizeof(real))) == 0;
}

/* --- misc --------------------------------------------------------------- */

void dlr_random_bytes(void* buf, size_t len) {
    FILE* fp = fopen("/dev/urandom", "rb");
    if (!fp || fread(buf, 1, len, fp) != len) {
        fprintf(stderr, "dlr(host): no entropy available\n");
        exit(1);
    }
    fclose(fp);
}

long dlr_exec(const char* path) {
    // Minimal-OS launches a .run bundle here. On the host there is
    // nothing meaningful to launch, and silently "succeeding" would
    // make a test look like it exercised something it did not.
    fprintf(stderr, "dlr(host): exec of '%s' not supported in the host backend\n", path);
    return DLR_INVALID;
}

int dlr_exec_wait(long pid, uint32_t timeout_ms) {
    (void)pid; (void)timeout_ms;
    return 0;
}
