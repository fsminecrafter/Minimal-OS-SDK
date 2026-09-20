#ifndef DLR_PROTO_H
#define DLR_PROTO_H

#include "dlr.h"
#include "dlr_crypto.h"

/*
 * Deliver's wire protocol, client side.
 *
 * Frame:   4-byte length prefix, BIG-endian, then that many bytes.
 *          (network.hpp's comment says little-endian. send_frame()
 *          calls htonl(). The code is the spec.)
 *
 * Session: server sends an unencrypted frame
 *              "DLR_SERVER|<name>|<needs_pw 0|1>|<proto>"
 *          client replies with an unencrypted frame
 *              "KEY:<base64 of 32 random bytes>"
 *          if the server needs a password it then sends an
 *          unencrypted AUTH_REQUEST, reads the password as an
 *          unencrypted frame, and answers HELLO_ACK or ERROR.
 *          Everything after that is sealed with the session key.
 *
 * Sealed payload: nonce(12) || AES-256-GCM(ciphertext) || tag(16),
 *          no associated data, plaintext being [type byte][body].
 *
 * Note what the key exchange is not: the session key travels in the
 * clear, so the encryption stops a passive listener from reading the
 * bytes but stops nothing else. Matching that is intentional here -
 * being "more secure" unilaterally just means being incompatible.
 * Fixing it means changing server and every client together behind a
 * PROTOCOL_VERSION bump.
 */

typedef struct {
    long     sock;
    uint8_t  key[DLR_KEY_LEN];
    int      encrypted;               /* a session key is in use */
    char     server_name[DLR_NAME_MAX];
    int      needs_password;
    int      protocol_version;

    uint8_t* frame;                   /* DLR_MAX_FRAME scratch, heap */
    uint8_t* plain;                   /* DLR_MAX_FRAME scratch, heap */
} dlr_session;

/*
 * Raw framing, exposed so the server (dlr_server.c) uses the very same
 * code as the client instead of a second copy that could drift. A frame
 * is a 4-byte big-endian length then that many bytes.
 *
 * dlr_frame_recv returns the frame length or -1; it refuses a length
 * over `cap` before reading a byte of the body.
 */
int  dlr_frame_send(long sock, const uint8_t* payload, uint32_t len);
long dlr_frame_recv(long sock, uint8_t* buf, uint32_t cap, uint32_t timeout_ms);

// Allocates the two scratch buffers. Returns 0 on success.
int  dlr_session_alloc(dlr_session* s);
void dlr_session_free(dlr_session* s);

// Connects and performs the handshake (including auth if the server
// asks and `password` is non-NULL). Returns 1 on success.
int  dlr_connect(dlr_session* s, uint32_t ip, uint16_t port, const char* password);
void dlr_disconnect(dlr_session* s);

// Sends [type][body] sealed if a session key is in use.
int  dlr_send_msg(dlr_session* s, uint8_t type, const void* body, uint32_t body_len);

// Receives one message. On success *out_type is the type byte and the
// body is in s->plain with *out_len bytes. Returns 1, or 0 on close,
// timeout, or a tag that does not verify.
int  dlr_recv_msg(dlr_session* s, uint8_t* out_type, uint32_t* out_len,
                  uint32_t timeout_ms);

// Probe without a full session: returns 1 and fills `out` if the
// address answers with a Deliver HELLO.
int  dlr_probe(uint32_t ip, uint16_t port, dlr_server* out, uint32_t timeout_ms);

// Round-trip latency in ms, or -1.
long dlr_ping(dlr_session* s);

/*
 * Downloads a package to `stage_path`, verifying the SHA-256 the
 * server sends in INSTALL_END. Returns:
 *    1  success
 *    0  transport/protocol failure
 *   -1  server replied INSTALL_ERROR (message in `err`, if non-NULL)
 * *out_format (if non-NULL) is DLR_FORMAT_TAR or DLR_FORMAT_MPKG, taken
 * from the size header; it is only meaningful when 1 is returned.
 * Progress is reported through `on_progress` (may be NULL); received
 * may exceed nothing useful until the SIZE header arrives, at which
 * point total becomes non-zero.
 */
int dlr_download(dlr_session* s, const char* pkg_name, const char* stage_path,
                 void (*on_progress)(uint64_t received, uint64_t total),
                 int* out_format,
                 char* err, size_t err_size);

#endif // DLR_PROTO_H
