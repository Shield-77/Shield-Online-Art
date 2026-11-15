"""Generate the SHIELD Online Art CIA banner and HOME Menu icon."""

from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont


ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / "assets"
MASTER_PATH = ASSETS / "shield-online-art-brand-master.png"
BANNER_PATH = ROOT / "banner.png"
ICON_PATH = ROOT / "icon.png"
FONT_BOLD = Path("C:/Windows/Fonts/arialbd.ttf")
FONT_REGULAR = Path("C:/Windows/Fonts/arial.ttf")


def load_font(path: Path, size: int):
    try:
        return ImageFont.truetype(str(path), size)
    except OSError:
        return ImageFont.load_default()


def fit_font(draw: ImageDraw.ImageDraw, text: str, path: Path, max_width: int,
             start_size: int):
    size = start_size
    while size > 12:
        font = load_font(path, size)
        if draw.textbbox((0, 0), text, font=font)[2] <= max_width:
            return font
        size -= 2
    return load_font(path, size)


def make_background(width: int, height: int) -> Image.Image:
    top = (5, 16, 31)
    bottom = (8, 31, 53)
    strip = Image.new("RGB", (1, height))
    pixels = []
    for y in range(height):
        t = y / max(height - 1, 1)
        pixels.append(tuple(round(a * (1 - t) + b * t)
                            for a, b in zip(top, bottom)))
    strip.putdata(pixels)
    background = strip.resize((width, height))

    glow = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    glow_draw = ImageDraw.Draw(glow)
    glow_draw.ellipse((80, 80, 690, 690), fill=(0, 198, 230, 42))
    glow_draw.ellipse((820, 150, 1500, 730), fill=(139, 51, 255, 20))
    glow = glow.filter(ImageFilter.GaussianBlur(100))
    return Image.alpha_composite(background.convert("RGBA"), glow)


def draw_shield(draw: ImageDraw.ImageDraw, box, line_scale: float = 1.0):
    left, top, side = box
    s = side / 512.0

    def p(points):
        return [(round(left + x * s), round(top + y * s)) for x, y in points]

    outer = [(256, 22), (455, 99), (438, 306), (256, 490),
             (74, 306), (57, 99)]
    inner = [(256, 49), (426, 115), (410, 294), (256, 455),
             (102, 294), (86, 115)]
    draw.polygon(p(outer), fill="#082b4a", outline="#19d7ee",
                 width=max(2, round(18 * s * line_scale)))
    draw.polygon(p(inner), fill="#0b1f37")

    # Picture frame and simple landscape motif, legible at the 3DS icon scale.
    x1, y1 = left + 132 * s, top + 174 * s
    x2, y2 = left + 380 * s, top + 338 * s
    stroke = max(2, round(13 * s * line_scale))
    radius = max(3, round(18 * s))
    draw.rounded_rectangle((x1, y1, x2, y2), radius=radius,
                           fill="#102d4b", outline="#f4fbff", width=stroke)
    draw.ellipse((left + 298 * s, top + 196 * s,
                  left + 332 * s, top + 230 * s), fill="#ff38a8")
    draw.polygon(p([(151, 306), (213, 244), (252, 280),
                    (296, 238), (360, 306)]), fill="#1cd5ee")
    draw.line(p([(151, 306), (213, 244), (252, 280),
                 (296, 238), (360, 306)]), fill="#effbff",
              width=max(1, round(3 * s)))

    # One small sparkle makes the icon read as an art/gallery app.
    cx, cy, r = left + 397 * s, top + 146 * s, 20 * s
    sparkle = [(cx, cy - r), (cx + r * .25, cy - r * .25),
               (cx + r, cy), (cx + r * .25, cy + r * .25),
               (cx, cy + r), (cx - r * .25, cy + r * .25),
               (cx - r, cy), (cx - r * .25, cy - r * .25)]
    draw.polygon(sparkle, fill="#ff38a8")


def make_master() -> Image.Image:
    width, height = 1536, 768
    image = make_background(width, height)
    draw = ImageDraw.Draw(image)

    draw_shield(draw, (92, 117, 620), 1.0)

    divider_x = 760
    draw.rounded_rectangle((divider_x, 134, divider_x + 10, 634),
                           radius=5, fill="#19d7ee")
    draw.rounded_rectangle((divider_x + 20, 189, divider_x + 27, 579),
                           radius=3, fill="#ff38a8")

    text_x = 830
    shield_font = fit_font(draw, "SHIELD", FONT_BOLD, 640, 126)
    draw.text((text_x, 180), "SHIELD", font=shield_font,
              fill="#f6fbff", stroke_width=1, stroke_fill="#c8faff")

    online_font = fit_font(draw, "ONLINE", FONT_BOLD, 415, 91)
    art_font = fit_font(draw, "ART", FONT_BOLD, 230, 91)
    draw.text((text_x, 337), "ONLINE", font=online_font, fill="#19d7ee")
    online_width = draw.textbbox((0, 0), "ONLINE", font=online_font)[2]
    draw.text((text_x + online_width + 22, 337), "ART",
              font=art_font, fill="#ff38a8")

    draw.rounded_rectangle((text_x, 482, 1464, 492), radius=5,
                           fill="#16496c")
    draw.rounded_rectangle((text_x, 482, 1060, 492), radius=5,
                           fill="#19d7ee")
    sub = "ART GALLERY  •  3DS / 2DS"
    sub_font = fit_font(draw, sub, FONT_REGULAR, 630, 39)
    draw.text((text_x, 535), sub, font=sub_font, fill="#dcebf6")

    # Small color chips, kept out of the text area for a clean HOME banner.
    for x, color in ((1400, "#19d7ee"), (1440, "#ff38a8"), (1480, "#8d5bff")):
        draw.rounded_rectangle((x, 646, x + 18, 664), radius=6, fill=color)

    return image.convert("RGB")


def make_icon() -> Image.Image:
    size = 512
    icon = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(icon)
    draw_shield(draw, (0, 0, size), 1.0)
    return icon.resize((48, 48), Image.Resampling.LANCZOS)


def main():
    ASSETS.mkdir(exist_ok=True)
    master = make_master()
    master.save(MASTER_PATH, optimize=True)
    master.resize((256, 128), Image.Resampling.LANCZOS).save(
        BANNER_PATH, optimize=True
    )
    make_icon().save(ICON_PATH, optimize=True)
    print("Prepared SHIELD Online Art assets: banner.png (256x128), icon.png (48x48).")


if __name__ == "__main__":
    main()
