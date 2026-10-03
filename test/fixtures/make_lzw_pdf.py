#!/usr/bin/env python3
"""Regenerates lzw.pdf, the /LZWDecode fixture: four Letter pages.

Page 0's content stream and page 1's image samples are LZW bombs: each
code extends the previous one by a byte, and a clear code restarts the
table before it fills, so about 5.6 KB of codes decode to 7.37 MB of zero
bytes, and 37 rounds to 260 MiB. The LZW codes are deflated on top
(/Filter [/FlateDecode /LZWDecode]), which keeps the file small and keeps
the Flate stage far under any inflate limit: only an LZW limit stops it.
Page 1's image claims 64 x 64 DeviceGray (black) over a 200 pt square at
(100, 400); the service draws nothing there, so the square stays white.

Pages 2 and 3 are honest LZW text, Helvetica 24pt "LZW text" at
(100, 700): page 2 with the default /EarlyChange 1, page 3 with
/EarlyChange 0 and a PNG Up predictor (/Predictor 12, /Columns 16)."""

import zlib
from pathlib import Path

CLEAR, EOD, FIRST = 256, 257, 258


class CodeWriter:
    """Packs LZW codes most significant bit first, switching the code width
    exactly where the decoder does: it adds a table entry for every code but
    the first after a clear, and widens once that entry's index (plus one
    with early change) reaches 511, 1023 or 2047."""

    def __init__(self, early_change=True):
        self.early = 1 if early_change else 0
        self.width = 9
        self.entries = 0
        self.first = True
        self.acc = 0
        self.nbits = 0
        self.out = bytearray()

    def emit(self, code):
        self.acc = (self.acc << self.width) | code
        self.nbits += self.width
        while self.nbits >= 8:
            self.nbits -= 8
            self.out.append((self.acc >> self.nbits) & 0xFF)
        self.acc &= (1 << self.nbits) - 1
        if code == CLEAR:
            self.width, self.entries, self.first = 9, 0, True
            return
        if code == EOD:
            return
        if not self.first:
            index = FIRST + self.entries
            self.entries += 1
            if index + self.early in (511, 1023, 2047):
                self.width += 1
        self.first = False

    def finish(self):
        self.emit(EOD)
        if self.nbits:
            self.out.append((self.acc << (8 - self.nbits)) & 0xFF)
        return bytes(self.out)


def lzw_encode(data, early_change=True):
    """Plain LZW over data; short inputs never fill the table."""
    w = CodeWriter(early_change)
    w.emit(CLEAR)
    table = {bytes([i]): i for i in range(256)}
    next_code = FIRST
    prefix = b""
    for byte in data:
        candidate = prefix + bytes([byte])
        if candidate in table:
            prefix = candidate
            continue
        w.emit(table[prefix])
        table[candidate] = next_code
        next_code += 1
        prefix = bytes([byte])
    if prefix:
        w.emit(table[prefix])
    return w.finish()


def lzw_bomb(rounds):
    """rounds x (1 + 2 + 3 + ... + 3838) zero bytes: per round, a literal zero,
    then every code names the entry the decoder is about to add, one byte
    longer each time, until the table is one short of full."""
    w = CodeWriter()
    for _ in range(rounds):
        w.emit(CLEAR)
        w.emit(0)
        for code in range(FIRST, 4095):
            w.emit(code)
    return w.finish()


def png_up(data, columns):
    """PNG Up predictor rows (filter byte 2), data padded to whole rows."""
    data += b" " * (-len(data) % columns)
    prev = bytes(columns)
    out = bytearray()
    for start in range(0, len(data), columns):
        row = data[start:start + columns]
        out.append(2)
        out += bytes((a - b) & 0xFF for a, b in zip(row, prev))
        prev = row
    return bytes(out)


def stream(dict_body, data):
    return (
        b"<< " + dict_body + b" /Length " + str(len(data)).encode()
        + b" >>\nstream\n" + data + b"\nendstream"
    )


bomb = zlib.compress(lzw_bomb(37), 9)
text = b"BT /F1 24 Tf 100 700 Td (LZW text) Tj ET"
font = b"/Resources << /Font << /F1 5 0 R >> >>"

objects = [
    b"<< /Type /Catalog /Pages 2 0 R >>",
    b"<< /Type /Pages /Kids [3 0 R 4 0 R 6 0 R 7 0 R] /Count 4 "
    b"/MediaBox [0 0 612 792] >>",
    b"<< /Type /Page /Parent 2 0 R /Contents 8 0 R " + font + b" >>",
    b"<< /Type /Page /Parent 2 0 R /Contents 9 0 R "
    b"/Resources << /XObject << /Im0 10 0 R >> >> >>",
    b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    b"<< /Type /Page /Parent 2 0 R /Contents 11 0 R " + font + b" >>",
    b"<< /Type /Page /Parent 2 0 R /Contents 12 0 R " + font + b" >>",
    stream(b"/Filter [/FlateDecode /LZWDecode]", bomb),
    stream(b"", b"q 200 0 0 200 100 400 cm /Im0 Do Q"),
    stream(
        b"/Type /XObject /Subtype /Image /Width 64 /Height 64"
        b" /ColorSpace /DeviceGray /BitsPerComponent 8"
        b" /Filter [/FlateDecode /LZWDecode]",
        bomb,
    ),
    stream(b"/Filter /LZWDecode", lzw_encode(text)),
    stream(
        b"/Filter /LZWDecode /DecodeParms << /EarlyChange 0"
        b" /Predictor 12 /Columns 16 >>",
        lzw_encode(png_up(text, 16), early_change=False),
    ),
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

Path(__file__).with_name("lzw.pdf").write_bytes(bytes(out))
print(f"wrote lzw.pdf ({len(out)} bytes)")
