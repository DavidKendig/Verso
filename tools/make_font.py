"""Builds res/fonts/JetBrainsMonoSlashed-*.ttf from the official JetBrains Mono release.

JetBrains Mono draws a dotted zero by default and offers a slashed zero through
the OpenType `zero` feature. Git mode renders text with GDI, which cannot enable
OpenType features, so this script makes the slashed zero the default glyph for
U+0030 in every style. The family is renamed to "JetBrains Mono Slashed" so it
never collides with an installed copy of JetBrains Mono. The SIL Open Font
License 1.1 permits this modification; the result stays under the same license
(res/fonts/OFL.txt).

Usage: python tools/make_font.py <folder containing JetBrainsMono-*.ttf from the 2.304 release>
Requires fontTools (pip install fonttools).
"""
import os
import shutil
import sys

from fontTools.ttLib import TTFont

FAMILY = "JetBrains Mono Slashed"
PS_FAMILY = "JetBrainsMonoSlashed"
STYLES = ["Regular", "Bold", "Italic", "BoldItalic"]


def slashed_zero_glyph(font):
    """Returns the glyph the `zero` feature substitutes for 'zero'."""
    gsub = font["GSUB"].table
    for record in gsub.FeatureList.FeatureRecord:
        if record.FeatureTag != "zero":
            continue
        for index in record.Feature.LookupListIndex:
            for sub in gsub.LookupList.Lookup[index].SubTable:
                mapping = getattr(sub, "mapping", None)
                if mapping and "zero" in mapping:
                    return mapping["zero"]
    sys.exit("the font has no `zero` feature for the digit zero")


def build(src_dir, out_dir, style):
    font = TTFont(os.path.join(src_dir, f"JetBrainsMono-{style}.ttf"))
    slashed = slashed_zero_glyph(font)
    for table in font["cmap"].tables:
        if 0x30 in table.cmap:
            table.cmap[0x30] = slashed

    subfamily = font["name"].getDebugName(2)  # "Regular", "Bold", "Italic", "Bold Italic"
    version = font["name"].getDebugName(5)
    names = {
        1: FAMILY,
        3: f"{version};{PS_FAMILY}-{style}",
        4: FAMILY if style == "Regular" else f"{FAMILY} {subfamily}",
        6: f"{PS_FAMILY}-{style}",
        16: FAMILY,
        10: "Modified from JetBrains Mono 2.304 for Verso: U+0030 uses the slashed zero by default.",
    }
    name = font["name"]
    for record in list(name.names):
        if record.nameID in (16, 17, 21, 22, 25):
            # Typographic/WWS names are rebuilt below; drop the originals.
            if record.nameID != 16:
                name.removeNames(nameID=record.nameID)
    for name_id, value in names.items():
        name.setName(value, name_id, 3, 1, 0x409)  # Windows, Unicode BMP, en-US
        name.setName(value, name_id, 1, 0, 0)      # Macintosh, Roman, English
    name.setName(subfamily, 17, 3, 1, 0x409)
    out = os.path.join(out_dir, f"{PS_FAMILY}-{style}.ttf")
    font.save(out)
    print(f"wrote {out} (U+0030 -> {slashed})")


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    src = sys.argv[1]
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "res", "fonts")
    os.makedirs(out, exist_ok=True)
    for style in STYLES:
        build(src, out, style)
    shutil.copyfile(os.path.join(src, "OFL.txt"), os.path.join(out, "OFL.txt"))
    print("copied OFL.txt")


if __name__ == "__main__":
    main()
