#!/bin/sh
# Builds and runs the .mpkg reader tests under ASan/UBSan.
# Needs the kernel repo for its LZSS compressor and for mkpkg.py:
#   MOS_KERNEL=/path/to/Minimal-OS  ./mpkg_test.sh
# (default: ../../../Minimal-OS, i.e. a sibling checkout)
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SRC="$HERE/../../programs/dlr"
KERNEL=${MOS_KERNEL:-"$HERE/../../../Minimal-OS"}
[ -f "$KERNEL/src/impl/compression/lzss.c" ] || { echo "set MOS_KERNEL to the Minimal-OS repo" >&2; exit 2; }

OUT="$HERE/build"; mkdir -p "$OUT"
gcc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -Wno-unused-parameter \
    -I"$SRC" -iquote "$KERNEL/src/intf" \
    "$HERE/mpkg_test.c" "$SRC/dlr_mpkg.c" "$SRC/dlr_tar.c" \
    "$KERNEL/src/impl/compression/lzss.c" \
    -o "$OUT/mpkg_test"

# A tree with compressible, incompressible, empty and nested content.
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
mkdir -p "$T/tree/sub/deeper"
python3 - "$T/tree" <<'PY'
import os, sys, random
r = sys.argv[1]; random.seed(3)
open(f"{r}/pkg.pkg", "w").write("[Info]\nname=x\n")
open(f"{r}/sub/text.txt", "w").write("The quick brown fox. " * 5000)
open(f"{r}/sub/deeper/random.bin", "wb").write(bytes(random.getrandbits(8) for _ in range(40000)))
open(f"{r}/empty", "wb").close()
open(f"{r}/sub/big.bin", "wb").write(bytes([i % 251 for i in range(300000)]))
PY
python3 "$KERNEL/tools/mkpkg/mkpkg.py" "$T/tree" "$T/tree.mpkg"
"$OUT/mpkg_test" "$T/tree.mpkg" "$T/tree"
