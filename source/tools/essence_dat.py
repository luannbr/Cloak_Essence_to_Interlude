#!/usr/bin/env python3
"""Decrypt a Lineage2Ver413 client .dat (RSA blocks + zlib) with the keys of the L2s DatEditor (cryptVersion.xml).

  python essence_dat.py <in.dat> <out.bin> [--keys H:\\...\\cryptVersion.xml]

Each 128-byte block is RSA-decrypted with (modulus, exp); byte 3 of the result is the payload size (<= 124) and the payload sits at the END
of the block. The concatenated payload is: u32 uncompressed size (LE) + zlib stream.
"""
import os, sys, zlib, struct, argparse
import xml.etree.ElementTree as ET

KEYS = os.environ.get('L2_ESSENCE_CRYPT_XML', '')


def load_keys(path=KEYS, code='413'):
    out = []
    for k in ET.parse(path).getroot().iter('key'):
        if k.get('type') == 'rsa' and k.get('decrypt') == 'true' and k.get('code') == code:
            out.append((k.get('name'), int(k.get('modulus'), 16), int(k.get('exp'), 16)))
    return out


def block_ok(c, n, e):
    if c >= n: return False
    b = pow(c, e, n).to_bytes(128, 'big')
    return b[0] == 0 and b[1] == 0 and b[2] == 0 and b[3] <= 124


def decrypt(raw, keys=None):
    assert raw[:28].decode('utf-16le') == 'Lineage2Ver413', 'not a Ver413 file'
    body = raw[28:]
    body = body[:len(body) // 128 * 128]                       # a few trailing bytes (DAT_ADD_END_BYTES) follow the last block
    blocks = [int.from_bytes(body[i:i + 128], 'big') for i in range(0, len(body), 128)]
    for name, n, e in (keys or load_keys()):
        if all(block_ok(c, n, e) for c in blocks[:4]):
            payload = bytearray()
            for c in blocks:
                b = pow(c, e, n).to_bytes(128, 'big')
                size = b[3]
                payload += b[128 - size:] if size else b''
            size_u = struct.unpack_from('<I', payload, 0)[0]
            try:
                data = zlib.decompress(bytes(payload[4:]))
            except zlib.error as ex:                                   # the checksum can be off when the file carries extra trailing bytes: keep what inflated
                d = zlib.decompressobj(-15); data = d.decompress(bytes(payload[6:]))                   # raw deflate: skip the 2-byte zlib header, no checksum
                print('warning: zlib %s - kept %d bytes' % (ex, len(data)), file=sys.stderr)
            if len(data) != size_u:
                print('warning: size field %d != inflated %d' % (size_u, len(data)), file=sys.stderr)
            return name, data
    raise ValueError('none of the keys decrypts this file')


if __name__ == '__main__':
    ap = argparse.ArgumentParser(); ap.add_argument('src'); ap.add_argument('dst')
    a = ap.parse_args()
    name, data = decrypt(open(a.src, 'rb').read())
    open(a.dst, 'wb').write(data)
    print('key %s: %d bytes -> %s' % (name, len(data), a.dst))
