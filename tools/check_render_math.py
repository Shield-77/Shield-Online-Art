"""Compare optimized renderer equations with the previous bilinear reference.

These are host-side mathematical checks, not a hardware FPS measurement.
"""

import random


def previous_blend(a, b, c, d, x, y):
    top = a * (256 - x) + b * x
    bottom = c * (256 - x) + d * x
    return (top * (256 - y) + bottom * y + 32768) >> 16


def optimized_blend(a, b, c, d, x, y):
    top = a * 256 + (b - a) * x
    bottom = c * 256 + (d - c) * x
    return (top * 256 + (bottom - top) * y + 32768) >> 16


def fit(width, height):
    if width * 240 > height * 360:
        return 360, max(1, height * 360 // width)
    return max(1, width * 240 // height), 240


def sample_axis(coordinate, dimension, stride):
    coordinate = min(coordinate, (dimension - 1) << 8)
    first = coordinate >> 8
    return first * stride, min(first + 1, dimension - 1) * stride, coordinate & 255


def pixel(x, y):
    return (x * 31 + y * 17 + (x ^ y) * 7) & 255


def previous_pixel(width, height, rotation, zoom, base, origin, x, y):
    rotated_w, rotated_h = (height, width) if rotation & 1 else (width, height)
    view_x = (x - origin[0]) * 25600 // zoom
    view_y = (y - origin[1]) * 25600 // zoom
    rx = min(view_x * rotated_w // base[0], (rotated_w - 1) << 8)
    ry = min(view_y * rotated_h // base[1], (rotated_h - 1) << 8)
    sx, sy = [(rx, ry), (ry, ((height - 1) << 8) - rx),
              (((width - 1) << 8) - rx, ((height - 1) << 8) - ry),
              (((width - 1) << 8) - ry, rx)][rotation]
    x0, y0 = sx >> 8, sy >> 8
    x1, y1 = min(x0 + 1, width - 1), min(y0 + 1, height - 1)
    return previous_blend(pixel(x0, y0), pixel(x1, y0), pixel(x0, y1),
                          pixel(x1, y1), sx & 255, sy & 255)


def mapped_pixel(width, height, rotation, zoom, base, origin, x, y):
    swapped = bool(rotation & 1)
    cw, rh = (height, width) if swapped else (width, height)
    cx = min(((x - origin[0]) * 25600 // zoom) * cw // base[0], (cw - 1) << 8)
    cy = min(((y - origin[1]) * 25600 // zoom) * rh // base[1], (rh - 1) << 8)
    if rotation in (1, 2):
        cx = ((cw - 1) << 8) - cx
    if rotation in (2, 3):
        cy = ((rh - 1) << 8) - cy
    column = sample_axis(cx, cw, width * 3 if swapped else 3)
    row = sample_axis(cy, rh, 3 if swapped else width * 3)

    def at(row_offset, column_offset):
        offset = row_offset + column_offset
        assert 0 <= offset <= (width * height - 1) * 3
        py, px = divmod(offset // 3, width)
        return pixel(px, py)

    return optimized_blend(at(row[0], column[0]), at(row[0], column[1]),
                           at(row[1], column[0]), at(row[1], column[1]),
                           column[2], row[2])


def main():
    rng = random.Random(77)
    for _ in range(20000):
        values = [rng.randrange(256) for _ in range(6)]
        assert previous_blend(*values) == optimized_blend(*values)
    checks = 0
    for width, height in [(1, 1), (1, 960), (1440, 1), (13, 21), (640, 360),
                           (1440, 960), (766, 960), (720, 480), (360, 240)]:
        for rotation in range(4):
            base = fit(height, width) if rotation & 1 else fit(width, height)
            for zoom in [100, 125, 175, 200, 225, 300, 400]:
                draw_w, draw_h = (v * zoom // 100 for v in base)
                max_x, max_y = max(0, (draw_w - 400) // 2), max(0, (draw_h - 240) // 2)
                for pan_x, pan_y in [(0, 0), (-max_x, -max_y), (max_x, max_y)]:
                    origin = ((400 - draw_w) // 2 + pan_x, (240 - draw_h) // 2 + pan_y)
                    for depth in (0, 6):
                        x0, x1 = max(origin[0], -depth), min(origin[0] + draw_w, 400 + depth)
                        y0, y1 = max(origin[1], 0), min(origin[1] + draw_h, 240)
                        for x, y in [(x0, y0), (x1 - 1, y1 - 1),
                                     (rng.randrange(x0, x1), rng.randrange(y0, y1))]:
                            old = previous_pixel(width, height, rotation, zoom, base, origin, x, y)
                            new = mapped_pixel(width, height, rotation, zoom, base, origin, x, y)
                            assert old == new, (width, height, rotation, zoom, x, y, old, new)
                            for eye_x in (x - depth, x + depth):
                                if 0 <= eye_x < 400:
                                    offset = (eye_x * 240 + 239 - y) * 3
                                    assert 0 <= offset <= 400 * 240 * 3 - 3
                            checks += 1
    # Area reduction suppresses the aliasing of a one-pixel checkerboard.
    assert (0 + 255 + 255 + 0 + 2) // 4 == 128
    print(f"PASS: 20,000 bilinear comparisons; {checks:,} geometry/pixel/bounds cases.")


if __name__ == "__main__":
    main()
