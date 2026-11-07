# SPDX-License-Identifier: Apache-2.0
"""Small readers for CUPS raster (v3, 'RaS3'/'3SaR') and PAM files, returning numpy arrays."""
import struct

import numpy as np

CUPS_HEADER_SIZE = 1796


def read_cups_raster(path):
    """Yield (header dict, HxWxC uint8 array) for each page of an uncompressed CUPS raster v3 file."""
    with open(path, "rb") as f:
        sync = f.read(4)
        # The sync word 0x52615333 ("RaS3") is written in native order, so "3SaR" on disk = little-endian.
        if sync == b"3SaR":
            end = "<"
        elif sync == b"RaS3":
            end = ">"
        else:
            raise ValueError(f"unsupported raster sync word {sync!r} (need uncompressed v3)")
        while True:
            hdr = f.read(CUPS_HEADER_SIZE)
            if len(hdr) < CUPS_HEADER_SIZE:
                return
            u = lambda off: struct.unpack_from(end + "I", hdr, off)[0]
            h = {
                "MediaType": hdr[64:128].split(b"\0")[0].decode(),
                "HWResolution": (u(276), u(280)),
                "ImagingBoundingBox": (u(284), u(288), u(292), u(296)),
                "PageSize": (u(352), u(356)),
                "cupsWidth": u(372),
                "cupsHeight": u(376),
                "cupsBitsPerColor": u(384),
                "cupsBitsPerPixel": u(388),
                "cupsBytesPerLine": u(392),
                "cupsColorOrder": u(396),
                "cupsColorSpace": u(400),
            }
            w, ht, bpl = h["cupsWidth"], h["cupsHeight"], h["cupsBytesPerLine"]
            data = np.frombuffer(f.read(bpl * ht), dtype=np.uint8)
            if data.size < bpl * ht:
                raise ValueError("truncated raster page")
            ch = h["cupsBitsPerPixel"] // 8
            yield h, data.reshape(ht, bpl)[:, : w * ch].reshape(ht, w, ch)


def read_pam(path):
    """Return (header dict, HxWxD uint8 array) for a P7 PAM file."""
    with open(path, "rb") as f:
        if f.readline().strip() != b"P7":
            raise ValueError("not a PAM file")
        h = {}
        while True:
            line = f.readline().strip()
            if line == b"ENDHDR":
                break
            k, _, v = line.partition(b" ")
            h[k.decode()] = v.decode()
        w, ht, d = int(h["WIDTH"]), int(h["HEIGHT"]), int(h["DEPTH"])
        return h, np.frombuffer(f.read(w * ht * d), dtype=np.uint8).reshape(ht, w, d)
