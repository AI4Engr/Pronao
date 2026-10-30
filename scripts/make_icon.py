#!/usr/bin/env python3
"""Makes a placeholder application icon: a rounded square in one colour with a white ring, at 16, 32 and 48 pixels.
    python3 scripts/make_icon.py <out.ico> [NAME] [RRGGBB]
The colour comes from RRGGBB, else from NAME (so different applications get different placeholders). The file is meant to be replaced by a real icon:
set ICON=<your.ico> in the build. Plain 32-bit BMP images, no compression and no libraries; the bytes are the same every time.
"""
import hashlib
import struct
import sys


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
        f.write(build(rgb))
    return 0


if __name__ == '__main__':
    sys.exit(main())
