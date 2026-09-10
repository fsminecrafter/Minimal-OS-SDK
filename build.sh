#!/bin/sh
set -eu

usage() {
    echo "Usage: $0 <source.c|source.cpp>" >&2
    exit 1
}

[ "$#" -eq 1 ] || usage
SOURCE=$1
[ -f "$SOURCE" ] || {
    echo "Source file not found: $SOURCE" >&2
    exit 1
}

case "$SOURCE" in
    *.c)
        COMPILER=${CC:-gcc}
        LANGUAGE_FLAGS="-std=gnu11"
        ;;
    *.cpp)
        COMPILER=${CXX:-g++}
        LANGUAGE_FLAGS="-std=gnu++11 -fno-exceptions -fno-rtti -fno-threadsafe-statics"
        ;;
    *)
        echo "Source must have a .c or .cpp extension: $SOURCE" >&2
        exit 1
        ;;
esac

CRT_COMPILER=${CC:-gcc}
LINKER=${LD:-ld}
command -v "$CRT_COMPILER" >/dev/null 2>&1 || {
    echo "Compiler not found: $CRT_COMPILER (run ./install-toolchain.sh or set CC)" >&2
    exit 1
}
if [ "$COMPILER" != "$CRT_COMPILER" ]; then
    command -v "$COMPILER" >/dev/null 2>&1 || {
        echo "Compiler not found: $COMPILER (run ./install-toolchain.sh or set CXX)" >&2
        exit 1
    }
fi
command -v "$LINKER" >/dev/null 2>&1 || {
    echo "Linker not found: $LINKER (run ./install-toolchain.sh or set LD)" >&2
    exit 1
}

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SOURCE_NAME=$(basename -- "$SOURCE")
PROGRAM_NAME=${SOURCE_NAME%.*}
BUILD_DIR="$SCRIPT_DIR/build"
OBJECT="$BUILD_DIR/$PROGRAM_NAME.o"
CRT_OBJECT="$BUILD_DIR/crt0.o"
ELF="$BUILD_DIR/$PROGRAM_NAME.elf"
OUTPUT="$SCRIPT_DIR/$PROGRAM_NAME.run"

mkdir -p "$BUILD_DIR"

COMMON_FLAGS="-ffreestanding -fno-builtin -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -fPIE"

"$CRT_COMPILER" -std=gnu11 $COMMON_FLAGS -I"$SCRIPT_DIR/include" -c "$SCRIPT_DIR/crt0.c" -o "$CRT_OBJECT"
"$COMPILER" $LANGUAGE_FLAGS $COMMON_FLAGS -I"$SCRIPT_DIR/include" -c "$SOURCE" -o "$OBJECT"
"$LINKER" -pie --no-dynamic-linker -T "$SCRIPT_DIR/link.ld" -e _start \
    -o "$ELF" "$CRT_OBJECT" "$OBJECT"

"$SCRIPT_DIR/mkrun.sh" "$ELF" "$OUTPUT"
echo "Built $OUTPUT"