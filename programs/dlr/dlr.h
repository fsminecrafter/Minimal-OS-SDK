#ifndef DLR_H
#define DLR_H

#include <stdint.h>
#include <stddef.h>

/* Wire constants - these mirror Deliver's types.hpp and must not drift. */
#define DLR_DEFAULT_PORT    4242
#define DLR_DISCOVERY_PORT  4243
#define DLR_PROTOCOL_VERSION 1

/* Message types (first byte of a decrypted payload). */
#define DLR_MSG_HELLO            0x01
#define DLR_MSG_HELLO_ACK        0x02
#define DLR_MSG_AUTH_REQUEST     0x03
#define DLR_MSG_AUTH_RESPONSE    0x04
#define DLR_MSG_INSTALL_REQUEST  0x10
#define DLR_MSG_INSTALL_DATA     0x11
#define DLR_MSG_INSTALL_END      0x12
#define DLR_MSG_INSTALL_ERROR    0x13
#define DLR_MSG_SEARCH_REQUEST   0x20
#define DLR_MSG_SEARCH_RESULT    0x21
#define DLR_MSG_PKG_LIST         0x22
#define DLR_MSG_PING             0x30
#define DLR_MSG_PONG             0x31
#define DLR_MSG_ERROR            0xFF

/*
 * The server streams the package in 64 KiB plaintext chunks
 * (BUFFER_SIZE in types.hpp), each sealed into its own frame, so a
 * frame can be 64 KiB + 1 type byte + 28 bytes of GCM overhead. Round
 * up and refuse anything larger rather than trusting the length
 * prefix: it arrives before authentication and a hostile one would
 * otherwise size an allocation.
 */
#define DLR_MAX_FRAME  (72u * 1024u)

/*
 * Archive format of a download.
 *
 * A Linux/Windows dlr_server tars the package and sends the tar. A
 * Minimal-OS server sends its native .mpkg instead (it already has an
 * archiver in the kernel and no tar writer). The server says which in
 * the size header that opens every transfer:
 *
 *     "SIZE:<bytes>"                                    a tar (legacy, unchanged)
 *     "SIZE:<bytes>|FORMAT=mpkg|OS=minimalos"           an .mpkg
 *
 * This is deliberately an extension of the existing header rather than
 * a new message: every shipped client reads the number after "SIZE:"
 * and stops at the first non-digit, so an old client keeps working as
 * far as the download goes. What an old client cannot do is extract the
 * result - it will fail at the tar step - which is why newer clients on
 * other platforms check the FORMAT field and say so up front.
 */
#define DLR_FORMAT_TAR   0
#define DLR_FORMAT_MPKG  1

/* Server-side layout (Minimal-OS only). */
#define DLR_SERVER_DIR        "0:/var/dlrd"
#define DLR_PKG_STORE         "0:/var/dlrd/packages"
#define DLR_HELLO_SRC_PORT    4244    /* source port of the UDP hello */
#define DLR_HELLO_INTERVAL_MS 5000
#define DLR_MAX_CLIENTS       3       /* concurrent per-connection processes */
#define DLR_SERVE_CHUNK       16384   /* plaintext bytes per INSTALL_DATA frame */
#define DLR_CLIENT_IDLE_MS    120000  /* drop a client that says nothing this long */

/* MinimaFS layout the client owns. */
#define DLR_CONFIG_DIR   "0:/etc/dlr"
#define DLR_SERVERS_FILE "0:/etc/dlr/servers.txt"
#define DLR_CACHE_DIR    "0:/var/dlr"
/* Named for what it is on the wire, not what it might be: the archive
 * is a tar when it came from a Linux/Windows server and an .mpkg when it
 * came from a Minimal-OS one, and the client only finds out from the
 * SIZE header. */
#define DLR_STAGE_FILE   "0:/var/dlr/download.bin"
#define DLR_INSTALL_DIR  "0:/programs"

#define DLR_MAX_SERVERS   16
#define DLR_NAME_MAX      64

typedef struct {
    uint32_t ip;
    uint16_t port;
    uint8_t  needs_password;
    char     name[DLR_NAME_MAX];
} dlr_server;

/* Parsed .pkg manifest - only the fields this platform can act on. */
typedef struct {
    char name[DLR_NAME_MAX];
    char version[32];
    char description[160];
    char arch[16];
    char operatingsystem[96];

    /* [Install.minimalos] */
    char mos_copy[256];     /* comma-separated file list, "" = everything */
    char mos_target[128];   /* destination directory */
    char mos_run[128];      /* one .run bundle to launch after copying */

    int  has_minimalos_section;
} dlr_pkg;

#endif // DLR_H
