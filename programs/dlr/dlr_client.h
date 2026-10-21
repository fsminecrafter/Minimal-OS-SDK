#ifndef DLR_CLIENT_H
#define DLR_CLIENT_H

#include "dlr.h"
#include "dlr_proto.h"

/*
 * Client operations shared by the `dlr` command and the Deliver Manager
 * app. Nothing in here prints: progress and messages go through a
 * dlr_io, so the terminal prints them and the app shows them in its log.
 */

typedef struct {
    // One finished line of text, without a newline. May be NULL.
    void (*log)(void* user, const char* line);
    // Download progress; total is 0 until the server sends the size.
    // May be NULL.
    void (*progress)(void* user, uint64_t received, uint64_t total);
    void* user;
} dlr_io;

// Writes "a.b.c.d" (at least 16 bytes).
void dlr_ip_format(uint32_t ip, char* out);

// Connects and performs the handshake. Returns 1 on success; on failure
// the reason has been logged and nothing needs freeing.
int  dlr_client_open(dlr_session* s, const dlr_server* srv, const char* password,
                     const dlr_io* io);
void dlr_client_close(dlr_session* s);

/*
 * Package lists. `list` and `search` both answer with
 * "name|version|description\n" lines (or "NONE"); this splits them.
 */
typedef struct {
    char name[DLR_NAME_MAX];
    char version[32];
    char description[160];
} dlr_pkg_info;

// Parses a PKG_LIST / SEARCH_RESULT body into `out`. Returns the count.
int dlr_parse_pkg_lines(const char* body, uint32_t len, dlr_pkg_info* out, int max);

// Asks a server for its packages (`query` NULL or "" = all of them).
// Returns the number filled into `out`, or -1 on failure (logged).
int dlr_client_packages(const dlr_server* srv, const char* password, const char* query,
                        dlr_pkg_info* out, int max, const dlr_io* io);

/*
 * Downloads `pkg_name` from `srv`, checks its SHA-256, and installs it
 * as its [Install.minimalos] section says (running its `run=` program,
 * if any). `force` installs a package that says it is for another
 * system. Returns 1 on success.
 */
int dlr_client_install(const dlr_server* srv, const char* password,
                       const char* pkg_name, int force, const dlr_io* io);

#endif // DLR_CLIENT_H
