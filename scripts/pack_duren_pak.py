#!/usr/bin/env python3
"""Build /homebrews/Duren.pak from the three legacy Duren resource packs.

Container layout (little-endian):
  magic[4]   = b'DPK1'
  version    u16 = 1
  count      u16
  reserved   u32 = 0
  entries[count] of 32 bytes:
    name[16]   NUL-padded ASCII ("gfx", "audio", "lang")
    offset     u32  absolute start of member payload
    size       u32
    reserved   u32 = 0
  then concatenated member blobs (original .pak bytes, unchanged).
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

MAGIC = b"DPK1"
VERSION = 1
HEADER_SIZE = 12
ENTRY_SIZE = 32
NAME_SIZE = 16

MEMBERS = (
    ("gfx", "duren_gfx.pak"),
    ("audio", "duren_audio.pak"),
    ("lang", "duren_lang.pak"),
)


def pack_name(name: str) -> bytes:
    raw = name.encode("ascii")
    if len(raw) >= NAME_SIZE:
        raise ValueError(f"member name too long: {name!r}")
    return raw + b"\0" * (NAME_SIZE - len(raw))


def build(members: list[tuple[str, Path]], out: Path) -> None:
    blobs: list[tuple[str, bytes]] = []
    for name, path in members:
        if not path.is_file():
            raise FileNotFoundError(path)
        blobs.append((name, path.read_bytes()))

    data_start = HEADER_SIZE + ENTRY_SIZE * len(blobs)
    offset = data_start
    entries = bytearray()
    payload = bytearray()

    for name, blob in blobs:
        entries += pack_name(name)
        # name[16] + offset/size/reserved0/reserved1 (4×u32) = 32 bytes
        entries += struct.pack("<IIII", offset, len(blob), 0, 0)
        assert len(entries) % ENTRY_SIZE == 0, "entry size mismatch"
        payload += blob
        # Keep following members 4-byte aligned (FatFs-friendly).
        pad = (-len(blob)) % 4
        if pad:
            payload += b"\0" * pad
        offset = data_start + len(payload)

    header = MAGIC + struct.pack("<HHI", VERSION, len(blobs), 0)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(header + entries + payload)
    print(f"wrote {out} ({out.stat().st_size} bytes, {len(blobs)} members)")
    for name, blob in blobs:
        print(f"  {name:5s} {len(blob):8d} B")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--gfx",
        type=Path,
        default=Path("sd_content/roms/homebrew/duren_gfx.pak"),
    )
    ap.add_argument(
        "--audio",
        type=Path,
        default=Path("sd_content/roms/homebrew/duren_audio.pak"),
    )
    ap.add_argument(
        "--lang",
        type=Path,
        default=Path("sd_content/roms/homebrew/duren_lang.pak"),
    )
    ap.add_argument(
        "-o",
        "--out",
        type=Path,
        default=Path("Duren.pak"),
    )
    args = ap.parse_args()

    members = [
        ("gfx", args.gfx),
        ("audio", args.audio),
        ("lang", args.lang),
    ]
    build(members, args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
