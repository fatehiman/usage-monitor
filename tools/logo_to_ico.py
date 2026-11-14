"""Makes a service icon (.ico, 16/20/24/32/48 px) from a logo image (PNG / ICO, best 128 px or more).
Run:  python -I tools/logo_to_ico.py <logo.png> services/<name>.ico
Logo source used for the services: https://www.google.com/s2/favicons?domain=<site>&sz=256
Needs Pillow.
"""
import sys
from PIL import Image

SIZES = [16, 20, 24, 32, 48]


def main(src, dst):
    im = Image.open(src).convert('RGBA')
    box = im.getchannel('A').getbbox()  # cut empty transparent borders
    if box:
        im = im.crop(box)
    side = max(im.size)  # square, logo centered
    sq = Image.new('RGBA', (side, side), (0, 0, 0, 0))
    sq.paste(im, ((side - im.width) // 2, (side - im.height) // 2))
    big = sq.resize((256, 256), Image.LANCZOS)
    big.save(dst, format='ICO', sizes=[(s, s) for s in SIZES])
    print('written', dst)


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
