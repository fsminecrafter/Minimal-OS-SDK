#ifndef MINIMALOS_STORAGE_H
#define MINIMALOS_STORAGE_H

/*
 * Disks and running kernel commands as root.
 *
 *   mos_storage_list()    disks and partitions (SYS_STORAGE, read-only)
 *   mos_storage_status()  is an install payload loaded, which device is 0:
 *   mos_root_auth()       check a MOSdesk account password (SYS_ROOT)
 *   mos_root_run()        start a kernel terminal command as root
 *   mos_root_read()       collect its output; tells whether it still runs
 */

#include <stdint.h>
#include <stddef.h>
#include "syscall.h"
#include "string.h"
#include "x86_64/user_syscalls.h"

static inline void mos_storage__copy(char* dst, const char* src, size_t size) {
    size_t i = 0;
    for (; src && src[i] && i + 1 < size; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static inline long mos_storage_list(syscall_storage_device_t* devices, uint32_t capacity) {
    syscall_storage_request_t r;
    memset(&r, 0, sizeof(r));
    r.size = sizeof(r);
    r.version = SYS_STORAGE_ABI_VERSION;
    r.op = SYS_STORAGE_LIST;
    r.capacity = capacity;
    r.devices = devices;
    return (long)mos_syscall1(SYS_STORAGE, (uint64_t)(uintptr_t)&r);
}

static inline long mos_storage_status(int* install_payload, char boot_device[16]) {
    syscall_storage_request_t r;
    memset(&r, 0, sizeof(r));
    r.size = sizeof(r);
    r.version = SYS_STORAGE_ABI_VERSION;
    r.op = SYS_STORAGE_STATUS;
    long res = (long)mos_syscall1(SYS_STORAGE, (uint64_t)(uintptr_t)&r);
    if (install_payload) *install_payload = (int)r.install_payload;
    if (boot_device) memcpy(boot_device, r.boot_device, 16);
    return res;
}

static inline uint64_t mos_root_call(syscall_root_t* r) {
    return mos_syscall1(SYS_ROOT, (uint64_t)(uintptr_t)r);
}

// 0 on success (a ticket valid for a few minutes), SYS_ERR_PERM if wrong.
static inline long mos_root_auth(const char* username, const char* password) {
    static syscall_root_t r;
    memset(&r, 0, sizeof(r));
    r.op = SYS_ROOT_AUTH;
    mos_storage__copy(r.username, username, sizeof(r.username));
    mos_storage__copy(r.password, password, sizeof(r.password));
    long res = (long)mos_root_call(&r);
    memset(&r, 0, sizeof(r));
    return res;
}

static inline long mos_root_run(const char* command) {
    static syscall_root_t r;
    memset(&r, 0, sizeof(r));
    r.op = SYS_ROOT_RUN;
    mos_storage__copy(r.command, command, sizeof(r.command));
    return (long)mos_root_call(&r);
}

// Appends new output to out (NUL-terminated, at most out_size - 1 bytes
// total). Returns 1 while the command runs, 0 when done, <0 on error.
static inline long mos_root_read(char* out, size_t out_size) {
    static syscall_root_t r;
    memset(&r, 0, sizeof(r));
    r.op = SYS_ROOT_READ;
    long res = (long)mos_root_call(&r);
    if (res != 0) return res < 0 ? res : -1;
    size_t have = strlen(out);
    for (uint32_t i = 0; i < r.output_length && have + 1 < out_size; i++) out[have++] = r.output[i];
    out[have] = '\0';
    return r.running ? 1 : 0;
}

#endif
