#include "dlr_server.h"
#include "dlr_port.h"
#include "dlr_proto.h"
#include "dlr_crypto.h"
#include "dlr_registry.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* --- helpers ------------------------------------------------------------ */

static size_t put_str(char* dst, size_t cap, size_t pos, const char* s) {
    while (*s && pos + 1 < cap) dst[pos++] = *s++;
    dst[pos] = '\0';
    return pos;
}

static size_t put_uint(char* dst, size_t cap, size_t pos, uint64_t v) {
    char tmp[24];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
    while (n-- > 0 && pos + 1 < cap) dst[pos++] = tmp[n];
    dst[pos] = '\0';
    return pos;
}

// Constant-time: how long a password check takes must not depend on how
// many leading characters were right.
static int ct_equal(const char* a, const char* b) {
    size_t la = strlen(a), lb = strlen(b);
    unsigned char diff = (unsigned char)(la != lb);
    size_t n = la < lb ? la : lb;
    for (size_t i = 0; i < n; i++) diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

// Fields on the wire are '|'-separated and lines '\n'-terminated, so
// neither may appear inside one. Replaced rather than rejected: a
// manifest description with a pipe in it should still be listable.
static void sanitize_field(char* out, size_t cap, const char* in) {
    size_t i = 0;
    for (; in[i] && i + 1 < cap; i++) {
        char c = in[i];
        if (c == '|') c = '/';
        else if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        out[i] = c;
    }
    out[i] = '\0';
}

static void hex_of_password(const char* pw, char out[65]) {
    uint8_t digest[32];
    dlr_sha256(pw, strlen(pw), digest);
    dlr_hex(digest, 32, out);
}

/* --- configuration ------------------------------------------------------ */

void dlr_server_cfg_default(dlr_server_cfg* cfg) {
    memset(cfg, 0, sizeof(*cfg));
    strcpy(cfg->name, "minimalos");
    cfg->port = 4242;
}

int dlr_server_parse_args(int argc, char** argv, int start, dlr_server_cfg* cfg, const char** bad) {
    int port_given = 0;

    for (int i = start; i < argc; i++) {
        const char* a = argv[i];
        int has_value = (i + 1 < argc);

        if (strcmp(a, "--name") == 0 && has_value) {
            sanitize_field(cfg->name, sizeof(cfg->name), argv[++i]);
        } else if (strcmp(a, "--port") == 0 && has_value) {
            int p = atoi(argv[++i]);
            if (p <= 0 || p > 65535) { if (bad) *bad = a; return 0; }
            cfg->port = (uint16_t)p;
            port_given = 1;
        } else if (strcmp(a, "--password") == 0 && has_value) {
            hex_of_password(argv[++i], cfg->pw_hash);
        } else if (strcmp(a, "--pwhash") == 0 && has_value) {
            const char* h = argv[++i];
            if (strlen(h) != 64) { if (bad) *bad = a; return 0; }
            memcpy(cfg->pw_hash, h, 65);
        } else if (strcmp(a, "--tls") == 0) {
            cfg->tls = 1;
        } else {
            if (bad) *bad = a;
            return 0;
        }
    }

    // Clients decide TLS-or-not from the port alone, so the two must agree.
    if (cfg->tls && !port_given) cfg->port = DLR_TLS_PORT;
    if (cfg->port == DLR_TLS_PORT) cfg->tls = 1;
    if (cfg->tls && cfg->port != DLR_TLS_PORT) {
        if (bad) *bad = "--tls (TLS servers must use port 4342 so clients can tell)";
        return 0;
    }
    return 1;
}

/* --- request handlers --------------------------------------------------- */

static int send_error(dlr_session* s, uint8_t type, const char* msg) {
    return dlr_send_msg(s, type, msg, (uint32_t)strlen(msg));
}

typedef struct {
    char*  buf;
    size_t cap;
    size_t len;
    int    full;
} list_out;

static int list_cb(void* user, const dlr_reg_entry* e) {
    list_out* o = (list_out*)user;
    char ver[DLR_REG_VERSION_MAX], desc[DLR_REG_DESC_MAX];
    sanitize_field(ver, sizeof(ver), e->version);
    sanitize_field(desc, sizeof(desc), e->description);

    // A line is "name|version|description\n". Stop before overflowing
    // the frame rather than send half a line; the client just sees a
    // shorter list.
    size_t need = strlen(e->name) + strlen(ver) + strlen(desc) + 3;
    if (o->len + need + 1 > o->cap) { o->full = 1; return 0; }

    size_t p = o->len;
    p = put_str(o->buf, o->cap, p, e->name);
    p = put_str(o->buf, o->cap, p, "|");
    p = put_str(o->buf, o->cap, p, ver);
    p = put_str(o->buf, o->cap, p, "|");
    p = put_str(o->buf, o->cap, p, desc);
    p = put_str(o->buf, o->cap, p, "\n");
    o->len = p;
    return 1;
}

// Keeps the reply well inside DLR_MAX_FRAME (type byte + GCM overhead).
#define DLR_LIST_MAX (DLR_MAX_FRAME - 256)

static int handle_list(dlr_session* s) {
    char* buf = (char*)malloc(DLR_LIST_MAX);
    if (!buf) return send_error(s, DLR_MSG_ERROR, "Out of memory");

    list_out o = { buf, DLR_LIST_MAX, 0, 0 };
    buf[0] = '\0';
    dlr_reg_each(list_cb, &o);

    int ok = dlr_send_msg(s, DLR_MSG_PKG_LIST, buf, (uint32_t)o.len);
    free(buf);
    return ok;
}

static int handle_search(dlr_session* s, const char* query) {
    char* buf = (char*)malloc(DLR_LIST_MAX);
    if (!buf) return send_error(s, DLR_MSG_ERROR, "Out of memory");

    list_out o = { buf, DLR_LIST_MAX, 0, 0 };
    buf[0] = '\0';
    dlr_reg_search(query, list_cb, &o);

    // The C++ server answers an empty result with the literal "NONE";
    // clients key off that.
    int ok = o.len ? dlr_send_msg(s, DLR_MSG_SEARCH_RESULT, buf, (uint32_t)o.len)
                   : dlr_send_msg(s, DLR_MSG_SEARCH_RESULT, "NONE", 4);
    free(buf);
    return ok;
}

// Returns 1 to keep the session, 0 if the connection is no longer usable.
static int handle_install(dlr_session* s, const char* name, uint32_t peer_ip) {
    if (!dlr_reg_name_ok(name)) return send_error(s, DLR_MSG_INSTALL_ERROR, "Invalid package name");

    char archive[256];
    if (!dlr_reg_find(name, archive, sizeof(archive))) {
        char msg[DLR_REG_NAME_MAX + 32];
        size_t p = put_str(msg, sizeof(msg), 0, "Package not found: ");
        put_str(msg, sizeof(msg), p, name);
        return send_error(s, DLR_MSG_INSTALL_ERROR, msg);
    }

    long size = dlr_file_size(archive);
    if (size <= 0) return send_error(s, DLR_MSG_INSTALL_ERROR, "Package archive is missing or empty");

    dlr_file* f = dlr_file_open_read(archive);
    if (!f) return send_error(s, DLR_MSG_INSTALL_ERROR, "Cannot open package archive");

    uint8_t* chunk = (uint8_t*)malloc(DLR_SERVE_CHUNK);
    if (!chunk) {
        dlr_file_close(f);
        return send_error(s, DLR_MSG_INSTALL_ERROR, "Out of memory");
    }

    // "SIZE:<n>|FORMAT=mpkg|OS=minimalos" - see dlr.h for why this is
    // an extension of the size header and not a separate message.
    char hdr[80];
    size_t p = put_str(hdr, sizeof(hdr), 0, "SIZE:");
    p = put_uint(hdr, sizeof(hdr), p, (uint64_t)size);
    p = put_str(hdr, sizeof(hdr), p, "|FORMAT=mpkg|OS=minimalos");

    int keep = 1;
    if (!dlr_send_msg(s, DLR_MSG_INSTALL_DATA, hdr, (uint32_t)p)) { keep = 0; goto done; }

    dlr_sha256_ctx hash;
    dlr_sha256_init(&hash);
    uint64_t sent = 0;

    for (;;) {
        long n = dlr_file_read(f, chunk, DLR_SERVE_CHUNK);
        if (n < 0) {
            // Mid-transfer failure: the client has already been told a
            // size, so say so explicitly instead of just stopping.
            send_error(s, DLR_MSG_INSTALL_ERROR, "Read error on the server");
            keep = 0;
            goto done;
        }
        if (n == 0) break;
        dlr_sha256_update(&hash, chunk, (size_t)n);
        if (!dlr_send_msg(s, DLR_MSG_INSTALL_DATA, chunk, (uint32_t)n)) { keep = 0; goto done; }
        sent += (uint64_t)n;
    }

    if (sent != (uint64_t)size) {
        // The archive changed under us (re-presented while downloading).
        // The digest would not match what we promised; do not send one.
        send_error(s, DLR_MSG_INSTALL_ERROR, "Package changed during download - try again");
        keep = 0;
        goto done;
    }

    uint8_t digest[32];
    char hex[65];
    dlr_sha256_final(&hash, digest);
    dlr_hex(digest, 32, hex);
    if (!dlr_send_msg(s, DLR_MSG_INSTALL_END, hex, 64)) keep = 0;
    else printf("dlr: sent '%s' (%ld bytes) to %u.%u.%u.%u\n", name, size,
                (peer_ip >> 24) & 255, (peer_ip >> 16) & 255, (peer_ip >> 8) & 255, peer_ip & 255);

done:
    free(chunk);
    dlr_file_close(f);
    return keep;
}

/* --- one client --------------------------------------------------------- */

int dlr_server_session(long conn, const dlr_server_cfg* cfg, uint32_t peer_ip) {
    dlr_session s;
    if (dlr_session_alloc(&s) != 0) { dlr_tcp_close(conn); return 1; }
    s.sock = conn;

    int rc = 1;

    // 1. HELLO, unencrypted: "DLR_SERVER|<name>|<needs_pw>|<proto>"
    char hello[DLR_NAME_MAX + 32];
    {
        size_t p = put_str(hello, sizeof(hello), 0, "DLR_SERVER|");
        p = put_str(hello, sizeof(hello), p, cfg->name);
        p = put_str(hello, sizeof(hello), p, cfg->pw_hash[0] ? "|1|" : "|0|");
        put_str(hello, sizeof(hello), p, "1");
    }
    if (!dlr_frame_send(s.sock, (const uint8_t*)hello, (uint32_t)strlen(hello))) goto out;

    // 2. KEY:<base64 of 32 bytes>, unencrypted. Anything else - a probe
    //    that connects, reads the hello and leaves (dlr_probe does
    //    exactly that), or a client that does not speak the protocol -
    //    just ends the session.
    {
        long n = dlr_frame_recv(s.sock, s.frame, DLR_MAX_FRAME, 5000);
        if (n < 5 || n > 4 + 64 || memcmp(s.frame, "KEY:", 4) != 0) { rc = 0; goto out; }

        char text[80];
        memcpy(text, s.frame + 4, (size_t)n - 4);
        text[n - 4] = '\0';

        uint8_t key[DLR_KEY_LEN + 4];
        long klen = dlr_b64_decode(text, key, sizeof(key));
        if (klen != DLR_KEY_LEN) goto out;
        memcpy(s.key, key, DLR_KEY_LEN);
        memset(key, 0, sizeof(key));
        s.encrypted = 1;
    }

    // 3. Password, if configured. Like the client, these frames are
    //    plain: the key is not used for anything until after this.
    if (cfg->pw_hash[0]) {
        static const uint8_t auth_req[1] = { DLR_MSG_AUTH_REQUEST };
        if (!dlr_frame_send(s.sock, auth_req, 1)) goto out;

        long n = dlr_frame_recv(s.sock, s.frame, DLR_MAX_FRAME, 30000);
        if (n < 0 || n > 256) goto out;

        char pw[257];
        memcpy(pw, s.frame, (size_t)n);
        pw[n] = '\0';

        char got[65];
        hex_of_password(pw, got);
        memset(pw, 0, sizeof(pw));

        if (ct_equal(got, cfg->pw_hash)) {
            static const uint8_t ack[1] = { DLR_MSG_HELLO_ACK };
            if (!dlr_frame_send(s.sock, ack, 1)) goto out;
        } else {
            uint8_t err[1 + 21];
            err[0] = DLR_MSG_ERROR;
            memcpy(err + 1, "Authentication failed", 21);
            dlr_frame_send(s.sock, err, sizeof(err));
            // Slow down guessing. This process serves one client, so
            // sleeping costs nobody else anything.
            dlr_sleep_ms(1000);
            rc = 0;
            goto out;
        }
    }

    // 4. Requests.
    for (;;) {
        uint8_t type = 0;
        uint32_t len = 0;
        if (!dlr_recv_msg(&s, &type, &len, DLR_CLIENT_IDLE_MS)) { rc = 0; break; }

        // The body lives in s.plain and the next dlr_send_msg
        // overwrites it, so copy out what the handler needs first.
        char arg[128];
        uint32_t alen = len < sizeof(arg) - 1 ? len : (uint32_t)sizeof(arg) - 1;
        memcpy(arg, s.plain + 1, alen);
        arg[alen] = '\0';

        int keep = 1;
        switch (type) {
            case DLR_MSG_INSTALL_REQUEST: keep = handle_install(&s, arg, peer_ip); break;
            case DLR_MSG_SEARCH_REQUEST:  keep = handle_search(&s, arg); break;
            case DLR_MSG_PKG_LIST:        keep = handle_list(&s); break;
            case DLR_MSG_PING:            keep = dlr_send_msg(&s, DLR_MSG_PONG, arg, alen); break;
            default: break;               // unknown type: ignored, as the C++ server does
        }
        if (!keep) { rc = 1; break; }
    }

out:
    dlr_disconnect(&s);          // closes the connection and wipes the key
    dlr_session_free(&s);
    return rc;
}

/* --- per-connection process --------------------------------------------- */

int dlr_server_child(int argc, char** argv) {
    // argv: <self> --conn <handle> <ip> [--name N] [--pwhash H]
    if (argc < 4 || strcmp(argv[1], "--conn") != 0) return 2;

    long conn = atoi(argv[2]);

    uint32_t ip = 0;
    {
        const char* p = argv[3];
        for (int part = 0; part < 4; part++) {
            uint32_t v = 0;
            while (*p >= '0' && *p <= '9') { v = v * 10 + (uint32_t)(*p - '0'); p++; }
            ip = (ip << 8) | (v & 255);
            if (*p == '.') p++;
        }
    }

    dlr_server_cfg cfg;
    dlr_server_cfg_default(&cfg);
    const char* bad = NULL;
    if (!dlr_server_parse_args(argc, argv, 4, &cfg, &bad)) {
        dlr_tcp_close(conn);
        return 2;
    }
    return dlr_server_session(conn, &cfg, ip);
}

/* --- accept loop -------------------------------------------------------- */

static volatile int g_stop = 0;
static void on_interrupt(void) { g_stop = 1; }

int dlr_server_run(const dlr_server_cfg* cfg) {
    long listener = dlr_tcp_listen(cfg->port);
    if (listener == DLR_INVALID) {
        printf("dlr: cannot listen on port %u (already in use, or no network)\n", cfg->port);
        return 1;
    }

    // Discovery hello. Optional: without it clients can still add the
    // server by address.
    long udp = dlr_udp_open(DLR_HELLO_SRC_PORT);
    if (udp == DLR_INVALID) printf("dlr: discovery broadcast disabled (could not open UDP)\n");

    char hello[DLR_NAME_MAX + 32];
    {
        size_t p = put_str(hello, sizeof(hello), 0, "DLR|");
        p = put_str(hello, sizeof(hello), p, cfg->name);
        p = put_str(hello, sizeof(hello), p, cfg->pw_hash[0] ? "|1|" : "|0|");
        p = put_uint(hello, sizeof(hello), p, cfg->port);
        put_str(hello, sizeof(hello), p, "\n");
    }

    // What every child needs to know. Passed on the command line as the
    // password HASH, never the password.
    const char* child_args[4];
    int child_argc = 0;
    child_args[child_argc++] = "--name";
    child_args[child_argc++] = cfg->name;
    if (cfg->pw_hash[0]) {
        child_args[child_argc++] = "--pwhash";
        child_args[child_argc++] = cfg->pw_hash;
    }

    long children[DLR_MAX_CLIENTS];
    for (int i = 0; i < DLR_MAX_CLIENTS; i++) children[i] = 0;

    dlr_on_interrupt(on_interrupt);

    printf("dlr: serving '%s' on port %u%s (Ctrl+C to stop)\n",
           cfg->name, cfg->port, cfg->pw_hash[0] ? ", password required" : "");

    uint64_t next_hello = 0, next_reap = 0;
    int accept_errors = 0;

    while (!g_stop) {
        uint64_t now = dlr_now_ms();

        if (udp != DLR_INVALID && now >= next_hello) {
            dlr_udp_broadcast(udp, 4243, hello, (uint32_t)strlen(hello));
            next_hello = now + DLR_HELLO_INTERVAL_MS;
        }

        // Forget children that have finished. Listing processes is a
        // syscall, so not on every 10 ms pass.
        if (now >= next_reap) {
            for (int i = 0; i < DLR_MAX_CLIENTS; i++) {
                if (children[i] && !dlr_proc_alive(children[i])) children[i] = 0;
            }
            next_reap = now + 250;
        }

        int active = 0, free_slot = -1;
        for (int i = 0; i < DLR_MAX_CLIENTS; i++) {
            if (children[i]) active++;
            else if (free_slot < 0) free_slot = i;
        }

        int got = 0;
        if (free_slot >= 0) {
            long conn = DLR_INVALID;
            uint32_t ip = 0;
            int r = dlr_tcp_accept(listener, &conn, &ip);

            if (r == 1) {
                got = 1;
                accept_errors = 0;
                long pid = dlr_spawn_conn(conn, ip, child_args, child_argc);
                if (pid == DLR_INVALID) {
                    printf("dlr: could not start a handler process; dropping a client\n");
                    dlr_tcp_close(conn);
                } else {
                    children[free_slot] = pid;
                }
            } else if (r < 0) {
                // A persistent error means the listener is gone (the
                // kernel reclaimed it, or the NIC went away).
                if (++accept_errors > 50) { printf("dlr: listener failed, stopping\n"); break; }
            }
        }

        // With every slot busy there is nothing to accept, so idle
        // gently; otherwise poll fast enough that a connect does not
        // wait noticeably.
        dlr_sleep_ms(got ? 1 : (free_slot >= 0 ? 10 : 50));
    }

    printf("dlr: stopping\n");
    dlr_tcp_unlisten(listener);
    if (udp != DLR_INVALID) dlr_udp_close(udp);
    return 0;
}
