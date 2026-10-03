#!/usr/bin/env python3
"""Regenerates encrypted.pdf: one empty Letter page under the standard
security handler (revision 2, 40-bit RC4) with user password "secret" and
owner password "owner". The page carries no strings or streams, so only
the /Encrypt dictionary depends on the key; it is computed here with the
standard library (ISO 32000-1, 7.6.3.3 algorithms 2 to 4)."""

import hashlib
from pathlib import Path

PAD = bytes.fromhex(
    "28bf4e5e4e758a4164004e56fffa01082e2e00b6d0683e802f0ca9fe6453697a"
)
USER = b"secret"
OWNER = b"owner"
PERMISSIONS = -44
FILE_ID = hashlib.md5(b"grpc-qparse encrypted.pdf").digest()


def padded(password):
    return (password + PAD)[:32]


def rc4(key, data):
    s = list(range(256))
    j = 0
    for i in range(256):
        j = (j + s[i] + key[i % len(key)]) % 256
        s[i], s[j] = s[j], s[i]
    out = bytearray()
    i = j = 0
    for byte in data:
        i = (i + 1) % 256
        j = (j + s[i]) % 256
        s[i], s[j] = s[j], s[i]
        out.append(byte ^ s[(s[i] + s[j]) % 256])
    return bytes(out)


owner_key = hashlib.md5(padded(OWNER)).digest()[:5]
o_entry = rc4(owner_key, padded(USER))
file_key = hashlib.md5(
    padded(USER) + o_entry + PERMISSIONS.to_bytes(4, "little", signed=True)
    + FILE_ID
).digest()[:5]
u_entry = rc4(file_key, PAD)

objects = [
    b"<< /Type /Catalog /Pages 2 0 R >>",
    b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>",
    b"<< /Filter /Standard /V 1 /R 2 /O <" + o_entry.hex().encode()
    + b"> /U <" + u_entry.hex().encode() + b"> /P "
    + str(PERMISSIONS).encode() + b" >>",
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
file_id = b"<" + FILE_ID.hex().encode() + b">"
out += (
    b"trailer\n<< /Size " + str(len(objects) + 1).encode()
    + b" /Root 1 0 R /Encrypt 4 0 R /ID [" + file_id + b" " + file_id
    + b"] >>\nstartxref\n" + str(xref_pos).encode() + b"\n%%EOF\n"
)

Path(__file__).with_name("encrypted.pdf").write_bytes(bytes(out))
print(f"wrote encrypted.pdf ({len(out)} bytes)")
