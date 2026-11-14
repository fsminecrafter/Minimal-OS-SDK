# Virtual memory

Memory of your own in whole 4 KiB pages: reserve a large range, run
generated code, give memory back to the system. Include `vm.h` (it includes
`minimalos.h`). The raw calls it builds on (`mos_vm_map_raw`,
`mos_vm_unmap_raw`, `mos_vm_protect_raw`, `mos_vm_decommit_raw`) are in
`minimalos.h`; the kernel call is `SYS_VM` (55).

```c
#include "vm.h"
#include "stdio.h"

int main(void) {
    uint8_t* big = mos_vm_map(NULL, (size_t)1 << 30, MOS_VM_PROT_RW, 0);   /* 1 GiB */
    if (!big) { printf("map failed (%ld)\n", mos_vm_last_error()); return 1; }

    big[123456789] = 1;                 /* only this page takes physical memory */
    mos_vm_decommit(big, (size_t)1 << 30);   /* give every page back, keep the range */
    mos_vm_unmap(big, (size_t)1 << 30);      /* remove it */
    return 0;
}
```

## What you get

- **Demand-zero pages.** A page reads as zero until it is written and takes
  physical memory from its first use. A mapping that is never touched costs
  nothing, so mapping 1 GiB "just in case" is cheap. If memory has run out
  when a page is first touched, the program is ended with a page fault;
  `MOS_VM_MAP_POPULATE` takes all the memory at once and fails with
  `SYS_ERR_NOMEM` instead.
- **Guard pages.** A mapping the kernel places (no `MOS_VM_MAP_FIXED`) has
  an unmapped page on each side. Reading or writing just before or after it
  is a page fault (CPU exception 14, reported to a waiting parent as
  `crashed` with `crash_vector == 14`), not silent corruption.
- **No execution by default.** Pages are not executable unless you give
  `MOS_VM_PROT_EXEC`. Running code from a mapping without it, from the main
  stack, or from `malloc` memory is a page fault.
- **Addresses from 2 TiB up** (`0x20000000000`), below the main stack.
  Your program's image is at 1 TiB, the main stack ends at
  `0x7FFFF0000000`, and `malloc` memory stays below 4 GiB.

Addresses given to `mos_vm_unmap`, `mos_vm_protect` and `mos_vm_decommit`
must be page aligned; lengths are rounded up to whole pages.

## Constants

| Name | Value | Meaning |
| --- | --- | --- |
| `MOS_VM_PAGE_SIZE` | 4096 | The page size |
| `MOS_VM_PROT_NONE` | 0 | No access (contents are kept) |
| `MOS_VM_PROT_READ` | 1 | Readable |
| `MOS_VM_PROT_WRITE` | 2 | Writable (implies readable) |
| `MOS_VM_PROT_EXEC` | 4 | Executable (implies readable) |
| `MOS_VM_PROT_RW` | 3 | Read and write |
| `MOS_VM_PROT_RX` | 5 | Read and execute |
| `MOS_VM_MAP_FIXED` | 1 | Map exactly at the given address, or fail |
| `MOS_VM_MAP_POPULATE` | 2 | Take the physical memory now |

## `mos_vm_map`

```c
void* mos_vm_map(void* hint, size_t length, int prot, int flags);
void* mos_vm_map_ex(void* hint, size_t length, int prot, int flags, long* error);
long  mos_vm_last_error(void);
```

Maps `length` bytes (rounded up to whole pages) with protection `prot`.

| Parameter | Description |
| --- | --- |
| `hint` | NULL: anywhere. Otherwise the lowest address you want: the kernel uses the first free place at or above it (the hint itself when it is free). With `MOS_VM_MAP_FIXED`: the exact address, which must be page aligned and free |
| `length` | Bytes. Must be greater than 0 |
| `prot` | `MOS_VM_PROT_*` bits |
| `flags` | 0, or `MOS_VM_MAP_FIXED` and/or `MOS_VM_MAP_POPULATE` |
| `error` | `mos_vm_map_ex` only: receives 0 or the error (may be NULL) |

**Returns** the address, or NULL on failure. The reason is in `*error` for
`mos_vm_map_ex`, and in `mos_vm_last_error()` (the last `mos_vm_map` call,
0 after a success; one value for the whole program) for `mos_vm_map`.
Errors: `SYS_ERR_INVAL` (zero or too large length, unknown `prot` or
`flags` bits, a `FIXED` address that is unaligned, outside user space or
already (partly) mapped), `SYS_ERR_NOMEM` (no room, or `POPULATE` could not
get all its pages; nothing stays mapped then).

**Notes**

- `FIXED` never replaces anything and adds no guard pages, so two fixed
  mappings can touch.
- With `POPULATE`, a size that cannot be satisfied (for example 64 GiB)
  fails with `SYS_ERR_NOMEM`. The attempt briefly uses all free memory, so
  do not use it to probe how much there is.

## `mos_vm_unmap`

```c
long mos_vm_unmap(void* address, size_t length);
```

Removes the pages of `[address, address + length)` and frees their memory.
Pages in the range that are not mapped are skipped, so unmapping part of a
mapping leaves the two pieces (each keeps its data; the hole faults), and
unmapping an unmapped range succeeds.

**Returns** `SYS_SUCCESS`, or `SYS_ERR_INVAL` (unaligned address, zero
length, outside user space).

## `mos_vm_protect`

```c
long mos_vm_protect(void* address, size_t length, int prot);
```

Changes the protection of `[address, address + length)`. The whole range
must be mapped, otherwise nothing changes. Contents are kept, also while
the protection is `MOS_VM_PROT_NONE`. This is how generated code is made
runnable: write it while the pages are read/write, then protect them
`MOS_VM_PROT_RX`.

**Returns** `SYS_SUCCESS`, or `SYS_ERR_INVAL` (part of the range is not
mapped, unaligned address, zero length, unknown `prot` bits),
`SYS_ERR_NOMEM`.

## `mos_vm_decommit`

```c
long mos_vm_decommit(void* address, size_t length);
```

Gives the physical memory of the range back. The pages stay mapped with
their protection and read as zero again. The whole range must be mapped.

**Returns** `SYS_SUCCESS` or `SYS_ERR_INVAL`.

## `mos_vm_reserve` and `mos_vm_commit`

```c
void* mos_vm_reserve(size_t length);
long  mos_vm_commit(void* address, size_t length);
```

`mos_vm_reserve` maps `length` bytes with `MOS_VM_PROT_NONE`: address space
that nothing can touch and that costs no memory. `mos_vm_commit` makes part
of it read/write (`mos_vm_protect(..., MOS_VM_PROT_RW)`); pages still take
memory only when first used. A growing buffer can reserve its maximum size
once and commit as it grows, and never moves.

## Example: running generated code

```c
static const uint8_t code[] = { 0xB8, 0x2A, 0, 0, 0, 0xC3 };   /* mov eax,42 ; ret */

uint8_t* p = mos_vm_map(NULL, 4096, MOS_VM_PROT_RW, 0);
for (unsigned i = 0; i < sizeof code; i++) p[i] = code[i];
mos_vm_protect(p, 4096, MOS_VM_PROT_RX);
int v = ((int (*)(void))p)();                  /* 42 */
```

## Threads on mapped stacks

`thread.h` adds
`int mos_thread_create_vm(mos_thread_t* t, void (*fn)(void*), void* arg, size_t stack_size, void* tls)`:
like `mos_thread_create_ex`, but the stack is a `SYS_VM` mapping (`0` for
128 KiB, rounded up to pages). It takes memory only as the thread uses it,
has an unmapped guard page on both sides, and `mos_thread_join` unmaps it.
A thread's entry point may be in any executable mapping (the image or a
`MOS_VM_PROT_EXEC` mapping) when you call `SYS_THREAD` yourself; code in a
non-executable mapping, in `malloc` memory or on the stack is refused with
`SYS_ERR_INVAL`.

## The system call

`SYS_VM` (55): `rdi` = operation, `SYS_VM_MAP` (`rsi` = `syscall_vm_map_t*`
with `size`, `address`, `length`, `prot`, `flags`), `SYS_VM_UNMAP`
(`rsi` = address, `rdx` = length), `SYS_VM_PROTECT` (also `r10` = prot) or
`SYS_VM_DECOMMIT` (`rsi` = address, `rdx` = length).
