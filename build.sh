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
        --slib)
            SLIB=1
            shift
            ;;
        --main)
            [ "$#" -ge 2 ] || usage
            MAIN_SOURCE=$2
            shift 2
            ;;
        -h|--help)
            usage
            ;;
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
            if [ -f "$SOURCE_ROOT/$candidate" ]; then
                MAIN_SOURCE="$candidate"
                break
            fi
        done
    fi

    if [ "$SLIB" -eq 0 ] && [ -z "$MAIN_SOURCE" ]; then
        echo "No main.cpp/main.c found; use --main <source>" >&2
        exit 1
    fi
else
    [ -f "$SOURCE_ROOT" ] || {
        echo "Source not found: $SOURCE_ROOT" >&2
        exit 1
    }

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
SLIB_CRT_OBJECT="$BUILD_DIR/slibcrt.o"
SLIB_CRT_OBJECT="$BUILD_DIR/slibcrt.o"
ELF="$BUILD_DIR/$PROGRAM_NAME.elf"
OUTPUT="$SCRIPT_DIR/$PROGRAM_NAME.$([ "$SLIB" -eq 1 ] && echo slib || echo run)"

# The OS image packages install2 as 0:/programs. Keep the dlr build in that
# directory when the SDK is checked out alongside the kernel source tree.
if [ "$SLIB" -eq 0 ] && [ "$PROGRAM_NAME" = "dlr" ] &&
    [ -d "$SCRIPT_DIR/../src/resources/install2" ]; then
    OUTPUT="$SCRIPT_DIR/../src/resources/install2/dlr.run"
fi

mkdir -p "$BUILD_DIR"

COMMON_FLAGS="
    -ffreestanding
    -fno-builtin
    -fno-stack-protector
    -fno-asynchronous-unwind-tables
    -fno-unwind-tables
    -fPIE
"

# Generic target macro for anything built through the Minimal-OS SDK.
TARGET_FLAGS="-DMINIMALOS_TARGET"

# Library-local headers must come BEFORE SDK headers.
# This makes <openssl/...> resolve to the library's own compatibility
# headers, while <stdio.h>, <stdlib.h>, <string.h>, etc. still resolve
# to the SDK when the library does not provide them itself.
INCLUDE_FLAGS="-I$SCRIPT_DIR/include"

if [ -d "$SOURCE_ROOT/include" ]; then
    INCLUDE_FLAGS="-I$SOURCE_ROOT/include $INCLUDE_FLAGS"
fi

if [ -d "$SOURCE_ROOT/src" ]; then
    INCLUDE_FLAGS="-I$SOURCE_ROOT/src $INCLUDE_FLAGS"
fi

OBJECTS=""

if [ "$SLIB" -eq 0 ]; then
    "$CRT_COMPILER" \
        -std=gnu11 \
        $COMMON_FLAGS \
        $TARGET_FLAGS \
        -I"$SCRIPT_DIR/include" \
        -c "$SCRIPT_DIR/crt0.c" \
        -o "$CRT_OBJECT"

    OBJECTS="$CRT_OBJECT"
else
    # Shared libraries don't get crt0.c because crt0 contains _start()
    # and expects a main(). Instead they get just the freestanding
    # memory primitives needed by compiler-generated calls.
    "$CRT_COMPILER" \
        -std=gnu11 \
        $COMMON_FLAGS \
        $TARGET_FLAGS \
        -I"$SCRIPT_DIR/include" \
        -c "$SCRIPT_DIR/slibcrt.c" \
        -o "$SLIB_CRT_OBJECT"

    OBJECTS="$SLIB_CRT_OBJECT"
fi

if [ "$SOURCE_WAS_DIR" -eq 1 ]; then
    if [ "$SLIB" -eq 1 ] && [ -d "$SOURCE_ROOT/src" ]; then
        # A library with a src/ tree gets only its implementation sources.
        # Tests/examples must not become part of the library ELF.
        SOURCES=$(
            find "$SOURCE_ROOT/src" -type f \
                \( -name '*.c' -o -name '*.cpp' \) \
                -print | sort
        )
    else
        SOURCES=$(
            find "$SOURCE_ROOT" -type f \
                \( -name '*.c' -o -name '*.cpp' \) \
                ! -path "$SOURCE_ROOT/tests/*" \
                ! -path "$SOURCE_ROOT/examples/*" \
                -print | sort
        )
    fi
else
    SOURCES=$SOURCE_FILE
fi

[ -n "$SOURCES" ] || {
    echo "No C/C++ sources found in $SOURCE_ROOT" >&2
    exit 1
}

for SOURCE in $SOURCES; do
    case "$SOURCE" in
        *.cpp)
            CURRENT_COMPILER=${CXX:-g++}
            CURRENT_FLAGS="
                -std=gnu++11
                -fno-exceptions
                -fno-rtti
                -fno-threadsafe-statics
            "
            ;;
        *)
            CURRENT_COMPILER=${CC:-gcc}
            CURRENT_FLAGS="-std=gnu11"
            ;;
    esac

    OBJECT="$BUILD_DIR/$(basename "${SOURCE%.*}").o"

    "$CURRENT_COMPILER" \
        $CURRENT_FLAGS \
        $COMMON_FLAGS \
        $TARGET_FLAGS \
        $INCLUDE_FLAGS \
        -c "$SOURCE" \
        -o "$OBJECT"

    OBJECTS="$OBJECTS $OBJECT"
done

ENTRY_POINT=_start
if [ "$SLIB" -eq 1 ]; then
    ENTRY_POINT=0
fi

"$LINKER" \
    -pie \
    --no-dynamic-linker \
    -T "$SCRIPT_DIR/link.ld" \
    -e "$ENTRY_POINT" \
    -o "$ELF" \
    $OBJECTS

if [ "$SLIB" -eq 1 ]; then
    EXPORTS="$BUILD_DIR/$PROGRAM_NAME.exports"
    NM=${NM:-nm}

    "$NM" -g --defined-only "$ELF" |
        awk '$3 != "" { print $3 }' |
        sort -u > "$EXPORTS"

    HEADERS=$(
        find "$SOURCE_ROOT" -type f \
            \( -name '*.h' -o -name '*.hpp' \) \
            -print | sort
    )

    python3 "$SCRIPT_DIR/tools/slibbundle.py" \
        "$ELF" \
        "$EXPORTS" \
        "$OUTPUT" \
        "$SOURCE_ROOT" \
        $HEADERS

    echo "Built shared library $OUTPUT"
else
    mkdir -p "$(dirname -- "$OUTPUT")"
    "$SCRIPT_DIR/mkrun.sh" "$ELF" "$OUTPUT"
    echo "Built $OUTPUT"
fi