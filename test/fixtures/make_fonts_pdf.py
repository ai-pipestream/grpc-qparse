#!/usr/bin/env python3
"""Regenerates fonts.pdf, the font-identity fixture: two Letter pages, each
drawing one line in its own embedded TrueType font. The two fonts share the
resource name /F1 and the subset name ABCDEF+SharedSans-Bold but embed
different programs, the way two subsets of one face cut by different
producers do, and the first one's /BaseFont (ABCDEF+SharedSans,Bold)
differs from its descriptor /FontName, as Windows-style names often do.

The programs are two small Noto fonts (SIL OFL 1.1) whose glyphs do not
matter here: the text is read through /WinAnsiEncoding and /Widths.
"""

from pathlib import Path

NOTO = Path("/usr/share/fonts/truetype/noto")
PROGRAM_A = (NOTO / "NotoSansLycian-Regular.ttf").read_bytes()
PROGRAM_B = (NOTO / "NotoSansLydian-Regular.ttf").read_bytes()

WIDTHS = b" ".join(b"600" for _ in range(32, 127))


def stream(dict_body: bytes, data: bytes) -> bytes:
    return (
        b"<< " + dict_body + b" /Length " + str(len(data)).encode() + b" >>\n"
        b"stream\n" + data + b"\nendstream"
    )


def font(base_font: bytes, descriptor: int) -> bytes:
    return (
        b"<< /Type /Font /Subtype /TrueType /BaseFont /" + base_font
        + b" /FirstChar 32 /LastChar 126 /Widths [" + WIDTHS + b"] "
        b"/Encoding /WinAnsiEncoding /FontDescriptor "
        + str(descriptor).encode() + b" 0 R >>"
    )


def descriptor(program: int) -> bytes:
    return (
        b"<< /Type /FontDescriptor /FontName /ABCDEF+SharedSans-Bold "
        b"/Flags 32 /FontBBox [-200 -300 1200 1000] /ItalicAngle 0 "
        b"/Ascent 900 /Descent -250 /CapHeight 700 /StemV 80 /FontFile2 "
        + str(program).encode() + b" 0 R >>"
    )


objects = [
    # 1: catalog
    b"<< /Type /Catalog /Pages 2 0 R >>",
    # 2: pages
    b"<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 /MediaBox [0 0 612 792] >>",
    # 3, 4: pages, each with its own /F1
    b"<< /Type /Page /Parent 2 0 R /Resources << /Font << /F1 5 0 R >> >> "
    b"/Contents 7 0 R >>",
    b"<< /Type /Page /Parent 2 0 R /Resources << /Font << /F1 6 0 R >> >> "
    b"/Contents 8 0 R >>",
    # 5, 6: the two fonts
    font(b"ABCDEF+SharedSans,Bold", 9),
    font(b"ABCDEF+SharedSans-Bold", 10),
    # 7, 8: contents
    stream(b"", b"BT /F1 18 Tf 72 700 Td (First subset) Tj ET"),
    stream(b"", b"BT /F1 18 Tf 72 700 Td (Second subset) Tj ET"),
    # 9, 10: descriptors
    descriptor(11),
    descriptor(12),
    # 11, 12: the programs
    stream(b"/Length1 " + str(len(PROGRAM_A)).encode(), PROGRAM_A),
    stream(b"/Length1 " + str(len(PROGRAM_B)).encode(), PROGRAM_B),
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

Path(__file__).with_name("fonts.pdf").write_bytes(bytes(out))
print(f"wrote fonts.pdf ({len(out)} bytes)")
