#include "dlr_proto.h"
#include "dlr_port.h"
#include <string.h>
#include <stdlib.h>

#define DLR_IDLE_TIMEOUT_MS 15000

/* --- framing ----------------------------------------------------------- */

static void put_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

static uint32_t get_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

int dlr_frame_send(long sock, const uint8_t* payload, uint32_t len) {
    uint8_t header[4];
    put_be32(header, len);
    if (!dlr_tcp_write(sock, header, 4)) return 0;
    if (len && !dlr_tcp_write(sock, payload, len)) return 0;
    return 1;
}

// Reads one frame into `buf`. Returns the length, or -1.
long dlr_frame_recv(long sock, uint8_t* buf, uint32_t cap, uint32_t timeout_ms) {
    uint8_t header[4];
    if (!dlr_tcp_read_exact(sock, header, 4, timeout_ms)) return -1;

    uint32_t len = get_be32(header);
    if (len == 0) return -1;

    // The length arrives before anything is authenticated, so it is
    // attacker-controlled. Refuse anything over the ceiling instead of
    // sizing a buffer from it.
    if (len > cap) return -1;

    if (!dlr_tcp_read_exact(sock, buf, len, timeout_ms)) return -1;
    return (long)len;
}

/* --- session ----------------------------------------------------------- */

int dlr_session_alloc(dlr_session* s) {
    memset(s, 0, sizeof(*s));
    s->sock = DLR_INVALID;
    s->frame = (uint8_t*)malloc(DLR_MAX_FRAME);
    s->plain = (uint8_t*)malloc(DLR_MAX_FRAME);
    if (!s->frame || !s->plain) {
        dlr_session_free(s);
        return -1;
    }
    return 0;
}

void dlr_session_free(dlr_session* s) {
    if (s->frame) { free(s->frame); s->frame = 0; }
    if (s->plain) { free(s->plain); s->plain = 0; }
}

static void parse_hello(dlr_session* s, const char* hello, uint32_t len) {
    // "DLR_SERVER|<name>|<needs_pw>|<proto>"
    char buf[256];
    uint32_t n = (len < sizeof(buf) - 1) ? len : (uint32_t)sizeof(buf) - 1;
    memcpy(buf, hello, n);
    buf[n] = '\0';

    char* fields[4] = { 0, 0, 0, 0 };
    int count = 0;
    char* p = buf;
    fields[count++] = p;
    while (*p && count < 4) {
        if (*p == '|') { *p = '\0'; fields[count++] = p + 1; }
        p++;
    }

    if (count > 1 && fields[1]) {
        size_t i = 0;
        while (fields[1][i] && i + 1 < sizeof(s->server_name)) {
            s->server_name[i] = fields[1][i];
            i++;
        }
        s->server_name[i] = '\0';
    }
    s->needs_password = (count > 2 && fields[2] && fields[2][0] == '1');
    s->protocol_version = (count > 3 && fields[3]) ? (fields[3][0] - '0') : 0;
}

int dlr_connect(dlr_session* s, uint32_t ip, uint16_t port, const char* password) {
    s->sock = (port == DLR_TLS_PORT) ? dlr_tls_open(ip, port, 5000)
                                     : dlr_tcp_open(ip, port, 5000);
    if (s->sock == DLR_INVALID) return 0;

    // 1. Server speaks first, unencrypted.
    long n = dlr_frame_recv(s->sock, s->frame, DLR_MAX_FRAME, 5000);
    if (n < 0) { dlr_disconnect(s); return 0; }
    if (n < 11 || memcmp(s->frame, "DLR_SERVER|", 11) != 0) { dlr_disconnect(s); return 0; }
    parse_hello(s, (const char*)s->frame, (uint32_t)n);

    // 2. Session key. Generated here, sent in the clear because that
    //    is what the protocol does - see the note in dlr_proto.h.
    dlr_random_bytes(s->key, DLR_KEY_LEN);

    char key_msg[8 + 64];
    memcpy(key_msg, "KEY:", 4);
    if (dlr_b64_encode(s->key, DLR_KEY_LEN, key_msg + 4, sizeof(key_msg) - 4) == 0) {
        dlr_disconnect(s);
        return 0;
    }
    if (!dlr_frame_send(s->sock, (const uint8_t*)key_msg, (uint32_t)strlen(key_msg))) {
        dlr_disconnect(s);
        return 0;
    }
    s->encrypted = 1;

    // 3. Auth, if asked. These frames are NOT encrypted - the server's
    //    authenticate() runs before the session key is used for
    //    anything, and the password crosses the wire in the clear.
    if (s->needs_password) {
        n = dlr_frame_recv(s->sock, s->frame, DLR_MAX_FRAME, 5000);
        if (n < 1 || s->frame[0] != DLR_MSG_AUTH_REQUEST) { dlr_disconnect(s); return 0; }

        if (!password) { dlr_disconnect(s); return 0; }
        if (!dlr_frame_send(s->sock, (const uint8_t*)password, (uint32_t)strlen(password))) {
            dlr_disconnect(s);
            return 0;
        }

        n = dlr_frame_recv(s->sock, s->frame, DLR_MAX_FRAME, 5000);
        if (n < 1 || s->frame[0] != DLR_MSG_HELLO_ACK) { dlr_disconnect(s); return 0; }
    }

    return 1;
}

void dlr_disconnect(dlr_session* s) {
    if (s->sock != DLR_INVALID) {
        dlr_tcp_close(s->sock);
        s->sock = DLR_INVALID;
    }
    s->encrypted = 0;
    // Wipe the key rather than leaving it in the heap block that
    // malloc will hand to the next caller.
    memset(s->key, 0, sizeof(s->key));
}

int dlr_send_msg(dlr_session* s, uint8_t type, const void* body, uint32_t body_len) {
    if (body_len + 1u + DLR_GCM_IV_LEN + DLR_GCM_TAG_LEN > DLR_MAX_FRAME) return 0;

    uint8_t* plain = s->plain;
    plain[0] = type;
    if (body_len) memcpy(plain + 1, body, body_len);
    uint32_t plain_len = body_len + 1;

    if (!s->encrypted) {
        return dlr_frame_send(s->sock, plain, plain_len);
    }

    // A fresh nonce per frame. Reuse under one key would be fatal, so
    // it is generated in exactly one place: here.
    uint8_t* out = s->frame;
    dlr_random_bytes(out, DLR_GCM_IV_LEN);
    size_t sealed = dlr_seal(s->key, plain, plain_len, out);
    return dlr_frame_send(s->sock, out, (uint32_t)sealed);
}

int dlr_recv_msg(dlr_session* s, uint8_t* out_type, uint32_t* out_len, uint32_t timeout_ms) {
    long n = dlr_frame_recv(s->sock, s->frame, DLR_MAX_FRAME, timeout_ms);
    if (n < 0) return 0;

    uint32_t plain_len;
    if (s->encrypted) {
        long opened = dlr_open(s->key, s->frame, (size_t)n, s->plain);
        if (opened < 1) return 0;       // bad tag, or an empty payload
        plain_len = (uint32_t)opened;
    } else {
        memcpy(s->plain, s->frame, (size_t)n);
        plain_len = (uint32_t)n;
    }

    *out_type = s->plain[0];
    *out_len = plain_len - 1;
    // Body starts at s->plain[1]; callers read it from there.
    return 1;
}

int dlr_probe(uint32_t ip, uint16_t port, dlr_server* out, uint32_t timeout_ms) {
    long sock = (port == DLR_TLS_PORT) ? dlr_tls_open(ip, port, timeout_ms)
                                       : dlr_tcp_open(ip, port, timeout_ms);
    if (sock == DLR_INVALID) return 0;

    uint8_t buf[256];
    long n = dlr_frame_recv(sock, buf, sizeof(buf), timeout_ms);
    dlr_tcp_close(sock);

    if (n < 11 || memcmp(buf, "DLR_SERVER|", 11) != 0) return 0;

    dlr_session tmp;
    memset(&tmp, 0, sizeof(tmp));
    parse_hello(&tmp, (const char*)buf, (uint32_t)n);

    out->ip = ip;
    out->port = port;
    out->needs_password = (uint8_t)tmp.needs_password;
    memcpy(out->name, tmp.server_name, sizeof(out->name));
    return 1;
}

long dlr_ping(dlr_session* s) {
    uint64_t start = dlr_now_ms();
    if (!dlr_send_msg(s, DLR_MSG_PING, "p", 1)) return -1;

    uint8_t type;
    uint32_t len;
    if (!dlr_recv_msg(s, &type, &len, 5000)) return -1;
    if (type != DLR_MSG_PONG) return -1;

    return (long)(dlr_now_ms() - start);
}

/* --- package download -------------------------------------------------- */

static uint64_t parse_size_header(const uint8_t* body, uint32_t len) {
    // Body is "SIZE:<decimal>".
    if (len < 6 || memcmp(body, "SIZE:", 5) != 0) return 0;
    uint64_t value = 0;
    for (uint32_t i = 5; i < len; i++) {
        if (body[i] < '0' || body[i] > '9') break;
        value = value * 10u + (uint64_t)(body[i] - '0');
    }
    return value;
}

// The header may carry "|KEY=VALUE" fields after the size; see dlr.h.
// Absent or unrecognised means a tar, which is what every server that
// predates the field sends.
static int parse_format_field(const uint8_t* body, uint32_t len) {
    static const char key[] = "|FORMAT=";
    const uint32_t klen = sizeof(key) - 1;
    for (uint32_t i = 0; i + klen <= len; i++) {
        if (memcmp(body + i, key, klen) != 0) continue;
        const uint8_t* v = body + i + klen;
        uint32_t remain = len - i - klen;
        if (remain >= 4 && memcmp(v, "mpkg", 4) == 0 &&
            (remain == 4 || v[4] == '|')) return DLR_FORMAT_MPKG;
        return DLR_FORMAT_TAR;
    }
    return DLR_FORMAT_TAR;
}

static void copy_err(char* err, size_t err_size, const uint8_t* body, uint32_t len) {
    if (!err || !err_size) return;
    uint32_t n = (len < err_size - 1) ? len : (uint32_t)err_size - 1;
    memcpy(err, body, n);
    err[n] = '\0';
}

int dlr_download(dlr_session* s, const char* pkg_name, const char* stage_path,
                 void (*on_progress)(uint64_t received, uint64_t total),
                 int* out_format,
                 char* err, size_t err_size) {
    if (out_format) *out_format = DLR_FORMAT_TAR;
    if (!dlr_send_msg(s, DLR_MSG_INSTALL_REQUEST, pkg_name, (uint32_t)strlen(pkg_name))) {
        return 0;
    }

    dlr_file* out = dlr_file_create(stage_path);
    if (!out) {
        copy_err(err, err_size, (const uint8_t*)"cannot create staging file", 26);
        return 0;
    }

    dlr_sha256_ctx hash;
    dlr_sha256_init(&hash);

    uint64_t expected = 0;
    uint64_t received = 0;
    int result = 0;

    for (;;) {
        uint8_t type;
        uint32_t len;
        if (!dlr_recv_msg(s, &type, &len, DLR_IDLE_TIMEOUT_MS)) {
            copy_err(err, err_size, (const uint8_t*)"connection lost mid-transfer", 28);
            result = 0;
            break;
        }

        const uint8_t* body = s->plain + 1;

        if (type == DLR_MSG_INSTALL_ERROR) {
            copy_err(err, err_size, body, len);
            result = -1;
            break;
        }

        if (type == DLR_MSG_INSTALL_DATA) {
            // The first INSTALL_DATA is the size header, not payload.
            if (expected == 0 && len >= 5 && memcmp(body, "SIZE:", 5) == 0) {
                expected = parse_size_header(body, len);
                if (out_format) *out_format = parse_format_field(body, len);
                if (on_progress) on_progress(0, expected);
                continue;
            }

            if (!dlr_file_write(out, body, len)) {
                copy_err(err, err_size, (const uint8_t*)"write to MinimaFS failed", 24);
                result = 0;
                break;
            }
            dlr_sha256_update(&hash, body, len);
            received += len;
            if (on_progress) on_progress(received, expected);
            continue;
        }

        if (type == DLR_MSG_INSTALL_END) {
            uint8_t digest[32];
            char got[65];
            dlr_sha256_final(&hash, digest);
            dlr_hex(digest, 32, got);

            char want[65];
            uint32_t n = (len < 64) ? len : 64;
            memcpy(want, body, n);
            want[n] = '\0';

            if (strcmp(got, want) != 0) {
                // Refuse to install something whose bytes do not match
                // what the server says it sent. The checksum is the
                // only integrity check that survives a truncated
                // transfer.
                copy_err(err, err_size, (const uint8_t*)"checksum mismatch", 17);
                result = 0;
                break;
            }
            result = 1;
            break;
        }

        // Anything else mid-transfer means the two sides disagree
        // about protocol state; stopping beats guessing.
        copy_err(err, err_size, (const uint8_t*)"unexpected message during transfer", 34);
        result = 0;
        break;
    }

    dlr_file_close(out);
    if (result != 1) dlr_remove(stage_path);
    return result;
}
