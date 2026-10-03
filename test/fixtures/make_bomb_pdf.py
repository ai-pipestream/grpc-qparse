#!/usr/bin/env python3
"""Regenerates bomb.pdf, the decompression-bomb fixture: two Letter pages,
each drawing one image XObject over a 200 pt square at (100, 400), with
the samples deflated twice (/Filter [/FlateDecode /FlateDecode]) so a
few hundred megabytes of zeros (black) fit in a few kilobytes.

Page 0's image is honest and huge: 12000 x 12000 DeviceRGB, 432 MB of
samples, more pixels than the default Render budget. Page 1's image
claims 64 x 64 DeviceGray (4 KB) but its stream inflates to 256 MiB: only
an inflate limit stops that one. The service draws neither, so both
squares stay white."""

import zlib
from pathlib import Path

CHUNK = 1 << 20


def deflate_twice(size):
    """size zero bytes, deflated, then deflated again."""
    inner = zlib.compressobj(9)
    zero = bytes(CHUNK)
    once = bytearray()
    for _ in range(size // CHUNK):
        once += inner.compress(zero)
    once += inner.compress(bytes(size % CHUNK))
    once += inner.flush()
    return zlib.compress(bytes(once), 9)


def stream(dict_body, data):
    return (
        b"<< " + dict_body + b" /Length " + str(len(data)).encode()
        + b" >>\nstream\n" + data + b"\nendstream"
    )


def image(width, height, colorspace, samples):
    return stream(
        b"/Type /XObject /Subtype /Image /Width " + str(width).encode()
        + b" /Height " + str(height).encode() + b" /ColorSpace /"
        + colorspace + b" /BitsPerComponent 8"
        b" /Filter [/FlateDecode /FlateDecode]",
        deflate_twice(samples),
    )


content = b"q 200 0 0 200 100 400 cm /Im0 Do Q"
objects = [
    b"<< /Type /Catalog /Pages 2 0 R >>",
    b"<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 /MediaBox [0 0 612 792] >>",
    b"<< /Type /Page /Parent 2 0 R /Contents 5 0 R "
    b"/Resources << /XObject << /Im0 6 0 R >> >> >>",
    b"<< /Type /Page /Parent 2 0 R /Contents 5 0 R "
    b"/Resources << /XObject << /Im0 7 0 R >> >> >>",
    stream(b"", content),
    image(12000, 12000, b"DeviceRGB", 12000 * 12000 * 3),
    image(64, 64, b"DeviceGray", 256 << 20),
]

out = bytearray(b"%PDF-1.7\n")
offsets = []
for i, body in enumerate(objects, start=1):
    offsets.append(len(out))
    out += str(i).encode() + b" 0 obj\n" + body + b"\nendobj\n"

xref_pos = len(out)
out += b"xref\n0 " + str(len(objects) + 1).encode() + b"\n"
out += b"0000000000 65535 f \n"
for off in offsets:
    out += f"{off:010d} 00000 n \n".encode()
out += (
    b"trailer\n<< /Size " + str(len(objects) + 1).encode() + b" /Root 1 0 R >>\n"
    b"startxref\n" + str(xref_pos).encode() + b"\n%%EOF\n"
)

Path(__file__).with_name("bomb.pdf").write_bytes(bytes(out))
print(f"wrote bomb.pdf ({len(out)} bytes)")
