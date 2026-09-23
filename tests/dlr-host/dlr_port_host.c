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
#include <dirent.h>
#include <signal.h>

static char g_root[512];

/* --- path mapping ------------------------------------------------------- */

// "0:/etc/dlr/x" -> "<root>/etc/dlr/x"
static const char* mapped(const char* path, char* out, size_t cap) {
    // Store management (`present`, `packages`, ...) runs before
    // dlr_port_init() - on Minimal-OS it is pure disk access and needs
    // no network - so the root cannot depend on init having happened.
    if (!g_root[0]) {
        const char* root = getenv("DLR_ROOT");
        snprintf(g_root, sizeof(g_root), "%s", root ? root : "./sandbox");
    }
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

struct dlr_file { FILE* fp; int used; };
#define HOST_MAX_OPEN 4
static struct dlr_file g_files[HOST_MAX_OPEN];

static struct dlr_file* slot_take(void) {
    for (int i = 0; i < HOST_MAX_OPEN; i++) {
        if (!g_files[i].used) { g_files[i].used = 1; return &g_files[i]; }
    }
    return NULL;
}

dlr_file* dlr_file_create(const char* path) {
    struct dlr_file* slot = slot_take();
    if (!slot) return NULL;
    char real[1024];
    mapped(path, real, sizeof(real));

    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", real);
    char* slash = strrchr(dir, '/');
    if (slash) { *slash = '\0'; mkdir_p(dir); }

    slot->fp = fopen(real, "wb");
    if (!slot->fp) { slot->used = 0; return NULL; }
    return slot;
}

int dlr_file_write(dlr_file* f, const void* buf, uint32_t len) {
    return f && fwrite(buf, 1, len, f->fp) == len;
}

void dlr_file_close(dlr_file* f) {
    if (!f) return;
    fclose(f->fp);
    f->used = 0;
}

dlr_file* dlr_file_open_read(const char* path) {
    struct dlr_file* slot = slot_take();
    if (!slot) return NULL;
    char real[1024];
    slot->fp = fopen(mapped(path, real, sizeof(real)), "rb");
    if (!slot->fp) { slot->used = 0; return NULL; }
    return slot;
}

long dlr_file_read(dlr_file* f, void* buf, uint32_t len) {
    if (!f) return -1;
    size_t n = fread(buf, 1, len, f->fp);
    if (n == 0 && ferror(f->fp)) return -1;
    return (long)n;
}

int dlr_is_dir(const char* path) {
    char real[1024];
    struct stat st;
    return stat(mapped(path, real, sizeof(real)), &st) == 0 && S_ISDIR(st.st_mode);
}

int dlr_dir_each(const char* path, dlr_dir_cb cb, void* user) {
    char real[1024];
    DIR* d = opendir(mapped(path, real, sizeof(real)));
    if (!d) return -1;

    // Collect first, call back after: the callback may itself list or
    // modify directories, and readdir() state is per-DIR* anyway, but
    // matching the Minimal-OS backend (a snapshot) keeps the two
    // behaving the same, including for a callback that deletes entries.
    char names[256][256];
    int is_dir[256];
    int n = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL && n < 256) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(names[n], sizeof(names[n]), "%s", e->d_name);
        char full[1300];
        struct stat st;
        snprintf(full, sizeof(full), "%s/%s", real, e->d_name);
        is_dir[n] = (stat(full, &st) == 0 && S_ISDIR(st.st_mode));
        n++;
    }
    closedir(d);

    int visited = 0;
    for (int i = 0; i < n; i++) {
        visited++;
        if (!cb(user, names[i], is_dir[i])) break;
    }
    return visited;
}

int dlr_remove_dir(const char* path) {
    char real[1024];
    return rmdir(mapped(path, real, sizeof(real))) == 0;
}

// Minimal-OS archives with a syscall; here the reference host tool does
// it (tools/mkpkg/mkpkg.py in the Minimal-OS repo), pointed to by
// $DLR_MKPKG. Same output name and same entry naming as the kernel.
int dlr_pack_dir(const char* dir, char* out_path, size_t out_cap) {
    const char* tool = getenv("DLR_MKPKG");
    if (!tool) { fprintf(stderr, "dlr(host): set DLR_MKPKG to mkpkg.py to pack directories\n"); return 0; }

    char real[1024], out_logical[512], out_real[1100], cmd[2600];
    mapped(dir, real, sizeof(real));
    snprintf(out_logical, sizeof(out_logical), "%s.mpkg", dir);
    mapped(out_logical, out_real, sizeof(out_real));

    snprintf(cmd, sizeof(cmd), "python3 '%s' '%s' '%s' >/dev/null 2>&1", tool, real, out_real);
    if (system(cmd) != 0) return 0;

    if (out_path) snprintf(out_path, out_cap, "%s", out_logical);
    return 1;
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

int dlr_getcwd(char* path, size_t path_size) {
    return getcwd(path, path_size) != NULL;
}

long dlr_read_input(void* buf, size_t len) {
    return (long)read(STDIN_FILENO, buf, len);
}

/* --- server ------------------------------------------------------------- */

long dlr_tcp_listen(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return DLR_INVALID;
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0 || listen(fd, 4) != 0) {
        close(fd);
        return DLR_INVALID;
    }
    return fd;
}

int dlr_tcp_accept(long listener, long* out_conn, uint32_t* out_ip) {
    struct pollfd pfd = { (int)listener, POLLIN, 0 };
    int r = poll(&pfd, 1, 0);
    if (r < 0) return -1;
    if (r == 0) return 0;

    struct sockaddr_in from;
    socklen_t len = sizeof(from);
    int fd = accept((int)listener, (struct sockaddr*)&from, &len);
    if (fd < 0) return (errno == EAGAIN || errno == EINTR) ? 0 : -1;
    *out_conn = fd;
    if (out_ip) *out_ip = ntohl(from.sin_addr.s_addr);
    return 1;
}

void dlr_tcp_unlisten(long listener) {
    if (listener >= 0) close((int)listener);
}

void dlr_set_self(const char* argv0) { (void)argv0; }

long dlr_spawn_conn(long conn, uint32_t peer_ip, const char* const* extra, int extra_count) {
    char handle_str[16], ip_str[20];
    snprintf(handle_str, sizeof(handle_str), "%ld", conn);
    snprintf(ip_str, sizeof(ip_str), "%u.%u.%u.%u", (peer_ip >> 24) & 255, (peer_ip >> 16) & 255,
             (peer_ip >> 8) & 255, peer_ip & 255);

    const char* argv[24];
    int argc = 0;
    argv[argc++] = "dlr-host";
    argv[argc++] = "--conn";
    argv[argc++] = handle_str;
    argv[argc++] = ip_str;
    for (int i = 0; i < extra_count && argc < 22; i++) argv[argc++] = extra[i];
    argv[argc] = NULL;

    pid_t pid = fork();
    if (pid < 0) return DLR_INVALID;
    if (pid == 0) {
        // Child: the connection fd is inherited across exec.
        execv("/proc/self/exe", (char* const*)argv);
        _exit(127);
    }

    // The kernel-side equivalent is a handoff: the parent stops being
    // the owner. On a host the equivalent is closing our copy of the
    // fd, otherwise the peer would not see EOF when the child closes.
    close((int)conn);
    return (long)pid;
}

int dlr_proc_alive(long pid) {
    int status;
    pid_t r = waitpid((pid_t)pid, &status, WNOHANG);
    return r == 0;      // 0 = still running; >0 reaped it; -1 no such child
}

static void (*g_interrupt_cb)(void);
static void on_signal(int sig) { (void)sig; if (g_interrupt_cb) g_interrupt_cb(); }

long dlr_tls_open(uint32_t ip, uint16_t port, uint32_t timeout_ms) {
    (void)ip; (void)port; (void)timeout_ms;
    fprintf(stderr, "dlr(host): the TLS transport is Minimal-OS only\n");
    return DLR_INVALID;
}

long dlr_tls_accept(long tcp_handle) {
    dlr_tcp_close(tcp_handle);
    return DLR_INVALID;
}

void dlr_on_interrupt(void (*cb)(void)) {
    g_interrupt_cb = cb;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
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
