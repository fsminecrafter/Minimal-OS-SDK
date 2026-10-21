#ifndef DLR_SERVER_H
#define DLR_SERVER_H

#include "dlr.h"

/*
 * Deliver SERVER for Minimal-OS. Wire-compatible with the C++
 * dlr_server (same framing, handshake, message types and crypto - it
 * reuses dlr_proto.c and dlr_crypto.c, so client and server cannot
 * disagree about either), with two deliberate differences:
 *
 *   - Downloads are .mpkg archives, not tars, announced in the SIZE
 *     header (see dlr.h). There is no tar writer on this platform.
 *   - A connection that does not open with "KEY:" is dropped. The C++
 * accept loop (dlr_server_run) that starts dlrconnect.run per client in
 * per-connection mode (`--conn <handle> ...`, dlr_server_child), handing
 * the accepted connection to it with
 * Process model. Minimal-OS has no threads, but it has processes, and
 * the kernel keeps connection handles in a global table. So the server
 * is one accept loop (dlr_server_run) that starts a copy of this same
 * program per client in per-connection mode (`--conn <handle> ...`,
 * dlr_server_child), handing the accepted connection to it with
 * SYS_NET_TCP_HANDOFF. If a child dies the kernel closes its
 * connection; if the accept loop dies the kernel closes its listener.
 */

typedef struct {
    char     name[DLR_NAME_MAX];
    char     pw_hash[65];
    uint16_t port;
    uint8_t  tls;          // 1 = serve the ktls record layer (forces port DLR_TLS_PORT)
} dlr_server_cfg;

void dlr_server_cfg_default(dlr_server_cfg* cfg);

// Reads --name / --port / --password / --pwhash from argv[start..].
// Returns 1 if every argument was understood; *bad points at the first
// one that was not.
int dlr_server_parse_args(int argc, char** argv, int start, dlr_server_cfg* cfg, const char** bad);

// Fills a configuration without going through argv (what --name,
// --password, --port and --tls do). `port` 0 means the default; an
// empty password means none. Returns 0 if the port and TLS disagree.
int dlr_server_cfg_set(dlr_server_cfg* cfg, const char* name, const char* password,
                       int port, int tls);

/*
 * The accept loop in pieces, for a program that has its own main loop
 * (the Deliver Manager app): start, then poll every few milliseconds,
 * then stop. dlr_server_run below is exactly that with a sleep.
 */
typedef struct {
    dlr_server_cfg cfg;
    long     listener;
    long     udp;
    char     hello[DLR_NAME_MAX + 32];
    const char* child_args[6];
    int      child_argc;
    long     children[DLR_MAX_CLIENTS];
    uint64_t next_hello;
    uint64_t next_reap;
    int      accept_errors;
    unsigned clients_served;

    // Called just before a client is served inside this process (which
    // blocks until the client leaves). May be NULL.
    void   (*on_client)(void* user, uint32_t peer_ip);
    void*    user;
} dlr_server_state;

// Listens and prepares the discovery hello. The state must stay where it
// is until dlr_server_stop. Returns 1 on success.
int  dlr_server_start(dlr_server_state* st, const dlr_server_cfg* cfg);

// One pass: discovery hello when due, then at most one accept. Returns
// 1 if a client was handled, 0 if nobody was waiting, -1 if the
// listener has failed (the server is then stopped).
int  dlr_server_poll(dlr_server_state* st);
void dlr_server_stop(dlr_server_state* st);

// Accept loop. Runs until interrupted; returns a process exit code.
int dlr_server_run(const dlr_server_cfg* cfg);

// One client session on an already-accepted connection. Closes `conn`.
// Returns 0 if the client left normally.
int dlr_server_session(long conn, const dlr_server_cfg* cfg, uint32_t peer_ip);

// Entry point for `<self> --conn <handle> <a.b.c.d> [--name N] [--pwhash H]`.
// argv[1] must be "--conn".
int dlr_server_child(int argc, char** argv);

#endif // DLR_SERVER_H
