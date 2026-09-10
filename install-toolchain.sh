#!/bin/sh
set -eu

if ! command -v apt-get >/dev/null 2>&1; then
    echo "This installer currently supports Debian and Ubuntu systems (apt-get required)." >&2
    exit 1
fi

if [ "$(id -u)" -eq 0 ]; then
    APT="apt-get"
elif command -v sudo >/dev/null 2>&1; then
    APT="sudo apt-get"
else
    echo "Run this script as root or install sudo, then run it again." >&2
    exit 1
fi

echo "Installing GCC, G++, and GNU binutils..."
$APT update
$APT install -y --no-install-recommends gcc g++ binutils
echo "Toolchain installation complete. Run ./build.sh <source.c|source.cpp>."