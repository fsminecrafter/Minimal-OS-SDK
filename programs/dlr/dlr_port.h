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

/* --- reading files, directories --------------------------------------- */

// Opens an existing file for reading. NULL on failure. Independent of the
// write handle above: a copy needs one of each open at the same time.
dlr_file* dlr_file_open_read(const char* path);

// Up to `len` bytes; returns the count (0 at end of file) or -1.
long      dlr_file_read(dlr_file* f, void* buf, uint32_t len);

int  dlr_is_dir(const char* path);

// Calls cb once per entry of a directory ("." and ".." are never
// reported). Stops early if cb returns 0. Returns the number of
// entries visited, or -1 if the directory cannot be read.
typedef int (*dlr_dir_cb)(void* user, const char* name, int is_dir);
int  dlr_dir_each(const char* path, dlr_dir_cb cb, void* user);

int  dlr_remove_dir(const char* path);     // empty directories only

// Archives a directory into "<dir>.mpkg" beside it (Minimal-OS's native
// package format) and writes that path to `out_path`. 1 on success.
int  dlr_pack_dir(const char* dir, char* out_path, size_t out_cap);

/* --- server ------------------------------------------------------------ */

// Starts listening. Returns a listener handle >= 0, or DLR_INVALID.
long dlr_tcp_listen(uint16_t port);

// Non-blocking. 1 = accepted (*out_conn and *out_ip set), 0 = nobody
// waiting, -1 = error.
int  dlr_tcp_accept(long listener, long* out_conn, uint32_t* out_ip);
void dlr_tcp_unlisten(long listener);

// Starts a new process running this same program in per-connection mode
// (`<self> --conn <handle> <ip> <extra...>`) and passes it ownership of
// the connection. Returns the child's pid, or DLR_INVALID; on failure
// the caller still owns the connection and must close it.
long dlr_spawn_conn(long conn, uint32_t peer_ip, const char* const* extra, int extra_count);
int  dlr_proc_alive(long pid);

// Must be called once with argv[0] before dlr_spawn_conn.
void dlr_set_self(const char* argv0);

// Asks for `cb` to be called when the user interrupts the program
// (Ctrl+C in the terminal). It must only set a flag.
void dlr_on_interrupt(void (*cb)(void));

/* --- misc -------------------------------------------------------------- */

void dlr_random_bytes(void* buf, size_t len);

// Launches a program. Returns a pid/handle >= 0, or DLR_INVALID if the
// platform cannot do it. Used only by an [Install.minimalos] `run=`
// directive.
long dlr_exec(const char* path);

// Waits for a process started by dlr_exec to leave the process table.
// Returns 1 if it exited within the timeout.
int dlr_exec_wait(long pid, uint32_t timeout_ms);

/* --- TLS-style transport (Minimal-OS to Minimal-OS only) ---------------- */

/*
 * TLS handles are ordinary `long` handles with DLR_TLS_FLAG or'ed in, so
 * dlr_tcp_write / dlr_tcp_read_exact / dlr_tcp_close work on either kind
 * and nothing above the port layer needs to know which it has.
 */
#define DLR_TLS_FLAG 0x1000L

// Connects and completes the handshake. Returns a flagged handle or DLR_INVALID.
long dlr_tls_open(uint32_t ip, uint16_t port, uint32_t timeout_ms);

// Server side: upgrades an accepted TCP handle that this process owns.
// Consumes `tcp_handle` on BOTH success and failure (on failure it is
// closed). Returns a flagged handle or DLR_INVALID.
long dlr_tls_accept(long tcp_handle);

#endif // DLR_PORT_H
