#!/usr/bin/env python3
"""Create a MINIRUN1 single-file program bundle."""

import os
import struct
import sys
import tempfile
from pathlib import Path

MAGIC = b"MINIRUN1"
VERSION = 1
HEADER = struct.Struct("<8sII")
ENTRY = struct.Struct("<64sII")


def usage():
    print(f"Usage: {Path(sys.argv[0]).name} <main.elf> <output-name.run>", file=sys.stderr)
    raise SystemExit(2)


def collect_entries(elf_path):
    entries = [("main.elf", elf_path)]
    resources = elf_path.parent / "Resources"
    if resources.is_dir():
        for path in sorted(resources.rglob("*")):
            if path.is_file():
                entries.append((path.relative_to(elf_path.parent).as_posix(), path))
    return entries


def encode_name(name):
    encoded = name.encode("utf-8")
    if len(encoded) >= 64:
        raise ValueError(f"Bundle entry name is too long: {name}")
    return encoded + b"\0" * (64 - len(encoded))


def write_bundle(elf_path, output_path):
    entries = collect_entries(elf_path)
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
    if len(sys.argv) != 3:
        usage()
    elf_path = Path(sys.argv[1]).resolve()
    output_path = Path(sys.argv[2]).resolve()
    if not elf_path.is_file():
        print(f"ELF file not found: {elf_path}", file=sys.stderr)
        return 1
    try:
        write_bundle(elf_path, output_path)
    except (OSError, ValueError) as error:
        print(f"Could not create {output_path}: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
