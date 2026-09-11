# Minimal-OS SDK (v1)

Minimal-OS SDK is a Software Development Kit for Minimal-OS, Linked here: [Minimal-OS](https://github.com/fsminecrafter/Minimal-OS).


Install the native compiler and linker tools on Debian or Ubuntu:

    ./install-toolchain.sh

Build a C or C++ program and package it as a `.run`:

    ./build.sh examples/hello.c

This creates `hello.run` as a single file containing the program bundle. For
C++, use the same command with a `.cpp` file:

    ./build.sh examples/hello.cpp

The build script compiles `crt0.c`, compiles the selected source, links
`main.elf` with `link.ld`, and packages it with `mkrun.sh`. It uses `gcc`,
`g++`, and `ld` by default. Set `CC`, `CXX`, or `LD` to use a cross-toolchain, for
example `CC=x86_64-elf-gcc CXX=x86_64-elf-g++ LD=x86_64-elf-ld`.

The equivalent manual steps are:

    x86_64-elf-gcc -std=gnu11 -ffreestanding -fno-builtin -fno-stack-protector \
        -fno-asynchronous-unwind-tables -fno-unwind-tables -fPIE \
        -Iinclude -c crt0.c -o crt0.o

    x86_64-elf-gcc -std=gnu11 -ffreestanding -fno-builtin -fno-stack-protector \
        -fno-asynchronous-unwind-tables -fno-unwind-tables -fPIE \
        -Iinclude -c examples/hello.c -o hello.o

    x86_64-elf-ld -pie --no-dynamic-linker -T link.ld -e _start \
        -o main.elf crt0.o hello.o

    ./mkrun.sh main.elf hello.run

Import the `hello.run` file itself into MinimaFS. Then inside Minimal-OS run:

    run 0:/programs/hello.run

The `.run` file uses the `MINIRUN1` format. Its manifest contains `main.elf`
and every file below a `Resources` directory when those files exist. The
manifest and payload are stored in the same regular file, so copying or
importing the bundle does not require creating a directory tree.

For the C++ example, compile with the same freestanding flags plus the C++
runtime restrictions:

    x86_64-elf-g++ -std=gnu++11 -ffreestanding -fno-builtin -fno-stack-protector \
        -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-exceptions \
        -fno-rtti -fno-threadsafe-statics -fPIE -Iinclude \
        -c examples/hello.cpp -o hello.o

The C and C++ examples can use `printf` from `include/stdio.h` and
`std::cout` from `include/iostream`. They are header-only and write through
`mos_write`; no libc or C++ standard library is linked. The supported printf
formats are `%s`, `%c`, `%d`, `%i`, `%u`, `%x`, `%X`, `%ld`, `%lu`, and `%%`.

## Requirements on the ELF
- x86_64, ELF64, PIE (`ET_DYN`), linked at base address 0 (link.ld does this).
- No external symbols - everything the program needs must be compiled
  into it. There's no dynamic linker; only `R_X86_64_RELATIVE`
  relocations are processed at load time.

## C++
`-fno-exceptions -fno-rtti -fno-threadsafe-statics` and skip global
objects with non-trivial constructors for now - `crt0.c` doesn't walk
`.init_array` yet (it could, using the same ctor-section-walk pattern
`commandhandler.c` already uses for `REGISTER_COMMAND`, if that's
wanted next).

## Syscalls available
- `mos_write(fd, buf, len)` - fd 1/2 only, goes to the terminal + serial.
- `mos_exit(code)`
- `mos_getpid()`
- `mos_uptime()` and `mos_sleep(ticks)`
- `mos_open(path, flags)`, `mos_read(fd, buf, len)`, `mos_close(fd)`
- `mos_exists(path)`, `mos_is_dir(path)`, and `mos_mkdir(path)`
- `mos_gettime()`, `mos_seek(fd, offset)`, and `mos_size(fd)`

The current x86_64 user syscall API is also available through the subsystem
headers under `include/x86_64/`:

- `x86_64/user_syscalls.h` - uint64_t syscall wrappers, graphics dispatch,
    manager dispatch, and USB dispatch.
- `x86_64/user_graphics.h` - primitive drawing, text, and resolution wrappers.
- `x86_64/user_managers.h` - GPU, audio, storage, and USB manager operations.
- `x86_64/user_usb.h` - USB initialization, polling, keyboard translation, and
    copied keyboard metadata.

For a shorter graphics API, include `graphics.h` and use the corresponding
`graphics_*` names. It is usable from both C and C++ without libc or the C++
standard library:

        #include "graphics.h"

        void main(void) {
                graphics_clear(0, 0, 0);
                graphics_fill_rectangle(10, 10, 100, 60, 40, 160, 240);
                graphics_text("hello", 20, 20, 255, 255, 255);
                mos_exit();
        }

The new raw syscall numbers are `SYS_GRAPHICS` (15), `SYS_MANAGER` (16), and
`SYS_USB` (17). The request structures and operation constants are available
from `x86_64/syscall.h`.

The syscall constants, error values, and kernel-side register frame are also
available from `include/syscall.h`. `SYS_O_RDONLY` is the only open flag
currently defined.

## If you didnt know

This SDK is designed for fsminecrafter/Minimal-OS.

(:
