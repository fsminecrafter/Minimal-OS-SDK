#ifndef DLR_DB_H
#define DLR_DB_H

#include "dlr.h"

/*
 * The known-servers list, and how it gets populated.
 *
 * Persisted as plain text in MinimaFS rather than anything structured:
 * it is at most a handful of lines, and a format a human can read in
 * the terminal is worth more here than one a parser likes.
 */

int dlr_db_load(dlr_server* servers, int max);
int dlr_db_save(const dlr_server* servers, int count);

// Adds or refreshes one entry. Returns the new count.
int dlr_db_add(dlr_server* servers, int count, int max, const dlr_server* add);

// Adds the servers queued in DLR_REPOS_FILE ("host[:port]" or
// "tls://host[:port]" per line): each is resolved and probed, added on
// success and dropped from the file; the rest stay queued for next time.
// say (may be NULL) gets one line per server tried. Returns the new count.
int dlr_db_take_queued(dlr_server* servers, int count, int max,
                       void (*say)(const char* line));

// Listens on UDP 4243 for window_ms, broadcasting a probe first.
// Returns the number of distinct servers heard, or -1 if the port
// could not be bound.
int dlr_discover(dlr_server* servers, int max, uint32_t window_ms,
                 void (*on_found)(const dlr_server*));

#endif // DLR_DB_H
