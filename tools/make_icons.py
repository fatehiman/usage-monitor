"""Makes res/app.ico (gauge) and services/*.ico (one colored dot per service). No extra libraries.
Run: python -I tools/make_icons.py  (from the project root)
"""
import math
import struct
import sys
from pathlib import Path

SIZES = [16, 20, 24, 32, 48]


def blend(dst, src, a):
    return tuple(int(d * (1 - a) + s * a) for d, s in zip(dst, src))


def render(size, shade):
    """shade(x, y) in 0..1 coords -> (r, g, b, a) or None. 4x4 supersampling."""
    px = []
    for y in range(size):
        row = []
        for x in range(size):
            acc = [0, 0, 0, 0]
            for sy in range(4):
                for sx in range(4):
                    u = (x + (sx + 0.5) / 4) / size
                    v = (y + (sy + 0.5) / 4) / size
                    c = shade(u, v)
                    if c:
                        r, g, b, a = c
                        acc[0] += r * a
                        acc[1] += g * a
                        acc[2] += b * a
                        acc[3] += a
            a = acc[3] / 16
            if acc[3]:
                row.append((acc[0] / acc[3], acc[1] / acc[3], acc[2] / acc[3], a))
            else:
                row.append((0, 0, 0, 0))
        px.append(row)
    return px


def gauge(u, v):
    dx, dy = u - 0.5, v - 0.5
    r = math.hypot(dx, dy)
    if r > 0.48:
        return None
    ang = math.degrees(math.atan2(-dy, dx))  # 0 = right, 90 = up
    col = (38, 110, 190)  # blue disc
    # ring track
    if 0.30 < r < 0.42 and (ang > -40 or ang < -140):
        # filled part of the arc from 220deg (bottom left) clockwise to ~20deg
        a = ang if ang > -40 else ang + 360
        col = (120, 220, 120) if a > 60 else (255, 255, 255)
        if a > 200:
            col = (255, 255, 255)
        return (*col, 1.0)
    # needle
    nx, ny = math.cos(math.radians(60)), math.sin(math.radians(60))
    t = dx * nx + (-dy) * ny
    d = abs(dx * ny - (-dy) * nx)
    if 0 <= t < 0.33 and d < 0.045:
        return (255, 255, 255, 1.0)
    if r < 0.075:
        return (255, 255, 255, 1.0)
    return (*col, 1.0)


def dot(u, v):
    r = math.hypot(u - 0.5, v - 0.5)
    if r > 0.46:
        return None
    if r < 0.18:
        return (255, 236, 220, 1.0)
    return (217, 119, 87, 1.0)


def ring_dot(col):
    """Plain colored disc with a light center (simple, no brand art)."""
    def shade(u, v):
        r = math.hypot(u - 0.5, v - 0.5)
        if r > 0.46:
            return None
        if r < 0.18:
            return (240, 244, 255, 1.0)
        return (*col, 1.0)
    return shade


def ico(shade):
    images = []
    for s in SIZES:
        px = render(s, shade)
        rows = b""
        for y in range(s - 1, -1, -1):  # bottom-up
            for x in range(s):
                r, g, b, a = px[y][x]
                rows += struct.pack("BBBB", int(b), int(g), int(r), int(a * 255 + 0.5))
        mask_row = ((s + 31) // 32) * 4
        mask = b"\x00" * (mask_row * s)
        hdr = struct.pack("<IiiHHIIiiII", 40, s, s * 2, 1, 32, 0, len(rows) + len(mask), 0, 0, 0, 0)
        images.append((s, hdr + rows + mask))
    out = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for s, data in images:
        out += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    for _, data in images:
        out += data
    return out


if __name__ == "__main__":
    root = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    (root / "res").mkdir(exist_ok=True)
    (root / "services").mkdir(exist_ok=True)
    (root / "res" / "app.ico").write_bytes(ico(gauge))
    (root / "services" / "claude.ico").write_bytes(ico(dot))
    (root / "services" / "deepseek.ico").write_bytes(ico(ring_dot((77, 107, 254))))  # blue
    (root / "services" / "soniox.ico").write_bytes(ico(ring_dot((32, 150, 140))))   # teal
    (root / "services" / "msgway.ico").write_bytes(ico(ring_dot((230, 120, 30))))   # orange
    (root / "services" / "mihansms.ico").write_bytes(ico(ring_dot((70, 160, 60))))  # green
    (root / "services" / "openrouter.ico").write_bytes(ico(ring_dot((100, 100, 120))))  # slate
    print("icons written")
