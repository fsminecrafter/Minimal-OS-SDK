#ifndef DLR_REGISTRY_H
#define DLR_REGISTRY_H

#include "dlr.h"

/*
 * Server-side package store, on MinimaFS. The C++ server's
 * package_registry.cpp scans a directory of .pkg manifests with
 * std::filesystem and shells out to tar; neither exists here, so this
 * is the same job in plain C over the port layer.
 *
 * Layout under the store root (DLR_PKG_STORE by default):
 *
 *     <root>/<name>/            the package's files, incl. its .pkg manifest
 *     <root>/<name>.mpkg        the archive clients download
 *
 * The archive is built ONCE, when the package is presented, not per
 * download. Two reasons that are specific to this platform: the
 * kernel's archiver writes to a fixed "<dir>.mpkg" path, so two
 * clients asking for the same package at once would race on one output
 * file; and compressing costs real time in a VM. A package is a
 * directory that has an archive beside it - nothing else is kept, so
 * there is no index to fall out of step with the files.
 */

#define DLR_REG_NAME_MAX  64
#define DLR_REG_VERSION_MAX 32
#define DLR_REG_DESC_MAX  160

typedef struct {
    char name[DLR_REG_NAME_MAX];        // what INSTALL_REQUEST must ask for
    char version[DLR_REG_VERSION_MAX];
    char description[DLR_REG_DESC_MAX];
} dlr_reg_entry;

// Overrides the store root (default DLR_PKG_STORE). Used by the host
// tests; the string is not copied and must outlive its use.
void dlr_reg_set_root(const char* root);
const char* dlr_reg_root(void);

// 1 if `name` is safe to use as a directory name: 1..63 characters from
// [A-Za-z0-9._-], not starting with '.'. Stricter than the C++ server
// (which only rejects "..", "/" and "\"): a name becomes a MinimaFS
// path here, and MinimaFS treats ':' as a drive separator.
int dlr_reg_name_ok(const char* name);

// Calls cb for every presented package (a directory with an archive
// beside it), in directory order. Stops if cb returns 0. Returns the
// number of entries reported.
typedef int (*dlr_reg_cb)(void* user, const dlr_reg_entry* entry);
int dlr_reg_each(dlr_reg_cb cb, void* user);

// Same, but only packages whose name or description contains `query`
// (case-insensitive). An empty query matches everything.
int dlr_reg_search(const char* query, dlr_reg_cb cb, void* user);

// 1 if `name` is a presented package; its archive path goes to `out`.
int dlr_reg_find(const char* name, char* out, size_t out_size);

// Copies the directory `src_dir` into the store as `name` (or, if name
// is NULL, the manifest's [Info] name) and builds its archive. Refuses
// to overwrite an existing package - remove it first. `err` receives a
// one-line reason on failure. Returns 1 on success.
int dlr_reg_present(const char* src_dir, const char* name, char* err, size_t err_size);

// Rebuilds the archive of a package already in the store (after its
// files were edited in place).
int dlr_reg_rebuild(const char* name, char* err, size_t err_size);

// Deletes the package's files and archive. 1 if it existed.
int dlr_reg_remove(const char* name);

#endif // DLR_REGISTRY_H
