"""Convert the 1024x1024 app icon master (PNG) into a multi-size Windows .ico.

Usage: python make_icon.py <icon.png> <out.ico>    (needs Pillow)
"""
import sys

from PIL import Image

ICO_SIZES = [(16, 16), (20, 20), (24, 24), (32, 32), (40, 40), (48, 48), (64, 64), (128, 128), (256, 256)]


def main(png_path: str, ico_path: str) -> None:
    with Image.open(png_path) as master:
        master.convert("RGBA").save(ico_path, format="ICO", sizes=ICO_SIZES)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
