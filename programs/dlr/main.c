/*
 * dlr - Deliver LAN package manager, Minimal-OS client.
 *
 * Speaks the same wire protocol as the Linux/Windows `dlr`, against an
 * unmodified dlr_server. It is a separate implementation rather than a
 * port of client.cpp, because that file is C++17 with std::string,
 * std::thread, OpenSSL, libcurl and std::filesystem, none of which
 * exist here. The protocol is the contract; the code is not shared.
 *
 * Build:  ./build.sh programs/dlr
 * Import: dlr.run into MinimaFS
 * Usage:  dhcp                       (terminal, once per boot)
 *         run 0:/programs/dlr.run scan
 *         run 0:/programs/dlr.run install hello-deliver
 *
 * The same program is also the SERVER (`serve`, `present`, ...): see
 * dlr_server.h. It is one binary because the server starts a copy of
 * itself for every client, and because build.sh links every .c in this
 * folder into a single .run anyway.
 */

#include "dlr.h"
#include "dlr_port.h"
#include "dlr_proto.h"
#include "dlr_pkg.h"
#include "dlr_db.h"
#include "dlr_tar.h"
#include "dlr_mpkg.h"
#include "dlr_crypto.h"
#include "dlr_registry.h"
#include "dlr_server.h"

#include "minimalos.h"
#include "stdio.h"
#include "stdlib.h"
#include "net.h"
#include <string.h>

static dlr_server g_servers[DLR_MAX_SERVERS];
static int g_server_count = 0;

static void print_ip(uint32_t ip) {
    char buf[16];
    mos_ip_to_string(ip, buf);
    printf("%s", buf);
}

/* --- server selection --------------------------------------------------- */

static void load_servers(void) {
    g_server_count = dlr_db_load(g_servers, DLR_MAX_SERVERS);
}

// Picks a server: by name if `wanted` is given, else the first known.
// Parses "ip" or "ip:port" into an ad-hoc entry. Returns 0 if `text`
// is not a literal address at all (so the caller can fall through to
// name matching) - a bad PORT after a good IP is still an error,
// reported here rather than silently ignored.
static int parse_address(const char* text, dlr_server* out) {
    char host[64];
    size_t i = 0;
    while (text[i] && text[i] != ':' && i + 1 < sizeof(host)) { host[i] = text[i]; i++; }
    host[i] = '\0';

    uint32_t ip = mos_ip_parse(host);
    if (!ip) return 0;

    memset(out, 0, sizeof(*out));
    out->ip = ip;
    out->port = DLR_DEFAULT_PORT;

    if (text[i] == ':') {
        uint32_t port = 0;
        int digits = 0;
        for (size_t j = i + 1; text[j]; j++) {
            if (text[j] < '0' || text[j] > '9') {
                printf("dlr: bad port in '%s'\n", text);
                return -1;
            }
            port = port * 10u + (uint32_t)(text[j] - '0');
            digits++;
        }
        if (digits == 0 || port == 0 || port > 65535) {
            printf("dlr: bad port in '%s'\n", text);
            return -1;
        }
        out->port = (uint16_t)port;
    }
    return 1;
}

// `wanted` is checked as a literal address FIRST: an address is
// unambiguous by construction, while a name is not - dlr_discover()
// and `dlr add` both key entries by (ip, port), so the same server
// reached two ways (its LAN address from a broadcast, 127.0.0.1 added
// by hand) legitimately produces two rows with an identical name.
// Matching by name in that case would silently pick whichever row
// happened to be added first; listing the collision and asking for an
// address instead is the only answer that is not a coin flip.
static dlr_server* pick_server(const char* wanted) {
    if (g_server_count == 0) {
        printf("dlr: no servers known. Run 'dlr scan', or 'dlr add <ip>'.\n");
        return 0;
    }
    if (!wanted) return &g_servers[0];

    static dlr_server ad_hoc;
    int addr_rc = parse_address(wanted, &ad_hoc);
    if (addr_rc < 0) return 0;          // parse_address already printed why
    if (addr_rc > 0) return &ad_hoc;

    int match = -1;
    int match_count = 0;
    for (int i = 0; i < g_server_count; i++) {
        if (strcmp(g_servers[i].name, wanted) == 0) {
            if (match_count == 0) match = i;
            match_count++;
        }
    }

    if (match_count == 1) return &g_servers[match];

    if (match_count > 1) {
        printf("dlr: %d servers are named '%s' - specify one by address:\n",
               match_count, wanted);
        for (int i = 0; i < g_server_count; i++) {
            if (strcmp(g_servers[i].name, wanted) != 0) continue;
            printf("  ");
            print_ip(g_servers[i].ip);
            printf(":%u\n", g_servers[i].port);
        }
        return 0;
    }

    printf("dlr: no server named '%s'\n", wanted);
    return 0;
}

static int open_session(dlr_session* s, dlr_server* srv, const char* password) {
    if (dlr_session_alloc(s) != 0) {
        printf("dlr: out of memory for session buffers\n");
        return 0;
    }

    printf("Connecting to ");
    print_ip(srv->ip);
    printf(":%u ...\n", srv->port);

    if (!dlr_connect(s, srv->ip, srv->port, password)) {
        printf("dlr: connection or handshake failed\n");
        dlr_session_free(s);
        return 0;
    }
    if (srv->needs_password && !password) {
        printf("dlr: this server requires a password (pass it as the last argument)\n");
        dlr_disconnect(s);
        dlr_session_free(s);
        return 0;
    }
    return 1;
}

static void close_session(dlr_session* s) {
    dlr_disconnect(s);
    dlr_session_free(s);
}

/* --- commands ----------------------------------------------------------- */

static void on_found(const dlr_server* s) {
    printf("  found '%s' at ", s->name);
    print_ip(s->ip);
    printf(":%u%s\n", s->port, s->needs_password ? "  (password required)" : "");
}

static int cmd_scan(void) {
    printf("Scanning the LAN for Deliver servers (5s) ...\n");
    load_servers();

    int before = g_server_count;
    int count = dlr_discover(g_servers, DLR_MAX_SERVERS, 5000, on_found);
    if (count < 0) {
        printf("dlr: could not bind UDP %u - is another program using it?\n",
               DLR_DISCOVERY_PORT);
        return 1;
    }
    g_server_count = count;

    if (count == before) {
        printf("No new servers found.\n");
        printf("Note: under QEMU '-netdev user' the guest is NATed and LAN\n");
        printf("broadcasts never arrive. Use a tap/bridge netdev, or\n");
        printf("'dlr add <ip>' to register a server by address.\n");
    }

    dlr_db_save(g_servers, g_server_count);
    printf("%d server(s) known.\n", g_server_count);
    return 0;
}

static int cmd_servers(void) {
    load_servers();
    if (g_server_count == 0) {
        printf("No servers known. Run 'dlr scan' or 'dlr add <ip>'.\n");
        return 0;
    }
    for (int i = 0; i < g_server_count; i++) {
        printf("  %-20s ", g_servers[i].name[0] ? g_servers[i].name : "(unnamed)");
        print_ip(g_servers[i].ip);
        printf(":%u%s\n", g_servers[i].port,
               g_servers[i].needs_password ? "  (password required)" : "");
    }
    return 0;
}

static int cmd_add(const char* spec) {
    // "<ip>" or "<ip>:<port>"
    char host[64];
    uint16_t port = DLR_DEFAULT_PORT;

    size_t i = 0;
    while (spec[i] && spec[i] != ':' && i + 1 < sizeof(host)) { host[i] = spec[i]; i++; }
    host[i] = '\0';
    if (spec[i] == ':') {
        uint32_t v = 0;
        for (size_t j = i + 1; spec[j] >= '0' && spec[j] <= '9'; j++) v = v * 10u + (uint32_t)(spec[j] - '0');
        if (v) port = (uint16_t)v;
    }

    uint32_t ip = dlr_resolve(host);
    if (!ip) {
        printf("dlr: cannot resolve '%s'\n", host);
        return 1;
    }

    printf("Probing ");
    print_ip(ip);
    printf(":%u ...\n", port);

    dlr_server found;
    if (!dlr_probe(ip, port, &found, 4000)) {
        printf("dlr: no Deliver server answered there\n");
        return 1;
    }

    load_servers();
    g_server_count = dlr_db_add(g_servers, g_server_count, DLR_MAX_SERVERS, &found);
    dlr_db_save(g_servers, g_server_count);

    printf("Added '%s'%s\n", found.name,
           found.needs_password ? " (password required)" : "");
    return 0;
}

// Both `list` and `search` get back "name|version|description\n" lines.
static void print_pkg_lines(const char* body, uint32_t len) {
    if (len == 0 || (len == 4 && memcmp(body, "NONE", 4) == 0)) {
        printf("  (nothing)\n");
        return;
    }

    char line[256];
    size_t n = 0;
    int shown = 0;

    for (uint32_t i = 0; i <= len; i++) {
        char c = (i < len) ? body[i] : '\n';
        if (c != '\n') {
            if (n + 1 < sizeof(line)) line[n++] = c;
            continue;
        }
        line[n] = '\0';
        n = 0;
        if (!line[0]) continue;

        char* v = strchr(line, '|');
        if (!v) { printf("  %s\n", line); shown++; continue; }
        *v++ = '\0';
        char* d = strchr(v, '|');
        if (d) *d++ = '\0';

        printf("  %-24s %-10s %s\n", line, v, d ? d : "");
        shown++;
    }
    if (!shown) printf("  (nothing)\n");
}

static int cmd_list(const char* server_name, const char* password) {
    load_servers();
    dlr_server* srv = pick_server(server_name);
    if (!srv) return 1;

    dlr_session s;
    if (!open_session(&s, srv, password)) return 1;

    int rc = 1;
    if (dlr_send_msg(&s, DLR_MSG_PKG_LIST, 0, 0)) {
        uint8_t type;
        uint32_t len;
        if (dlr_recv_msg(&s, &type, &len, 8000) && type == DLR_MSG_PKG_LIST) {
            printf("Packages on '%s':\n", s.server_name);
            print_pkg_lines((const char*)s.plain + 1, len);
            rc = 0;
        } else {
            printf("dlr: no usable reply\n");
        }
    }

    close_session(&s);
    return rc;
}

static int cmd_search(const char* query, const char* server_name, const char* password) {
    load_servers();
    dlr_server* srv = pick_server(server_name);
    if (!srv) return 1;

    dlr_session s;
    if (!open_session(&s, srv, password)) return 1;

    int rc = 1;
    if (dlr_send_msg(&s, DLR_MSG_SEARCH_REQUEST, query, (uint32_t)strlen(query))) {
        uint8_t type;
        uint32_t len;
        if (dlr_recv_msg(&s, &type, &len, 8000) && type == DLR_MSG_SEARCH_RESULT) {
            printf("Results for '%s':\n", query);
            print_pkg_lines((const char*)s.plain + 1, len);
            rc = 0;
        } else {
            printf("dlr: no usable reply\n");
        }
    }

    close_session(&s);
    return rc;
}

static int cmd_ping(const char* server_name, const char* password) {
    load_servers();
    dlr_server* srv = pick_server(server_name);
    if (!srv) return 1;

    dlr_session s;
    if (!open_session(&s, srv, password)) return 1;

    int rc = 1;
    for (int i = 0; i < 3; i++) {
        long ms = dlr_ping(&s);
        if (ms < 0) { printf("  no reply\n"); break; }
        printf("  pong from '%s' in %d ms\n", s.server_name, (int)ms);
        rc = 0;
        dlr_sleep_ms(300);
    }

    close_session(&s);
    return rc;
}

/* --- install ------------------------------------------------------------ */

typedef struct {
    const char* target_dir;
    const dlr_pkg* pkg;
    const char* requested_name;  /* the name passed to `dlr install` */
    int only_listed;         /* honour pkg->mos_copy */
    int written;
    int skipped;
} install_ctx;

static int name_in_copy_list(const dlr_pkg* pkg, const char* name) {
    const char* cursor = pkg->mos_copy;
    char entry[128];
    while (dlr_pkg_next_copy_entry(&cursor, entry, sizeof(entry))) {
        if (strcmp(entry, name) == 0) return 1;
    }
    return 0;
}

/*
 * Reduces a tar entry to the path a package author actually wrote in
 * `copy=`. Two wrapper conventions exist in the wild and both get
 * stripped here, in order:
 *
 *  - `tar -cf x -C dir .` (a hand-built package, or the SDK's own
 *    build.sh output) prefixes every entry with "./".
 *
 *  - `dlr_server presentfolder <dir> <name>` - the common case, and
 *    what a real dlr_server does - tars from its data directory with
 *    every entry prefixed "<name>/", one path component. Stripping it
 *    only when it equals the package's own name (not blindly stripping
 *    the first component of every archive) avoids mangling a package
 *    that has a real top-level directory of its own.
 *
 * The result is used for both copy= matching and the installed path,
 * so a package author writing `copy=hello.txt` and a client unpacking
 * `mostest/hello.txt` agree on what "hello.txt" means.
 */
static const char* canonical_rel(const char* name, const char* pkg_name,
                                 const char* requested_name,
                                 char* scratch, size_t scratch_size) {
    if (name[0] == '.' && name[1] == '/') name += 2;

    // The server always tars from its data directory using the NAME
    // IT WAS PRESENTED AS (build_tar() in package_registry.cpp), which
    // is the name the user passed to `dlr install`, not necessarily
    // the manifest's own [Info] name= - those are usually the same but
    // are not required to be. Try the requested name first since it is
    // the one guaranteed to match server behaviour; fall back to the
    // manifest name for a hand-built archive with no server involved.
    const char* candidates[2] = { requested_name, pkg_name };
    for (int i = 0; i < 2; i++) {
        const char* cand = candidates[i];
        if (!cand || !cand[0]) continue;
        size_t plen = strlen(cand);
        if (strncmp(name, cand, plen) == 0 && name[plen] == '/') {
            name += plen + 1;
            break;
        }
    }

    if (!name[0]) return ".";
    size_t n = strlen(name);
    if (n >= scratch_size) n = scratch_size - 1;
    memcpy(scratch, name, n);
    scratch[n] = '\0';
    return scratch;
}

static int join_path(char* out, size_t cap, const char* dir, const char* rel) {
    size_t n = 0;
    for (const char* p = dir; *p && n + 1 < cap; p++) out[n++] = *p;
    if (n && out[n - 1] != '/' && n + 1 < cap) out[n++] = '/';
    for (const char* p = rel; *p && n + 1 < cap; p++) out[n++] = *p;
    if (n + 1 >= cap) return 0;
    out[n] = '\0';
    return 1;
}

// Creates every parent directory of a file path.
static int ensure_parent(const char* path) {
    char dir[256];
    size_t n = strlen(path);
    if (n >= sizeof(dir)) return 0;

    memcpy(dir, path, n + 1);
    char* slash = 0;
    for (size_t i = 0; i < n; i++) if (dir[i] == '/') slash = dir + i;
    if (!slash) return 1;
    *slash = '\0';
    return dlr_mkdirs(dir);
}

static int install_on_dir(void* user, const char* name) {
    install_ctx* ctx = (install_ctx*)user;
    char scratch[256];
    const char* rel = canonical_rel(name, ctx->pkg->name, ctx->requested_name,
                                    scratch, sizeof(scratch));
    if (!rel[0] || strcmp(rel, ".") == 0) return 1;

    char full[256];
    if (!join_path(full, sizeof(full), ctx->target_dir, rel)) return 1;
    dlr_mkdirs(full);
    return 1;
}

static int install_on_file(void* user, const char* name, const uint8_t* data, uint32_t len) {
    install_ctx* ctx = (install_ctx*)user;
    char scratch[256];
    const char* rel = canonical_rel(name, ctx->pkg->name, ctx->requested_name,
                                    scratch, sizeof(scratch));

    // The manifest travels inside the bundle; it is metadata, not
    // payload, so it does not get installed.
    size_t rel_len = strlen(rel);
    if (rel_len > 4 && strcmp(rel + rel_len - 4, ".pkg") == 0) return 1;

    if (ctx->only_listed && !name_in_copy_list(ctx->pkg, rel)) {
        ctx->skipped++;
        return 1;
    }

    char full[256];
    if (!join_path(full, sizeof(full), ctx->target_dir, rel)) {
        printf("  ! path too long: %s\n", rel);
        return 1;
    }
    if (!ensure_parent(full)) {
        printf("  ! cannot create directory for %s\n", rel);
        return 1;
    }

    if (!dlr_file_put(full, data, len)) {
        printf("  ! write failed: %s\n", full);
        return 0;                       // aborts extraction
    }

    printf("  -> %s (%u bytes)\n", full, len);
    ctx->written++;
    return 1;
}

/* First pass: locate and parse the .pkg manifest. */
typedef struct {
    dlr_pkg* pkg;
    int found;
} manifest_ctx;

static int manifest_on_file(void* user, const char* name, const uint8_t* data, uint32_t len) {
    manifest_ctx* ctx = (manifest_ctx*)user;
    const char* rel = (name[0] == '.' && name[1] == '/') ? name + 2 : name;
    size_t n = strlen(rel);
    if (n > 4 && strcmp(rel + n - 4, ".pkg") == 0 && !ctx->found) {
        ctx->found = dlr_pkg_parse((const char*)data, len, ctx->pkg);
    }
    return 1;
}

static int manifest_on_dir(void* user, const char* name) {
    (void)user; (void)name;
    return 1;
}

static uint64_t g_last_report = 0;

static void progress(uint64_t received, uint64_t total) {
    // One line per 64 KiB: the terminal is slow and a per-chunk
    // repaint costs more than the download does.
    if (received && (received - g_last_report) < 65536 && received != total) return;
    g_last_report = received;

    if (total) {
        printf("  %u / %u KiB\n", (unsigned)(received / 1024), (unsigned)(total / 1024));
    } else {
        printf("  %u KiB\n", (unsigned)(received / 1024));
    }
}

static int cmd_install(const char* pkg_name, const char* server_name,
                       const char* password, int force) {
    load_servers();
    dlr_server* srv = pick_server(server_name);
    if (!srv) return 1;

    if (!dlr_mkdirs(DLR_CACHE_DIR)) {
        printf("dlr: cannot create %s\n", DLR_CACHE_DIR);
        return 1;
    }

    dlr_session s;
    if (!open_session(&s, srv, password)) return 1;

    printf("Requesting '%s' from '%s' ...\n", pkg_name, s.server_name);
    g_last_report = 0;

    char err[128];
    err[0] = '\0';
    int format = DLR_FORMAT_TAR;
    int rc = dlr_download(&s, pkg_name, DLR_STAGE_FILE, progress, &format, err, sizeof(err));
    close_session(&s);

    if (rc == -1) {
        printf("dlr: server refused: %s\n", err);
        return 1;
    }
    if (rc != 1) {
        printf("dlr: download failed: %s\n", err[0] ? err : "unknown error");
        return 1;
    }
    printf("Download verified (SHA-256 matches).\n");

    long size = dlr_file_size(DLR_STAGE_FILE);
    if (size <= 0) {
        printf("dlr: staged file is empty\n");
        return 1;
    }

    uint8_t* archive = (uint8_t*)malloc((size_t)size);
    if (!archive) {
        printf("dlr: out of memory for a %u KiB archive\n", (unsigned)(size / 1024));
        return 1;
    }
    if (dlr_file_slurp(DLR_STAGE_FILE, archive, (uint32_t)size) != size) {
        printf("dlr: cannot read staged archive\n");
        free(archive);
        return 1;
    }

    // A Minimal-OS server says "FORMAT=mpkg"; every other server sends
    // a tar without saying so. Trust the magic bytes over the header
    // if they disagree - an .mpkg is unmistakable and a tar never
    // starts with them, so the file itself is the better witness.
    if (dlr_mpkg_is_archive(archive, (size_t)size)) format = DLR_FORMAT_MPKG;
    long (*extract)(const uint8_t*, size_t, const dlr_tar_sink*) =
        (format == DLR_FORMAT_MPKG) ? dlr_mpkg_extract_mem : dlr_tar_extract_mem;
    const char* format_name = (format == DLR_FORMAT_MPKG) ? ".mpkg" : "tar";

    // Pass 1: manifest.
    dlr_pkg pkg;
    manifest_ctx mctx = { &pkg, 0 };
    dlr_tar_sink msink = { manifest_on_file, manifest_on_dir, &mctx };
    if (extract(archive, (size_t)size, &msink) < 0) {
        printf("dlr: the package is not a readable %s archive\n", format_name);
        free(archive);
        return 1;
    }

    if (!mctx.found) {
        printf("dlr: no .pkg manifest inside the bundle\n");
        free(archive);
        return 1;
    }

    printf("Package: %s %s\n", pkg.name, pkg.version[0] ? pkg.version : "");
    if (pkg.description[0]) printf("  %s\n", pkg.description);

    const char* why = "";
    if (!dlr_pkg_runs_here(&pkg, &why)) {
        printf("dlr: %s\n", why);
        printf("     (arch=%s os=%s)\n",
               pkg.arch[0] ? pkg.arch : "any",
               pkg.operatingsystem[0] ? pkg.operatingsystem : "any");
        if (!force) {
            printf("     Refusing. Pass --force to install anyway.\n");
            free(archive);
            return 1;
        }
        printf("     --force given; installing regardless.\n");
    }

    if (!pkg.has_minimalos_section) {
        // Not fatal: a package of plain data files installs fine with
        // the defaults. It is worth saying out loud, though, because
        // anything relying on installscript/installcommand will not
        // have run.
        printf("Note: no [Install.minimalos] section; copying everything to %s.\n",
               DLR_INSTALL_DIR);
    }

    install_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.pkg = &pkg;
    ctx.requested_name = pkg_name;
    ctx.target_dir = pkg.mos_target[0] ? pkg.mos_target : DLR_INSTALL_DIR;
    ctx.only_listed = pkg.mos_copy[0] ? 1 : 0;

    if (!dlr_mkdirs(ctx.target_dir)) {
        printf("dlr: cannot create %s\n", ctx.target_dir);
        free(archive);
        return 1;
    }

    printf("Installing into %s ...\n", ctx.target_dir);
    dlr_tar_sink isink = { install_on_file, install_on_dir, &ctx };
    long entries = extract(archive, (size_t)size, &isink);
    free(archive);

    if (entries < 0) {
        printf("dlr: extraction aborted\n");
        return 1;
    }

    printf("Installed %d file(s)%s.\n", ctx.written,
           ctx.skipped ? " (some skipped by copy=)" : "");

    dlr_remove(DLR_STAGE_FILE);

    if (pkg.mos_run[0]) {
        char run_path[256];
        const char* target = pkg.mos_run;

        // An absolute MinimaFS path is used as-is; a bare name is
        // resolved inside the install directory. A package cannot ask
        // us to run something it did not ship.
        if (strchr(target, ':')) {
            size_t n = strlen(target);
            if (n >= sizeof(run_path)) { printf("dlr: run= path too long\n"); return 1; }
            memcpy(run_path, target, n + 1);
        } else if (!join_path(run_path, sizeof(run_path), ctx.target_dir, target)) {
            printf("dlr: run= path too long\n");
            return 1;
        }

        if (!dlr_exists(run_path)) {
            printf("dlr: run= names '%s', which the package did not install\n", run_path);
            return 1;
        }

        printf("Running %s ...\n", run_path);
        long pid = dlr_exec(run_path);
        if (pid == DLR_INVALID) {
            printf("dlr: could not launch it (SYS_EXEC failed)\n");
            return 1;
        }
        if (!dlr_exec_wait(pid, 30000)) {
            printf("dlr: post-install program is still running after 30s; leaving it.\n");
        }
    }

    printf("Done.\n");
    return 0;
}

/* --- server commands ---------------------------------------------------- */

static int print_entry_cb(void* user, const dlr_reg_entry* e) {
    (*(int*)user)++;
    printf("  %-24s %-10s %s\n", e->name, e->version[0] ? e->version : "-", e->description);
    return 1;
}

static int cmd_packages(void) {
    printf("Packages presented from %s:\n", dlr_reg_root());
    int n = 0;
    dlr_reg_each(print_entry_cb, &n);
    if (n == 0) printf("  (none - use 'dlr present <dir>')\n");
    return 0;
}

static int cmd_present(const char* dir, const char* name) {
    char err[128];
    err[0] = '\0';
    printf("Presenting %s ...\n", dir);
    if (!dlr_reg_present(dir, name, err, sizeof(err))) {
        printf("dlr: cannot present: %s\n", err[0] ? err : "unknown error");
        return 1;
    }
    printf("Presented. Clients can now install it; 'dlr serve' to start serving.\n");
    return 0;
}

static int cmd_unpresent(const char* name) {
    if (!dlr_reg_remove(name)) { printf("dlr: no package '%s' in the store\n", name); return 1; }
    printf("Removed '%s'.\n", name);
    return 0;
}

static int cmd_rebuild(const char* name) {
    char err[128];
    err[0] = '\0';
    if (!dlr_reg_rebuild(name, err, sizeof(err))) {
        printf("dlr: cannot rebuild: %s\n", err[0] ? err : "unknown error");
        return 1;
    }
    printf("Rebuilt the archive for '%s'.\n", name);
    return 0;
}

static int cmd_serve(int argc, char** argv) {
    dlr_server_cfg cfg;
    dlr_server_cfg_default(&cfg);
    const char* bad = 0;
    if (!dlr_server_parse_args(argc, argv, 2, &cfg, &bad)) {
        printf("dlr: bad or incomplete option '%s'\n", bad ? bad : "?");
        printf("usage: dlr serve [--name <n>] [--port <p>] [--password <pw>]\n");
        return 1;
    }
    return dlr_server_run(&cfg);
}

/* --- entry point -------------------------------------------------------- */

static void usage(void) {
    printf("dlr - Deliver LAN package manager (Minimal-OS client)\n\n");
    printf("  dlr scan                       find servers on the LAN\n");
    printf("  dlr servers                    list known servers\n");
    printf("  dlr add <ip>[:port]            register a server by address\n");
    printf("  dlr list [server]              list a server's packages\n");
    printf("  dlr search <query> [server]    search packages\n");
    printf("  dlr ping [server]              measure round-trip time\n");
    printf("  dlr install <pkg> [server]     download, verify and install\n\n");
    printf("Serving packages to other machines:\n");
    printf("  dlr present <dir> [name]       add a package directory to this server's store\n");
    printf("  dlr packages                   list what this server offers\n");
    printf("  dlr unpresent <name>           remove one\n");
    printf("  dlr rebuild <name>             rebuild its archive after editing its files\n");
    printf("  dlr serve [--name n] [--port p] [--password pw]\n\n");
    printf("Options: --password <pw>   --force\n");
    printf("Run 'dhcp' in the terminal once per boot before using dlr.\n");
}

int main(int argc, char** argv) {
    // argv[0] is the .run path, so the command is argv[1].
    if (argc < 2) { usage(); return 0; }

    // The server starts a copy of this program per client; that copy
    // needs the network but none of the client's option parsing.
    dlr_set_self(argv[0]);
    if (strcmp(argv[1], "--conn") == 0) {
        if (dlr_port_init() != 0) return 1;
        return dlr_server_child(argc, argv);
    }

    // `serve` has its own options (--name/--port/--password), so it is
    // dispatched before the generic loop below can misfile them as
    // positionals.
    if (strcmp(argv[1], "serve") == 0) {
        if (dlr_port_init() != 0) return 1;
        return cmd_serve(argc, argv);
    }

    const char* password = 0;
    int force = 0;

    const char* positional[4] = { 0, 0, 0, 0 };
    int pos_count = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--password") == 0 && i + 1 < argc) {
            password = argv[++i];
        } else if (strcmp(argv[i], "--force") == 0) {
            force = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage();
            return 0;
        } else if (pos_count < 4) {
            positional[pos_count++] = argv[i];
        }
    }

    if (pos_count == 0) { usage(); return 0; }
    const char* cmd = positional[0];

    // Managing the local store is disk-only; it must work before 'dhcp'.
    if (strcmp(cmd, "packages") == 0) return cmd_packages();
    if (strcmp(cmd, "present") == 0) {
        if (pos_count < 2) { printf("usage: dlr present <dir> [name]\n"); return 1; }
        return cmd_present(positional[1], positional[2]);
    }
    if (strcmp(cmd, "unpresent") == 0) {
        if (pos_count < 2) { printf("usage: dlr unpresent <name>\n"); return 1; }
        return cmd_unpresent(positional[1]);
    }
    if (strcmp(cmd, "rebuild") == 0) {
        if (pos_count < 2) { printf("usage: dlr rebuild <name>\n"); return 1; }
        return cmd_rebuild(positional[1]);
    }

    if (dlr_port_init() != 0) return 1;

    if (strcmp(cmd, "scan") == 0)    return cmd_scan();
    if (strcmp(cmd, "servers") == 0) return cmd_servers();

    if (strcmp(cmd, "add") == 0) {
        if (pos_count < 2) { printf("usage: dlr add <ip>[:port]\n"); return 1; }
        return cmd_add(positional[1]);
    }
    if (strcmp(cmd, "list") == 0)   return cmd_list(positional[1], password);
    if (strcmp(cmd, "ping") == 0)   return cmd_ping(positional[1], password);

    if (strcmp(cmd, "search") == 0) {
        if (pos_count < 2) { printf("usage: dlr search <query> [server]\n"); return 1; }
        return cmd_search(positional[1], positional[2], password);
    }
    if (strcmp(cmd, "install") == 0) {
        if (pos_count < 2) { printf("usage: dlr install <pkg> [server]\n"); return 1; }
        return cmd_install(positional[1], positional[2], password, force);
    }

    printf("dlr: unknown command '%s'\n\n", cmd);
    usage();
    return 1;
}
