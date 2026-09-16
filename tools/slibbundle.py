#!/usr/bin/env python3
"""Create a MINISLIB1 shared-library bundle."""

import os
import struct
import sys
import tempfile
from pathlib import Path

MAGIC = b"MINISLIB1"
VERSION = 1
HEADER = struct.Struct("<9sII")
ENTRY = struct.Struct("<64sII")


def encode_name(name):
    encoded = name.encode("utf-8")
    if len(encoded) >= 64:
        raise ValueError(f"Bundle entry name is too long: {name}")
    return encoded + b"\0" * (64 - len(encoded))


def collect_entries(elf_path, header_paths, export_path, header_root):
    entries = [("library.elf", elf_path), ("exports.txt", export_path)]
    for path in sorted(header_paths):
        relative = path.relative_to(header_root).as_posix()
        entries.append((f"include/{relative}", path))
    return entries


def write_bundle(elf_path, header_paths, export_path, output_path, header_root):
    entries = collect_entries(elf_path, header_paths, export_path, header_root)
    header_size = HEADER.size + ENTRY.size * len(entries)
    offsets = []
    offset = header_size
    for _, path in entries:
        size = path.stat().st_size
        if size > 0xFFFFFFFF:
            raise ValueError(f"Bundle entry is too large: {path}")
        offsets.append((offset, size))
        offset += size

    output_path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary_name = tempfile.mkstemp(prefix=f".{output_path.name}.", dir=output_path.parent)
    try:
        with os.fdopen(fd, "wb") as output:
            output.write(HEADER.pack(MAGIC, VERSION, len(entries)))
            for (name, _), (entry_offset, size) in zip(entries, offsets):
                output.write(ENTRY.pack(encode_name(name), entry_offset, size))
            for _, path in entries:
                with path.open("rb") as source:
                    while chunk := source.read(1024 * 1024):
                        output.write(chunk)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary_name, output_path)
    except BaseException:
        try:
            os.unlink(temporary_name)
        except FileNotFoundError:
            pass
        raise


def main():
    if len(sys.argv) < 5:
        print(f"Usage: {Path(sys.argv[0]).name} <library.elf> <exports.txt> <output.slib> <header-root> [headers...]", file=sys.stderr)
        return 2

    elf_path = Path(sys.argv[1]).resolve()
    export_path = Path(sys.argv[2]).resolve()
    output_path = Path(sys.argv[3]).resolve()
    header_root = Path(sys.argv[4]).resolve()
    header_paths = [Path(path).resolve() for path in sys.argv[5:]]
    if not elf_path.is_file() or not export_path.is_file():
        print("ELF and export manifest must exist", file=sys.stderr)
        return 1
    if any(not path.is_file() for path in header_paths):
        print("All header paths must be files", file=sys.stderr)
        return 1

    try:
        write_bundle(elf_path, header_paths, export_path, output_path, header_root)
    except (OSError, ValueError) as error:
        print(f"Could not create {output_path}: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
