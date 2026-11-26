"""Neutral XMB icon for the release launcher (converter/ICON0.PNG, 144x80): own design in the converter's style
(racing stripes, "GT2 PSP"), no game artwork. Run once: python scripts/make_release_icon.py (needs Pillow + a bold
font; the PNG is committed, the converter only copies it)."""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

PROJ = Path(__file__).resolve().parent.parent
FONTS = ["C:/Windows/Fonts/ariblk.ttf", "/System/Library/Fonts/Supplemental/Arial Black.ttf",
         "/usr/share/fonts/truetype/msttcorefonts/Arial_Black.ttf"]


def font(size: int):
    for f in FONTS:
        if Path(f).exists():
            return ImageFont.truetype(f, size)
    raise SystemExit("no bold font found")


def main() -> None:
    s = 4                                              # draw at 4x, then scale down (anti-aliasing)
    w, h = 144 * s, 80 * s
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((0, 0, w - 1, h - 1), radius=8 * s, fill=(10, 13, 21, 255))
    x = 14 * s                                         # slanted stripes: red, blue, white
    for col, length in (((227, 36, 43), 46), ((29, 91, 216), 26), ((238, 242, 248), 10)):
        y0, y1 = 14 * s, 19 * s
        d.polygon([(x + 2 * s, y0), (x + length * s + 2 * s, y0), (x + length * s, y1), (x, y1)], fill=col)
        x += (length + 4) * s
    f = font(30 * s)
    text_img = Image.new("RGBA", (w * 2, h), (0, 0, 0, 0))
    td = ImageDraw.Draw(text_img)
    td.text((0, 0), "GT2", font=f, fill=(238, 242, 248, 255))
    gw = td.textlength("GT2 ", font=f)
    td.text((gw, 0), "PSP", font=f, fill=(227, 36, 43, 255))
    bbox = text_img.getbbox()
    text_img = text_img.crop(bbox)
    # italic: shear
    k = 0.18
    tw, th = text_img.size
    text_img = text_img.transform((int(tw + k * th), th), Image.AFFINE, (1, -k, k * th, 0, 1, 0), Image.BICUBIC)
    scale = min((w - 28 * s) / text_img.width, (h - 34 * s) / text_img.height)
    text_img = text_img.resize((int(text_img.width * scale), int(text_img.height * scale)), Image.LANCZOS)
    im.alpha_composite(text_img, (14 * s, 28 * s))
    im = im.resize((144, 80), Image.LANCZOS)
    out = PROJ / "converter" / "ICON0.PNG"
    im.save(out, optimize=True)
    print(out, out.stat().st_size, "bytes")


if __name__ == "__main__":
    main()
