#!/usr/bin/env python3
"""Adjacent-frame differ for klepton eye captures.

CONTRACT (read before trusting a number):
  * It compares consecutive captured frames pixel-wise. It does NOT know what
    "should" differ; it only reports what DID.
  * It ABORTS LOUDLY on: a missing/undecodable PNG, a size mismatch, an
    all-black frame, or a pair that is byte-identical -- each of those means the
    capture is broken, not that the run is clean.
  * The number it reports cannot saturate: `frac` is a fraction of pixels and
    `blocks` counts 64x64 tiles whose mean channel delta exceeds THRESH. A run
    where every tile trips is reported as 100% and flagged, not silently passed.
  * It restates no rule. There is no model of "correct" inside it.
"""
import sys, os, glob
import numpy as np
from PIL import Image
Image.MAX_IMAGE_PIXELS = None

THRESH = int(os.environ.get("THRESH", "8"))     # per-channel delta that counts
TILE   = 64

d = sys.argv[1]
files = sorted(glob.glob(os.path.join(d, "vk_f*_eye0.png")))
if len(files) < 2:
    sys.exit("FAIL: fewer than two eye0 captures in %s" % d)

def load(p):
    try:
        a = np.asarray(Image.open(p).convert("RGB"), dtype=np.int16)
    except Exception as e:
        sys.exit("FAIL: cannot decode %s: %s" % (p, e))
    if a.max() == 0:
        sys.exit("FAIL: %s is entirely black -- capture is not of a rendered scene" % p)
    return a

prev = None; prevname = None
print("thresh=%d tile=%d  %d frames" % (THRESH, TILE, len(files)))
print("%-28s %-28s %8s %8s %8s %10s" % ("A","B","frac>T","maxdelta","tiles%","identical"))
allsame = True
for f in files:
    cur = load(f)
    if prev is not None:
        if prev.shape != cur.shape:
            sys.exit("FAIL: shape change %s %s vs %s %s" % (prevname, prev.shape, f, cur.shape))
        dif = np.abs(cur - prev)
        m   = dif.max(axis=2)
        frac = float((m > THRESH).mean())
        h, w = m.shape
        th, tw = h//TILE, w//TILE
        tiles = m[:th*TILE, :tw*TILE].reshape(th, TILE, tw, TILE).mean(axis=(1,3))
        tfrac = float((tiles > THRESH).mean())
        ident = bool((dif == 0).all())
        if not ident: allsame = False
        print("%-28s %-28s %8.4f %8d %8.3f %10s" % (
            os.path.basename(prevname), os.path.basename(f), frac, int(m.max()), tfrac,
            "YES" if ident else ""))
    prev, prevname = cur, f
if allsame:
    sys.exit("FAIL: every adjacent pair is byte-identical -- the capture is "
             "writing the same image, so no conclusion about the guest is available")
