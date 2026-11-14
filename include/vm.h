#ifndef MINIMALOS_VM_H
#define MINIMALOS_VM_H

/*
 * Virtual memory of your own (SYS_VM): whole 4 KiB pages that the kernel
 * places above 2 TiB, with a protection you choose.
 *
 *   uint8_t* big = mos_vm_map(NULL, 1u << 30, MOS_VM_PROT_RW, 0);   // 1 GiB
 *   if (!big) printf("failed: %ld\n", mos_vm_last_error());
 *   big[12345] = 1;                 // only this page takes physical memory
 *   mos_vm_decommit(big, 1u << 30); // give it all back, keep the mapping
 *   mos_vm_unmap(big, 1u << 30);    // remove the mapping
 *
 * What you get:
 *   - Pages read as zero until written and take physical memory only from
 *     their first use, so a large mapping that is never touched costs
 *     nothing. (Memory can still run out later: a first touch that finds
 *     none ends the program with a page fault. MOS_VM_MAP_POPULATE takes it
 *     all at once, and fails with SYS_ERR_NOMEM instead.)
 *   - A mapping the kernel places has an unmapped page on both sides:
 *     running off either end crashes (CPU exception 14) instead of
 *     silently touching something else.
 *   - Memory is not executable unless MOS_VM_PROT_EXEC is given, which
 *     makes a mapping the place for generated or loaded code. Thread entry
 *     points may be in such a mapping (SYS_THREAD).
 *
 * malloc() is separate and unchanged (a system call on the kernel heap
 * below 4 GiB, never executable); see docs/memory.md for when to use which.
 *
 * Addresses and lengths of unmap/protect/decommit are in whole pages:
 * `address` must be page aligned, `length` is rounded up. Errors are the
 * kernel's negative SYS_ERR_* values (SYS_ERR_INVAL, SYS_ERR_NOMEM).
 */

#include <stddef.h>
#include <stdint.h>
#include "minimalos.h"

#define MOS_VM_PAGE_SIZE 4096u

/* Protection. Writable implies readable, and so does executable. */
#define MOS_VM_PROT_NONE  SYS_VM_PROT_NONE
#define MOS_VM_PROT_READ  SYS_VM_PROT_READ
#define MOS_VM_PROT_WRITE SYS_VM_PROT_WRITE
#define MOS_VM_PROT_EXEC  SYS_VM_PROT_EXEC
#define MOS_VM_PROT_RW    (SYS_VM_PROT_READ | SYS_VM_PROT_WRITE)
#define MOS_VM_PROT_RX    (SYS_VM_PROT_READ | SYS_VM_PROT_EXEC)

/* Flags of mos_vm_map(). */
#define MOS_VM_MAP_FIXED    SYS_VM_MAP_FIXED     /* exactly at `hint`, which must be free */
#define MOS_VM_MAP_POPULATE SYS_VM_MAP_POPULATE  /* take the physical memory now */

/* The error of the last failed mos_vm_map() (0 after a success). One
 * value for the whole program, not per thread. */
long mos_vm_errno __attribute__((weak));

static inline long mos_vm_last_error(void) { return mos_vm_errno; }

/* Rounds up to a whole number of pages. */
static inline size_t mos_vm_round_up(size_t length) {
    return (length + (MOS_VM_PAGE_SIZE - 1u)) & ~(size_t)(MOS_VM_PAGE_SIZE - 1u);
}

/* Maps `length` bytes (rounded up to pages) with protection `prot`.
 * hint == NULL: anywhere. A hint without MOS_VM_MAP_FIXED is the lowest
 * address wanted (the kernel picks the first free place at or above it).
 * With MOS_VM_MAP_FIXED the mapping is at `hint` (page aligned) or the call
 * fails: nothing is ever replaced, and no guard pages are added.
 * On failure returns NULL and stores the reason in *error (may be NULL):
 * SYS_ERR_INVAL (zero or huge length, bad prot/flags, unaligned or busy
 * FIXED address) or SYS_ERR_NOMEM (no room, or POPULATE found too little
 * memory; nothing stays mapped then). */
static inline void* mos_vm_map_ex(void* hint, size_t length, int prot, int flags,
                                  long* error) {
    syscall_vm_map_t r;
    r.size = sizeof(r);
    r.address = (uint64_t)(uintptr_t)hint;
    r.length = length;
    r.prot = (uint32_t)prot;
    r.flags = (uint32_t)flags;
    long a = mos_vm_map_raw(&r);
    if (a < 0) {
        if (error) *error = a;
        return NULL;
    }
    if (error) *error = 0;
    return (void*)(uintptr_t)a;
}

/* mos_vm_map_ex() that keeps the error for mos_vm_last_error(). */
static inline void* mos_vm_map(void* hint, size_t length, int prot, int flags) {
    long e = 0;
    void* p = mos_vm_map_ex(hint, length, prot, flags, &e);
    mos_vm_errno = e;
    return p;
}

/* Removes the pages of [address, address+length). Pages that are not mapped
 * are skipped, so part of a mapping (leaving two pieces) or several
 * mappings at once can go. Returns 0, or SYS_ERR_INVAL (unaligned address,
 * zero length). */
static inline long mos_vm_unmap(void* address, size_t length) {
    return mos_vm_unmap_raw(address, length);
}

/* Changes the protection of [address, address+length), all of which must be
 * mapped (else SYS_ERR_INVAL and nothing changes). Contents are kept, also
 * while the protection is MOS_VM_PROT_NONE. Returns 0 or a negative error. */
static inline long mos_vm_protect(void* address, size_t length, int prot) {
    return mos_vm_protect_raw(address, length, (unsigned)prot);
}

/* Gives the physical memory of [address, address+length) back. The pages
 * stay mapped (with their protection) and read as zero again. The whole
 * range must be mapped. Returns 0 or a negative error. */
static inline long mos_vm_decommit(void* address, size_t length) {
    return mos_vm_decommit_raw(address, length);
}

/* Reserves address space: a PROT_NONE mapping that nothing can touch and
 * that costs no memory. Grow into it with mos_vm_commit(). NULL on failure. */
static inline void* mos_vm_reserve(size_t length) {
    return mos_vm_map(NULL, length, MOS_VM_PROT_NONE, 0);
}

/* Makes [address, address+length) of a reserved range readable and
 * writable (pages still take memory only when first used). Returns 0 or a
 * negative error. */
static inline long mos_vm_commit(void* address, size_t length) {
    return mos_vm_protect(address, length, MOS_VM_PROT_RW);
}

#endif
