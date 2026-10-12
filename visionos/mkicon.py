#!/usr/bin/env python3
"""Build the visionOS layered app icon from a flat source image.

visionOS does not take a single square PNG. An app icon is a
`.solidimagestack` of exactly three 1024x1024 layers — Front, Middle, Back —
which the system parallaxes against each other as your gaze moves and lights
with its own specular pass. The Home View then masks the whole stack to a
CIRCLE, so anything outside the inscribed disc is not "cropped a little", it is
gone.

That shape is what this script exists to produce, because the useful art we
have is one flat 1024x1024 with an alpha channel:

  Back    solid, opaque, edge-to-edge. It is the only layer that fills the
          circle, so it is what the icon's colour actually is. Per-target,
          from targets.py's `iconback` — black where the brief says black,
          SUPERHOT white where the art is the red key figure. Opaque and
          full-bleed is not decoration — a transparent or inset back layer
          leaves the mask showing through and the icon reads as a floating
          fragment rather than a disc.
  Middle  the bloom, blurred and slightly enlarged. It carries no detail; its
          whole job is to sit at a different parallax depth from the Front so
          the glow slides against the lettering instead of moving with it.
          Without it the icon still works and simply looks flatter.
  Front   the crisp mark, inset to the safe box below.

The inset is the part that is easy to get wrong. A square icon's corners are
outside the circle, so content is fitted into a box that is itself inside the
inscribed disc: half-diagonal sqrt((W/2)^2 + (H/2)^2) must stay under the
radius, with margin left for the system's own shadow at the rim. SAFE_W/SAFE_H
below are chosen against that, not by eye.

Trimming happens on ALPHA_FLOOR rather than alpha > 0 for the same reason the
box is conservative: this artwork is neon, so its faintest halo reaches nearly
to the source edges and a literal bbox would be the whole canvas — scaling to
that fits the halo and leaves the lettering tiny.

    ./mkicon.py [source.ico|source.png] [-o Assets.xcassets] [--back r,g,b]

The target (KLEPTON_TARGET, beatsaber when unset) supplies the source art and
the back colour from targets.py's `icon`/`iconback`; both CLI overrides exist
for trying an artwork without editing the table. Output is Assets.xcassets,
which gen_xcodeproj.py compiles in as AppIcon.
"""
import json, os, sys
from PIL import Image, ImageFilter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import targets as targets_mod

HERE = os.path.dirname(os.path.abspath(__file__))
SIZE = 1024                  # every layer, exactly; the stack is not scalable
SAFE_W, SAFE_H = 780, 560    # content box, inside the circular mask
ALPHA_FLOOR = 24             # below this is halo, not mark — see module docstring
SPECK = 3                    # opaque px below which a row/column is debris
SPECK_RATIO = 0.003          # ...or this fraction of the line, whichever is more
BLUR = 26                    # Middle's bloom radius
BLOOM_SCALE = 1.06           # Middle drawn slightly larger than Front
BLOOM_ALPHA = 0.85

INFO = {"author": "xcode", "version": 1}


def load_flat(path):
    """The source as RGBA at native size. An .ico holds several sizes; Pillow
    opens the largest by asking for it explicitly, which plain open() does not
    do — it yields whichever entry is first, and that is usually the 16x16."""
    img = Image.open(path)
    if getattr(img, "format", None) == "ICO":
        largest = max(img.ico.sizes())
        img = img.ico.getimage(largest)
    return img.convert("RGBA")


def content_bbox(alpha):
    """The mark's box, ignoring rows and columns that hold only specks.

    `Image.getbbox()` is a literal extent: ONE surviving pixel in a corner is
    as load-bearing as the whole logo. That is not hypothetical — the deadpool
    art is a mask on a full-bleed yellow field with the yellow keyed to alpha,
    and keying leaves a handful of stragglers where the field's colour drifted
    (an anti-aliased edge, compression ringing). Its real mark spans 156..926,
    five single pixels sit at rows 1/1079 and columns 110/1072/1078, and the
    literal bbox is therefore 969x1079 — so `fit_centred` scaled by 0.52 where
    the mark itself wanted 0.72, and the logo landed at 39% of the canvas
    instead of 55%. It reads as "the icon art is too small"; the art is fine.

    A line is content if it carries at least `SPECK` opaque pixels.
    Deliberately tiny: it must drop 1-2 pixel debris without eating a real
    mark's extremes, where a pointed or thin-stroked logo legitimately has very
    few pixels. The floor keeps it meaningful on small sources; the ratio keeps
    it from scaling into a real feature on large ones.
    """
    w, h = alpha.size
    px = alpha.load()
    speck_row = max(SPECK, round(w * SPECK_RATIO))
    speck_col = max(SPECK, round(h * SPECK_RATIO))
    rows = [y for y in range(h)
            if sum(1 for x in range(w) if px[x, y] >= ALPHA_FLOOR) >= speck_row]
    cols = [x for x in range(w)
            if sum(1 for y in range(h) if px[x, y] >= ALPHA_FLOOR) >= speck_col]
    if not rows or not cols:
        # Everything is speck-thin. A hairline logo is still a logo, so fall
        # back to the literal extent rather than cropping to nothing.
        return alpha.point(lambda a: 255 if a >= ALPHA_FLOOR else 0).getbbox()
    return (cols[0], rows[0], cols[-1] + 1, rows[-1] + 1)


def fit_centred(src, box_w, box_h, scale=1.0):
    """Trim to the mark, scale to fit the safe box, centre on a SIZE canvas."""
    alpha = src.getchannel("A")
    bbox = content_bbox(alpha)
    mark = src.crop(bbox) if bbox else src

    f = min(box_w / mark.width, box_h / mark.height) * scale
    mark = mark.resize((max(1, round(mark.width * f)),
                        max(1, round(mark.height * f))), Image.LANCZOS)

    canvas = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    canvas.paste(mark, ((SIZE - mark.width) // 2, (SIZE - mark.height) // 2))
    return canvas


def layers(src, back_rgb):
    front = fit_centred(src, SAFE_W, SAFE_H)

    # The bloom is built from the already-fitted Front so the two stay
    # registered: blurring the source and fitting afterwards would trim to a
    # different bbox and the glow would sit off-centre from the lettering.
    middle = fit_centred(src, SAFE_W, SAFE_H, scale=BLOOM_SCALE)
    middle = middle.filter(ImageFilter.GaussianBlur(BLUR))
    a = middle.getchannel("A").point(lambda v: int(v * BLOOM_ALPHA))
    middle.putalpha(a)

    # RGB, not RGBA: the back layer is the icon's ground and must be opaque.
    # `back_rgb` is either a colour triple or an already-loaded RGB image (see
    # load_back) — a flat ground is the common case, but an artwork whose
    # design IS its ground (Simpsons Hit & Run's blue gradient) reads as a
    # badge where a single averaged blue reads as a sticker.
    back = (back_rgb if isinstance(back_rgb, Image.Image)
            else Image.new("RGB", (SIZE, SIZE), back_rgb))
    return {"Front": front, "Middle": middle, "Back": back}


def write_stack(out_dir, imgs):
    stack = os.path.join(out_dir, "AppIcon.solidimagestack")
    os.makedirs(stack, exist_ok=True)
    # Order is front-to-back and is load-bearing: the catalog compiler assigns
    # parallax depth by position, not by the layer's name.
    write_json(os.path.join(stack, "Contents.json"),
               {"info": INFO,
                "layers": [{"filename": f"{n}.solidimagestacklayer"}
                           for n in ("Front", "Middle", "Back")]})
    for name, img in imgs.items():
        layer = os.path.join(stack, f"{name}.solidimagestacklayer")
        content = os.path.join(layer, "Content.imageset")
        os.makedirs(content, exist_ok=True)
        write_json(os.path.join(layer, "Contents.json"), {"info": INFO})
        png = f"{name}.png"
        img.save(os.path.join(content, png))
        write_json(os.path.join(content, "Contents.json"),
                   {"images": [{"filename": png, "idiom": "vision", "scale": "2x"}],
                    "info": INFO})
    write_json(os.path.join(out_dir, "Contents.json"), {"info": INFO})
    return stack


def write_json(path, obj):
    with open(path, "w") as f:
        json.dump(obj, f, indent=2)
        f.write("\n")


def load_back(text, base_dir):
    """The back layer, from targets.py's `iconback`: either "r,g,b" or a path to
    an image, relative to the repo root.

    An image ground is COVER-fitted — scaled until it covers the square, then
    centre-cropped — because the back layer is a ground and must not letterbox.
    A vertical gradient authored taller than it is wide therefore keeps its
    middle band; supply the ground already square if the whole range matters
    (icons/simpsons-back.png is that, generated from the poster art's own
    gradient column so the poster's logo is not drawn twice — the mark is the
    FRONT layer's job)."""
    if "," in text:
        return parse_rgb(text)
    path = text if os.path.isabs(text) else os.path.join(base_dir, text)
    if not os.path.exists(path):
        print(f"!! no such back-layer image: {path}", file=sys.stderr)
        sys.exit(1)
    img = Image.open(path).convert("RGB")
    f = max(SIZE / img.width, SIZE / img.height)
    img = img.resize((max(1, round(img.width * f)), max(1, round(img.height * f))),
                     Image.LANCZOS)
    left = (img.width - SIZE) // 2
    top = (img.height - SIZE) // 2
    return img.crop((left, top, left + SIZE, top + SIZE))


def parse_rgb(text):
    try:
        r, g, b = (int(c) for c in text.split(","))
        if not (0 <= r <= 255 and 0 <= g <= 255 and 0 <= b <= 255):
            raise ValueError
        return (r, g, b)
    except ValueError:
        print(f"!! bad back colour {text!r} — want 'r,g,b' 0..255", file=sys.stderr)
        sys.exit(1)


def main(argv):
    # The target supplies both halves of the art direction (targets.py carries
    # them next to display/bundle); the overrides are for trying an artwork
    # without editing the table.
    klt = targets_mod.resolve(os.environ.get("KLEPTON_TARGET") or targets_mod.DEFAULT)
    # A row with no art is not an error and must not be a traceback: run.sh
    # calls this in step 1/5 under `set -o pipefail`, so `KeyError: 'icon'`
    # killed the whole build — which is how tetris's first device build died,
    # 40 s of runtime work in and nothing to do with the device. Say what is
    # missing, leave whatever Assets.xcassets already holds, and let the build
    # go on; an override argument still works on a row that states nothing.
    src_path = os.path.join(HERE, "..", klt["icon"]) if klt.get("icon") else None
    root = os.path.join(HERE, "..")
    back = load_back(klt.get("iconback") or "0,0,0", root)
    out_dir = os.path.join(HERE, "Assets.xcassets")
    args = [a for a in argv[1:]]
    if "-o" in args:
        i = args.index("-o")
        out_dir = args[i + 1]
        del args[i:i + 2]
    if "--back" in args:
        i = args.index("--back")
        back = load_back(args[i + 1], root)
        del args[i:i + 2]
    if args:
        src_path = args[0]

    if src_path is None:
        print(f"[mkicon] {klt['name']}: no `icon` in targets.py — keeping "
              f"whatever Assets.xcassets already holds")
        return 0

    if not os.path.exists(src_path):
        # The art lives in the gitignored icons/ directory (targets.py's `icon`),
        # so a checkout without it is normal: keep the stack already there.
        print(f"[mkicon] {klt['name']}: no {src_path} — keeping whatever "
              f"Assets.xcassets already holds")
        return 0

    stack = write_stack(out_dir, layers(load_flat(src_path), back))
    print(f"[mkicon] {klt['name']}: {os.path.basename(src_path)} -> {stack}")
    shown = (f"{klt.get('iconback')} (image ground)"
             if isinstance(back, Image.Image) else back)
    print(f"          Back {shown}, Middle blur {BLUR}, Front fit {SAFE_W}x{SAFE_H}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
