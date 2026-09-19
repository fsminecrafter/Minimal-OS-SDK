#ifndef DLR_PKG_H
#define DLR_PKG_H

#include "dlr.h"

/*
 * Reader for Deliver's .pkg manifest (INI-ish), limited to what
 * Minimal-OS can act on: [Info] plus [Install.minimalos].
 *
 * [Install]'s installscript/installcommand are deliberately ignored -
 * both are shell, and there is no shell. See docs/platform-install.md
 * in the Deliver repo for why the Minimal-OS section declares intent
 * instead of a command line.
 */

void dlr_pkg_init(dlr_pkg* pkg);

// Returns 1 if the manifest had at least a name.
int dlr_pkg_parse(const char* text, size_t len, dlr_pkg* pkg);

// 1 if this package claims to run on Minimal-OS/x86_64. On 0, *why
// points at a short human-readable reason.
int dlr_pkg_runs_here(const dlr_pkg* pkg, const char** why);

// Iterates a comma-separated `copy=` list.
int dlr_pkg_next_copy_entry(const char** cursor, char* out, size_t out_size);

#endif // DLR_PKG_H
