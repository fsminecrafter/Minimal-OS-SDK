# Minimal-OS SDK (v1)

Minimal-OS SDK is a Software Development Kit for Minimal-OS, Linked here: [Minimal-OS](https://github.com/fsminecrafter/Minimal-OS).


Build a program, package it as a `.run`:

    x86_64-elf-gcc -std=gnu11 -ffreestanding -fno-builtin -fno-stack-protector \
        -fno-asynchronous-unwind-tables -fno-unwind-tables -fPIE \
        -Iinclude -c crt0.c -o crt0.o

    x86_64-elf-gcc -std=gnu11 -ffreestanding -fno-builtin -fno-stack-protector \
        -fno-asynchronous-unwind-tables -fno-unwind-tables -fPIE \
        -Iinclude -c examples/hello.c -o hello.o

    x86_64-elf-ld -pie --no-dynamic-linker -T link.ld -e _start \
        -o main.elf crt0.o hello.o

    ./mkrun.sh main.elf hello.run

Then inside Minimal-OS: `run 0:/programs/hello.run`

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

The syscall constants, error values, and kernel-side register frame are also
available from `include/syscall.h`. `SYS_O_RDONLY` is the only open flag
currently defined.

## If you didnt know

This SDK is designed for fsminecrafter/Minimal-OS.

(:
