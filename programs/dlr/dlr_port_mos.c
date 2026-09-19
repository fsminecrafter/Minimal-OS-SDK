#include "dlr_port.h"

#include "minimalos.h"
#include "syscall.h"
#include "net.h"
#include "stdlib.h"
#include "stdio.h"
#include <string.h>

/*
 * Minimal-OS backend: SYS_NET for the wire, MinimaFS for storage.
 *
 * Two things worth knowing about the stack underneath this:
 *
 * - SYS_ERR_BUSY is not a failure. The kernel net stack builds frames
 *   in file-static buffers, so only one caller can be inside it at a
 *   time; every call here retries rather than giving up.
 *
 * - Nothing makes progress unless someone calls in. There is no
 *   background receive. A loop that stops calling recv stops
 *   receiving, so the read paths poll on a yield rather than a spin.
 */

#define DLR_RETRY_SLEEP_TICKS 2

static int is_busy(long rc) { return rc == (long)SYS_ERR_BUSY; }

int dlr_port_init(void) {
    syscall_net_status_t st;
    if (mos_net_status(&st) != (long)SYS_SUCCESS) {
        printf("dlr: SYS_NET unavailable - is this kernel built with net_syscall.c?\n");
        return -1;
    }
    if (!st.has_driver) {
        printf("dlr: no network interface present\n");
        return -1;
    }
    if (!st.configured) {
        printf("dlr: no IP address. Run 'dhcp' in the terminal first.\n");
        return -1;
    }
    return 0;
}

uint64_t dlr_now_ms(void) {
    // mos_uptime() is PIT ticks; the PIT is configured at 1 kHz, so
    // one tick is one millisecond closely enough for timeouts.
    return (uint64_t)mos_uptime();
}

void dlr_sleep_ms(uint32_t ms) {
    mos_sleep((long)ms);
}

/* --- TCP --------------------------------------------------------------- */

long dlr_tcp_open(uint32_t ip, uint16_t port, uint32_t timeout_ms) {
    for (int attempt = 0; attempt < 20; attempt++) {
        long h = mos_tcp_connect(ip, port, timeout_ms);
        if (is_busy(h)) { mos_sleep(DLR_RETRY_SLEEP_TICKS); continue; }
        if (h <= 0) return DLR_INVALID;
        return h;
    }
    return DLR_INVALID;
}

int dlr_tcp_write(long handle, const void* buf, uint32_t len) {
    const uint8_t* p = (const uint8_t*)buf;
    uint32_t sent = 0;

    while (sent < len) {
        long n = mos_tcp_send(handle, p + sent, len - sent);
        if (is_busy(n)) { mos_sleep(DLR_RETRY_SLEEP_TICKS); continue; }
        if (n <= 0) return 0;
        sent += (uint32_t)n;
    }
    return 1;
}

int dlr_tcp_read_exact(long handle, void* buf, uint32_t len, uint32_t idle_timeout_ms) {
    uint8_t* p = (uint8_t*)buf;
    uint32_t got = 0;
    uint64_t last_progress = dlr_now_ms();

    while (got < len) {
        long n = mos_tcp_recv(handle, p + got, len - got);

        if (is_busy(n)) { mos_sleep(DLR_RETRY_SLEEP_TICKS); continue; }
        if (n < 0) return 0;                       // peer closed, drained

        if (n == 0) {
            if (dlr_now_ms() - last_progress > idle_timeout_ms) return 0;
            mos_sleep(DLR_RETRY_SLEEP_TICKS);
            continue;
        }

        got += (uint32_t)n;
        last_progress = dlr_now_ms();
    }
    return 1;
}

void dlr_tcp_close(long handle) {
    for (int attempt = 0; attempt < 20; attempt++) {
        long rc = mos_tcp_close(handle);
        if (!is_busy(rc)) return;
        mos_sleep(DLR_RETRY_SLEEP_TICKS);
    }
}

/* --- UDP --------------------------------------------------------------- */

long dlr_udp_open(uint16_t local_port) {
    for (int attempt = 0; attempt < 20; attempt++) {
        long h = mos_udp_bind(local_port);
        if (is_busy(h)) { mos_sleep(DLR_RETRY_SLEEP_TICKS); continue; }
        return (h > 0) ? h : DLR_INVALID;
    }
    return DLR_INVALID;
}

int dlr_udp_broadcast(long handle, uint16_t dst_port, const void* buf, uint32_t len) {
    for (int attempt = 0; attempt < 20; attempt++) {
        long rc = mos_udp_send(handle, SYSCALL_NET_IP_BROADCAST, dst_port, 0, buf, len);
        if (is_busy(rc)) { mos_sleep(DLR_RETRY_SLEEP_TICKS); continue; }
        return rc > 0;
    }
    return 0;
}

long dlr_udp_read(long handle, void* buf, uint32_t len,
                  uint32_t* out_from_ip, uint32_t timeout_ms) {
    uint64_t deadline = dlr_now_ms() + timeout_ms;

    for (;;) {
        uint16_t from_port = 0;
        long n = mos_udp_recv(handle, buf, len, out_from_ip, &from_port);

        if (is_busy(n)) { mos_sleep(DLR_RETRY_SLEEP_TICKS); continue; }
        if (n > 0) return n;
        if (n < 0) return 0;
        if (dlr_now_ms() >= deadline) return 0;
        mos_sleep(DLR_RETRY_SLEEP_TICKS);
    }
}

void dlr_udp_close(long handle) {
    mos_udp_unbind(handle);
}

uint32_t dlr_resolve(const char* host) {
    uint32_t ip = mos_ip_parse(host);
    if (ip) return ip;

    for (int attempt = 0; attempt < 10; attempt++) {
        long rc = mos_net_resolve(host, &ip, 5000);
        if (is_busy(rc)) { mos_sleep(DLR_RETRY_SLEEP_TICKS); continue; }
        return (rc == (long)SYS_SUCCESS) ? ip : 0;
    }
    return 0;
}

/* --- files ------------------------------------------------------------- */

struct dlr_file {
    int fd;
};

// One static handle: the client never has two files open at once, and
// a heap allocation per open would be pure ceremony.
static struct dlr_file g_file_slot;
static int g_file_slot_used = 0;

dlr_file* dlr_file_create(const char* path) {
    if (g_file_slot_used) return NULL;

    // MinimaFS has no O_TRUNC and no create-on-open: SYS_OPEN only
    // ever opens what already exists, so the sequence is delete (if
    // present) -> SYS_CREATE -> open writable.
    if (mos_exists(path)) mos_delete(path);

    if (mos_create(path, "binary", "bin") != (long)SYS_SUCCESS) return NULL;

    int fd = (int)mos_open(path, SYS_O_RDWR);
    if (fd <= 0) return NULL;

    g_file_slot.fd = fd;
    g_file_slot_used = 1;
    return &g_file_slot;
}

int dlr_file_write(dlr_file* f, const void* buf, uint32_t len) {
    if (!f) return 0;
    long n = mos_fwrite(f->fd, buf, len);
    return n == (long)len;
}

void dlr_file_close(dlr_file* f) {
    if (!f) return;
    mos_close(f->fd);
    g_file_slot_used = 0;
}

long dlr_file_size(const char* path) {
    int fd = (int)mos_open(path, SYS_O_RDONLY);
    if (fd <= 0) return -1;
    long size = mos_size(fd);
    mos_close(fd);
    return size;
}

long dlr_file_slurp(const char* path, void* buf, uint32_t max) {
    int fd = (int)mos_open(path, SYS_O_RDONLY);
    if (fd <= 0) return -1;

    long size = mos_size(fd);
    if (size < 0 || (uint32_t)size > max) { mos_close(fd); return -1; }

    long got = mos_read(fd, buf, (size_t)size);
    mos_close(fd);
    return got;
}

int dlr_file_put(const char* path, const void* buf, uint32_t len) {
    dlr_file* f = dlr_file_create(path);
    if (!f) return 0;
    int ok = dlr_file_write(f, buf, len);
    dlr_file_close(f);
    return ok;
}

int dlr_mkdirs(const char* path) {
    char partial[256];
    size_t n = strlen(path);
    if (n >= sizeof(partial)) return 0;

    for (size_t i = 0; i <= n; i++) {
        char c = path[i];
        if (c != '/' && c != '\0') continue;
        if (i == 0) continue;

        memcpy(partial, path, i);
        partial[i] = '\0';

        // "0:" alone is a drive, not a directory.
        if (partial[i - 1] == ':') continue;

        if (!mos_exists(partial)) {
            if (mos_mkdir(partial) != (long)SYS_SUCCESS && !mos_exists(partial)) return 0;
        }
    }
    return 1;
}

int dlr_exists(const char* path) {
    return mos_exists(path) == 1;
}

int dlr_remove(const char* path) {
    return mos_delete(path) == (long)SYS_SUCCESS;
}

/* --- misc -------------------------------------------------------------- */

void dlr_random_bytes(void* buf, size_t len) {
    if (mos_random_bytes(buf, len) == (long)len) return;

    // SYS_RANDOM should never fail, but a silent fallback to a fixed
    // buffer would hand out a predictable session key. Make it loud.
    printf("dlr: SYS_RANDOM failed - refusing to continue with a weak key\n");
    mos_exit(1);
}

long dlr_exec(const char* path) {
    long pid = mos_exec(path, 0, 0);
    return (pid > 0) ? pid : DLR_INVALID;
}

int dlr_exec_wait(long pid, uint32_t timeout_ms) {
    static syscall_process_info_t procs[64];
    uint64_t deadline = dlr_now_ms() + timeout_ms;

    while (dlr_now_ms() < deadline) {
        long count = mos_pslist(procs, 64);
        int still_running = 0;
        for (long i = 0; i < count; i++) {
            if ((long)procs[i].pid == pid &&
                procs[i].state != SYSCALL_PROC_STATE_ZOMBIE &&
                procs[i].state != SYSCALL_PROC_STATE_TERMINATED) {
                still_running = 1;
                break;
            }
        }
        if (!still_running) return 1;
        mos_sleep(10);
    }
    return 0;
}
