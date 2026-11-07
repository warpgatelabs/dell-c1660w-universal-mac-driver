#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Turn an imageable-area CUPS raster (what a CUPS driver receives) into a full-page sRGB
CUPS raster (what an IPP Everywhere client renders), by adding white margins.

Usage: full_page_raster.py IN.ras OUT.ras [MARGIN_PX] [--rotate-even]
  default margin 97 px = 11.62 pt; --rotate-even rotates pages 2, 4, ... by 180 degrees, as
  a client does for long-edge duplex when the printer asks for "rotated" back sides.

Feed OUT.ras to macOS's /usr/libexec/cups/filter/rastertopwg to get PWG raster.
"""
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rasterio import CUPS_HEADER_SIZE, read_cups_raster  # noqa: E402

CSPACE_SRGB = 19


def main(src, dst, margin=97, rotate_even=False):
    with open(src, "rb") as f:
        sync = f.read(4)
    if sync != b"3SaR":
        sys.exit("expected a little-endian uncompressed v3 raster")
    with open(src, "rb") as f, open(dst, "wb") as out:
        f.read(4)
        out.write(sync)
        for n, (h, page) in enumerate(read_cups_raster(src), 1):
            hdr = bytearray(f.read(CUPS_HEADER_SIZE))
            f.seek(h["cupsBytesPerLine"] * h["cupsHeight"], 1)
            pw, ph = h["PageSize"]
            res = h["HWResolution"][0]
            fw, fh = round(pw * res / 72), round(ph * res / 72)
            full = np.full((fh, fw, 3), 255, np.uint8)
            hh, ww = page.shape[:2]
            full[margin:margin + hh, margin:margin + ww] = page[:, :, :3]
            if rotate_even and n % 2 == 0:
                full = full[::-1, ::-1]
            put = lambda off, v: struct.pack_into("<I", hdr, off, v)
            putf = lambda off, v: struct.pack_into("<f", hdr, off, v)
            for i, v in enumerate((0, 0, pw, ph)):
                put(284 + 4 * i, int(v))          # ImagingBoundingBox
            put(372, fw)                          # cupsWidth
            put(376, fh)                          # cupsHeight
            put(392, fw * 3)                      # cupsBytesPerLine
            put(400, CSPACE_SRGB)                 # cupsColorSpace
            for i, v in enumerate((0, 0, pw, ph)):
                putf(436 + 4 * i, float(v))       # cupsImagingBBox
            out.write(hdr)
            out.write(full.tobytes())


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    rest = [a for a in sys.argv[3:] if a != "--rotate-even"]
    main(sys.argv[1], sys.argv[2], *(int(x) for x in rest), rotate_even="--rotate-even" in sys.argv)
