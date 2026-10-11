#!/bin/sh
set -eu

SDK_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
HOME_DIR=${HOME:?HOME must be set}
BIN_DIR=${MSDK_BINDIR:-"$HOME_DIR/.local/bin"}
BASHRC="$HOME_DIR/.bashrc"
PATH_BEGIN="# >>> Minimal-OS SDK MSDK PATH >>>"
PATH_END="# <<< Minimal-OS SDK MSDK PATH <<<"

if [ ! -x "$SDK_DIR/build.sh" ] || [ ! -f "$SDK_DIR/msdk" ]; then
    echo "MSDK: build.sh or msdk launcher is missing from $SDK_DIR" >&2
    exit 1
fi

mkdir -p "$BIN_DIR"
ln -sfn "$SDK_DIR/msdk" "$BIN_DIR/msdk"
ln -sfn "$SDK_DIR/msdk" "$BIN_DIR/MSDK"

if ! grep -Fqx "$PATH_BEGIN" "$BASHRC" 2>/dev/null; then
    mkdir -p "$(dirname -- "$BASHRC")"
    {
        printf '\n%s\n' "$PATH_BEGIN"
        printf 'case ":$PATH:" in\n'
        printf '  *":%s:"*) ;;\n' "$BIN_DIR"
        printf '  *) export PATH="%s:$PATH" ;;\n' "$BIN_DIR"
        printf 'esac\n%s\n' "$PATH_END"
    } >> "$BASHRC"
fi

printf 'Installed MSDK and msdk in %s\n' "$BIN_DIR"
printf 'Configured the MSDK path in %s\n' "$BASHRC"
printf 'Run: source %s\n' "$BASHRC"