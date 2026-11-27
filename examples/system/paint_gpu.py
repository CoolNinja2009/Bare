"""paint_gpu.py: paints an RTX 5090-style card (Founders-like: gunmetal body,
polished frame, two ribbed fin panels split by a silver X, a glowing
GEFORCE RTX edge) over the graphics card in the Simple System picture.

    python paint_gpu.py <pcmr.png from the page> assets/pcmr.png

Drawn from shapes here (no photo), at 4x, then scaled down into the slot the
old card took (1920x1080 picture: x 700..1128, y 597..672)."""
import sys
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

S = 4                                   # supersampling
X0, Y0, X1, Y1 = 694, 588, 1134, 690    # the patch we redraw (picture pixels)
W, H = (X1 - X0) * S, (Y1 - Y0) * S

def P(x, y):                            # picture pixels -> patch pixels at 4x
    return ((x - X0) * S, (y - Y0) * S)

src = Image.open(sys.argv[1]).convert('RGBA')
patch = src.crop((X0, Y0, X1, Y1)).resize((W, H), Image.LANCZOS)

# card geometry (picture pixels)
cx0, cy0, cx1, cy1 = 704, 597, 1128, 673   # whole card
face_y1 = 649                               # face above, edge below

def lin(a, b, n):
    return np.linspace(a, b, n)[:, None]

def rounded(box, r, fill, img=None):
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([*P(*box[:2]), *P(*box[2:])], radius=r * S, fill=fill)

layer = Image.new('RGBA', (W, H), (0, 0, 0, 0))

# soft shadow on the board under the card
sh = Image.new('L', (W, H), 0)
ImageDraw.Draw(sh).rounded_rectangle([*P(cx0 + 4, cy0 + 8), *P(cx1 + 6, cy1 + 9)], radius=10 * S, fill=200)
sh = sh.filter(ImageFilter.GaussianBlur(7 * S))
shadow = Image.new('RGBA', (W, H), (0, 0, 0, 0))
shadow.putalpha(sh)
layer = Image.alpha_composite(layer, shadow)

# polished frame: a vertical metal gradient (bright rim on top, dark under)
mask = Image.new('L', (W, H), 0)
ImageDraw.Draw(mask).rounded_rectangle([*P(cx0, cy0), *P(cx1, cy1)], radius=9 * S, fill=255)
ys = np.arange(H) / S + Y0
t = np.clip((ys - cy0) / (cy1 - cy0), 0, 1)
metal = np.interp(t, [0, .04, .10, .5, .62, .66, .95, 1], [120, 178, 100, 78, 60, 104, 52, 30])
frame = np.zeros((H, W, 4), np.uint8)
frame[..., 0] = metal[:, None] * 0.97
frame[..., 1] = metal[:, None] * 0.98
frame[..., 2] = metal[:, None]
frame[..., 3] = 255
xs = np.arange(W) / S + X0               # light falls off towards the left end
shade = np.clip(0.78 + 0.22 * (xs - cx0) / (cx1 - cx0), 0, 1)
frame[..., :3] = (frame[..., :3] * shade[None, :, None]).astype(np.uint8)
fr = Image.fromarray(frame)
fr.putalpha(mask)
layer = Image.alpha_composite(layer, fr)

d = ImageDraw.Draw(layer)
# the face: inset dark plate
inset = (cx0 + 5, cy0 + 4, cx1 - 5, face_y1 - 2)
d.rounded_rectangle([*P(*inset[:2]), *P(*inset[2:])], radius=6 * S, fill=(30, 31, 33, 255))

# two fin panels, ribbed (fine vertical lines), each tapering to a point at the
# middle; the polished bands between the points make the X, and the triangles
# above and below the crossing are the dark centre plate
fl0, fr1 = cx0 + 10, cx1 - 10
fy0, fy1 = cy0 + 7, face_y1 - 5
my = (fy0 + fy1) / 2
mid = 918
reach, tip = 62, 14            # where a panel starts to taper, how far its point stops from the middle

def panel_poly(side):
    if side < 0:
        return [(fl0, fy0), (mid - reach, fy0), (mid - tip, my), (mid - reach, fy1), (fl0, fy1)]
    return [(fr1, fy0), (mid + reach, fy0), (mid + tip, my), (mid + reach, fy1), (fr1, fy1)]

def fins(poly, slant):
    yy, xx = np.mgrid[0:H, 0:W]
    rib = 0.5 + 0.5 * np.cos((xx + yy * slant) / (1.1 * S) * np.pi)   # ~2.2 px pitch
    v = (yy / S + Y0 - fy0) / (fy1 - fy0)
    base = 20 + 24 * (1 - np.abs(v - 0.35) * 1.6).clip(0, 1)
    val = (base * (0.55 + 0.45 * rib)).astype(np.uint8)
    a = np.dstack([val, val, (val * 1.04).clip(0, 255).astype(np.uint8), np.full_like(val, 255)])
    m = Image.new('L', (W, H), 0)
    ImageDraw.Draw(m).polygon([P(x, y) for x, y in poly], fill=255)
    img = Image.fromarray(a)
    img.putalpha(m)
    layer.alpha_composite(img)

# the plate and the X's bands first, the panels over them
d.rectangle([*P(mid - reach - 4, fy0), *P(mid + reach + 4, fy1)], fill=(42, 43, 46, 255))
band = 4.2
for sgn in (-1, 1):
    a = (mid - reach - 3, fy0 if sgn < 0 else fy1)
    b = (mid + reach + 3, fy1 if sgn < 0 else fy0)
    d.line([*P(*a), *P(*b)], fill=(150, 152, 156, 255), width=int(band * 2 * S))
    d.line([*P(a[0], a[1] - 1.2), *P(b[0], b[1] - 1.2)], fill=(205, 207, 211, 255), width=int(1.2 * S))
try:
    small = ImageFont.truetype('C:/Windows/Fonts/bahnschrift.ttf', 6 * S)
except OSError:
    small = ImageFont.load_default()
lw = d.textlength('RTX 5090', font=small)
d.text((P(mid, fy0 + 1.5)[0] - lw / 2, P(mid, fy0 + 1.5)[1]), 'RTX 5090', font=small, fill=(165, 167, 171, 255))
fins(panel_poly(-1), 0.25)
fins(panel_poly(1), -0.25)
for side in (-1, 1):   # a thin polished lip round each panel
    pts = [P(x, y) for x, y in panel_poly(side)]
    d.line(pts + [pts[0]], fill=(128, 130, 134, 255), width=S, joint='curve')

# the edge below the face: darker band, the glowing GEFORCE RTX on it
d.rounded_rectangle([*P(cx0 + 2, face_y1 + 1), *P(cx1 - 2, cy1 - 2)], radius=6 * S, fill=(40, 41, 44, 255))
d.line([*P(cx0 + 8, face_y1 + 1), *P(cx1 - 8, face_y1 + 1)], fill=(190, 192, 196, 255), width=S)   # rim highlight
try:
    big = ImageFont.truetype('C:/Windows/Fonts/bahnschrift.ttf', 15 * S)
    big.set_variation_by_name('Bold')
except Exception:
    big = ImageFont.truetype('C:/Windows/Fonts/arialbd.ttf', 14 * S)
text = 'GEFORCE RTX'
tw = d.textlength(text, font=big)
tx, ty = P(cx1 - 18, face_y1 + 3.5)
tx -= tw
glow = Image.new('RGBA', (W, H), (0, 0, 0, 0))
ImageDraw.Draw(glow).text((tx, ty), text, font=big, fill=(235, 245, 255, 255))
blur = glow.filter(ImageFilter.GaussianBlur(4 * S))
bl = np.asarray(blur).copy(); bl[..., 3] = (bl[..., 3] * 0.9).astype(np.uint8)
layer = Image.alpha_composite(layer, Image.fromarray(bl))
layer = Image.alpha_composite(layer, glow)

out = Image.alpha_composite(patch, layer).resize((X1 - X0, Y1 - Y0), Image.LANCZOS)
pic = src.copy()
pic.paste(out, (X0, Y0))
# keep the PCIe bracket at the left end from the original picture
pic.paste(src.crop((680, 594, 704, 676)), (680, 594))
pic.save(sys.argv[2], optimize=True)
