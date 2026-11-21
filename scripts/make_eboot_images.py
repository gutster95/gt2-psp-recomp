"""XMB images of the EBOOT from the user's artwork in Images/ (not in git): build/ICON0.PNG (144x80, the logo fitted
with transparent borders) and build/PIC1.PNG (480x272 background, scaled to cover and centre-cropped).
Run: python scripts/make_eboot_images.py   (build.sh picks the files up when they exist)"""

from pathlib import Path

from PIL import Image

PROJ = Path(__file__).resolve().parent.parent
SRC_ICON = PROJ / "Images" / "icon.png"
SRC_BG = PROJ / "Images" / "gran-turismo-2.webp"
OUT = PROJ / "build"


def icon(src: Path, dst: Path, size=(144, 80)) -> None:
    im = Image.open(src).convert("RGBA")
    im.thumbnail(size, Image.LANCZOS)
    out = Image.new("RGBA", size, (0, 0, 0, 0))
    out.paste(im, ((size[0] - im.width) // 2, (size[1] - im.height) // 2), im)
    out.save(dst)


def background(src: Path, dst: Path, size=(480, 272)) -> None:
    im = Image.open(src).convert("RGB")
    k = max(size[0] / im.width, size[1] / im.height)
    im = im.resize((round(im.width * k), round(im.height * k)), Image.LANCZOS)
    x, y = (im.width - size[0]) // 2, (im.height - size[1]) // 2
    im.crop((x, y, x + size[0], y + size[1])).save(dst)


if __name__ == "__main__":
    OUT.mkdir(exist_ok=True)
    icon(SRC_ICON, OUT / "ICON0.PNG")
    background(SRC_BG, OUT / "PIC1.PNG")
    print("build/ICON0.PNG, build/PIC1.PNG")
