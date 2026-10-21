#!/usr/bin/env python3
"""Draws Resources/icon.bmp (32x32, 32-bit with alpha): a parcel with a
down arrow, the Deliver Manager desktop icon. Run it after editing."""
import struct
from pathlib import Path

S = 32
px = [[(0, 0, 0, 0)] * S for _ in range(S)]

def rect(x0, y0, x1, y1, c):
    for y in range(y0, y1):
        for x in range(x0, x1):
            px[y][x] = c

BOX, LID, TAPE, ARROW = (196, 140, 84, 255), (222, 170, 110, 255), (150, 100, 56, 255), (61, 174, 233, 255)
rect(4, 12, 28, 29, BOX)        # body
rect(3, 9, 29, 13, LID)         # lid
rect(14, 9, 18, 29, TAPE)       # tape
# arrow pointing into the box
rect(14, 1, 18, 8, ARROW)
for i in range(5):
    rect(11 + i, 6 + i, 21 - i, 7 + i, ARROW)

rows = b"".join(bytes((b, g, r, a)) for y in reversed(range(S)) for (r, g, b, a) in px[y])
header = struct.pack("<2sIHHI", b"BM", 54 + len(rows), 0, 0, 54)
dib = struct.pack("<IiiHHIIiiII", 40, S, S, 1, 32, 0, len(rows), 2835, 2835, 0, 0)
out = Path(__file__).resolve().parent / "Resources" / "icon.bmp"
out.write_bytes(header + dib + rows)
print(f"wrote {out}")
