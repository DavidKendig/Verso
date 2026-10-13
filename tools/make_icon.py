"""Builds Verso's application icons from the logo, res/VersoLogoGold.png.

Writes, for each platform's packaging:
  res/Verso.ico           Windows (16-256 px, linked into Verso.exe)
  res/Verso.icns          macOS app bundles
  res/icons/verso-<n>.png Linux desktop entries (hicolor sizes)

Requires Pillow: python -m pip install pillow
Run with: python tools/make_icon.py
"""
import os

from PIL import Image

RES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "res")
LOGO = os.path.join(RES, "VersoLogoGold.png")
ICO_SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
LINUX_SIZES = [16, 32, 48, 64, 128, 256, 512]


def square(img):
    """Centers the logo on a transparent square canvas (it is square already)."""
    side = max(img.size)
    canvas = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    canvas.paste(img, ((side - img.width) // 2, (side - img.height) // 2))
    return canvas


def main():
    logo = square(Image.open(LOGO).convert("RGBA"))
    ico = os.path.join(RES, "Verso.ico")
    logo.save(ico, format="ICO", sizes=[(s, s) for s in ICO_SIZES])
    print("wrote", os.path.normpath(ico), os.path.getsize(ico), "bytes")

    icns = os.path.join(RES, "Verso.icns")
    logo.resize((1024, 1024), Image.LANCZOS).save(icns, format="ICNS")
    print("wrote", os.path.normpath(icns), os.path.getsize(icns), "bytes")

    linux = os.path.join(RES, "icons")
    os.makedirs(linux, exist_ok=True)
    for s in LINUX_SIZES:
        path = os.path.join(linux, f"verso-{s}.png")
        logo.resize((s, s), Image.LANCZOS).save(path, format="PNG", optimize=True)
    print("wrote", os.path.normpath(linux), "for", ", ".join(str(s) for s in LINUX_SIZES), "px")


if __name__ == "__main__":
    main()
