#!/bin/bash
# Usage: mkrun.sh <main.elf> <output-name.run>
set -euo pipefail

ELF=${1:-}
OUT=${2:-}
if [ -z "$ELF" ] || [ -z "$OUT" ]; then
    echo "Usage: mkrun.sh <main.elf> <output-name.run>" >&2
    exit 1
fi
if [ ! -f "$ELF" ]; then
    echo "ELF file not found: $ELF" >&2
    exit 1
fi

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
python3 "$SCRIPT_DIR/tools/runbundle.py" "$ELF" "$OUT"
echo "Created $OUT as a single-file MINIRUN1 bundle"
echo "Import the .run file itself into MinimaFS; do not copy it as a directory."