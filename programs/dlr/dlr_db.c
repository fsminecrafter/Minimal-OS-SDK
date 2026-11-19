#include "dlr_db.h"
#include "dlr_port.h"
#include "dlr_proto.h"
#include <string.h>
#include <stdio.h>

static void append_uint(char* buf, size_t cap, size_t* pos, uint32_t v) {
    char tmp[12];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n-- > 0 && *pos + 1 < cap) buf[(*pos)++] = tmp[n];
}

static uint32_t read_uint(const char** p) {
    uint32_t v = 0;
    while (**p == ' ') (*p)++;
    while (**p >= '0' && **p <= '9') { v = v * 10u + (uint32_t)(**p - '0'); (*p)++; }
    return v;
}

int dlr_db_load(dlr_server* servers, int max) {
    static char buf[4096];
    long n = dlr_file_slurp(DLR_SERVERS_FILE, buf, sizeof(buf) - 1);
    if (n <= 0) return 0;
    buf[n] = '\0';

    int count = 0;
    const char* p = buf;

    // One record per line: "<ip-as-uint32> <port> <needs_pw> <name>"
    while (*p && count < max) {
        dlr_server s;
        memset(&s, 0, sizeof(s));

        s.ip = read_uint(&p);
        s.port = (uint16_t)read_uint(&p);
        s.needs_password = (uint8_t)read_uint(&p);

        while (*p == ' ') p++;
        size_t i = 0;
        while (*p && *p != '\n' && i + 1 < sizeof(s.name)) s.name[i++] = *p++;
        s.name[i] = '\0';
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;

        if (s.ip && s.port) servers[count++] = s;
    }
    return count;
}

int dlr_db_save(const dlr_server* servers, int count) {
    static char buf[4096];
    size_t pos = 0;

    for (int i = 0; i < count && pos + 128 < sizeof(buf); i++) {
        append_uint(buf, sizeof(buf), &pos, servers[i].ip);
        buf[pos++] = ' ';
        append_uint(buf, sizeof(buf), &pos, servers[i].port);
        buf[pos++] = ' ';
        append_uint(buf, sizeof(buf), &pos, servers[i].needs_password);
        buf[pos++] = ' ';
        for (const char* n = servers[i].name; *n && pos + 2 < sizeof(buf); n++) {
            if (*n != '\n') buf[pos++] = *n;
        }
        buf[pos++] = '\n';
    }

    if (!dlr_mkdirs(DLR_CONFIG_DIR)) return 0;
    return dlr_file_put(DLR_SERVERS_FILE, buf, (uint32_t)pos);
}

int dlr_db_add(dlr_server* servers, int count, int max, const dlr_server* add) {
    for (int i = 0; i < count; i++) {
        if (servers[i].ip == add->ip && servers[i].port == add->port) {
            // Refresh name and auth flag; the server may have been
            // renamed or had a password added since the last scan.
            servers[i] = *add;
            return count;
        }
    }
    if (count >= max) return count;
    servers[count] = *add;
    return count + 1;
}

/*
 * LAN discovery.
 *
 * Two sources of truth, and we take both: the "DLR_DISCOVER\n"
 * broadcast we send (which a running server answers), and the
 * unsolicited hello every server already broadcasts on a five-second
 * timer. Listening for the second is why the scan window is long
 * enough to catch one.
 */
int dlr_discover(dlr_server* servers, int max, uint32_t window_ms,
                 void (*on_found)(const dlr_server*)) {
    long sock = dlr_udp_open(DLR_DISCOVERY_PORT);
    if (sock == DLR_INVALID) return -1;

    const char* probe = "DLR_DISCOVER\n";
    dlr_udp_broadcast(sock, DLR_DISCOVERY_PORT, probe, (uint32_t)strlen(probe));

    int count = 0;
    uint64_t deadline = dlr_now_ms() + window_ms;
    char buf[513];

    while (dlr_now_ms() < deadline && count < max) {
        uint32_t from_ip = 0;
        long n = dlr_udp_read(sock, buf, sizeof(buf) - 1, &from_ip, 400);
        if (n <= 0) continue;
        buf[n] = '\0';

        // "DLR|<name>|<needs_pw>|<tcp_port>"
        if (memcmp(buf, "DLR|", 4) != 0) continue;

        dlr_server s;
        memset(&s, 0, sizeof(s));
        s.ip = from_ip;
        s.port = DLR_DEFAULT_PORT;

        char* fields[4] = { 0, 0, 0, 0 };
        int fc = 0;
        char* p = buf;
        fields[fc++] = p;
        while (*p && fc < 4) {
            if (*p == '|') { *p = '\0'; fields[fc++] = p + 1; }
            p++;
        }
        if (fc < 4) continue;

        size_t i = 0;
        while (fields[1][i] && i + 1 < sizeof(s.name)) { s.name[i] = fields[1][i]; i++; }
        s.name[i] = '\0';
        s.needs_password = (fields[2][0] == '1');

        const char* port_str = fields[3];
        uint32_t port = read_uint(&port_str);
        if (port) s.port = (uint16_t)port;

        int before = count;
        count = dlr_db_add(servers, count, max, &s);
        if (count != before && on_found) on_found(&s);
    }

    dlr_udp_close(sock);
    return count;
}

// msg = a + b + c (truncated to size).
static void join3(char* msg, size_t size, const char* a, const char* b, const char* c) {
    size_t n = 0;
    const char* parts[3] = { a, b, c };
    for (int k = 0; k < 3; k++)
        for (const char* q = parts[k]; q && *q && n + 1 < size; q++) msg[n++] = *q;
    msg[n] = '\0';
}

// "tls://host:port" -> host, port (tls:// = DLR_TLS_PORT unless given).
static int parse_spec(const char* spec, char* host, size_t host_size, uint16_t* port) {
    int tls = 0;
    if (strncmp(spec, "tls://", 6) == 0) { tls = 1; spec += 6; }
    *port = tls ? DLR_TLS_PORT : DLR_DEFAULT_PORT;
    size_t i = 0;
    while (spec[i] && spec[i] != ':' && i + 1 < host_size) { host[i] = spec[i]; i++; }
    host[i] = '\0';
    if (spec[i] == ':') {
        uint32_t v = 0;
        for (size_t j = i + 1; spec[j] >= '0' && spec[j] <= '9'; j++) v = v * 10u + (uint32_t)(spec[j] - '0');
        if (!v || v > 65535) return 0;
        *port = (uint16_t)v;
    }
    return host[0] != '\0';
}

int dlr_db_take_queued(dlr_server* servers, int count, int max,
                       void (*say)(const char* line)) {
    static char buf[1024];
    static char keep[1024];
    long n = dlr_file_slurp(DLR_REPOS_FILE, buf, sizeof(buf) - 1);
    if (n <= 0) return count;
    buf[n] = '\0';

    size_t kept = 0;
    int added = 0;
    char* p = buf;
    while (*p) {
        char* line = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = '\0';
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\r' || line[len - 1] == ' ')) line[--len] = '\0';
        while (*line == ' ') line++;
        if (!*line || *line == '#') continue;

        char host[64], msg[160];
        uint16_t port;
        dlr_server found;
        int ok = 0;
        if (parse_spec(line, host, sizeof(host), &port)) {
            uint32_t ip = dlr_resolve(host);
            ok = ip && dlr_probe(ip, port, &found, 4000);
        } else {
            if (say) { join3(msg, sizeof(msg), "dlr: dropping bad repository '", line, "'"); say(msg); }
            continue;
        }
        if (ok) {
            count = dlr_db_add(servers, count, max, &found);
            added++;
            if (say) {
                join3(msg, sizeof(msg), "dlr: added repository ", line, " ('");
                join3(msg, sizeof(msg), msg, found.name, "')");
                say(msg);
            }
        } else {
            if (say) { join3(msg, sizeof(msg), "dlr: repository ", line, " not reachable yet; will try again"); say(msg); }
            size_t ln = strlen(line);
            if (kept + ln + 2 < sizeof(keep)) {
                memcpy(keep + kept, line, ln);
                kept += ln;
                keep[kept++] = '\n';
            }
        }
    }
    if (added) dlr_db_save(servers, count);
    dlr_file_put(DLR_REPOS_FILE, keep, (uint32_t)kept);
    return count;
}
