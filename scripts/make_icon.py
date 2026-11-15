#!/usr/bin/env python3
"""Makes a placeholder application icon: a rounded square in one colour with a white ring.
    python3 scripts/make_icon.py <out.ico | out.png> [NAME] [RRGGBB]
An .ico (Windows) holds 16, 32 and 48 pixels as plain 32-bit BMP images; a .png (Linux) is one 64 x 64 image. The colour comes from RRGGBB, else from NAME (so different
applications get different placeholders). The file is meant to be replaced by a real icon: set ICON=<your file> in the build. No libraries (zlib only, for the PNG);
the bytes are the same every time.
"""
import hashlib
import struct
import sys
import zlib


def colour_for(name):
    digest = hashlib.sha256(name.encode('utf-8')).digest()
    hue = digest[0] / 255.0 * 6.0              # a saturated colour that is dark enough for a white ring to show
    c = 0.55
    x = c * (1 - abs(hue % 2 - 1))
    r, g, b = [(c, x, 0), (x, c, 0), (0, c, x), (0, x, c), (x, 0, c), (c, 0, x)][min(int(hue), 5)]
    m = 0.25
    return tuple(int((v + m) * 255) for v in (r, g, b))


def image(size, rgb):
    """The XOR bitmap (bottom-up BGRA) and the AND mask of one square image."""
    radius = size * 0.22
    centre = (size - 1) / 2.0
    pixels = []
    for y in range(size - 1, -1, -1):          # bottom-up
        row = []
        for x in range(size):
            dx, dy = abs(x - centre), abs(y - centre)
            edge = centre - radius
            inside = True
            if dx > edge and dy > edge:        # in a corner: round it
                inside = ((dx - edge) ** 2 + (dy - edge) ** 2) ** 0.5 <= radius + 0.5
            if not inside:
                row.append((0, 0, 0, 0))
                continue
            d = ((x - centre) ** 2 + (y - centre) ** 2) ** 0.5
            ring = size * 0.26 <= d <= size * 0.38
            r, g, b = (255, 255, 255) if ring else rgb
            row.append((b, g, r, 255))
        pixels.append(row)
    xor = b''.join(bytes(p) for row in pixels for p in row)
    mask_row = ((size + 31) // 32) * 4
    return xor, bytes(mask_row * size)


def build(rgb):
    sizes = [16, 32, 48]
    images = []
    for s in sizes:
        xor, mask = image(s, rgb)
        header = struct.pack('<IiiHHIIiiII', 40, s, s * 2, 1, 32, 0, len(xor) + len(mask), 0, 0, 0, 0)
        images.append(header + xor + mask)
    out = struct.pack('<HHH', 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    for s, data in zip(sizes, images):
        out += struct.pack('<BBBBHHII', s, s, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    return out + b''.join(images)


def build_png(rgb, size=64):
    xor, _ = image(size, rgb)                  # bottom-up BGRA
    rows = [xor[i * size * 4:(i + 1) * size * 4] for i in range(size)][::-1]
    raw = b''
    for row in rows:
        raw += bytes([0]) + b''.join(bytes((row[i + 2], row[i + 1], row[i], row[i + 3])) for i in range(0, len(row), 4))

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    return bytes.fromhex('89504e470d0a1a0a') + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    name = sys.argv[2] if len(sys.argv) > 2 else 'app'
    if len(sys.argv) > 3:
        text = sys.argv[3].lstrip('#')
        if len(text) != 6 or any(c not in '0123456789abcdefABCDEF' for c in text):
            print('the colour must be six hex digits, RRGGBB')
            return 2
        rgb = tuple(int(text[i:i + 2], 16) for i in (0, 2, 4))
    else:
        rgb = colour_for(name)
    with open(sys.argv[1], 'wb') as f:
        f.write(build_png(rgb) if sys.argv[1].lower().endswith('.png') else build(rgb))
    return 0


if __name__ == '__main__':
    sys.exit(main())
