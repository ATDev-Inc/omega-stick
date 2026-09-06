#!/usr/bin/env python3
"""Regenerate the wordmark files.

    python assets/logo/build-wordmark.py

  wordmark.svg       OMEGA STICK, neon gradient
  wordmark-mono.svg  OMEGA STICK, currentColor
  lockup.svg         the mark above the wordmark

The letters are drawn, not set: each one is a centreline of straight lines and
arcs -- circular everywhere but the S, whose bowl has to be an ellipse -- stroked
with round caps and round joins. That round terminal is
the same tube the mark reads as, which is the point -- mark and wordmark come
out the same object, so the lockup looks like one drawing rather than a symbol
sitting next to some type.

Design space is 100 units of cap height, baseline at y = 100, and every
centreline is inset half a stem from the edge of the letter, so the ink lands
on the cap line and the baseline exactly. Change STEM and everything re-solves:
the round letters stay circles, and the sidebearings, which are measured off
the shapes rather than typed in, follow the new silhouettes.
"""
import io, math, os, re

HERE = os.path.dirname(os.path.abspath(__file__))
MARK = os.path.join(HERE, "omega-mono.svg")   # any mark file carries the trace

# --- the dials -------------------------------------------------------------
CAP      = 100.0   # design cap height; everything below is in these units
STEM     = 17.0    # stroke weight
PAD      = 30.0    # margin around the ink in the emitted files
OUT_CAP  = 140.0   # cap height in the emitted wordmark files

# Spacing. Sidebearings are measured, not typed in: each side of a letter gets
# BEARING minus a share of however much that side's silhouette recedes from its
# own extreme, so an open shape sits closer than a flat one without anybody
# having to kern it by eye. REACH is how deep a recess still counts -- past it
# a notch is just background and stops pulling the letter in.
BEARING  = 13.0
RECESS   = 0.50    # share of the mean recess taken back off the bearing
REACH    = 20.0
MINGAP   = 8.0     # never let two letters come closer than this
OVERSHOOT = 1.3    # how far a round or pointed form runs past cap and baseline
WORDSPACE = 33.0   # extra air between OMEGA and STICK, on top of the pair gap

# Lockup: the mark centred over the wordmark. Both of these are in wordmark cap
# heights and both are measured off ink, not off boxes -- the mark files carry a
# margin for their glow, and sizing to that instead would make the mark look
# smaller and sit further away than the numbers say.
LOCK_W    = 760.0
LOCK_PAD  = 30.0
MARK_CAPS = 3.9    # the mark's ink height
LOCK_GAP  = 0.72   # air between the mark's feet and the wordmark's cap line

PALETTE = [("0%", "#FF12E8"), ("24%", "#C022FF"), ("50%", "#7A3BFF"),
           ("76%", "#2A8CFF"), ("100%", "#25D8F2")]

H = STEM / 2.0                          # centreline inset
R = (CAP + 2 * OVERSHOOT - STEM) / 2.0  # a round letter's centreline radius
TOP = H - OVERSHOOT                     # where a round or pointed form peaks
BOT = 100.0 - TOP                       # and where it bottoms out

# A circle that stops exactly on the cap line looks smaller than a flat-topped
# letter beside it, so the round letters are a touch bigger and run past both
# lines -- O, C, G and S by their curve, A and M by their point. Everything
# else sits on the lines proper. R follows from OVERSHOOT, so the round letters
# stay true circles at any weight rather than turning into ellipses.


# --- centreline primitives -------------------------------------------------
# Angles are mathematical: 0 east, 90 north, growing counter-clockwise on
# screen. A point at angle a on an ellipse is (cx + rx cos a, cy - ry sin a).

def line(x1, y1, x2, y2):
    return ("L", (x1, y1, x2, y2))


def arc(cx, cy, r, a0, a1, ry=None):
    return ("A", (cx, cy, r, r if ry is None else ry, a0, a1))


def _f(v):
    s = "%.3f" % v
    s = s.rstrip("0").rstrip(".")
    return s if s not in ("", "-0") else "0"


def emit(prim, scale, ox, oy):
    """One centreline primitive as SVG path data, scaled and translated."""
    def X(v):
        return _f(ox + v * scale)

    def Y(v):
        return _f(oy + v * scale)

    kind, p = prim
    if kind == "L":
        return "M%s %sL%s %s" % (X(p[0]), Y(p[1]), X(p[2]), Y(p[3]))
    cx, cy, rx, ry, a0, a1 = p

    def pt(a):
        return (cx + rx * math.cos(math.radians(a)),
                cy - ry * math.sin(math.radians(a)))

    # SVG sweep-flag 1 sweeps clockwise on screen, i.e. down our angle.
    sweep = 0 if a1 > a0 else 1
    # Split into pieces of at most 180 degrees, so large-arc is never ambiguous
    # and a full circle -- which no single arc command can express -- works.
    n = max(1, int(math.ceil(abs(a1 - a0) / 180.0 - 1e-9)))
    d = "M%s %s" % (X(pt(a0)[0]), Y(pt(a0)[1]))
    for i in range(1, n + 1):
        x, y = pt(a0 + (a1 - a0) * i / n)
        d += "A%s %s 0 0 %d %s %s" % (_f(rx * scale), _f(ry * scale), sweep,
                                      X(x), Y(y))
    return d


# --- the alphabet ----------------------------------------------------------
# A glyph is just its centrelines, drawn in a box whose left edge is x = 0 and
# whose cap height is 100. Nothing here declares a width: every ink box and
# every sidebearing is measured off the shapes further down.

def _a_glyph():
    w, apex, bar = 92.0, 46.0, 68.0
    # Where the crossbar meets each leg, solved rather than eyeballed.
    t = (100.0 - H - bar) / (100.0 - H - TOP)
    lx = H + (apex - H) * t
    return [line(H, 100 - H, apex, TOP), line(apex, TOP, w - H, 100 - H),
            line(lx, bar, w - lx, bar)]


def _s_glyph():
    """The S: two halves walked out from the waist, the lower one a little wider.

    The S is the only letter here that is not plainly circles and lines, and it
    took several attempts, so the dead ends are recorded rather than left to be
    rediscovered.

    A circular bowl does not work. One deep enough to fill the cap height is far
    too narrow to fill the width, so the letter came out as a small bowl top
    left plus a long flat sweep over to the terminal -- radius 16 to radius 54
    across one letter, where the O holds 42.8 all the way round.

    An elliptical bowl with a straight spine tangent to it does not work either,
    and that one is worth understanding because the geometry looks reasonable.
    Fix the spine angle and the tangent point follows, and at any sensible width
    the straight run comes out around 47 units in a letter 64 wide: a dead
    straight bar through the middle. Skeletons pulled off five real faces have
    no straight section at all -- the tangent eases from vertical at the left
    extreme to its shallowest at the waist and back, so curvature falls towards
    zero at the inflection instead of the letter carrying a straight third.

    So each half is walked out from the waist as a chain of tangent arcs whose
    radius tightens into the left extreme and opens back out over the top. Two
    numbers per half are solved rather than chosen -- the length of the short
    run at the waist and the scale of the radii -- from the two things that half
    has to hit: the side it reaches where it turns vertical, and the cap line or
    baseline where it turns horizontal. Both enter the position linearly, so it
    is one 2x2 solve each. Both halves leave the waist along the same heading
    and both start straight, so the tangent matches there and the curvature is
    zero on both sides of the inflection.

    The halves are NOT congruent, and that is the point of WAIST. Rotate one
    half onto the other exactly and the letter measures symmetrical but reads
    top-heavy, which is the usual illusion in S, B, X and 8. Every reference
    measured corrects it: the lower half is wider than the upper by 1.06 in
    Corbel and Verdana, 1.08 in Segoe UI, 1.09 in Trebuchet and 1.16 in Century
    Gothic. Nudging the waist left of centre buys exactly that -- it shortens
    the upper half's reach and lengthens the lower one's -- and nudging it up
    puts the inflection where the references put it, a little above the middle.

    Keep the ramp shallow and the radii large. The counter is the inside of the
    tightest turn, so it pays for a steep ramp: at 2.6 the S closes up into a
    blob with two slits beside a wide open O. Held at 1.3 the tightest radius
    stays near 16, about twice the half-stem, and the counters stay open.
    """
    INK, SPINE = 64.0, 30.0
    BELOW = [(55.0, 1.3), (45.0, 1.0)]   # waist -> left extreme, turn share, radius
    ABOVE = [(45.0, 1.0), (55.0, 1.6)]   # left extreme -> cap line
    TERM_TURN, TERM_REACH = 60.0, 0.80   # the terminal, and how far out it lands
    WAIST = (-1.5, -2.0)                 # left of centre, and above it

    xw = (INK - STEM) / 2.0 + H + WAIST[0]
    yw = 50.0 + WAIST[1]
    rad = lambda d: math.radians(d)
    step = lambda h, t: (math.cos(rad(h + 90)) - math.cos(rad(h + 90 + t)),
                         math.sin(rad(h + 90)) - math.sin(rad(h + 90 + t)))
    h0 = 180.0 + SPINE
    u = (math.cos(rad(h0)), math.sin(rad(h0)))
    share = lambda seq, total: [(t / sum(x[0] for x in seq) * total, m) for t, m in seq]
    below, above = share(BELOW, 90.0 - SPINE), share(ABOVE, 90.0)

    def walk(seq, h):
        acc = (0.0, 0.0)
        for t, mul in seq:
            d = step(h, t)
            acc = (acc[0] + mul * d[0], acc[1] + mul * d[1])
            h += t
        return acc, h

    def outward(tx, ty):
        """One half, from the waist to the side at tx and the line at ty."""
        cb, h270 = walk(below, h0)      # to where the curve runs vertical
        ca, _ = walk(above, h270)       # on to where it runs horizontal
        # [L, k] solves: x at the vertical = tx, y at the horizontal = ty
        det = u[0] * (cb[1] + ca[1]) - cb[0] * u[1]
        L = ((tx - xw) * (cb[1] + ca[1]) - cb[0] * (ty - yw)) / det
        k = (u[0] * (ty - yw) - (tx - xw) * u[1]) / det
        p, h = (xw + L * u[0], yw + L * u[1]), h0
        out = [line(xw, yw, p[0], p[1])]

        def turn(t, r):
            nonlocal p, h
            c = (p[0] + r * math.cos(rad(h + 90)), p[1] + r * math.sin(rad(h + 90)))
            out.append(arc(c[0], c[1], r, 90.0 - h, 90.0 - h - t))
            p = (c[0] + r * math.cos(rad(h + t - 90)),
                 c[1] + r * math.sin(rad(h + t - 90)))
            h += t

        for t, mul in below + above:
            turn(t, k * mul)
        # The terminal's radius is whatever carries it TERM_REACH of the way back.
        turn(TERM_TURN, (xw + TERM_REACH * (xw - tx) - p[0]) / math.sin(rad(TERM_TURN)))
        return out

    prims = outward(H, TOP)
    for kind, q in outward(2 * xw - (INK - H), 2 * yw - BOT):
        prims.append(line(2 * xw - q[0], 2 * yw - q[1], 2 * xw - q[2], 2 * yw - q[3])
                     if kind == "L" else
                     arc(2 * xw - q[0], 2 * yw - q[1], q[2], q[4] + 180.0, q[5] + 180.0))
    return prims


def _k_glyph():
    w, j = 76.0, 52.0       # j is where arm and leg meet the stem
    return [line(H, H, H, 100 - H), line(H, j, w - H, H),
            line(H, j, w - H, 100 - H)]


GLYPHS = {
    "O": [arc(50, 50, R, 0, 360)],
    "M": [line(H, 100 - H, H, H), line(H, H, 52, BOT),
          line(52, BOT, 104 - H, H), line(104 - H, H, 104 - H, 100 - H)],
    "E": [line(H, H, H, 100 - H), line(H, H, 76 - H, H),
          line(H, 50, 76 - H - 5, 50), line(H, 100 - H, 76 - H, 100 - H)],
    "G": [arc(50, 50, R, 52, 360), line(52, 50, 50 + R, 50)],
    "A": _a_glyph(),
    "S": _s_glyph(),
    "T": [line(H, H, 76 - H, H), line(38, H, 38, 100 - H)],
    "I": [line(H, H, H, 100 - H)],
    "C": [arc(50, 50, R, 50, 310)],
    "K": _k_glyph(),
}

def shift(prim, dx):
    kind, p = prim
    if kind == "A":
        return ("A", (p[0] + dx,) + p[1:])
    return ("L", (p[0] + dx, p[1], p[2] + dx, p[3]))


# --- measured spacing ------------------------------------------------------
# The stroke is a disk of radius H swept along the centreline, so the ink's
# silhouette follows from sampling the centreline densely and asking, row by
# row, how far left and right the swept disks reach. Nothing here is rasterised.

ROWS = 400   # scanlines across a glyph's own ink height


def _samples(prims, step=0.25):
    """Points along every centreline in a glyph, roughly `step` units apart."""
    pts = []
    for kind, p in prims:
        if kind == "L":
            x1, y1, x2, y2 = p
            n = max(2, int(math.hypot(x2 - x1, y2 - y1) / step))
            pts += [(x1 + (x2 - x1) * i / n, y1 + (y2 - y1) * i / n)
                    for i in range(n + 1)]
        else:
            cx, cy, rx, ry, a0, a1 = p
            n = max(4, int(abs(a1 - a0) / 360.0 * 2 * math.pi * max(rx, ry) / step))
            for i in range(n + 1):
                a = math.radians(a0 + (a1 - a0) * i / n)
                pts.append((cx + rx * math.cos(a), cy - ry * math.sin(a)))
    return pts


def profile(prims):
    """Per-row leftmost and rightmost ink, over the glyph's own ink height.

    Rows are spread across what the glyph actually covers rather than across
    the cap height, so a round letter's overshoot is measured like the rest of
    it instead of being clipped off at the cap line.
    """
    pts = _samples(prims)
    y0 = min(q[1] for q in pts) - H
    y1 = max(q[1] for q in pts) + H
    left = [None] * ROWS
    right = [None] * ROWS
    for px, py in pts:
        lo = max(0, int(math.ceil((py - H - y0) / (y1 - y0) * (ROWS - 1))))
        hi = min(ROWS - 1, int((py + H - y0) / (y1 - y0) * (ROWS - 1)))
        for r in range(lo, hi + 1):
            dy = y0 + r / (ROWS - 1) * (y1 - y0) - py
            dx = math.sqrt(max(0.0, H * H - dy * dy))
            a, b = px - dx, px + dx
            if left[r] is None or a < left[r]:
                left[r] = a
            if right[r] is None or b > right[r]:
                right[r] = b
    return left, right, y0, y1


def bearings(prims):
    """(left bearing, right bearing, ink left, ink right) for one glyph."""
    left, right, _, _ = profile(prims)
    x0 = min(v for v in left if v is not None)
    x1 = max(v for v in right if v is not None)
    recess = lambda vals, edge, sign: sum(
        REACH if v is None else min(REACH, max(0.0, sign * (v - edge)))
        for v in vals) / len(vals)
    return (BEARING - RECESS * recess(left, x0, 1),
            BEARING - RECESS * recess(right, x1, -1), x0, x1)


METRICS = {ch: bearings(g) for ch, g in GLYPHS.items()}


def layout(text):
    """Place the glyphs. Returns (primitives, ink width)."""
    prims, pen, prev, space = [], 0.0, None, False
    for ch in text:
        if ch == " ":
            space = True
            continue
        lsb, _, x0, x1 = METRICS[ch]
        if prev is not None:
            pen += max(MINGAP, METRICS[prev][1] + lsb) + (WORDSPACE if space else 0)
        space = False
        prims += [shift(g, pen - x0) for g in GLYPHS[ch]]
        pen += x1 - x0
        prev = ch
    return prims, pen


def gradient(gid, x1, x2):
    stops = "".join('<stop offset="%s" stop-color="%s"/>' % s for s in PALETTE)
    return ('<linearGradient id="%s" gradientUnits="userSpaceOnUse" x1="%s" '
            'y1="0" x2="%s" y2="0">%s</linearGradient>' % (gid, _f(x1), _f(x2), stops))


def stroked(d, paint, scale):
    return ('<path d="%s" fill="none" stroke="%s" stroke-width="%s" '
            'stroke-linecap="round" stroke-linejoin="round"/>'
            % (d, paint, _f(STEM * scale)))


def mark_source():
    """The traced outline and its true ink box, read from the mark files.

    The shared viewBox is the outline plus a margin for the glow, so it is the
    wrong box to lay out against; this samples the curves for the real one.
    """
    svg = io.open(MARK, encoding="utf-8").read()
    d = re.search(r'\sd="(M[^"]+)"', svg).group(1)
    tok = re.findall(r"[MCZ]|-?\d+\.?\d*", d)
    pts, i, cur, start = [], 0, (0.0, 0.0), (0.0, 0.0)
    while i < len(tok):
        if tok[i] == "M":
            cur = start = (float(tok[i + 1]), float(tok[i + 2]))
            pts.append(cur); i += 3
        elif tok[i] == "C":
            c = [float(v) for v in tok[i + 1:i + 7]]
            for k in range(1, 17):          # control points overstate the hull
                u, v = k / 16.0, 1 - k / 16.0
                pts.append((v**3 * cur[0] + 3*v*v*u*c[0] + 3*v*u*u*c[2] + u**3 * c[4],
                            v**3 * cur[1] + 3*v*v*u*c[1] + 3*v*u*u*c[3] + u**3 * c[5]))
            cur = (c[4], c[5]); i += 7
        else:
            cur = start; i += 1
    xs, ys = [q[0] for q in pts], [q[1] for q in pts]
    return d, (min(xs), min(ys), max(xs) - min(xs), max(ys) - min(ys))


def build_lockup(text="OMEGA STICK"):
    prims, ink = layout(text)
    d_mark, (vx, vy, vw, vh) = mark_source()
    content = LOCK_W - 2 * LOCK_PAD
    sw = content / ink                       # wordmark: design units -> lockup units
    cap = CAP * sw
    sm = MARK_CAPS * cap / vh                # mark: its own units -> lockup units
    mw = vw * sm
    _, _, _, y1 = profile(prims)
    top = LOCK_PAD + MARK_CAPS * cap + LOCK_GAP * cap   # the wordmark's cap line
    height = top + (y1 - CAP) * sw + cap + LOCK_PAD
    d_word = "".join(emit(p, sw, LOCK_PAD, top) for p in prims)
    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %s %s" width="%s" '
            'height="%s" role="img"><defs>%s%s'
            '<filter id="g" x="-35%%" y="-35%%" width="170%%" height="170%%">'
            '<feGaussianBlur stdDeviation="13" result="b"/><feMerge>'
            '<feMergeNode in="b"/><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/>'
            '</feMerge></filter></defs>'
            '<g filter="url(#g)" transform="translate(%s,%s) scale(%s)">'
            '<path d="%s" fill="url(#n)" fill-rule="evenodd"/>'
            '<path d="%s" fill="none" fill-rule="evenodd" stroke="#fff" stroke-width="11.0" '
            'stroke-linejoin="round" opacity=".9"/></g>%s</svg>\n'
            % (_f(LOCK_W), _f(height), _f(LOCK_W), _f(height),
               gradient("n", 50, 954), gradient("nw", LOCK_PAD, LOCK_W - LOCK_PAD),
               _f((LOCK_W - mw) / 2 - vx * sm), _f(LOCK_PAD - vy * sm), _f(sm),
               d_mark, d_mark, stroked(d_word, "url(#nw)", sw)))


def build_wordmark(text="OMEGA STICK"):
    prims, ink = layout(text)
    s = OUT_CAP / CAP
    # The round letters run past the cap line and the baseline, so pad the ink
    # rather than the cap height or the margin comes out short top and bottom.
    _, _, y0, y1 = profile(prims)
    w, h = ink * s + 2 * PAD, (y1 - y0) * s + 2 * PAD
    d = "".join(emit(p, s, PAD, PAD - y0 * s) for p in prims)
    head = ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %s %s" '
            'width="%s" height="%s" role="img">'
            % (_f(w), _f(h), _f(w), _f(h)))
    colour = (head + "<defs>" + gradient("n", PAD, w - PAD) + "</defs>"
              + stroked(d, "url(#n)", s) + "</svg>\n")
    mono = head + stroked(d, "currentColor", s) + "</svg>\n"
    return colour, mono, ink


if __name__ == "__main__":
    colour, mono, ink = build_wordmark()
    for name, svg in (("wordmark.svg", colour), ("wordmark-mono.svg", mono),
                      ("lockup.svg", build_lockup())):
        # LF, like every other text file in this tree -- see .gitattributes
        io.open(os.path.join(HERE, name), "w", encoding="utf-8",
                newline="\n").write(svg)
        print("%-18s %5d bytes" % (name, len(svg)))
    print("wordmark ink %.1f x %.1f units at stem %.1f" % (ink, CAP, STEM))
