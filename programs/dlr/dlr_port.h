#ifndef DLR_PORT_H
#define DLR_PORT_H

#include <stdint.h>
#include <stddef.h>

/*
 * Everything the client needs from the system underneath it.
 *
 * Two backends implement this: dlr_port_mos.c (Minimal-OS, SYS_NET +
 * MinimaFS) and, outside this folder, tests/dlr-host/dlr_port_host.c
 * (BSD sockets + stdio). The second one exists so the protocol and
 * install logic can be run against a real dlr_server on Linux before
 * anyone tries to debug it inside QEMU with a serial log.
 *
 * Keep this surface small. Every function added here is a function
 * that has to be written twice and kept honest twice.
 */

#define DLR_INVALID (-1)

/* --- lifecycle --------------------------------------------------------- */

// Returns 0 on success. On Minimal-OS this checks the interface is up.
int dlr_port_init(void);

/* --- time -------------------------------------------------------------- */

uint64_t dlr_now_ms(void);
void     dlr_sleep_ms(uint32_t ms);

/* --- TCP --------------------------------------------------------------- */

// Returns a handle >= 0, or DLR_INVALID.
long dlr_tcp_open(uint32_t ip, uint16_t port, uint32_t timeout_ms);

// 1 on success, 0 on failure. Handles partial sends.
int  dlr_tcp_write(long handle, const void* buf, uint32_t len);

// 1 if exactly len bytes were read before the timeout, 0 otherwise.
// The timeout is measured from the last byte received, not from entry,
// so a slow bulk transfer does not trip it.
int  dlr_tcp_read_exact(long handle, void* buf, uint32_t len, uint32_t idle_timeout_ms);

void dlr_tcp_close(long handle);

/* --- UDP --------------------------------------------------------------- */

long dlr_udp_open(uint16_t local_port);
int  dlr_udp_broadcast(long handle, uint16_t dst_port, const void* buf, uint32_t len);

// Returns the datagram length, or 0 if none arrived before the timeout.
long dlr_udp_read(long handle, void* buf, uint32_t len,
                  uint32_t* out_from_ip, uint32_t timeout_ms);
void dlr_udp_close(long handle);

// Dotted quad or hostname. Returns 0 on failure.
uint32_t dlr_resolve(const char* host);

/* --- files ------------------------------------------------------------- */

typedef struct dlr_file dlr_file;

// Creates or truncates. NULL on failure.
dlr_file* dlr_file_create(const char* path);
int       dlr_file_write(dlr_file* f, const void* buf, uint32_t len);
void      dlr_file_close(dlr_file* f);

// Size of an existing file in bytes, or -1.
long dlr_file_size(const char* path);

// Reads a whole file into buf. Returns the byte count, or -1.
long dlr_file_slurp(const char* path, void* buf, uint32_t max);

// Writes a whole file in one call, replacing any existing one.
int dlr_file_put(const char* path, const void* buf, uint32_t len);

// Creates every missing component of a directory path.
int dlr_mkdirs(const char* path);

int dlr_exists(const char* path);
int dlr_remove(const char* path);

/* --- misc -------------------------------------------------------------- */

void dlr_random_bytes(void* buf, size_t len);

// Launches a program. Returns a pid/handle >= 0, or DLR_INVALID if the
// platform cannot do it. Used only by an [Install.minimalos] `run=`
// directive.
long dlr_exec(const char* path);

// Waits for a process started by dlr_exec to leave the process table.
// Returns 1 if it exited within the timeout.
int dlr_exec_wait(long pid, uint32_t timeout_ms);

#endif // DLR_PORT_H
