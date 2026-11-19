#include "dlr_proto.h"
#include "dlr_port.h"
#include <string.h>

/*
 * dlr over UDP, client side. The server half is tools/dlr-server/dlrd.py,
 * which has the wire description ("DLU1"). In short:
 *
 *   datagram = "DLU1" op body
 *   HELLO  0x01                                  -> 0x81 cookie(16) flags name
 *   others carry cookie(16) proof(16) before their body:
 *   PING 0x02 | LIST 0x03 page | SEARCH 0x04 page query | INFO 0x05 name
 *   GET  0x06 nlen name offset(u64) n(u8)        -> n datagrams 0x86 offset data(<=1000)
 *
 * The server keeps no state, so loss is handled here: every request is
 * repeated until its answer arrives, and a download asks for the chunks it
 * is still missing.
 *
 * To keep list, search, ping and download unchanged, a UDP session fakes the
 * TCP session's message stream: dlr_send_msg remembers the request and
 * dlr_recv_msg produces the replies a TCP server would have sent
 * (PKG_LIST, SEARCH_RESULT, PONG, or INSTALL_DATA "SIZE:" + chunks +
 * INSTALL_END).
 */

#define U_MAGIC      "DLU1"
#define U_HELLO      0x01
#define U_PING       0x02
#define U_LIST       0x03
#define U_SEARCH     0x04
#define U_INFO       0x05
#define U_GET        0x06
#define U_ERR        0xFF
#define U_CHUNK      1000
#define U_WINDOW     16
#define U_DGRAM_MAX  1400

static struct {
    uint32_t ip;
    uint16_t port;
    uint8_t  cookie[16];
    uint8_t  key[32];
    int      has_key;
    char     err[128];

    uint8_t  req;                    /* request waiting for its replies */
    char     arg[128];
    uint32_t arg_len;
    int      state;                  /* 0 start, 1 streaming, 2 finished */

    uint64_t size;
    uint8_t  sha[32];
    uint32_t total;                  /* chunks in the package */
    uint32_t next;                   /* next chunk to hand to the caller */
    uint32_t wbase;                  /* first chunk held in the window */
    uint8_t  wgot[U_WINDOW];
    uint16_t wlen[U_WINDOW];
    uint8_t  wbuf[U_WINDOW][U_CHUNK];
    int      window_valid;

    uint8_t  dgram[U_DGRAM_MAX + 64];
} U;

static void be64_put(uint8_t* p, uint64_t v) { for (int i = 7; i >= 0; i--) { p[i] = (uint8_t)v; v >>= 8; } }
static uint64_t be64_get(const uint8_t* p) { uint64_t v = 0; for (int i = 0; i < 8; i++) v = (v << 8) | p[i]; return v; }

static void set_err(const char* text, uint32_t len) {
    if (len >= sizeof(U.err)) len = sizeof(U.err) - 1;
    memcpy(U.err, text, len);
    U.err[len] = '\0';
}

/* Sends one request datagram. `with_auth` = everything except HELLO. */
static int send_req(dlr_session* s, uint8_t op, const uint8_t* body, uint32_t blen, int with_auth) {
    static uint8_t out[U_DGRAM_MAX];
    if (blen > 300) return 0;
    memcpy(out, U_MAGIC, 4);
    out[4] = op;
    uint32_t n = 5;
    if (with_auth) {
        memcpy(out + n, U.cookie, 16);
        uint8_t msg[16 + 1 + 300], mac[32];
        memcpy(msg, U.cookie, 16);
        msg[16] = op;
        if (blen) memcpy(msg + 17, body, blen);
        if (U.has_key) dlr_hmac_sha256(U.key, 32, msg, 17 + blen, mac);
        else memset(mac, 0, sizeof(mac));
        memcpy(out + n + 16, mac, 16);
        n += 32;
    }
    if (blen) memcpy(out + n, body, blen);
    n += blen;
    return dlr_udp_send(s->sock, U.ip, U.port, out, n);
}

/* Reads one datagram from the server. Returns its length, or 0 on timeout. */
static long read_dgram(dlr_session* s, uint32_t timeout_ms) {
    uint64_t deadline = dlr_now_ms() + timeout_ms;
    for (;;) {
        uint64_t now = dlr_now_ms();
        if (now >= deadline) return 0;
        uint32_t from = 0;
        long n = dlr_udp_read(s->sock, U.dgram, U_DGRAM_MAX, &from, (uint32_t)(deadline - now));
        if (n <= 0) return 0;
        if (from == U.ip && n >= 5 && memcmp(U.dgram, U_MAGIC, 4) == 0) return n;
    }
}

static int hello(dlr_session* s, int* out_pw, char* name, size_t name_cap) {
    for (int attempt = 0; attempt < 4; attempt++) {
        if (!send_req(s, U_HELLO, 0, 0, 0)) return 0;
        long n = read_dgram(s, 1500);
        if (n >= 22 && U.dgram[4] == (0x80 | U_HELLO)) {
            memcpy(U.cookie, U.dgram + 5, 16);
            if (out_pw) *out_pw = U.dgram[21] & 1;
            if (name) {
                size_t len = (size_t)n - 22;
                if (len >= name_cap) len = name_cap - 1;
                memcpy(name, U.dgram + 22, len);
                name[len] = '\0';
            }
            return 1;
        }
    }
    return 0;
}

/* One request, one reply (not GET). Returns the reply length (datagram in
 * U.dgram, op at [4]), 0 on no answer, -1 on a server error (text in U.err). */
static long rpc(dlr_session* s, uint8_t op, const uint8_t* body, uint32_t blen, uint8_t want) {
    int refreshed = 0;
    for (int attempt = 0; attempt < 5; attempt++) {
        if (!send_req(s, op, body, blen, 1)) return 0;
        long n = read_dgram(s, 1500);
        if (n <= 0) continue;
        if (U.dgram[4] == want) return n;
        if (U.dgram[4] == U_ERR) {
            set_err((const char*)U.dgram + 5, (uint32_t)n - 5);
            if (strcmp(U.err, "stale cookie") == 0 && !refreshed) {   // 5-minute cookie ran out
                refreshed = 1;
                if (!hello(s, 0, 0, 0)) return 0;
                continue;
            }
            return -1;
        }
    }
    return 0;
}

int dlr_udp_connect(dlr_session* s, uint32_t ip, uint16_t port, const char* password) {
    memset(&U, 0, sizeof(U));
    U.ip = ip;
    U.port = port;

    long sock = DLR_INVALID;
    for (int attempt = 0; attempt < 8 && sock == DLR_INVALID; attempt++) {
        uint16_t r;
        dlr_random_bytes(&r, sizeof(r));
        sock = dlr_udp_open((uint16_t)(30000u + (r % 30000u)));
    }
    if (sock == DLR_INVALID) return 0;
    s->sock = sock;
    s->udp = 1;

    int needs_pw = 0;
    char name[DLR_NAME_MAX];
    if (!hello(s, &needs_pw, name, sizeof(name))) { dlr_disconnect(s); return 0; }
    memcpy(s->server_name, name, sizeof(s->server_name));
    s->needs_password = needs_pw;
    s->protocol_version = DLR_PROTOCOL_VERSION;
    s->encrypted = 0;

    if (needs_pw) {
        if (!password) { dlr_disconnect(s); return 0; }
        dlr_sha256(password, strlen(password), U.key);
        U.has_key = 1;
        // Check the password now, as the TCP handshake does.
        static const uint8_t echo[1] = { 'p' };
        if (rpc(s, U_PING, echo, 1, 0x80 | U_PING) <= 0) { dlr_disconnect(s); return 0; }
    }
    return 1;
}

int dlr_udp_probe(uint32_t ip, uint16_t port, dlr_server* out, uint32_t timeout_ms) {
    (void)timeout_ms;
    dlr_session s;
    dlr_session_alloc(&s);
    memset(&U, 0, sizeof(U));
    U.ip = ip;
    U.port = port;

    long sock = DLR_INVALID;
    for (int attempt = 0; attempt < 8 && sock == DLR_INVALID; attempt++) {
        uint16_t r;
        dlr_random_bytes(&r, sizeof(r));
        sock = dlr_udp_open((uint16_t)(30000u + (r % 30000u)));
    }
    if (sock == DLR_INVALID) return 0;
    s.sock = sock;
    s.udp = 1;

    int pw = 0;
    char name[DLR_NAME_MAX];
    int ok = hello(&s, &pw, name, sizeof(name));
    dlr_udp_close(sock);
    if (!ok) return 0;

    out->ip = ip;
    out->port = port;
    out->udp = 1;
    out->needs_password = (uint8_t)pw;
    memcpy(out->name, name, sizeof(out->name));
    return 1;
}

int dlr_udp_send_msg(dlr_session* s, uint8_t type, const void* body, uint32_t len) {
    (void)s;
    if (len >= sizeof(U.arg)) len = sizeof(U.arg) - 1;
    U.req = type;
    U.arg_len = len;
    if (len) memcpy(U.arg, body, len);
    U.arg[len] = '\0';
    U.state = 0;
    U.window_valid = 0;
    return 1;
}

static int reply(dlr_session* s, uint8_t type, const void* body, uint32_t len,
                 uint8_t* out_type, uint32_t* out_len) {
    if (len + 1 > DLR_MAX_FRAME) len = DLR_MAX_FRAME - 1;
    s->plain[0] = type;
    if (len) memcpy(s->plain + 1, body, len);
    *out_type = type;
    *out_len = len;
    return 1;
}

static int reply_error(dlr_session* s, uint8_t type, const char* text, uint8_t* out_type, uint32_t* out_len) {
    return reply(s, type, text, (uint32_t)strlen(text), out_type, out_len);
}

/* LIST / SEARCH: gather every page into one message. */
static int do_list(dlr_session* s, uint8_t* out_type, uint32_t* out_len) {
    int search = (U.req == DLR_MSG_SEARCH_REQUEST);
    uint32_t got = 0;
    uint32_t total = 1;
    for (uint32_t page = 0; page < total; page++) {
        uint8_t body[2 + 128];
        body[0] = (uint8_t)(page >> 8);
        body[1] = (uint8_t)page;
        uint32_t blen = 2;
        if (search) { memcpy(body + 2, U.arg, U.arg_len); blen += U.arg_len; }

        long n = rpc(s, search ? U_SEARCH : U_LIST, body, blen, 0x80 | U_LIST);
        if (n == -1) return reply_error(s, DLR_MSG_ERROR, U.err, out_type, out_len);
        if (n < 9) return 0;
        total = ((uint32_t)U.dgram[7] << 8) | U.dgram[8];
        uint32_t chunk = (uint32_t)n - 9;
        if (got + chunk > DLR_MAX_FRAME - 2) break;           // as many whole pages as fit
        memcpy(s->plain + 1 + got, U.dgram + 9, chunk);
        got += chunk;
    }
    s->plain[0] = search ? DLR_MSG_SEARCH_RESULT : DLR_MSG_PKG_LIST;
    *out_type = s->plain[0];
    *out_len = got;
    return 1;
}

/* Brings chunks [first, first+U_WINDOW) in. 1 on success, 0 on loss, -1 server error. */
static int fetch_window(dlr_session* s, uint32_t first) {
    uint32_t count = U.total - first;
    if (count > U_WINDOW) count = U_WINDOW;
    memset(U.wgot, 0, sizeof(U.wgot));
    U.wbase = first;
    U.window_valid = 1;

    for (int attempt = 0; attempt < 8; attempt++) {
        uint32_t missing = count;
        for (uint32_t i = 0; i < count; i++) if (!U.wgot[i]) { missing = i; break; }
        if (missing == count) return 1;

        uint8_t body[1 + 128 + 9];
        uint32_t nlen = U.arg_len;
        body[0] = (uint8_t)nlen;
        memcpy(body + 1, U.arg, nlen);
        be64_put(body + 1 + nlen, (uint64_t)(first + missing) * U_CHUNK);
        body[1 + nlen + 8] = (uint8_t)(count - missing);
        if (!send_req(s, U_GET, body, 1 + nlen + 9, 1)) return 0;

        uint32_t want = count - missing, have = 0;
        int any = 0;
        while (have < want) {
            // The answers come back to back, so once one has arrived a short
            // silence means the rest were lost: ask again instead of waiting.
            long n = read_dgram(s, any ? 250u : (attempt == 0 ? 2500u : 1500u));
            if (n <= 0) break;
            if (U.dgram[4] == U_ERR) {
                set_err((const char*)U.dgram + 5, (uint32_t)n - 5);
                return -1;
            }
            if (U.dgram[4] != (0x80 | U_GET) || n < 13) continue;
            any = 1;
            uint64_t off = be64_get(U.dgram + 5);
            if (off % U_CHUNK) continue;
            uint64_t idx = off / U_CHUNK;
            if (idx < first || idx >= first + count) continue;
            uint32_t slot = (uint32_t)(idx - first);

            uint64_t expect = U.size - idx * U_CHUNK;
            if (expect > U_CHUNK) expect = U_CHUNK;
            if ((uint64_t)(n - 13) != expect) continue;       // truncated or wrong: ask again
            if (!U.wgot[slot]) {
                memcpy(U.wbuf[slot], U.dgram + 13, (size_t)expect);
                U.wlen[slot] = (uint16_t)expect;
                U.wgot[slot] = 1;
                have++;
            }
        }
    }
    return 0;
}

static void put_uint(char* dst, uint64_t v, size_t* pos) {
    char tmp[24];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
    while (n-- > 0) dst[(*pos)++] = tmp[n];
}

static int do_install(dlr_session* s, uint8_t* out_type, uint32_t* out_len) {
    if (U.state == 0) {
        long n = rpc(s, U_INFO, (const uint8_t*)U.arg, U.arg_len, 0x80 | U_INFO);
        if (n == -1) { U.state = 2; return reply_error(s, DLR_MSG_INSTALL_ERROR, U.err, out_type, out_len); }
        if (n < 46) return 0;
        U.size = be64_get(U.dgram + 5);
        memcpy(U.sha, U.dgram + 13, 32);
        U.total = (uint32_t)((U.size + U_CHUNK - 1) / U_CHUNK);
        U.next = 0;
        U.state = 1;

        char hdr[40];
        size_t p = 0;
        memcpy(hdr, "SIZE:", 5);
        p = 5;
        put_uint(hdr, U.size, &p);                 // no FORMAT field: a tar, as the Linux server sends
        return reply(s, DLR_MSG_INSTALL_DATA, hdr, (uint32_t)p, out_type, out_len);
    }

    if (U.state == 1) {
        if (U.next >= U.total) {
            char hex[65];
            dlr_hex(U.sha, 32, hex);
            U.state = 2;
            return reply(s, DLR_MSG_INSTALL_END, hex, 64, out_type, out_len);
        }
        if (!U.window_valid || U.next < U.wbase || U.next >= U.wbase + U_WINDOW || !U.wgot[U.next - U.wbase]) {
            int rc = fetch_window(s, U.next);
            if (rc < 0) { U.state = 2; return reply_error(s, DLR_MSG_INSTALL_ERROR, U.err, out_type, out_len); }
            if (rc == 0) { U.state = 2; return 0; }
        }
        uint32_t slot = U.next - U.wbase;
        U.next++;
        return reply(s, DLR_MSG_INSTALL_DATA, U.wbuf[slot], U.wlen[slot], out_type, out_len);
    }
    return 0;                                      // finished: like a closed connection
}

int dlr_udp_recv_msg(dlr_session* s, uint8_t* out_type, uint32_t* out_len, uint32_t timeout_ms) {
    (void)timeout_ms;                              // each step has its own retry schedule
    switch (U.req) {
        case DLR_MSG_PING: {
            if (U.state) return 0;
            U.state = 2;
            long n = rpc(s, U_PING, (const uint8_t*)U.arg, U.arg_len, 0x80 | U_PING);
            if (n < 5) return 0;
            return reply(s, DLR_MSG_PONG, U.dgram + 5, (uint32_t)n - 5, out_type, out_len);
        }
        case DLR_MSG_PKG_LIST:
        case DLR_MSG_SEARCH_REQUEST:
            if (U.state) return 0;
            U.state = 2;
            return do_list(s, out_type, out_len);
        case DLR_MSG_INSTALL_REQUEST:
            return do_install(s, out_type, out_len);
        default:
            return 0;
    }
}
