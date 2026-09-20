#ifndef DLR_TAR_H
#define DLR_TAR_H

#include <stdint.h>
#include <stddef.h>

/*
 * Read-only ustar (POSIX.1-1988) extractor.
 *
 * The Linux/Windows Deliver server builds its bundles by shelling out
 * to /usr/bin/tar, and it has to keep doing that - switching it to
 * .mpkg would break every existing client on those platforms. So the
 * Minimal-OS side learns to read tar rather than that server learning
 * to write something else. (A Minimal-OS server is the other way
 * round: it sends .mpkg, its native archive - see dlr_mpkg.h and the
 * FORMAT field described in dlr.h. dlr_tar_sink is shared by both
 * extractors so the install path does not care which it got.)
 *
 * Only what a package actually contains is handled: regular files
 * (type '0' and the legacy '\0'), directories ('5'), and GNU long
 * names ('L'). Symlinks, hardlinks, devices and sparse files are
 * skipped with a warning - a package that needs them is a package this
 * platform cannot install anyway.
 */

typedef struct {
    // Called once per regular file. Return 0 to abort extraction.
    // `name` is the path inside the archive, already checked for
    // traversal.
    int (*on_file)(void* user, const char* name, const uint8_t* data, uint32_t len);
    int (*on_dir)(void* user, const char* name);
    void* user;
} dlr_tar_sink;

// Extracts from an in-memory archive. Returns the number of entries
// handled, or -1 on a malformed archive.
long dlr_tar_extract_mem(const uint8_t* archive, size_t len, const dlr_tar_sink* sink);

// Rejects absolute paths, "..", drive prefixes and backslashes.
// Returns 1 if the name is safe to join onto an install directory.
int dlr_tar_name_is_safe(const char* name);

#endif // DLR_TAR_H
