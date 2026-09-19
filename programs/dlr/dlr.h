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

/* MinimaFS layout the client owns. */
#define DLR_CONFIG_DIR   "0:/etc/dlr"
#define DLR_SERVERS_FILE "0:/etc/dlr/servers.txt"
#define DLR_CACHE_DIR    "0:/var/dlr"
#define DLR_STAGE_FILE   "0:/var/dlr/download.tar"
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
