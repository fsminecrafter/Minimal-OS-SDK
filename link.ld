ENTRY(_start)

SECTIONS
{
    . = 0;

    .text : {
        *(.text*)
    }

    .rodata : {
        *(.rodata*)
    }

    .data : {
        *(.data*)
    }

    /* Static constructors. crt0.c walks this range before calling
     * main(), so C++ globals with non-trivial constructors and
     * __attribute__((constructor)) functions both run. KEEP() stops
     * --gc-sections from discarding entries nothing references
     * directly - which is every entry, by definition. */
    .init_array : {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE_HIDDEN(__init_array_end = .);
    }

    .bss : {
        *(.bss*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.note.*)
    }
}
