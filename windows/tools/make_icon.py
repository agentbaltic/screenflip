#!/usr/bin/env python3
"""Generate windows/app.ico — a small ScreenFlip tray icon: a blue rounded square
with a white double-headed horizontal arrow (the left<->right "flip" motif, the
Windows analogue of the macOS menu-bar "U+21CB"). Produces 16/32/48 px images in
one .ico. Pure stdlib (no PIL needed)."""
import struct, os, math

BLUE = (45, 125, 246)   # R,G,B  (#2D7DF6)
WHITE = (255, 255, 255)

def rounded(x, y, s, m, r):
    if x < m or y < m or x >= s - m or y >= s - m:
        return False
    cx = min(x - m, (s - 1 - m) - x)
    cy = min(y - m, (s - 1 - m) - y)
    if cx < r and cy < r:
        return (r - cx) ** 2 + (r - cy) ** 2 <= r * r
    return True

def arrow(x, y, s):
    # geometry scaled to size s
    yc = s / 2.0
    half_shaft = max(1, round(s * 0.045))
    headtipL = s * 0.18
    headbaseL = s * 0.34
    headbaseR = s * 0.66
    headtipR = s * 0.82
    th = s * 0.16   # arrowhead half-height at base
    # shaft
    if headbaseL <= x <= headbaseR and abs(y - yc) <= half_shaft:
        return True
    # left head (tip at headtipL, base at headbaseL)
    if headtipL <= x <= headbaseL:
        frac = (x - headtipL) / max(1e-6, (headbaseL - headtipL))
        if abs(y - yc) <= frac * th:
            return True
    # right head (tip at headtipR, base at headbaseR)
    if headbaseR <= x <= headtipR:
        frac = (headtipR - x) / max(1e-6, (headtipR - headbaseR))
        if abs(y - yc) <= frac * th:
            return True
    return False

def render(s):
    m = max(1, round(s * 0.06))
    r = max(2, round(s * 0.22))
    px = []  # top-down list of (B,G,R,A)
    for y in range(s):
        for x in range(s):
            if rounded(x, y, s, m, r):
                if arrow(x, y, s):
                    c = WHITE
                else:
                    c = BLUE
                px.append((c[2], c[1], c[0], 255))
            else:
                px.append((0, 0, 0, 0))
    return px

def image_bytes(s):
    px = render(s)
    # BITMAPINFOHEADER (biHeight doubled for XOR+AND)
    hdr = struct.pack('<IiiHHIIiiII', 40, s, s * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    xor = bytearray()
    for y in range(s - 1, -1, -1):           # bottom-up
        for x in range(s):
            b, g, r, a = px[y * s + x]
            xor += bytes((b, g, r, a))
    # AND mask: 1bpp, rows padded to 4 bytes; 1 = transparent
    row_bytes = ((s + 31) // 32) * 4
    amask = bytearray()
    for y in range(s - 1, -1, -1):
        bits = bytearray(row_bytes)
        for x in range(s):
            a = px[y * s + x][3]
            if a == 0:
                bits[x // 8] |= (0x80 >> (x % 8))
        amask += bits
    return hdr + bytes(xor) + bytes(amask)

def main():
    sizes = [16, 32, 48]
    images = [image_bytes(s) for s in sizes]
    out = bytearray()
    out += struct.pack('<HHH', 0, 1, len(sizes))     # ICONDIR
    offset = 6 + 16 * len(sizes)
    for s, img in zip(sizes, images):
        w = 0 if s == 256 else s
        out += struct.pack('<BBBBHHII', w, w, 0, 0, 1, 32, len(img), offset)
        offset += len(img)
    for img in images:
        out += img
    dst = os.path.join(os.path.dirname(__file__), '..', 'app.ico')
    with open(os.path.abspath(dst), 'wb') as f:
        f.write(out)
    print('wrote', os.path.abspath(dst), len(out), 'bytes')

if __name__ == '__main__':
    main()
