#ifndef DLR_MPKG_H
#define DLR_MPKG_H

#include "dlr_tar.h"    /* dlr_tar_sink: shared so the install path is format-agnostic */

/*
 * Read-only .mpkg extractor.
 *
 * .mpkg is Minimal-OS's own archive (layout in the kernel's
 * pkgformat.h). The kernel can unpack one to disk (mos_pkg_unzip), but
 * that goes straight to a directory; the client's install path wants
 * files handed to it one at a time, in memory, so it can read the
 * manifest before deciding where anything goes - exactly what
 * dlr_tar_extract_mem gives it for tar. This is the same thing for
 * .mpkg, and it fills the same dlr_tar_sink.
 *
 * Both compression methods the kernel writes are handled: STORE and
 * LZSS. The LZSS decoder is a copy of the kernel's (lzss.c) because a
 * user program cannot link kernel code; tests/dlr-host checks it
 * against the kernel's own compressor.
 */

// Returns 1 if the buffer starts like an .mpkg (magic + supported version).
int  dlr_mpkg_is_archive(const uint8_t* archive, size_t len);

// Extracts from an in-memory archive. Returns the number of entries
// handled, or -1 if the archive is malformed, an entry name is unsafe,
// or the sink aborts.
long dlr_mpkg_extract_mem(const uint8_t* archive, size_t len, const dlr_tar_sink* sink);

// Decodes one LZSS payload. Returns the number of bytes produced, or 0 on
// any error (including output that would exceed out_cap).
uint32_t dlr_lzss_decode(const uint8_t* in, uint32_t in_size, uint8_t* out, uint32_t out_cap);

#endif // DLR_MPKG_H
