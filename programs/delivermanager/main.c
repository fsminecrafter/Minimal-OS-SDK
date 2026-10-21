/*
 * Deliver Manager - a window for Deliver, the LAN package manager.
 *
 * "Get packages" finds Deliver servers on the network, lists and searches
 * their packages and installs them. "Share packages" offers packages from
 * this computer and runs a Deliver server while the window is open.
 *
 * Everything Deliver-specific comes from programs/dlr (the same code as
 * the `dlr` command); this file is only the window. Network work blocks,
 * so a click queues a job and the next frame runs it, after the window
 * has shown what is happening.
 *
 *     ./build.sh programs/delivermanager
 */
#include "easyui.h"
#include "net.h"

#include "../dlr/dlr.h"
#include "../dlr/dlr_port.h"
#include "../dlr/dlr_db.h"
#include "../dlr/dlr_client.h"
#include "../dlr/dlr_registry.h"
#include "../dlr/dlr_server.h"

#define WIN_W 680
#define WIN_H 476
#define MAX_PACKAGES 64
#define LOG_MAX_LINES 150

/* --- text helpers ------------------------------------------------------- */

typedef struct {
    char text[EASYUI_TEXT_MAX];
    int  n;
} fmt_t;

static fmt_t* f_s(fmt_t* f, const char* s) {
    while (s && *s && f->n + 1 < (int)sizeof(f->text)) f->text[f->n++] = *s++;
    f->text[f->n] = '\0';
    return f;
}

static fmt_t* f_u(fmt_t* f, unsigned long v) {
    char d[24];
    int k = 0;
    do { d[k++] = (char)('0' + v % 10u); v /= 10u; } while (v && k < 23);
    while (k > 0 && f->n + 1 < (int)sizeof(f->text)) f->text[f->n++] = d[--k];
    f->text[f->n] = '\0';
    return f;
}

static fmt_t* f_ip(fmt_t* f, uint32_t ip) {
    char t[16];
    dlr_ip_format(ip, t);
    return f_s(f, t);
}

static void f_reset(fmt_t* f) { f->n = 0; f->text[0] = '\0'; }

static int parse_port(const char* s) {
    int v = 0, digits = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (*s - '0');
        if (v > 65535) return -1;
        digits++;
    }
    return digits ? v : 0;
}

/* --- widgets ------------------------------------------------------------ */

static int w_net, w_tab_get, w_tab_share;
static int w_progress, w_log;

/* Get packages */
static int w_servers, w_scan, w_forget, w_address, w_add;
static int w_search, w_search_btn, w_packages, w_details, w_password, w_force, w_install;

/* Share packages */
static int w_srv_name, w_srv_port, w_srv_tls, w_srv_password, w_srv_toggle, w_srv_status;
static int w_store, w_rebuild, w_unpresent, w_store_refresh, w_folder, w_present, w_newpkg;

static int g_get_widgets[24], g_get_count;
static int g_share_widgets[24], g_share_count;

static int add_get(int w)   { if (g_get_count < 24) g_get_widgets[g_get_count++] = w; return w; }
static int add_share(int w) { if (g_share_count < 24) g_share_widgets[g_share_count++] = w; return w; }

/* --- state -------------------------------------------------------------- */

static dlr_server   g_servers[DLR_MAX_SERVERS];
static int          g_server_count;
static dlr_pkg_info g_packages[MAX_PACKAGES];
static int          g_package_count;
static int          g_package_server = -1;   // whose packages are listed

static dlr_server_state g_srv;
static int              g_serving;

static int  g_busy;          // a job is running: buttons do nothing

typedef enum {
    JOB_NONE, JOB_SCAN, JOB_ADD, JOB_LIST, JOB_INSTALL,
    JOB_START_SERVER, JOB_PRESENT, JOB_REBUILD
} job_t;
static job_t g_job;

/* --- log and repainting ------------------------------------------------- */

/*
 * Runs one frame of the window from inside a long job, so new log lines
 * and the progress bar show up. Every handler checks g_busy, so clicks
 * that arrive meanwhile do nothing; closing the window ends the program
 * once the job is over.
 */
static void pump(void) {
    static long last;
    if (!g_busy) return;    // outside a job the normal loop repaints
    long now = mos_uptime();
    if (now - last < 40) return;
    last = now;
    easyui.update();
}

static void log_line(const char* line) {
    if (easyui.itemcount(w_log) >= LOG_MAX_LINES) easyui.clearitems(w_log);
    easyui.additem(w_log, line);
    // Selecting the newest line scrolls the list to it; then drop the
    // highlight (the scroll position stays).
    easyui.setvalue(w_log, easyui.itemcount(w_log) - 1);
    easyui.setvalue(w_log, -1);
    pump();
}

static void io_log(void* user, const char* line) { (void)user; log_line(line); }

static void io_progress(void* user, uint64_t received, uint64_t total) {
    (void)user;
    if (total) {
        easyui.setrange(w_progress, 0, 1000);
        easyui.setvalue(w_progress, (int)(received * 1000u / total));
    }
    pump();
}

static const dlr_io g_io = { io_log, io_progress, 0 };

static void status_box(const char* title, const char* text) {
    log_line(text);
    easyui.messagebox(title, text);
}

/* --- network ------------------------------------------------------------ */

// 1 if this computer has an address. Otherwise says why.
static int network_ready(int explain) {
    syscall_net_status_t st;
    const char* why = 0;
    if (mos_net_status(&st) != (long)SYS_SUCCESS) why = "This system has no network support.";
    else if (!st.has_driver) why = "No network card was found.";
    else if (!st.configured) why = "No IP address yet. Wait for DHCP, or run 'dhcp' in a terminal.";
    if (!why) return 1;
    if (explain) status_box("No network", why);
    return 0;
}

static void refresh_network_label(int timer) {
    (void)timer;
    syscall_net_status_t st;
    fmt_t f = { {0}, 0 };
    if (mos_net_status(&st) != (long)SYS_SUCCESS || !st.has_driver) f_s(&f, "Network: none");
    else if (!st.configured) f_s(&f, "Network: waiting for DHCP");
    else f_ip(f_s(&f, "This computer: "), st.local_ip);
    if (strcmp(easyui.gettext(w_net), f.text) != 0) easyui.settext(w_net, f.text);
}

/* --- Get packages ------------------------------------------------------- */

static void fill_servers(void) {
    int keep = easyui.getvalue(w_servers);
    easyui.clearitems(w_servers);
    for (int i = 0; i < g_server_count; i++) {
        fmt_t f = { {0}, 0 };
        // "name  a.b.c.d[:port] [TLS] [locked]"; the usual ports are left out.
        const dlr_server* s = &g_servers[i];
        f_ip(f_s(f_s(&f, s->name[0] ? s->name : "(unnamed)"), "  "), s->ip);
        if (s->port != DLR_DEFAULT_PORT && s->port != DLR_TLS_PORT) f_u(f_s(&f, ":"), s->port);
        if (s->port == DLR_TLS_PORT) f_s(&f, " TLS");
        if (s->needs_password) f_s(&f, " locked");
        easyui.additem(w_servers, f.text);
    }
    if (keep >= 0 && keep < g_server_count) easyui.setvalue(w_servers, keep);
    else if (g_server_count) easyui.setvalue(w_servers, 0);
    easyui.setenabled(w_forget, g_server_count > 0);
}

static void clear_packages(const char* why) {
    g_package_count = 0;
    g_package_server = -1;
    easyui.clearitems(w_packages);
    easyui.settext(w_details, why);
    easyui.setenabled(w_install, 0);
}

static void fill_packages(void) {
    easyui.clearitems(w_packages);
    for (int i = 0; i < g_package_count; i++) {
        fmt_t f = { {0}, 0 };
        f_s(&f, g_packages[i].name);
        if (g_packages[i].version[0]) f_s(f_s(&f, "  "), g_packages[i].version);
        easyui.additem(w_packages, f.text);
    }
    if (g_package_count) {
        easyui.setvalue(w_packages, 0);
    } else {
        easyui.settext(w_details, "No packages found.");
        easyui.setenabled(w_install, 0);
    }
}

static void on_package_selected(int index) {
    if (index < 0 || index >= g_package_count) {
        easyui.setenabled(w_install, 0);
        return;
    }
    const dlr_pkg_info* p = &g_packages[index];
    fmt_t f = { {0}, 0 };
    f_s(&f, p->name);
    if (p->version[0]) f_s(f_s(&f, " "), p->version);
    f_s(f_s(&f, ": "), p->description[0] ? p->description : "no description");
    easyui.settext(w_details, f.text);
    easyui.setenabled(w_install, !g_busy);
}

static const dlr_server* selected_server(void) {
    int i = easyui.getvalue(w_servers);
    return (i >= 0 && i < g_server_count) ? &g_servers[i] : 0;
}

// The password to use for `srv`, or NULL. Asks for one if it is needed.
static int server_password(const dlr_server* srv, const char** out) {
    const char* pw = easyui.gettext(w_password);
    *out = pw[0] ? pw : 0;
    if (srv->needs_password && !*out) {
        easyui.messagebox("Password needed",
                          "This server needs a password. Type it in the password box first.");
        easyui.setfocus(w_password);
        return 0;
    }
    return 1;
}

static void queue(job_t job) {
    if (g_busy || g_job != JOB_NONE) return;
    g_job = job;
}

static void on_scan(int v)    { (void)v; queue(JOB_SCAN); }
static void on_add(int v)     { (void)v; if (easyui.gettext(w_address)[0]) queue(JOB_ADD); }
static void on_search(int v)  { (void)v; if (selected_server()) queue(JOB_LIST); }
static void on_install(int v) { (void)v; if (g_package_server >= 0 && easyui.getvalue(w_packages) >= 0) queue(JOB_INSTALL); }

static void on_server_selected(int index) {
    if (g_busy || index < 0 || index >= g_server_count) return;
    if (index == g_package_server) return;
    if (!network_ready(0)) {
        clear_packages("The network is not up yet.");
        return;
    }
    clear_packages("");
    queue(JOB_LIST);
}

static void on_forget(int v) {
    (void)v;
    if (g_busy) return;
    int i = easyui.getvalue(w_servers);
    if (i < 0 || i >= g_server_count) return;
    fmt_t f = { {0}, 0 };
    f_s(f_s(f_s(&f, "Forgot server '"), g_servers[i].name), "'.");
    for (int j = i; j + 1 < g_server_count; j++) g_servers[j] = g_servers[j + 1];
    g_server_count--;
    dlr_db_save(g_servers, g_server_count);
    log_line(f.text);
    clear_packages("Pick a server to see its packages.");
    easyui.setvalue(w_servers, -1);
    fill_servers();     // selects the first server, which lists its packages
}

static void scan_found(const dlr_server* s) {
    fmt_t f = { {0}, 0 };
    f_u(f_s(f_ip(f_s(f_s(f_s(&f, "  found '"), s->name), "' at "), s->ip), ":"), s->port);
    log_line(f.text);
}

static void job_scan(void) {
    if (!network_ready(1)) return;
    log_line("Looking for Deliver servers on the network (5 s) ...");
    pump();
    // dlr_discover fills a fresh list; merge it so servers that were
    // added by address are kept.
    static dlr_server heard[DLR_MAX_SERVERS];
    int count = dlr_discover(heard, DLR_MAX_SERVERS, 5000, scan_found);
    if (count < 0) {
        status_box("Scan failed", "Could not listen for servers on UDP port 4243. Is a scan already running?");
        return;
    }
    int before = g_server_count;
    for (int i = 0; i < count; i++)
        g_server_count = dlr_db_add(g_servers, g_server_count, DLR_MAX_SERVERS, &heard[i]);
    dlr_db_save(g_servers, g_server_count);
    fill_servers();
    fmt_t f = { {0}, 0 };
    f_s(f_u(f_s(&f, "Scan finished: "), (unsigned long)count), " server(s) answered");
    if (g_server_count > before) f_s(f_u(f_s(&f, ", "), (unsigned long)(g_server_count - before)), " new");
    log_line(f_s(&f, ".")->text);
    if (!count) log_line("If you know a server's address, type it below the list and press Add.");
    if (g_package_server < 0 && g_server_count) g_job = JOB_LIST;
}

static void job_add(void) {
    if (!network_ready(1)) return;
    const char* spec = easyui.gettext(w_address);
    int tls = 0;
    if (strncmp(spec, "tls://", 6) == 0) { tls = 1; spec += 6; }
    if (strncmp(spec, "http://", 7) == 0 || strncmp(spec, "https://", 8) == 0) {
        status_box("Not a Deliver server", "Web addresses are not Deliver servers. Enter an IP address.");
        return;
    }

    char host[64];
    int i = 0;
    while (spec[i] && spec[i] != ':' && i + 1 < (int)sizeof(host)) { host[i] = spec[i]; i++; }
    host[i] = '\0';
    int port = tls ? DLR_TLS_PORT : DLR_DEFAULT_PORT;
    if (spec[i] == ':') {
        port = parse_port(spec + i + 1);
        if (port <= 0) { status_box("Bad address", "The port must be a number from 1 to 65535."); return; }
    }
    if (tls && port != DLR_TLS_PORT) {
        status_box("Bad address", "TLS servers use port 4342.");
        return;
    }

    uint32_t ip = dlr_resolve(host);
    if (!ip) {
        status_box("Unknown address", "That address could not be found.");
        return;
    }

    fmt_t f = { {0}, 0 };
    log_line(f_s(f_u(f_s(f_ip(f_s(&f, "Asking "), ip), ":"), (unsigned long)port), " ...")->text);
    dlr_server found;
    if (!dlr_probe(ip, (uint16_t)port, &found, 4000)) {
        status_box("No server there", "No Deliver server answered at that address.");
        return;
    }
    g_server_count = dlr_db_add(g_servers, g_server_count, DLR_MAX_SERVERS, &found);
    dlr_db_save(g_servers, g_server_count);
    easyui.settext(w_address, "");
    fill_servers();
    for (int k = 0; k < g_server_count; k++) {
        if (g_servers[k].ip == found.ip && g_servers[k].port == found.port) easyui.setvalue(w_servers, k);
    }
    f_reset(&f);
    log_line(f_s(f_s(f_s(&f, "Added '"), found.name), "'.")->text);
    g_package_server = -1;
    g_job = JOB_LIST;
}

static void job_list(void) {
    const dlr_server* srv = selected_server();
    if (!srv) return;
    int index = easyui.getvalue(w_servers);
    const char* pw;
    if (!network_ready(1) || !server_password(srv, &pw)) {
        clear_packages("Not connected.");
        return;
    }
    const char* query = easyui.gettext(w_search);
    easyui.settext(w_details, "Asking the server ...");
    pump();
    int n = dlr_client_packages(srv, pw, query, g_packages, MAX_PACKAGES, &g_io);
    if (n < 0) {
        clear_packages("Could not get the package list. See the log below.");
        return;
    }
    g_package_count = n;
    g_package_server = index;
    fmt_t f = { {0}, 0 };
    f_s(f_s(f_s(f_u(&f, (unsigned long)n), n == 1 ? " package on '" : " packages on '"),
            srv->name), "'");
    if (query[0]) f_s(f_s(f_s(&f, " matching '"), query), "'");
    log_line(f.text);
    fill_packages();
}

static void job_install(void) {
    // The server the list came from, which is not necessarily the one
    // highlighted now (the selection can change while a job runs).
    const dlr_server* srv = (g_package_server >= 0 && g_package_server < g_server_count)
        ? &g_servers[g_package_server] : 0;
    int index = easyui.getvalue(w_packages);
    if (!srv || index < 0 || index >= g_package_count) return;
    const char* pw;
    if (!network_ready(1) || !server_password(srv, &pw)) return;

    char name[DLR_NAME_MAX];
    strcpy(name, g_packages[index].name);
    easyui.setrange(w_progress, 0, 1000);
    easyui.setvalue(w_progress, 0);

    int ok = dlr_client_install(srv, pw, name, easyui.getvalue(w_force), &g_io);
    easyui.setvalue(w_progress, ok ? 1000 : 0);
    fmt_t f = { {0}, 0 };
    if (ok) {
        f_s(f_s(f_s(&f, "'"), name), "' is installed.");
        easyui.messagebox("Installed", f.text);
    } else {
        f_s(f_s(f_s(&f, "'"), name), "' was not installed. The log says why.");
        easyui.messagebox("Install failed", f.text);
    }
}

/* --- Share packages ----------------------------------------------------- */

static int store_entry(void* user, const dlr_reg_entry* e) {
    (void)user;
    fmt_t f = { {0}, 0 };
    f_s(&f, e->name);
    if (e->version[0]) f_s(f_s(&f, "  "), e->version);
    if (e->description[0]) f_s(f_s(&f, "  -  "), e->description);
    easyui.additem(w_store, f.text);
    return 1;
}

static void fill_store(void) {
    easyui.clearitems(w_store);
    dlr_reg_each(store_entry, 0);
    int has = easyui.itemcount(w_store) > 0;
    if (has) easyui.setvalue(w_store, 0);
    easyui.setenabled(w_rebuild, has);
    easyui.setenabled(w_unpresent, has);
}

// The package name of the selected store row (the text before the first space).
static int selected_store_name(char* out, int cap) {
    int i = easyui.getvalue(w_store);
    if (i < 0) return 0;
    const char* row = easyui.getitem(w_store, i);
    int n = 0;
    while (row[n] && row[n] != ' ' && n + 1 < cap) { out[n] = row[n]; n++; }
    out[n] = '\0';
    return n > 0;
}

static void update_server_status(void) {
    fmt_t f = { {0}, 0 };
    if (!g_serving) {
        f_s(&f, "Stopped. Start the server to let other computers install the packages on the right.");
    } else {
        f_u(f_s(f_s(f_s(&f, "Serving '"), g_srv.cfg.name), "' on port "), g_srv.cfg.port);
        if (g_srv.cfg.tls) f_s(&f, " (TLS)");
        if (g_srv.cfg.pw_hash[0]) f_s(&f, ", password required");
        f_s(f_u(f_s(&f, ". Clients served: "), g_srv.clients_served), ".");
    }
    easyui.settext(w_srv_status, f.text);
    easyui.settext(w_srv_toggle, g_serving ? "Stop server" : "Start server");
    easyui.setstyle(w_srv_toggle, g_serving ? EASYUI_DANGER : EASYUI_PRIMARY);
    easyui.setenabled(w_srv_name, !g_serving);
    easyui.setenabled(w_srv_port, !g_serving);
    easyui.setenabled(w_srv_tls, !g_serving);
    easyui.setenabled(w_srv_password, !g_serving);
}

static void stop_server(void) {
    if (!g_serving) return;
    dlr_server_stop(&g_srv);
    g_serving = 0;
    log_line("Server stopped.");
    update_server_status();
}

static void on_client(void* user, uint32_t ip) {
    (void)user;
    fmt_t f = { {0}, 0 };
    log_line(f_s(f_ip(f_s(&f, "Serving a client at "), ip), " ...")->text);
    update_server_status();
    pump();
}

static void job_start_server(void) {
    if (!network_ready(1)) return;
    int port = parse_port(easyui.gettext(w_srv_port));
    if (port < 0) { status_box("Bad port", "The port must be a number from 1 to 65535."); return; }
    int tls = easyui.getvalue(w_srv_tls);
    if (tls && port && port != DLR_TLS_PORT) {
        status_box("Bad port", "A TLS server always uses port 4342.");
        return;
    }

    dlr_server_cfg cfg;
    if (!dlr_server_cfg_set(&cfg, easyui.gettext(w_srv_name), easyui.gettext(w_srv_password),
                            port, tls)) {
        status_box("Bad settings", "Those server settings do not work together.");
        return;
    }
    memset(&g_srv, 0, sizeof(g_srv));
    g_srv.on_client = on_client;
    if (!dlr_server_start(&g_srv, &cfg)) {
        status_box("Server not started", "The port is in use, or the network is down.");
        return;
    }
    g_serving = 1;
    fmt_t f = { {0}, 0 };
    log_line(f_u(f_s(f_s(f_s(&f, "Serving '"), cfg.name), "' on port "), cfg.port)->text);
    if (easyui.itemcount(w_store) == 0)
        log_line("Nothing is offered yet: present a package folder first.");
    update_server_status();
}

static void on_server_toggle(int v) {
    (void)v;
    if (g_busy) return;
    if (g_serving) stop_server();
    else queue(JOB_START_SERVER);
}

static void on_present(int v)  { (void)v; if (easyui.gettext(w_folder)[0]) queue(JOB_PRESENT); }
static void on_rebuild(int v)  { (void)v; if (easyui.getvalue(w_store) >= 0) queue(JOB_REBUILD); }
static void on_store_refresh(int v) { (void)v; if (!g_busy) fill_store(); }

static void job_present(void) {
    char err[128];
    err[0] = '\0';
    const char* dir = easyui.gettext(w_folder);
    fmt_t f = { {0}, 0 };
    log_line(f_s(f_s(f_s(&f, "Presenting "), dir), " (packing it can take a moment) ...")->text);
    pump();
    if (!dlr_reg_present(dir, 0, err, sizeof(err))) {
        f_reset(&f);
        status_box("Not presented", f_s(f_s(&f, "Could not present it: "),
                                        err[0] ? err : "unknown error")->text);
        return;
    }
    easyui.settext(w_folder, "");
    log_line("Presented. Other computers can install it while the server runs.");
    fill_store();
}

static void job_rebuild(void) {
    char name[DLR_NAME_MAX], err[128];
    err[0] = '\0';
    if (!selected_store_name(name, sizeof(name))) return;
    fmt_t f = { {0}, 0 };
    log_line(f_s(f_s(f_s(&f, "Rebuilding '"), name), "' ...")->text);
    pump();
    f_reset(&f);
    if (!dlr_reg_rebuild(name, err, sizeof(err)))
        status_box("Not rebuilt", f_s(f_s(&f, "Could not rebuild it: "), err[0] ? err : "unknown error")->text);
    else
        log_line(f_s(f_s(f_s(&f, "Rebuilt the archive of '"), name), "'.")->text);
    fill_store();
}

static void on_unpresent(int v) {
    (void)v;
    char name[DLR_NAME_MAX];
    if (g_busy || !selected_store_name(name, sizeof(name))) return;
    fmt_t f = { {0}, 0 };
    if (dlr_reg_remove(name)) log_line(f_s(f_s(f_s(&f, "Stopped offering '"), name), "'.")->text);
    else log_line(f_s(f_s(f_s(&f, "There is no package '"), name), "' in the store.")->text);
    fill_store();
}

/*
 * Writes a starter manifest into the folder, named after the folder, so
 * a folder of files can be presented after filling in the description.
 */
static void on_new_manifest(int v) {
    (void)v;
    if (g_busy) return;
    const char* dir = easyui.gettext(w_folder);
    if (!dir[0]) {
        easyui.messagebox("Folder needed", "Type the folder that holds the package's files first.");
        easyui.setfocus(w_folder);
        return;
    }
    if (!dlr_is_dir(dir)) {
        status_box("No such folder", "That folder does not exist.");
        return;
    }

    // Package name: the folder's last part, limited to [A-Za-z0-9._-].
    const char* base = dir;
    for (const char* p = dir; *p; p++) if ((*p == '/' || *p == ':') && p[1]) base = p + 1;
    char name[DLR_NAME_MAX];
    int n = 0;
    for (const char* p = base; *p && *p != '/' && n + 1 < (int)sizeof(name); p++) {
        char c = *p;
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                 c == '.' || c == '_' || c == '-';
        name[n++] = ok ? c : '-';
    }
    name[n] = '\0';
    if (!n || name[0] == '.' || !dlr_reg_name_ok(name)) strcpy(name, "mypackage");

    char path[256];
    fmt_t f = { {0}, 0 };
    f_s(&f, dir);
    if (f.n && f.text[f.n - 1] != '/') f_s(&f, "/");
    f_s(f_s(&f, name), ".pkg");
    if (f.n + 1 >= (int)sizeof(f.text)) { status_box("Path too long", "Choose a shorter folder path."); return; }
    strcpy(path, f.text);
    if (dlr_exists(path)) {
        f_reset(&f);
        status_box("Already there", f_s(f_s(&f, path), " already exists.")->text);
        return;
    }

    static char manifest[512];
    int len = 0;
    const char* parts[] = {
        "[Info]\nname=", name,
        "\nversion=1.0.0\ndescription=Describe ", name,
        " here\narch=x86_64\noperatingsystem=MINIMALOS\n\n"
        "[Install.minimalos]\ncopy=\ntarget=0:/programs\n", 0
    };
    for (int i = 0; parts[i]; i++)
        for (const char* p = parts[i]; *p && len + 1 < (int)sizeof(manifest); p++) manifest[len++] = *p;
    manifest[len] = '\0';

    f_reset(&f);
    if (!dlr_file_put(path, manifest, (uint32_t)len)) {
        status_box("Not written", f_s(f_s(&f, "Could not write "), path)->text);
        return;
    }
    log_line(f_s(f_s(&f, "Wrote "), path)->text);
    log_line("Edit its description (and copy=/target= if needed), then Present the folder.");
}

/* --- tabs --------------------------------------------------------------- */

static void show_tab(int tab) {

    for (int i = 0; i < g_get_count; i++) easyui.setvisible(g_get_widgets[i], tab == 0);
    for (int i = 0; i < g_share_count; i++) easyui.setvisible(g_share_widgets[i], tab == 1);
    easyui.setstyle(w_tab_get, tab == 0 ? EASYUI_PRIMARY : EASYUI_SECONDARY);
    easyui.setstyle(w_tab_share, tab == 1 ? EASYUI_PRIMARY : EASYUI_SECONDARY);
    if (tab == 1) fill_store();
}

static void on_tab(int v) {
    (void)v;
    show_tab(easyui.current() == w_tab_share ? 1 : 0);
}

/* --- main loop ---------------------------------------------------------- */

static void set_busy(int busy) {
    g_busy = busy;
    int buttons[] = { w_scan, w_add, w_forget, w_search_btn, w_install, w_srv_toggle,
                      w_present, w_rebuild, w_unpresent, w_store_refresh, w_newpkg,
                      w_tab_get, w_tab_share };
    for (unsigned i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++)
        easyui.setenabled(buttons[i], !busy);
    if (!busy) {
        // Put back what depends on a selection.
        easyui.setenabled(w_forget, g_server_count > 0);
        easyui.setenabled(w_install, easyui.getvalue(w_packages) >= 0);
        int has = easyui.itemcount(w_store) > 0;
        easyui.setenabled(w_rebuild, has);
        easyui.setenabled(w_unpresent, has);
    }
    easyui.settitle(busy ? "Deliver Manager - working ..." : "Deliver Manager");
}

static void on_frame(int now) {
    (void)now;
    if (g_busy) return;     // a pump() from inside a job

    // Several jobs chain (adding a server lists its packages), so run
    // until none is queued.
    while (g_job != JOB_NONE) {
        job_t job = g_job;
        g_job = JOB_NONE;
        set_busy(1);
        switch (job) {
        case JOB_SCAN:         job_scan(); break;
        case JOB_ADD:          job_add(); break;
        case JOB_LIST:         job_list(); break;
        case JOB_INSTALL:      job_install(); break;
        case JOB_START_SERVER: job_start_server(); break;
        case JOB_PRESENT:      job_present(); break;
        case JOB_REBUILD:      job_rebuild(); break;
        default: break;
        }
        set_busy(0);
    }

    if (g_serving) {
        g_busy = 1;          // a client is served inline and may pump()
        unsigned before = g_srv.clients_served;
        int r = dlr_server_poll(&g_srv);
        g_busy = 0;
        if (r < 0) {
            g_serving = 0;
            log_line("The server stopped: its network connection went away.");
            update_server_status();
        } else if (g_srv.clients_served != before) {
            log_line("Client finished.");
            update_server_status();
        }
    }
}

static void on_close(int v) {
    (void)v;
    stop_server();
    easyui.quit();
}

/* --- building the window ------------------------------------------------ */

static void build_get_tab(void) {
    add_get(easyui.createpanel(16, 96, 240, 272, "Servers"));
    w_servers = add_get(easyui.createlistbox(28, 124, 216, 158));
    easyui.recallfunc(w_servers, ONCHANGE, on_server_selected);
    w_scan = add_get(easyui.createbutton(28, 290, 104, 30, "Scan network"));
    easyui.recallfunc(w_scan, ONCLICK, on_scan);
    w_forget = add_get(easyui.createbutton(140, 290, 104, 30, "Forget"));
    easyui.setstyle(w_forget, EASYUI_SECONDARY);
    easyui.recallfunc(w_forget, ONCLICK, on_forget);
    w_address = add_get(easyui.createtextinput(28, 328, 150, 30, "IP or IP:port"));
    easyui.recallfunc(w_address, ONSUBMIT, on_add);
    w_add = add_get(easyui.createbutton(184, 328, 60, 30, "Add"));
    easyui.setstyle(w_add, EASYUI_SECONDARY);
    easyui.recallfunc(w_add, ONCLICK, on_add);

    add_get(easyui.createpanel(268, 96, 396, 272, "Packages"));
    w_search = add_get(easyui.createtextinput(280, 124, 264, 30, "Search (empty = all)"));
    easyui.recallfunc(w_search, ONSUBMIT, on_search);
    w_search_btn = add_get(easyui.createbutton(552, 124, 100, 30, "Search"));
    easyui.setstyle(w_search_btn, EASYUI_SECONDARY);
    easyui.recallfunc(w_search_btn, ONCLICK, on_search);
    w_packages = add_get(easyui.createlistbox(280, 162, 372, 110));
    easyui.recallfunc(w_packages, ONCHANGE, on_package_selected);
    easyui.recallfunc(w_packages, ONSUBMIT, on_install);
    w_details = add_get(easyui.createlabel(280, 276, 372, 42, "Pick a server to see its packages."));
    w_password = add_get(easyui.createtextinput(280, 326, 132, 30, "Server password"));
    easyui.setpassword(w_password, 1);
    w_force = add_get(easyui.createcheckbox(420, 326, 124, 30, "Any system", 0));
    w_install = add_get(easyui.createbutton(552, 326, 100, 30, "Install"));
    easyui.recallfunc(w_install, ONCLICK, on_install);
    easyui.setenabled(w_install, 0);
}

static void build_share_tab(void) {
    add_share(easyui.createpanel(16, 96, 240, 272, "Server"));
    w_srv_name = add_share(easyui.createtextinput(28, 124, 216, 30, "Server name"));
    easyui.settext(w_srv_name, "minimalos");
    easyui.setmaxlength(w_srv_name, DLR_NAME_MAX - 1);
    w_srv_port = add_share(easyui.createtextinput(28, 160, 96, 30, "Port"));
    easyui.settext(w_srv_port, "4242");
    easyui.setmaxlength(w_srv_port, 5);
    w_srv_tls = add_share(easyui.createtoggle(132, 160, 112, 30, "TLS", 0));
    w_srv_password = add_share(easyui.createtextinput(28, 196, 216, 30, "Password (optional)"));
    easyui.setpassword(w_srv_password, 1);
    w_srv_toggle = add_share(easyui.createbutton(28, 234, 216, 34, "Start server"));
    easyui.recallfunc(w_srv_toggle, ONCLICK, on_server_toggle);
    w_srv_status = add_share(easyui.createlabel(28, 274, 216, 86, ""));

    add_share(easyui.createpanel(268, 96, 396, 272, "Packages this computer offers"));
    w_store = add_share(easyui.createlistbox(280, 124, 372, 128));
    w_rebuild = add_share(easyui.createbutton(280, 260, 120, 30, "Rebuild"));
    easyui.setstyle(w_rebuild, EASYUI_SECONDARY);
    easyui.recallfunc(w_rebuild, ONCLICK, on_rebuild);
    w_unpresent = add_share(easyui.createbutton(408, 260, 120, 30, "Stop offering"));
    easyui.setstyle(w_unpresent, EASYUI_DANGER);
    easyui.recallfunc(w_unpresent, ONCLICK, on_unpresent);
    w_store_refresh = add_share(easyui.createbutton(536, 260, 116, 30, "Refresh"));
    easyui.setstyle(w_store_refresh, EASYUI_SECONDARY);
    easyui.recallfunc(w_store_refresh, ONCLICK, on_store_refresh);
    w_folder = add_share(easyui.createtextinput(280, 296, 372, 30,
                                                "Package folder, e.g. 0:/users/me/hello"));
    easyui.recallfunc(w_folder, ONSUBMIT, on_present);
    w_present = add_share(easyui.createbutton(280, 332, 180, 30, "Present folder"));
    easyui.recallfunc(w_present, ONCLICK, on_present);
    w_newpkg = add_share(easyui.createbutton(468, 332, 184, 30, "New .pkg in folder"));
    easyui.setstyle(w_newpkg, EASYUI_SECONDARY);
    easyui.recallfunc(w_newpkg, ONCLICK, on_new_manifest);
}

int main(void) {
    if (!easyui.init("Deliver Manager", WIN_W, WIN_H)) {
        mos_puts("Deliver Manager needs the desktop (MOSdesk). Use 'dlr' in the terminal instead.\n");
        return 1;
    }
    mos_window_set_resizable(easyui.window(), false);

    int title = easyui.createlabel(16, 10, 330, 32, "Deliver Manager");
    easyui.settextsize(title, 3);
    w_net = easyui.createlabel(352, 14, 312, 24, "");
    easyui.setalign(w_net, EASYUI_ALIGN_RIGHT);
    w_tab_get = easyui.createbutton(16, 50, 160, 32, "Get packages");
    easyui.recallfunc(w_tab_get, ONCLICK, on_tab);
    w_tab_share = easyui.createbutton(184, 50, 160, 32, "Share packages");
    easyui.recallfunc(w_tab_share, ONCLICK, on_tab);
    easyui.createseparator(16, 88, WIN_W - 32);

    build_get_tab();
    build_share_tab();

    w_progress = easyui.createprogress(16, 378, WIN_W - 32, 8, 1000);
    w_log = easyui.createlistbox(16, 392, WIN_W - 32, WIN_H - 400);

    easyui.recallfunc(EASYUI_WINDOW, ONFRAME, on_frame);
    easyui.recallfunc(EASYUI_WINDOW, ONCLOSE, on_close);
    easyui.settimer(2000, refresh_network_label);
    refresh_network_label(0);

    g_server_count = dlr_db_load(g_servers, DLR_MAX_SERVERS);
    if (g_server_count < 0) g_server_count = 0;
    fill_servers();
    update_server_status();
    show_tab(0);

    log_line("Welcome to Deliver Manager.");
    if (!network_ready(0)) {
        log_line("The network is not up yet. Wait for DHCP, or run 'dhcp' in a terminal.");
    } else if (!g_server_count) {
        log_line("Press 'Scan network' to find Deliver servers, or add one by address.");
    }

    return easyui.run();
}
