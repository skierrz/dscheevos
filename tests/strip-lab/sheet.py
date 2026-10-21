import sys, glob, re
from PIL import Image, ImageDraw
d, out = sys.argv[1], sys.argv[2]
fs = sorted(glob.glob(d + "/screen.ppm.f*.png"), key=lambda f: int(re.search(r"\.f(\d+)\.png", f).group(1)))
ims = [Image.open(f) for f in fs]
w, h = ims[0].size; cols = 6; rows = (len(ims) + cols - 1) // cols
sheet = Image.new("RGB", (cols * w, rows * (h + 12)), "white")
dr = ImageDraw.Draw(sheet)
for i, (f, im) in enumerate(zip(fs, ims)):
    x, y = (i % cols) * w, (i // cols) * (h + 12)
    sheet.paste(im, (x, y + 12)); dr.text((x + 2, y), re.search(r"\.f(\d+)\.png", f).group(1), fill="black")
sheet.save(out)
