#!/usr/bin/env python3
"""Regenerates annots.pdf: one 200 x 200 point page whose content stream
fills a black square at (10, 10)-(30, 30) and leaves the rest of the page
unpainted, plus a /Square annotation over (50, 50)-(150, 150) whose normal
appearance fills it green. The Render options test reads the unpainted
corner for the background and the annotation's middle for
omit_annotations. Kept as a generator so the fixture is reproducible; the
checked in annots.pdf is the test input."""

from pathlib import Path


def stream(dictionary: bytes, data: bytes) -> bytes:
    return (
        b"<< " + dictionary + b" /Length " + str(len(data)).encode()
        + b" >>\nstream\n" + data + b"\nendstream"
    )


objects = [
    b"<< /Type /Catalog /Pages 2 0 R >>",
    b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
    b"/Resources << >> /Contents 4 0 R /Annots [5 0 R] >>",
    stream(b"", b"0 0 0 rg 10 10 20 20 re f"),
    b"<< /Type /Annot /Subtype /Square /Rect [50 50 150 150] /F 4 "
    b"/AP << /N 6 0 R >> >>",
    stream(
        b"/Type /XObject /Subtype /Form /BBox [0 0 100 100] /Resources << >>",
        b"0 1 0 rg 0 0 100 100 re f",
    ),
]

out = bytearray(b"%PDF-1.4\n")
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

Path(__file__).with_name("annots.pdf").write_bytes(bytes(out))
print(f"wrote annots.pdf ({len(out)} bytes)")
