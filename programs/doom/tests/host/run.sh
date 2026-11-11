#!/bin/sh
# Host test of the Doom port: builds the engine and the port's C library
# as a freestanding Linux program (no host C library), plays a scripted
# stretch of Freedoom (or another IWAD) and checks what came out.
#   tests/host/run.sh <iwad> [work dir]
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
IWAD=${1:?usage: run.sh <iwad> [work dir]}
WORK=${2:-$ROOT/build/host-test}
CC=${HOSTCC:-cc}
rm -rf "$WORK"
mkdir -p "$WORK"
FLAGS="-std=gnu11 -O1 -g -ffreestanding -fno-builtin -fno-stack-protector -fno-pie -no-pie -static -nostdlib
       -nostdinc -isystem $($CC -print-file-name=include) -I $ROOT/src -I $ROOT/include -I $HERE/sdk"
$CC $FLAGS -o "$WORK/doom_host" "$HERE/platform.c" "$ROOT/libc/libc.c" "$ROOT"/src/*.c -lgcc
"$WORK/doom_host" "$IWAD" "$WORK" > "$WORK/run1.log" 2>&1 || { tail -20 "$WORK/run1.log"; exit 1; }
"$WORK/doom_host" "$IWAD" "$WORK" again > "$WORK/run2.log" 2>&1 || { tail -20 "$WORK/run2.log"; exit 1; }
echo "run 1:"; grep -E "^(frame|exit|[0-9]+ frames)|Error" "$WORK/run1.log" || true
echo "run 2:"; grep -E "^(exit|[0-9]+ frames)|Error" "$WORK/run2.log" || true
