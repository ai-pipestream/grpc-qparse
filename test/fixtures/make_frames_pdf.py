#!/usr/bin/env python3
"""Regenerates frames.pdf, the page-frame fixture: eight Letter pages that
draw the same things at the same user-space positions and differ only in
how the page is framed, so every page must report the same geometry less
its CropBox origin.

Each page shows Helvetica 24pt "Frame" at (72, 700) and fills a red 60x30
rectangle at (300, 500), shifted by the MediaBox origin on page 6 and moved
inside the CropBox on page 7. The root /Pages node carries the MediaBox and
the resources, so every page inherits them.

  0  upright
  1  /Rotate 90 on the page, with a URI link over the text
  2  /Rotate 180
  3  /Rotate 270
  4  /Rotate 90 inherited from an intermediate /Pages node; its resources
     come from the root, two levels up
  5  CropBox [36 36 576 756], a URI link over the text, a text field, and
     "Edge" at (20, 400) straddling the CropBox's left edge
  6  MediaBox [100 200 712 992], everything drawn 100/200 further out
  7  /Rotate 90 with CropBox [50 60 562 732]: "Frame" at (72, 650), the
     rectangle at (300, 400), a 2x2 image at (120, 200) sized 50x30, a URI
     link over the text and a text field
"""

from pathlib import Path

FRAME_TEXT = b"BT /F1 24 Tf 72 700 Td (Frame) Tj ET\n"
RED_BOX = b"q 1 0 0 rg 300 500 60 30 re f Q\n"


def stream(dict_body: bytes, data: bytes) -> bytes:
    return (
        b"<< " + dict_body + b" /Length " + str(len(data)).encode() + b" >>\n"
        b"stream\n" + data + b"\nendstream"
    )


def link(rect: bytes, uri: bytes) -> bytes:
    return (
        b"<< /Type /Annot /Subtype /Link /Rect [" + rect + b"] /F 4 "
        b"/A << /S /URI /URI (" + uri + b") >> >>"
    )


def text_field(name: bytes, rect: bytes) -> bytes:
    return (
        b"<< /Type /Annot /Subtype /Widget /FT /Tx /T (" + name + b") "
        b"/V (framed) /Rect [" + rect + b"] /F 4 >>"
    )


# Object numbers, fixed so the cross references below stay readable.
CATALOG, ROOT, NODE4 = 1, 2, 3
PAGES = [4, 5, 6, 7, 8, 9, 10, 11]
FONT, IMAGE = 12, 13
CONTENT, CONTENT5, CONTENT6, CONTENT7 = 14, 15, 16, 17
LINK1, LINK5, FIELD5, LINK7, FIELD7 = 18, 19, 20, 21, 22


def ref(num: int) -> bytes:
    return str(num).encode() + b" 0 R"


def page(parent: int, contents: int, extra: bytes = b"") -> bytes:
    return (
        b"<< /Type /Page /Parent " + ref(parent) + b" /Contents "
        + ref(contents) + extra + b" >>"
    )


objects = {
    CATALOG: b"<< /Type /Catalog /Pages " + ref(ROOT) + b" /AcroForm << "
    b"/Fields [" + ref(FIELD5) + b" " + ref(FIELD7) + b"] "
    b"/DA (/Helv 0 Tf 0 g) /DR << /Font << /Helv " + ref(FONT) + b" >> >> "
    b">> >>",
    ROOT: b"<< /Type /Pages /Kids [" + b" ".join(
        ref(n) for n in [PAGES[0], PAGES[1], PAGES[2], PAGES[3], NODE4,
                         PAGES[5], PAGES[6], PAGES[7]]
    ) + b"] /Count 8 /MediaBox [0 0 612 792] "
    b"/Resources << /Font << /F1 " + ref(FONT) + b" >> "
    b"/XObject << /Im0 " + ref(IMAGE) + b" >> >> >>",
    NODE4: b"<< /Type /Pages /Parent " + ref(ROOT) + b" /Kids ["
    + ref(PAGES[4]) + b"] /Count 1 /Rotate 90 >>",
    PAGES[0]: page(ROOT, CONTENT),
    PAGES[1]: page(ROOT, CONTENT, b" /Rotate 90 /Annots [" + ref(LINK1) + b"]"),
    PAGES[2]: page(ROOT, CONTENT, b" /Rotate 180"),
    PAGES[3]: page(ROOT, CONTENT, b" /Rotate 270"),
    PAGES[4]: page(NODE4, CONTENT),
    PAGES[5]: page(
        ROOT, CONTENT5,
        b" /CropBox [36 36 576 756] /Annots [" + ref(LINK5) + b" "
        + ref(FIELD5) + b"]",
    ),
    PAGES[6]: page(ROOT, CONTENT6, b" /MediaBox [100 200 712 992]"),
    PAGES[7]: page(
        ROOT, CONTENT7,
        b" /Rotate 90 /CropBox [50 60 562 732] /Annots [" + ref(LINK7)
        + b" " + ref(FIELD7) + b"]",
    ),
    FONT: b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    IMAGE: stream(
        b"/Type /XObject /Subtype /Image /Width 2 /Height 2 "
        b"/ColorSpace /DeviceRGB /BitsPerComponent 8",
        bytes([0, 0, 255, 0, 255, 0, 0, 255, 0, 0, 0, 255]),
    ),
    CONTENT: stream(b"", FRAME_TEXT + RED_BOX),
    CONTENT5: stream(
        b"", FRAME_TEXT + RED_BOX + b"BT /F1 24 Tf 20 400 Td (Edge) Tj ET\n"
    ),
    CONTENT6: stream(
        b"",
        b"BT /F1 24 Tf 172 900 Td (Frame) Tj ET\n"
        b"q 1 0 0 rg 400 700 60 30 re f Q\n",
    ),
    CONTENT7: stream(
        b"",
        b"BT /F1 24 Tf 72 650 Td (Frame) Tj ET\n"
        b"q 1 0 0 rg 300 400 60 30 re f Q\n"
        b"q 50 0 0 30 120 200 cm /Im0 Do Q\n",
    ),
    # The link rectangles are stored top corner first on purpose: a /Rect
    # may name any two opposite corners.
    LINK1: link(b"200 725 70 695", b"https://example.com/rotated"),
    LINK5: link(b"200 725 70 695", b"https://example.com/cropped"),
    FIELD5: text_field(b"cropped_field", b"300 300 450 320"),
    LINK7: link(b"70 645 200 675", b"https://example.com/both"),
    FIELD7: text_field(b"rotated_field", b"300 250 450 270"),
}

out = bytearray(b"%PDF-1.7\n")
offsets = {}
for num in sorted(objects):
    offsets[num] = len(out)
    out += str(num).encode() + b" 0 obj\n" + objects[num] + b"\nendobj\n"

size = max(objects) + 1
xref_pos = len(out)
out += b"xref\n0 " + str(size).encode() + b"\n"
out += b"0000000000 65535 f \n"
for num in range(1, size):
    out += f"{offsets[num]:010d} 00000 n \n".encode()
out += (
    b"trailer\n<< /Size " + str(size).encode() + b" /Root " + ref(CATALOG)
    + b" >>\nstartxref\n" + str(xref_pos).encode() + b"\n%%EOF\n"
)

Path(__file__).with_name("frames.pdf").write_bytes(bytes(out))
print(f"wrote frames.pdf ({len(out)} bytes)")
