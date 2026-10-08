"""Lineage2Ver413 .dat container: [28-byte UTF-16 header][N x 128-byte RSA blocks][20-byte trailer]

Each decrypted 128-byte block = [4-byte big-endian size (<=124)][size bytes of data][zero padding].
The concatenated data is [4-byte little-endian uncompressed length][zlib stream] = the table L2ClientDat describes.

The client of this project has the community "encdec" public key (n=75B4..., e=0x1d) built in, which is why
files can be written with the matching private exponent.
"""
import hashlib, struct, sys, zlib

HDR = 'Lineage2Ver413'.encode('utf-16le')          # 28 bytes
N = int('75B4D6DE5C016544068A1ACF125869F43D2E09FC55B8B1E289556DAF9B8757635593446288B3653DA1CE91C87BB1A5C18F16323495C55D7D72C0890A83F69BFD1FD9434EB1C02F3E4679EDFA43309319070129C267C85604D87BB65BAE205DE3707AF1D2108881ABB567C3B3D069AE67C3A4C6A3AA93D26413D4C66094AE2039', 16)
E = 0x1d
D = int('30b4c2d798d47086145c75063c8e841e719776e400291d7838d3e6c4405b504c6a07f8fca27f32b86643d2649d1d5f124cdd0bf272f0909dd7352fe10a77b34d831043d9ae541f8263c6fe3d1c14c2f04e43a7253a6dda9a8c1562cbd493c1b631a1957618ad5dfe5ca28553f746e2fc6f2db816c7db223ec91e955081c1de65', 16)


def split(data: bytes):
    if data[:28] != HDR:
        raise ValueError('not a Lineage2Ver413 file')
    return data[28:-20], data[-20:]


def block_data(blk: bytes) -> bytes:
    """[size:4 BE][zeros][data (size bytes)][zeros up to a multiple of 4] with data+alignment flush against the block end."""
    size = struct.unpack('>I', blk[:4])[0]
    if size > 124:
        raise ValueError('bad block size %d' % size)
    start = 128 - ((size + 3) & ~3)
    return blk[start:start + size]


def make_block(chunk: bytes) -> bytes:
    size = len(chunk)
    pad = (-size) % 4
    blk = struct.pack('>I', size) + b'\x00' * (124 - size - pad) + chunk + b'\x00' * pad
    assert len(blk) == 128
    return blk


def decode_payload(data: bytes, strict: bool = True) -> bytes:
    body, _trailer = split(data)
    out = bytearray()
    for i in range(0, len(body), 128):
        blk = pow(int.from_bytes(body[i:i + 128], 'big'), E, N).to_bytes(128, 'big')
        out += block_data(blk)
    n = struct.unpack('<I', out[:4])[0]
    if strict:
        raw = zlib.decompress(bytes(out[4:]))
        if len(raw) != n:
            raise ValueError('length mismatch %d != %d' % (len(raw), n))
        return raw
    return zlib.decompressobj().decompress(bytes(out[4:]))     # real files: stream is not terminated, length field is only a hint


def extract_compressed(data: bytes) -> bytes:
    """[4-byte LE uncompressed length][zlib stream] exactly as stored (before RSA)."""
    body, _ = split(data)
    out = bytearray()
    for i in range(0, len(body), 128):
        out += block_data(pow(int.from_bytes(body[i:i + 128], 'big'), E, N).to_bytes(128, 'big'))
    return bytes(out)


def build_file(comp: bytes) -> bytes:
    """comp = [4-byte LE uncompressed length][zlib stream] -> complete Lineage2Ver413 file (needs the private exponent)."""
    body = bytearray()
    for i in range(0, len(comp), 124):
        body += pow(int.from_bytes(make_block(comp[i:i + 124]), 'big'), D, N).to_bytes(128, 'big')
    out = HDR + bytes(body)
    return out + trailer(out)


def encode_payload(raw: bytes) -> bytes:
    return build_file(struct.pack('<I', len(raw)) + zlib.compress(raw, 9))


def trailer(prefix: bytes) -> bytes:
    """12 zero bytes + CRC32(header+blocks) + 4 zero bytes (endianness verified against real files)."""
    return b'\x00' * 12 + struct.pack(TRAILER_CRC_FMT, zlib.crc32(prefix) & 0xffffffff) + b'\x00' * 4


TRAILER_CRC_FMT = '<I'


if __name__ == '__main__':
    p = sys.argv[1]
    data = open(p, 'rb').read()
    raw = decode_payload(data)
    body, tr = split(data)
    print('decoded', len(raw), 'bytes; blocks', len(body) // 128)
    cands = {
        'sha1(header+body)': hashlib.sha1(data[:-20]).digest(),
        'sha1(body)': hashlib.sha1(body).digest(),
        'sha1(raw)': hashlib.sha1(raw).digest(),
        'sha1(file w/o header)': hashlib.sha1(data[28:-20]).digest(),
    }
    print('trailer', tr.hex())
    for k, v in cands.items():
        print('  ', k, 'MATCH' if v == tr else '-')
    print('first 160 raw bytes:', raw[:160].hex())
