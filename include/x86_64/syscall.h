#ifndef MINIMALOS_X86_64_SYSCALL_H
#define MINIMALOS_X86_64_SYSCALL_H

#include <stdint.h>

#define SYS_WRITE   1
#define SYS_EXIT    2
#define SYS_GETPID  3
#define SYS_UPTIME  4
#define SYS_SLEEP   5
#define SYS_OPEN    6
#define SYS_READ    7
#define SYS_CLOSE   8
#define SYS_EXISTS  9
#define SYS_IS_DIR  10
#define SYS_GETTIME 11
#define SYS_SEEK    12
#define SYS_SIZE    13
#define SYS_MKDIR   14
#define SYS_GRAPHICS 15
#define SYS_MANAGER  16
#define SYS_USB      17

#define SYS_FWRITE        18
#define SYS_LISTDIR       19
#define SYS_DELETE        20
#define SYS_RMDIR         21
#define SYS_GET_METADATA  22
#define SYS_TELL          23
#define SYS_EOF           24
#define SYS_PKG           25

#define SYS_O_RDONLY 0

#define SYS_SUCCESS       0
#define SYS_ERR_GENERIC  ((uint64_t)-1)
#define SYS_ERR_BADFD    ((uint64_t)-2)
#define SYS_ERR_NOTFOUND ((uint64_t)-3)
#define SYS_ERR_INVAL    ((uint64_t)-4)
#define SYS_ERR_PERM     ((uint64_t)-5)

typedef enum {
    SYS_GRAPHICS_GET_WIDTH = 1,
    SYS_GRAPHICS_GET_HEIGHT,
    SYS_GRAPHICS_CLEAR,
    SYS_GRAPHICS_PIXEL,
    SYS_GRAPHICS_LINE,
    SYS_GRAPHICS_CIRCLE,
    SYS_GRAPHICS_FILL_CIRCLE,
    SYS_GRAPHICS_RECTANGLE,
    SYS_GRAPHICS_FILL_RECTANGLE,
    SYS_GRAPHICS_TRIANGLE,
    SYS_GRAPHICS_FILL_TRIANGLE,
    SYS_GRAPHICS_ELLIPSE,
    SYS_GRAPHICS_TEXT,
    SYS_GRAPHICS_MEASURE_TEXT,
    SYS_GRAPHICS_SET_RESOLUTION
} syscall_graphics_op_t;

typedef struct {
    uint64_t op;
    int64_t args[10];
    const char* text;
    uint32_t* out_width;
    uint32_t* out_height;
} syscall_graphics_request_t;

typedef enum {
    SYS_MANAGER_GPU = 1,
    SYS_MANAGER_AUDIO,
    SYS_MANAGER_STORAGE,
    SYS_MANAGER_USB
} syscall_manager_id_t;

typedef enum {
    SYS_MANAGER_INIT = 1,
    SYS_MANAGER_UPDATE,
    SYS_MANAGER_START,
    SYS_MANAGER_HAS_DATA,
    SYS_MANAGER_SET_SAMPLE_RATE
} syscall_manager_op_t;

typedef enum {
    SYS_USB_INIT = 1,
    SYS_USB_POLL,
    SYS_USB_HAS_KEYBOARD,
    SYS_USB_KEY_TO_ASCII,
    SYS_USB_KEY_IS_PRESSED,
    SYS_USB_KEYBOARD_INFO
} syscall_usb_op_t;

typedef struct {
    uint8_t address;
    uint8_t port;
    uint8_t state;
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t class_code;
    uint8_t is_keyboard;
} syscall_usb_keyboard_info_t;

#define SYSCALL_DIRENT_NAME_SIZE 64

#define SYSCALL_DIRENT_TYPE_FILE       0
#define SYSCALL_DIRENT_TYPE_DIR        1
#define SYSCALL_DIRENT_TYPE_EXECUTABLE 2
#define SYSCALL_DIRENT_TYPE_SYMLINK    3

typedef struct {
    char name[SYSCALL_DIRENT_NAME_SIZE];
    uint8_t type;
    uint8_t hidden;
} syscall_dirent_t;

typedef struct {
    char filetype[64];
    char fileformat[16];
    uint32_t data_length;
    uint8_t runnable;
    uint8_t hidden;
    uint64_t entrypoint;
    char created_date[32];
    char last_changed[32];
} syscall_file_metadata_t;

typedef enum {
    SYS_PKG_UNZIP = 1,
    SYS_PKG_ZIP,
    SYS_PKG_INFO,
} syscall_pkg_op_t;

typedef struct {
    uint64_t op;
    const char* path;
    const char* extra;
    char* out_path;
    uint32_t out_path_size;
    uint32_t* out_count;
    uint32_t* out_failed;
} syscall_pkg_request_t;

typedef struct __attribute__((packed)) {
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t rbp;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
} syscall_regs_t;

#define SYS_SYSINFO 26

typedef enum {
    SYS_SYSINFO_CPU_USAGE       = 0x01, // avg CPU usage % across all online cores
    SYS_SYSINFO_RAM_TOTAL       = 0x02, // total RAM, bytes
    SYS_SYSINFO_RAM_USED        = 0x03, // used RAM (heap + PMM), bytes
    SYS_SYSINFO_RAM_FREE        = 0x04, // free RAM, bytes
    SYS_SYSINFO_UPTIME_MS       = 0x05,
    SYS_SYSINFO_PROCESS_COUNT   = 0x06,
    SYS_SYSINFO_CORE_COUNT      = 0x07, // online core count
    SYS_SYSINFO_HEAP_USED       = 0x08, // allocator (kernel heap), bytes
    SYS_SYSINFO_HEAP_FREE       = 0x09,
    SYS_SYSINFO_PMM_USED_PAGES  = 0x0A, // 4KB physical pages
    SYS_SYSINFO_PMM_FREE_PAGES  = 0x0B,
    // 0x0C-0x0F reserved for future global stats

    // Per-core usage: op = SYS_SYSINFO_CORE_USAGE_BASE + core_id.
    // Deliberately last/open-ended since core count can vary between
    // boots (SMP bring-up may bring up fewer cores than expected).
    SYS_SYSINFO_CORE_USAGE_BASE = 0x10,
} syscall_sysinfo_op_t;

void syscall_dispatch(syscall_regs_t* regs);

#endif
