#!/bin/sh
# Builds the Minimal-OS dlr client against the host backend, for
# interop testing against a real dlr_server. See README.md here.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SRC="$HERE/../../programs/dlr"

gcc -O1 -g -Wall -Wextra -Wno-unused-parameter \
    -I"$SRC" -I"$HERE" -I"$HERE/shim-include" \
    -include "$HERE/shims.h" \
    \
    "$SRC/main.c" "$SRC/dlr_proto.c" "$SRC/dlr_crypto.c" \
    "$SRC/dlr_tar.c" "$SRC/dlr_pkg.c" "$SRC/dlr_db.c" \
    "$HERE/dlr_port_host.c" \
    -o "$HERE/dlr-host"
echo "Built $HERE/dlr-host"
