# Numeric read of the PVS-residency rig frames: mean linear-ish RGB over a
# region, plus a green-excess metric (G - (R+B)/2). Used to state the A/B in
# numbers as well as by eye.
import sys
from PIL import Image

def stat(path, box=None):
    im = Image.open(path).convert('RGB')
    if box: im = im.crop(box)
    px = im.load(); w, h = im.size
    n = 0; r = g = b = 0
    for y in range(0, h, 3):
        for x in range(0, w, 3):
            p = px[x, y]; r += p[0]; g += p[1]; b += p[2]; n += 1
    r /= n; g /= n; b /= n
    return r, g, b, g - (r + b) / 2.0

for p in sys.argv[1:]:
    r, g, b, ge = stat(p)
    print(f"{p:52s} mean RGB {r:6.1f} {g:6.1f} {b:6.1f}   green-excess {ge:+6.2f}")
