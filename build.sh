#!/bin/sh
set -eu

usage() {
    echo "Usage: $0 [--slib] [--main <source.c|source.cpp>] <source.c|source.cpp|folder>" >&2
    exit 1
}

SLIB=0
MAIN_SOURCE=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        --slib) SLIB=1; shift;;
        --main)
            [ "$#" -ge 2 ] || usage
            MAIN_SOURCE=$2
            shift 2
            ;;
        -h|--help) usage;;
        *)
            [ -z "${SOURCE_ROOT:-}" ] || usage
            SOURCE_ROOT=$1
            shift
            ;;
    esac
done

[ -n "${SOURCE_ROOT:-}" ] || usage
SOURCE_WAS_DIR=0
if [ -d "$SOURCE_ROOT" ]; then
    SOURCE_WAS_DIR=1
    SOURCE_ROOT=$(CDPATH= cd -- "$SOURCE_ROOT" && pwd)
    if [ "$SLIB" -eq 0 ] && [ -z "$MAIN_SOURCE" ]; then
        for candidate in main.cpp main.c; do
            if [ -f "$SOURCE_ROOT/$candidate" ]; then MAIN_SOURCE="$candidate"; break; fi
        done
    fi
    if [ "$SLIB" -eq 0 ] && [ -z "$MAIN_SOURCE" ]; then
        echo "No main.cpp/main.c found; use --main <source>" >&2
        exit 1
    fi
else
    [ -f "$SOURCE_ROOT" ] || { echo "Source not found: $SOURCE_ROOT" >&2; exit 1; }
    SOURCE_FILE=$SOURCE_ROOT
    SOURCE_ROOT=$(CDPATH= cd -- "$(dirname -- "$SOURCE_ROOT")" && pwd)
    MAIN_SOURCE=$(basename -- "${MAIN_SOURCE:-$SOURCE_FILE}")
fi

if [ -n "$MAIN_SOURCE" ] && [ ! -f "$SOURCE_ROOT/$MAIN_SOURCE" ]; then
    echo "Main source not found: $SOURCE_ROOT/$MAIN_SOURCE" >&2
    exit 1
fi

CRT_COMPILER=${CC:-gcc}
LINKER=${LD:-ld}
command -v "$CRT_COMPILER" >/dev/null 2>&1 || {
    echo "Compiler not found: $CRT_COMPILER (run ./install-toolchain.sh or set CC)" >&2
    exit 1
}
command -v "${CXX:-g++}" >/dev/null 2>&1 || {
    echo "Compiler not found: ${CXX:-g++} (run ./install-toolchain.sh or set CXX)" >&2
    exit 1
}
command -v "$LINKER" >/dev/null 2>&1 || {
    echo "Linker not found: $LINKER (run ./install-toolchain.sh or set LD)" >&2
    exit 1
}

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROGRAM_NAME=$(basename -- "$SOURCE_ROOT")
if [ "$SOURCE_WAS_DIR" -eq 0 ]; then
    PROGRAM_NAME=${MAIN_SOURCE%.*}
fi
BUILD_DIR="$SCRIPT_DIR/build"
CRT_OBJECT="$BUILD_DIR/crt0.o"
ELF="$BUILD_DIR/$PROGRAM_NAME.elf"
OUTPUT="$SCRIPT_DIR/$PROGRAM_NAME.$([ "$SLIB" -eq 1 ] && echo slib || echo run)"

mkdir -p "$BUILD_DIR"

COMMON_FLAGS="-ffreestanding -fno-builtin -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -fPIE"

OBJECTS=""
if [ "$SLIB" -eq 0 ]; then
    "$CRT_COMPILER" -std=gnu11 $COMMON_FLAGS -I"$SCRIPT_DIR/include" -c "$SCRIPT_DIR/crt0.c" -o "$CRT_OBJECT"
    OBJECTS="$CRT_OBJECT"
fi

if [ "$SOURCE_WAS_DIR" -eq 1 ]; then
    SOURCES=$(find "$SOURCE_ROOT" -type f \( -name '*.c' -o -name '*.cpp' \) -print | sort)
else
    SOURCES=$SOURCE_FILE
fi
[ -n "$SOURCES" ] || { echo "No C/C++ sources found in $SOURCE_ROOT" >&2; exit 1; }
for SOURCE in $SOURCES; do
    case "$SOURCE" in
        *.cpp) CURRENT_COMPILER=${CXX:-g++}; CURRENT_FLAGS="-std=gnu++11 -fno-exceptions -fno-rtti -fno-threadsafe-statics";;
        *) CURRENT_COMPILER=${CC:-gcc}; CURRENT_FLAGS="-std=gnu11";;
    esac
    OBJECT="$BUILD_DIR/$(basename "${SOURCE%.*}").o"
    "$CURRENT_COMPILER" $CURRENT_FLAGS $COMMON_FLAGS -I"$SCRIPT_DIR/include" -I"$SOURCE_ROOT" -c "$SOURCE" -o "$OBJECT"
    OBJECTS="$OBJECTS $OBJECT"
done
ENTRY_POINT=_start
if [ "$SLIB" -eq 1 ]; then ENTRY_POINT=0; fi
"$LINKER" -pie --no-dynamic-linker -T "$SCRIPT_DIR/link.ld" -e "$ENTRY_POINT" \
    -o "$ELF" $OBJECTS

if [ "$SLIB" -eq 1 ]; then
    EXPORTS="$BUILD_DIR/$PROGRAM_NAME.exports"
    NM=${NM:-nm}
    "$NM" -g --defined-only "$ELF" | awk '$3 != "" { print $3 }' | sort -u > "$EXPORTS"
    HEADERS=$(find "$SOURCE_ROOT" -type f \( -name '*.h' -o -name '*.hpp' \) -print | sort)
    python3 "$SCRIPT_DIR/tools/slibbundle.py" "$ELF" "$EXPORTS" "$OUTPUT" "$SOURCE_ROOT" $HEADERS
    echo "Built shared library $OUTPUT"
else
    "$SCRIPT_DIR/mkrun.sh" "$ELF" "$OUTPUT"
    echo "Built $OUTPUT"
fi