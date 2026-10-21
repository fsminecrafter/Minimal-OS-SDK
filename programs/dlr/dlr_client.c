#include "dlr_client.h"
#include "dlr_port.h"
#include "dlr_pkg.h"
#include "dlr_tar.h"
#include "dlr_mpkg.h"
#include <stdlib.h>
#include <string.h>

/* --- messages ----------------------------------------------------------- */

/* There is no snprintf in the SDK; messages are built from pieces. */
typedef struct {
    char   text[256];
    size_t n;
} line_buf;

static line_buf* ln_s(line_buf* l, const char* s) {
    while (s && *s && l->n + 1 < sizeof(l->text)) l->text[l->n++] = *s++;
    l->text[l->n] = '\0';
    return l;
}

static line_buf* ln_u(line_buf* l, uint64_t v) {
    char digits[24];
    int k = 0;
    do { digits[k++] = (char)('0' + v % 10u); v /= 10u; } while (v && k < 23);
    while (k > 0 && l->n + 1 < sizeof(l->text)) l->text[l->n++] = digits[--k];
    l->text[l->n] = '\0';
    return l;
}

static void ln_emit(const dlr_io* io, line_buf* l) {
    if (io && io->log) io->log(io->user, l->text);
    l->n = 0;
    l->text[0] = '\0';
}

static void say(const dlr_io* io, const char* a, const char* b, const char* c) {
    line_buf l = { {0}, 0 };
    ln_s(&l, a); ln_s(&l, b); ln_s(&l, c);
    ln_emit(io, &l);
}

void dlr_ip_format(uint32_t ip, char* out) {
    line_buf l = { {0}, 0 };
    ln_u(&l, (ip >> 24) & 0xFFu); ln_s(&l, ".");
    ln_u(&l, (ip >> 16) & 0xFFu); ln_s(&l, ".");
    ln_u(&l, (ip >> 8) & 0xFFu);  ln_s(&l, ".");
    ln_u(&l, ip & 0xFFu);
    memcpy(out, l.text, l.n + 1);
}

/* --- sessions ----------------------------------------------------------- */

int dlr_client_open(dlr_session* s, const dlr_server* srv, const char* password,
                    const dlr_io* io) {
    if (dlr_session_alloc(s) != 0) {
        say(io, "dlr: out of memory for session buffers", 0, 0);
        return 0;
    }

    char ip[16];
    dlr_ip_format(srv->ip, ip);
    line_buf l = { {0}, 0 };
    ln_emit(io, ln_s(ln_u(ln_s(ln_s(ln_s(&l, "Connecting to "), ip), ":"), srv->port), " ..."));

    if (!dlr_connect(s, srv->ip, srv->port, password)) {
        say(io, "dlr: connection or handshake failed", 0, 0);
        dlr_session_free(s);
        return 0;
    }
    if (srv->needs_password && !password) {
        say(io, "dlr: this server requires a password", 0, 0);
        dlr_disconnect(s);
        dlr_session_free(s);
        return 0;
    }
    return 1;
}

void dlr_client_close(dlr_session* s) {
    dlr_disconnect(s);
    dlr_session_free(s);
}

/* --- package lists ------------------------------------------------------ */

static void copy_field(char* dst, size_t cap, const char* src, size_t len) {
    if (len >= cap) len = cap - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

int dlr_parse_pkg_lines(const char* body, uint32_t len, dlr_pkg_info* out, int max) {
    if (len == 0 || (len == 4 && memcmp(body, "NONE", 4) == 0)) return 0;

    int count = 0;
    uint32_t start = 0;
    for (uint32_t i = 0; i <= len && count < max; i++) {
        if (i < len && body[i] != '\n') continue;
        const char* line = body + start;
        uint32_t line_len = i - start;
        start = i + 1;
        if (line_len && line[line_len - 1] == '\r') line_len--;
        if (!line_len) continue;

        // name|version|description; a line with no '|' is just a name.
        uint32_t bar1 = line_len, bar2 = line_len;
        for (uint32_t j = 0; j < line_len; j++) {
            if (line[j] != '|') continue;
            if (bar1 == line_len) bar1 = j;
            else { bar2 = j; break; }
        }

        dlr_pkg_info* p = &out[count];
        memset(p, 0, sizeof(*p));
        copy_field(p->name, sizeof(p->name), line, bar1);
        if (bar1 < line_len) {
            copy_field(p->version, sizeof(p->version), line + bar1 + 1, bar2 - bar1 - 1);
            if (bar2 < line_len)
                copy_field(p->description, sizeof(p->description),
                           line + bar2 + 1, line_len - bar2 - 1);
        }
        if (p->name[0]) count++;
    }
    return count;
}

int dlr_client_packages(const dlr_server* srv, const char* password, const char* query,
                        dlr_pkg_info* out, int max, const dlr_io* io) {
    dlr_session s;
    if (!dlr_client_open(&s, srv, password, io)) return -1;

    int search = query && query[0];
    uint8_t want = search ? DLR_MSG_SEARCH_RESULT : DLR_MSG_PKG_LIST;
    int rc = -1;
    int sent = search
        ? dlr_send_msg(&s, DLR_MSG_SEARCH_REQUEST, query, (uint32_t)strlen(query))
        : dlr_send_msg(&s, DLR_MSG_PKG_LIST, 0, 0);
    if (sent) {
        uint8_t type;
        uint32_t len;
        if (dlr_recv_msg(&s, &type, &len, 8000) && type == want)
            rc = dlr_parse_pkg_lines((const char*)s.plain + 1, len, out, max);
        else
            say(io, "dlr: no usable reply", 0, 0);
    } else {
        say(io, "dlr: the request could not be sent", 0, 0);
    }

    dlr_client_close(&s);
    return rc;
}

/* --- install ------------------------------------------------------------ */

typedef struct {
    const char* target_dir;
    const dlr_pkg* pkg;
    const char* requested_name;  /* the name the user asked to install */
    int only_listed;             /* honour pkg->mos_copy */
    int written;
    int skipped;
    const dlr_io* io;
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
 * Reduces an archive entry to the path a package author actually wrote
 * in `copy=`. Two wrapper conventions exist in the wild and both get
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
    // is the name the user asked to install, not necessarily the
    // manifest's own [Info] name= - those are usually the same but are
    // not required to be. Try the requested name first since it is the
    // one guaranteed to match server behaviour; fall back to the
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
        say(ctx->io, "  ! path too long: ", rel, 0);
        return 1;
    }
    if (!ensure_parent(full)) {
        say(ctx->io, "  ! cannot create directory for ", rel, 0);
        return 1;
    }

    if (!dlr_file_put(full, data, len)) {
        say(ctx->io, "  ! write failed: ", full, 0);
        return 0;                       // aborts extraction
    }

    line_buf l = { {0}, 0 };
    ln_emit(ctx->io, ln_s(ln_u(ln_s(ln_s(ln_s(&l, "  -> "), full), " ("), len), " bytes)"));
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

/* dlr_download's progress callback has no user pointer. */
static const dlr_io* g_progress_io;
static void forward_progress(uint64_t received, uint64_t total) {
    if (g_progress_io && g_progress_io->progress)
        g_progress_io->progress(g_progress_io->user, received, total);
}

// Runs the package's run= program after it was installed. 1 on success.
static int run_after_install(const dlr_pkg* pkg, const char* target_dir, const dlr_io* io) {
    char run_path[256];
    const char* target = pkg->mos_run;

    // An absolute MinimaFS path is used as-is; a bare name is resolved
    // inside the install directory. A package cannot ask us to run
    // something it did not ship.
    if (strchr(target, ':')) {
        size_t n = strlen(target);
        if (n >= sizeof(run_path)) { say(io, "dlr: run= path too long", 0, 0); return 0; }
        memcpy(run_path, target, n + 1);
    } else if (!join_path(run_path, sizeof(run_path), target_dir, target)) {
        say(io, "dlr: run= path too long", 0, 0);
        return 0;
    }

    if (!dlr_exists(run_path)) {
        say(io, "dlr: run= names '", run_path, "', which the package did not install");
        return 0;
    }

    say(io, "Running ", run_path, " ...");
    long pid = dlr_exec(run_path);
    if (pid == DLR_INVALID) {
        say(io, "dlr: could not launch it (SYS_EXEC failed)", 0, 0);
        return 0;
    }
    if (!dlr_exec_wait(pid, 30000)) {
        say(io, "dlr: post-install program is still running after 30s; leaving it.", 0, 0);
    }
    return 1;
}

int dlr_client_install(const dlr_server* srv, const char* password,
                       const char* pkg_name, int force, const dlr_io* io) {
    if (!dlr_mkdirs(DLR_CACHE_DIR)) {
        say(io, "dlr: cannot create ", DLR_CACHE_DIR, 0);
        return 0;
    }

    dlr_session s;
    if (!dlr_client_open(&s, srv, password, io)) return 0;

    say(io, "Requesting '", pkg_name, "' ...");

    char err[128];
    err[0] = '\0';
    int format = DLR_FORMAT_TAR;
    g_progress_io = io;
    int rc = dlr_download(&s, pkg_name, DLR_STAGE_FILE, forward_progress, &format,
                          err, sizeof(err));
    g_progress_io = 0;
    dlr_client_close(&s);

    if (rc == -1) {
        say(io, "dlr: server refused: ", err, 0);
        return 0;
    }
    if (rc != 1) {
        say(io, "dlr: download failed: ", err[0] ? err : "unknown error", 0);
        return 0;
    }
    say(io, "Download verified (SHA-256 matches).", 0, 0);

    long size = dlr_file_size(DLR_STAGE_FILE);
    if (size <= 0) {
        say(io, "dlr: staged file is empty", 0, 0);
        return 0;
    }

    uint8_t* archive = (uint8_t*)malloc((size_t)size);
    if (!archive) {
        line_buf l = { {0}, 0 };
        ln_emit(io, ln_s(ln_u(ln_s(&l, "dlr: out of memory for a "), (uint64_t)size / 1024u),
                         " KiB archive"));
        return 0;
    }
    if (dlr_file_slurp(DLR_STAGE_FILE, archive, (uint32_t)size) != size) {
        say(io, "dlr: cannot read staged archive", 0, 0);
        free(archive);
        return 0;
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
        say(io, "dlr: the package is not a readable ", format_name, " archive");
        free(archive);
        return 0;
    }

    if (!mctx.found) {
        say(io, "dlr: no .pkg manifest inside the bundle", 0, 0);
        free(archive);
        return 0;
    }

    {
        line_buf l = { {0}, 0 };
        ln_s(ln_s(ln_s(&l, "Package: "), pkg.name), pkg.version[0] ? " " : "");
        ln_emit(io, ln_s(&l, pkg.version));
    }
    if (pkg.description[0]) say(io, "  ", pkg.description, 0);

    const char* why = "";
    if (!dlr_pkg_runs_here(&pkg, &why)) {
        say(io, "dlr: ", why, 0);
        line_buf l = { {0}, 0 };
        ln_emit(io, ln_s(ln_s(ln_s(ln_s(ln_s(&l, "     (arch="),
                         pkg.arch[0] ? pkg.arch : "any"), " os="),
                         pkg.operatingsystem[0] ? pkg.operatingsystem : "any"), ")"));
        if (!force) {
            say(io, "     Refusing. Pass --force to install it anyway.", 0, 0);
            free(archive);
            return 0;
        }
        say(io, "     Forced; installing regardless.", 0, 0);
    }

    if (!pkg.has_minimalos_section) {
        // Not fatal: a package of plain data files installs fine with
        // the defaults. It is worth saying out loud, though, because
        // anything relying on installscript/installcommand will not
        // have run.
        say(io, "Note: no [Install.minimalos] section; copying everything to ",
            DLR_INSTALL_DIR, ".");
    }

    install_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.pkg = &pkg;
    ctx.requested_name = pkg_name;
    ctx.target_dir = pkg.mos_target[0] ? pkg.mos_target : DLR_INSTALL_DIR;
    ctx.only_listed = pkg.mos_copy[0] ? 1 : 0;
    ctx.io = io;

    if (!dlr_mkdirs(ctx.target_dir)) {
        say(io, "dlr: cannot create ", ctx.target_dir, 0);
        free(archive);
        return 0;
    }

    say(io, "Installing into ", ctx.target_dir, " ...");
    dlr_tar_sink isink = { install_on_file, install_on_dir, &ctx };
    long entries = extract(archive, (size_t)size, &isink);
    free(archive);

    if (entries < 0) {
        say(io, "dlr: extraction aborted", 0, 0);
        return 0;
    }

    line_buf l = { {0}, 0 };
    ln_emit(io, ln_s(ln_s(ln_u(ln_s(&l, "Installed "), (uint64_t)ctx.written), " file(s)"),
                     ctx.skipped ? " (some skipped by copy=)." : "."));

    dlr_remove(DLR_STAGE_FILE);

    if (pkg.mos_run[0] && !run_after_install(&pkg, ctx.target_dir, io)) return 0;

    say(io, "Done.", 0, 0);
    return 1;
}
