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
 *     server falls back to plaintext; no shipped client ever asks for
 *     that, and accepting it only gives a network attacker a downgrade.
 *
 * Process model. Minimal-OS has no threads, but it has processes, and
 * the kernel keeps connection handles in a global table. So the server
 * is one accept loop (dlr_server_run) that starts a copy of this same
 * program per client in per-connection mode (`--conn <handle> ...`,
 * dlr_server_child), handing the accepted connection to it with
 * SYS_NET_TCP_HANDOFF. If a child dies the kernel closes its
 * connection; if the accept loop dies the kernel closes its listener.
 */

typedef struct {
    char     name[DLR_NAME_MAX];   // shown to clients; no '|' or newline
    char     pw_hash[65];          // lowercase SHA-256 hex; "" = no password
    uint16_t port;
} dlr_server_cfg;

void dlr_server_cfg_default(dlr_server_cfg* cfg);

// Reads --name / --port / --password / --pwhash from argv[start..].
// Returns 1 if every argument was understood; *bad points at the first
// one that was not.
int dlr_server_parse_args(int argc, char** argv, int start, dlr_server_cfg* cfg, const char** bad);

// Accept loop. Runs until interrupted; returns a process exit code.
int dlr_server_run(const dlr_server_cfg* cfg);

// One client session on an already-accepted connection. Closes `conn`.
// Returns 0 if the client left normally.
int dlr_server_session(long conn, const dlr_server_cfg* cfg, uint32_t peer_ip);

// Entry point for `<self> --conn <handle> <a.b.c.d> [--name N] [--pwhash H]`.
// argv[1] must be "--conn".
int dlr_server_child(int argc, char** argv);

#endif // DLR_SERVER_H
