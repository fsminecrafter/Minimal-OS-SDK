#ifndef MINIMALOS_NET_H
#define MINIMALOS_NET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "minimalos.h"
#include "syscall.h"

/*
 * Userland networking for Minimal-OS (SYS_NET).
 *
 * Addresses are host-order uint32_t throughout - 192.168.1.10 is
 * 0xC0A8010A - matching how the kernel's ip.c stores them. There is no
 * htonl()/ntohl() dance at this layer; you only need byte swapping for
 * values you put on the wire yourself.
 *
 * The interface must already have an address (run 'dhcp' in the
 * terminal first). Acquiring one is a privileged operation: a user
 * program calling mos_net_dhcp() gets SYS_ERR_PERM, by design, because
 * it would rewrite the address for every process on the machine.
 *
 * Everything here is blocking-but-polled: the kernel stack only makes
 * progress when someone calls into it, so a program that stops calling
 * recv stops receiving. Long waits should go through the *_deadline
 * helpers at the bottom, which yield with mos_sleep() between polls
 * instead of spinning.
 */

// ---------------------------------------------------------------------------
// Raw wrappers
// ---------------------------------------------------------------------------

static inline long mos_net_call(syscall_net_request_t* request) {
    return mos_syscall(SYS_NET, (long)(uintptr_t)request, 0, 0);
}

static inline long mos_net_status(syscall_net_status_t* out) {
    syscall_net_request_t r = {0};
    r.op = SYS_NET_STATUS;
    r.out_status = out;
    return mos_net_call(&r);
}

static inline long mos_net_poll(void) {
    syscall_net_request_t r = {0};
    r.op = SYS_NET_POLL;
    return mos_net_call(&r);
}

// Privileged: returns SYS_ERR_PERM from a .run program. Kept here so
// kernel-privilege callers have one place for it.
static inline long mos_net_dhcp(uint32_t timeout_ms) {
    syscall_net_request_t r = {0};
    r.op = SYS_NET_DHCP;
    r.timeout_ms = timeout_ms;
    return mos_net_call(&r);
}

static inline long mos_net_resolve(const char* hostname, uint32_t* out_ip,
                                   uint32_t timeout_ms) {
    syscall_net_request_t r = {0};
    r.op = SYS_NET_RESOLVE;
    r.host = hostname;
    r.out_ip = out_ip;
    r.timeout_ms = timeout_ms;
    return mos_net_call(&r);
}

// Returns a handle > 0, or a negative SYS_ERR_* value.
static inline long mos_tcp_connect(uint32_t ip, uint16_t port, uint32_t timeout_ms) {
    uint64_t handle = 0;
    syscall_net_request_t r = {0};
    r.op = SYS_NET_TCP_CONNECT;
    r.ip = ip;
    r.port = port;
    r.timeout_ms = timeout_ms;
    r.out_handle = &handle;
    long rc = mos_net_call(&r);
    if (rc != (long)SYS_SUCCESS) return rc;
    return (long)handle;
}

// Returns bytes sent. A short count is possible and is not an error -
// the kernel gives up on a chunk after its retransmit budget.
static inline long mos_tcp_send(long handle, const void* buf, uint32_t len) {
    syscall_net_request_t r = {0};
    r.op = SYS_NET_TCP_SEND;
    r.handle = (uint64_t)handle;
    r.buf = (void*)buf;
    r.len = len;
    return mos_net_call(&r);
}

// Returns bytes read, 0 if nothing has arrived yet, or SYS_ERR_NOTFOUND
// once the peer has closed and the buffer is drained.
static inline long mos_tcp_recv(long handle, void* buf, uint32_t len) {
    syscall_net_request_t r = {0};
    r.op = SYS_NET_TCP_RECV;
    r.handle = (uint64_t)handle;
    r.buf = buf;
    r.len = len;
    return mos_net_call(&r);
}

static inline long mos_tcp_state(long handle) {
    syscall_net_request_t r = {0};
    r.op = SYS_NET_TCP_STATE;
    r.handle = (uint64_t)handle;
    return mos_net_call(&r);
}

static inline long mos_tcp_close(long handle) {
    syscall_net_request_t r = {0};
    r.op = SYS_NET_TCP_CLOSE;
    r.handle = (uint64_t)handle;
    return mos_net_call(&r);
}

static inline long mos_udp_bind(uint16_t local_port) {
    uint64_t handle = 0;
    syscall_net_request_t r = {0};
    r.op = SYS_NET_UDP_BIND;
    r.local_port = local_port;
    r.out_handle = &handle;
    long rc = mos_net_call(&r);
    if (rc != (long)SYS_SUCCESS) return rc;
    return (long)handle;
}

static inline long mos_udp_unbind(long handle) {
    syscall_net_request_t r = {0};
    r.op = SYS_NET_UDP_UNBIND;
    r.handle = (uint64_t)handle;
    return mos_net_call(&r);
}

// Returns bytes of the next queued datagram, or 0 if none is waiting.
// Datagrams larger than the buffer are truncated; the remainder is
// discarded, as with any datagram socket.
static inline long mos_udp_recv(long handle, void* buf, uint32_t len,
                                uint32_t* out_from_ip, uint16_t* out_from_port) {
    syscall_net_request_t r = {0};
    r.op = SYS_NET_UDP_RECV;
    r.handle = (uint64_t)handle;
    r.buf = buf;
    r.len = len;
    r.out_from_ip = out_from_ip;
    r.out_from_port = out_from_port;
    return mos_net_call(&r);
}

// dst_ip may be SYSCALL_NET_IP_BROADCAST. src_port 0 means "use the
// port this handle is bound to".
static inline long mos_udp_send(long handle, uint32_t dst_ip, uint16_t dst_port,
                                uint16_t src_port, const void* buf, uint32_t len) {
    syscall_net_request_t r = {0};
    r.op = SYS_NET_UDP_SEND;
    r.handle = (uint64_t)handle;
    r.ip = dst_ip;
    r.port = dst_port;
    r.local_port = src_port;
    r.buf = (void*)buf;
    r.len = len;
    return mos_net_call(&r);
}

static inline long mos_random(void* buf, uint32_t len) {
    return mos_syscall(SYS_RANDOM, (long)(uintptr_t)buf, (long)len, 0);
}

// ---------------------------------------------------------------------------
// Address helpers
// ---------------------------------------------------------------------------

static inline uint32_t mos_ip_parse(const char* dotted) {
    uint32_t result = 0, octet = 0;
    int octets = 0;
    for (const char* p = dotted; ; p++) {
        if (*p >= '0' && *p <= '9') {
            octet = octet * 10u + (uint32_t)(*p - '0');
            if (octet > 255) return 0;
        } else if (*p == '.' || *p == '\0') {
            result = (result << 8) | (octet & 0xFFu);
            octet = 0;
            octets++;
            if (*p == '\0') break;
        } else {
            return 0;
        }
    }
    return (octets == 4) ? result : 0;
}

// out must be at least 16 bytes.
static inline void mos_ip_to_string(uint32_t ip, char* out) {
    int pos = 0;
    for (int shift = 24; shift >= 0; shift -= 8) {
        uint32_t v = (ip >> shift) & 0xFFu;
        if (v >= 100) out[pos++] = (char)('0' + v / 100);
        if (v >= 10)  out[pos++] = (char)('0' + (v / 10) % 10);
        out[pos++] = (char)('0' + v % 10);
        if (shift) out[pos++] = '.';
    }
    out[pos] = '\0';
}

static inline uint16_t mos_htons(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
static inline uint32_t mos_htonl(uint32_t v) {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}
#define mos_ntohs mos_htons
#define mos_ntohl mos_htonl

// ---------------------------------------------------------------------------
// Blocking helpers
// ---------------------------------------------------------------------------
//
// mos_uptime() is in PIT ticks; mos_sleep() takes the same unit. These
// helpers treat one tick as roughly one millisecond, which is close
// enough for timeouts and avoids every caller reinventing the loop.

static inline bool mos_net_is_up(void) {
    syscall_net_status_t st;
    if (mos_net_status(&st) != (long)SYS_SUCCESS) return false;
    return st.has_driver && st.configured;
}

// Sends the whole buffer or returns false. Retries short sends.
static inline bool mos_tcp_send_all(long handle, const void* buf, uint32_t len) {
    const uint8_t* p = (const uint8_t*)buf;
    uint32_t sent = 0;
    while (sent < len) {
        long n = mos_tcp_send(handle, p + sent, len - sent);
        if (n <= 0) return false;
        sent += (uint32_t)n;
    }
    return true;
}

// Reads exactly len bytes or returns false (peer closed, or the
// deadline passed with no progress).
static inline bool mos_tcp_recv_exact(long handle, void* buf, uint32_t len,
                                      uint32_t timeout_ms) {
    uint8_t* p = (uint8_t*)buf;
    uint32_t got = 0;
    long idle_start = mos_uptime();

    while (got < len) {
        long n = mos_tcp_recv(handle, p + got, len - got);
        if (n < 0) return false;                  // closed
        if (n == 0) {
            if ((mos_uptime() - idle_start) > (long)timeout_ms) return false;
            mos_sleep(2);
            continue;
        }
        got += (uint32_t)n;
        idle_start = mos_uptime();
    }
    return true;
}

// Waits up to timeout_ms for a datagram. Returns its length, or 0.
static inline long mos_udp_recv_timeout(long handle, void* buf, uint32_t len,
                                        uint32_t* out_from_ip, uint16_t* out_from_port,
                                        uint32_t timeout_ms) {
    long start = mos_uptime();
    for (;;) {
        long n = mos_udp_recv(handle, buf, len, out_from_ip, out_from_port);
        if (n > 0) return n;
        if (n < 0) return n;
        if ((mos_uptime() - start) > (long)timeout_ms) return 0;
        mos_sleep(2);
    }
}

// Dotted-quad or hostname. Returns 0 on failure.
static inline uint32_t mos_net_lookup(const char* host, uint32_t timeout_ms) {
    uint32_t ip = mos_ip_parse(host);
    if (ip) return ip;
    if (mos_net_resolve(host, &ip, timeout_ms) != (long)SYS_SUCCESS) return 0;
    return ip;
}

#endif // MINIMALOS_NET_H
