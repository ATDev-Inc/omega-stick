#!/usr/bin/env python3
"""Regenerate the three generated marks in this folder.

    python assets/logo/build-scene.py

  scene.svg       the full scene: mark on the perspective grid
  scene-mark.svg  the same canvas and sky with the whole floor removed
  omega-neon.svg  the mark on its own, transparent, drop-in for omega.svg

scene.svg is the static twin of scene.html: same camera, same framing, same
palette, no animation. The two drifted apart once before, so the geometry is
generated from the constants below rather than drawn by hand -- keep these in
step with the custom properties at the top of scene.html and the two files stay
in step with each other.

The mark is the traced outline, shared byte for byte with omega.svg and the
other marks in this folder and read from omega-mono.svg at build time, so the
trace lives in exactly one place. scene.html deliberately uses a *constructed*
outline instead; the two differ slightly around the feet, which is expected and
documented in README.md.

In the two scene files one unit is 0.1cqw: the 1000x1000 viewBox is the square
scene box, so a scene.html value in cqw or cqh multiplies by 10. omega-neon.svg
instead keeps the outline's own coordinates and the viewBox the other mark files
use, so it drops into a layout exactly where omega.svg would.
"""
import io, math, os, random, re

HERE = os.path.dirname(os.path.abspath(__file__))
SRC  = os.path.join(HERE, "omega-mono.svg")   # any mark file carries the trace

# --- constants, mirroring the custom properties in scene.html ---------------
HZ     = 380.0   # --horizon 38cqh, measured up from the bottom edge
P      = 658.0   # --persp 65.8cqh; must stay HZ * tan(--tilt)
TILT   = 60.0    # --tilt
CELL_X = 120.0   # --cell-x 12cqw, vertical-line pitch at the near edge
CELL_Z = 260.0   # --cell-z 26cqh, depth pitch in plane space
NX     = 20      # --span 480cqw = 40 cells, so +-20 either side of centre
NZ     = 17      # --depth 442cqh = 17 cells
SPAN_H = 2400.0  # --span / 2
LW_X   = 2.2     # --lw-x 0.22cqw
LW_Z   = 4.4     # --lw-z 0.44cqw, plane space
MARK_W = 384.0   # --mark-w 38.4cqw
BASE_Y = 1000 - 315.0   # --base 31.5cqh, where the feet meet the floor

# The mark files all share this viewBox: the outline's bbox with ~26.5 units of
# padding. Matching it is what makes omega-neon.svg swap for omega.svg without
# anything moving, so the glow below is sized to stay inside that margin.
MARK_VIEWBOX = "24.3 89.8 955.4 823.0"
MARK_VB_W, MARK_VB_H = 955, 823

SIN, COS = math.sin(math.radians(TILT)), math.cos(math.radians(TILT))
HZ_Y = 1000 - HZ

def u_of(d):  return d * SIN / (P + d * SIN)   # plane depth -> fraction of horizon
def y_of(u):  return 1000 - HZ * u

U_FAR = u_of(CELL_Z * NZ)
K_FAR = 1 - U_FAR


def traced_path():
    """The outline, plus a true bbox sampled off the curves."""
    svg = io.open(SRC, encoding="utf-8").read()
    m = re.search(r'\sd="(M[^"]+)"', svg)
    if not m:
        raise SystemExit("no path found in %s" % SRC)
    d = m.group(1)
    toks = re.findall(r"[MCZ]|-?\d+\.?\d*", d)
    pts, i = [], 0
    cur = start = (0.0, 0.0)
    while i < len(toks):
        t = toks[i]
        if t == "M":
            cur = (float(toks[i + 1]), float(toks[i + 2])); start = cur
            pts.append(cur); i += 3
        elif t == "C":
            p1 = (float(toks[i + 1]), float(toks[i + 2]))
            p2 = (float(toks[i + 3]), float(toks[i + 4]))
            p3 = (float(toks[i + 5]), float(toks[i + 6]))
            for k in range(1, 33):          # control points overstate the hull
                u = k / 32.0; v = 1 - u
                pts.append((v**3*cur[0] + 3*v*v*u*p1[0] + 3*v*u*u*p2[0] + u**3*p3[0],
                            v**3*cur[1] + 3*v*v*u*p1[1] + 3*v*u*u*p2[1] + u**3*p3[1]))
            cur = p3; i += 7
        else:
            cur = start; i += 1
    xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
    return d, (min(xs), max(xs), min(ys), max(ys))


D, (X0, X1, Y0, Y1) = traced_path()
S = MARK_W / (X1 - X0)                       # outline units -> scene units
MARK_H = (Y1 - Y0) * S

def place(feet_y):
    return "translate(%.3f,%.3f) scale(%.6f)" % (500 - (X0 + X1) / 2 * S, feet_y - Y1 * S, S)

MT_SCENE  = place(BASE_Y)                    # feet on the grid
MT_FLOAT  = place((1000 + MARK_H) / 2)       # centred, for the floorless variant

def f(v): return ("%.2f" % v).rstrip("0").rstrip(".")

def stops(pairs):
    return "".join('<stop offset="%s%%" stop-color="%s" stop-opacity="%s"/>'
                   % (f(o), c, f(a)) for o, c, a in pairs)

def rg(gid, cx, cy, r, ry, pairs):
    """A CSS radial-gradient with separate x and y radii: an SVG circle
    squashed vertically about its own centre."""
    k = ry / r
    return ('<radialGradient id="%s" gradientUnits="userSpaceOnUse" cx="%s" cy="%s" r="%s"'
            ' gradientTransform="translate(0,%s) scale(1,%s)">%s</radialGradient>'
            % (gid, f(cx), f(cy), f(r), f(cy * (1 - k)), f(k), stops(pairs)))

NEON = [(0, "#FF12E8", 1), (24, "#C022FF", 1), (50, "#7A3BFF", 1),
        (76, "#2A8CFF", 1), (100, "#25D8F2", 1)]
# Screen-space, so no pre-distortion: the colour just follows height above the
# floor. scene.html has to pre-distort its stops only because its gradient is
# painted in plane space and foreshortened afterwards; this is what that
# arrangement works out to once projected.
GRID = [(0, "#25D8F2", 1), (24, "#2A8CFF", 1), (50, "#7A3BFF", 1),
        (76, "#C022FF", 1), (100, "#FF12E8", 1)]
TUBE = [(0, "#fff", .70), (38, "#fff", .34), (72, "#fff", .12), (100, "#fff", .05)]


def mark_defs():
    """Path, clip and the two gradients the mark needs, shared by all three."""
    return [
        '<path id="m" d="%s"/>' % D,
        '<clipPath id="inside" clipPathUnits="userSpaceOnUse"><use href="#m"/></clipPath>',
        # objectBoundingBox, not userSpaceOnUse: the mark and its reflection sit
        # in differently transformed groups, and a userSpaceOnUse gradient would
        # restart in each one and render every piece a single flat colour.
        '<linearGradient id="neon" gradientUnits="objectBoundingBox" x1=".04" y1="0" x2=".92" y2="0">%s</linearGradient>' % stops(NEON),
        '<linearGradient id="tube" gradientUnits="objectBoundingBox" x1="0" y1="0" x2="0" y2="1">%s</linearGradient>' % stops(TUBE),
    ]


def mark_body(mt, scale):
    """Fill, tube highlight and rim. `scale` converts a fraction of the mark's
    width into stroke units in whatever space the marks are drawn in."""
    open_g = ('<g transform="%s">' % mt) if mt else "<g>"
    return [
        open_g,
        '<use href="#m" fill="url(#neon)"/>',
        # The tube highlight: a fat white stroke clipped to the inside of the
        # shape, leaving a band hugging the contour from within, on the counter
        # as well as the outer edge, since both are the one path.
        '<g clip-path="url(#inside)"><use href="#m" fill="none" stroke="url(#tube)" stroke-width="%s" stroke-linejoin="round"/></g>' % f(0.04 * scale),
        "</g>",
        ('<g filter="url(#rimglow)">' + (open_g if mt else "<g>")),
        '<use href="#m" fill="none" stroke="#fff" stroke-width="%s" stroke-linejoin="round" opacity=".92"/>' % f(0.016 * scale),
        "</g></g>",
    ]


def write(name, parts):
    out = os.path.join(HERE, name)
    svg = "\n".join(parts) + "\n"
    io.open(out, "w", encoding="utf-8", newline="\n").write(svg)
    print("  %-16s %6d bytes" % (name, len(svg)))


def scene_head(label):
    return ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1000 1000"'
            ' width="1000" height="1000" role="img" aria-label="Omega Stick">',
            "<title>Omega Stick</title>",
            "<!-- Generated by build-scene.py (%s). Edit that, not this. -->" % label]


# --------------------------------------------------------------------------
# scene.svg
# --------------------------------------------------------------------------
def build_scene():
    # A vertical is a trapezium, not a stroke: at depth u the plane's x scale is
    # (1-u) and the line's own width is carried along with it.
    verts = []
    for m in range(-NX, NX + 1):
        X, h = CELL_X * m, LW_X / 2
        verts.append('<polygon points="%s 1000 %s 1000 %s %s %s %s"/>' % (
            f(500 + X - h), f(500 + X + h),
            f(500 + (X + h) * K_FAR), f(y_of(U_FAR)),
            f(500 + (X - h) * K_FAR), f(y_of(U_FAR))))

    # A depth line's thickness goes as (1-u)^2 -- foreshortening squares it --
    # and the plane narrows as (1-u), so the far ones do not span the width.
    deps = []
    for j in range(NZ + 1):
        u = u_of(CELL_Z * j); k = 1 - u
        t = LW_Z * COS * k * k
        x0 = max(0.0, 500 - SPAN_H * k); x1 = min(1000.0, 500 + SPAN_H * k)
        deps.append('<rect x="%s" y="%s" width="%s" height="%s"/>' % (
            f(x0), f(y_of(u) - t / 2), f(x1 - x0), f(t)))

    p = scene_head("scene")
    p.append("<defs>")
    p += mark_defs()
    p.append('<linearGradient id="grid" gradientUnits="userSpaceOnUse" x1="0" y1="1000" x2="0" y2="%s">%s</linearGradient>'
             % (f(HZ_Y), stops(GRID)))
    p.append(sky_gradient())
    p.append(rg("pool1", 500, HZ_Y, 720, 240, [(0, "#C43EFF", .30), (72, "#C43EFF", 0)]))
    p.append(rg("pool2", 500, HZ_Y + 120, 1320, 560, [(0, "#4E1CD2", .40), (74, "#4E1CD2", 0)]))
    p.append(rg("aura", 500, 395, 440, 320, AURA))
    p.append(rg("haze1", 500, 685, 260, 58, [(0, "#FFACFF", .98), (76, "#FFACFF", 0)]))
    p.append(rg("haze2", 500, 685, 620, 100, [(0, "#FF1EEC", .74), (76, "#FF1EEC", 0)]))
    p.append(rg("haze3", 500, 685, 1260, 82, [(0, "#AC3CFF", .52), (80, "#AC3CFF", 0)]))
    p.append(rg("spill", 500, 674.6, 280, 45, [(0, "#E876FF", .62), (66, "#E876FF", 0)]))
    p.append(vignette())
    # The floor fade, matching scene.html: clear before the plane's far edge so
    # the cut never shows, and full strength only low enough down that a depth
    # line is still about a pixel wide.
    p.append('<linearGradient id="fadeg" gradientUnits="userSpaceOnUse" x1="0" y1="%s" x2="0" y2="1000">%s</linearGradient>'
             % (f(HZ_Y), stops([(0, "#fff", 0), (17, "#fff", 0), (25, "#fff", .25),
                                (34, "#fff", .70), (45, "#fff", 1), (100, "#fff", 1)])))
    p.append('<mask id="fade"><rect width="1000" height="1000" fill="url(#fadeg)"/></mask>')
    p += starfade()
    p.append('<linearGradient id="reflectg" gradientUnits="userSpaceOnUse" x1="0" y1="%s" x2="0" y2="794.5">%s</linearGradient>'
             % (f(BASE_Y), stops([(0, "#fff", 1), (100, "#fff", 0)])))
    p.append('<mask id="reflectfade"><rect width="1000" height="1000" fill="url(#reflectg)"/></mask>')
    p += scene_filters()
    p.append('<filter id="gridglow" x="-10%" y="-10%" width="120%" height="120%"><feGaussianBlur stdDeviation="4"/></filter>')
    p.append('<filter id="soft" x="-30%" y="-30%" width="160%" height="160%"><feGaussianBlur stdDeviation="8"/></filter>')
    p.append('<g id="floor" fill="url(#grid)">%s%s</g>' % ("".join(verts), "".join(deps)))
    p.append("</defs>")

    p.append('<rect width="1000" height="1000" fill="url(#sky)"/>')
    p.append('<rect width="1000" height="1000" fill="url(#pool2)"/>')
    p.append('<rect width="1000" height="1000" fill="url(#pool1)"/>')
    p.append('<g mask="url(#starfade)">%s</g>' % stars())
    p.append('<rect width="1000" height="1000" fill="url(#aura)"/>')

    # The reflection goes behind the floor, so it fills the dark between the
    # grid lines. Drawn over them it dims every line it crosses and reads as a
    # smudge instead of light.
    p.append('<g mask="url(#reflectfade)" filter="url(#soft)">')
    p.append('<g transform="translate(0,%s) scale(1,-0.55) translate(0,%s)">' % (f(BASE_Y), f(-BASE_Y)))
    p.append('<g transform="%s"><use href="#m" fill="url(#neon)" fill-opacity=".34"/></g></g></g>' % MT_SCENE)

    p.append('<g mask="url(#fade)"><use href="#floor" filter="url(#gridglow)" opacity=".55"/><use href="#floor"/></g>')

    p.append('<rect width="1000" height="1000" fill="url(#haze3)"/>')
    p.append('<rect width="1000" height="1000" fill="url(#haze2)"/>')
    p.append('<rect width="1000" height="1000" fill="url(#haze1)"/>')
    p.append('<rect width="1000" height="1000" fill="url(#spill)"/>')

    p += scene_mark_stack(MT_SCENE)
    p.append('<rect width="1000" height="1000" fill="url(#vig)"/>')
    p.append("</svg>")
    write("scene.svg", p)


# --------------------------------------------------------------------------
# scene-mark.svg -- same canvas and sky, no floor
# --------------------------------------------------------------------------
def build_scene_mark():
    p = scene_head("scene-mark")
    p.append("<defs>")
    p += mark_defs()
    # No horizon, so the sky loses its floor-anchored pools and the light pools
    # on the mark itself instead. Without that the frame reads as a scene with
    # its floor missing rather than as a mark in space.
    p.append('<linearGradient id="sky" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="0" y2="1000">%s</linearGradient>'
             % stops([(0, "#08052A", 1), (55, "#0C0638", 1), (100, "#0F0742", 1)]))
    p.append(rg("aura", 500, 500, 520, 470, AURA))
    p += starfade()
    p.append(vignette())
    p += scene_filters()
    p.append("</defs>")
    p.append('<rect width="1000" height="1000" fill="url(#sky)"/>')
    p.append('<g mask="url(#starfade)">%s</g>' % stars())
    p.append('<rect width="1000" height="1000" fill="url(#aura)"/>')
    p += scene_mark_stack(MT_FLOAT)
    p.append('<rect width="1000" height="1000" fill="url(#vig)"/>')
    p.append("</svg>")
    write("scene-mark.svg", p)


# --------------------------------------------------------------------------
# omega-neon.svg -- the mark alone, transparent
# --------------------------------------------------------------------------
def build_omega_neon():
    p = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="%s" width="%d" height="%d" role="img" aria-label="Omega Stick">'
         % (MARK_VIEWBOX, MARK_VB_W, MARK_VB_H),
         "<title>Omega Stick</title>",
         "<!-- Generated by build-scene.py (omega-neon). Edit that, not this. -->",
         "<defs>"]
    p += mark_defs()
    # Drawn in the outline's own units, and the viewBox is the tight one the
    # other mark files use, so the halo has only ~26.5 units of margin. Held to
    # the same radius omega.svg uses, or it would clip at the edge of the box.
    p.append('<filter id="glow" x="-25%" y="-25%" width="150%" height="150%">'
             '<feGaussianBlur stdDeviation="13" result="b"/>'
             '<feMerge><feMergeNode in="b"/><feMergeNode in="b"/></feMerge></filter>')
    p.append('<filter id="rimglow" x="-25%" y="-25%" width="150%" height="150%">'
             '<feGaussianBlur stdDeviation="7" result="b"/>'
             '<feMerge><feMergeNode in="b"/><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge></filter>')
    p.append("</defs>")
    p.append('<g filter="url(#glow)" opacity=".85"><use href="#m" fill="url(#neon)"/></g>')
    p += mark_body(None, (X1 - X0))
    p.append("</svg>")
    write("omega-neon.svg", p)


# --- pieces shared by the two scene files ----------------------------------
AURA = [(0, "#C850FF", .30), (26, "#B242FA", .20), (48, "#8C32EE", .10),
        (72, "#7028DE", .035), (100, "#6022D2", 0)]

def sky_gradient():
    return ('<linearGradient id="sky" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="0" y2="1000">%s</linearGradient>'
            % stops([(0, "#0A0730", 1), (44, "#0C0638", 1), (78, "#14084C", 1), (100, "#1B0A5E", 1)]))

def vignette():
    # Ramped from zero rather than held clear and then switched on: a kink in
    # the falloff shows up as a visible ellipse against an even field.
    return rg("vig", 500, 440, 1300, 1120,
              [(0, "#030112", 0), (44, "#030112", .04), (66, "#030112", .13),
               (84, "#030112", .27), (100, "#030112", .42)])

def starfade():
    return ['<linearGradient id="starfadeg" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="0" y2="1000">%s</linearGradient>'
            % stops([(6, "#fff", 1), (50, "#fff", .5), (76, "#fff", 0), (100, "#fff", 0)]),
            '<mask id="starfade"><rect width="1000" height="1000" fill="url(#starfadeg)"/></mask>']

def stars():
    # Seeded, so rebuilding does not reshuffle the sky, and both scene files
    # get the same one.
    random.seed(7)
    out = []
    for _ in range(38):
        out.append('<circle cx="%s" cy="%s" r="%s" fill="%s" opacity="%s"/>' % (
            f(random.uniform(8, 992)), f(random.uniform(10, 750)),
            f(random.uniform(.55, 1.1)),
            random.choice(("#fff", "#fff", "#CFE8FF", "#FFD9FF", "#A8DFFF", "#FFC8F5")),
            f(random.uniform(.3, .72))))
    return "".join(out)

def scene_filters():
    return ['<filter id="bloom" x="-60%" y="-60%" width="220%" height="220%"><feGaussianBlur stdDeviation="55"/></filter>',
            '<filter id="glow" x="-40%" y="-40%" width="180%" height="180%"><feGaussianBlur stdDeviation="19"/></filter>',
            '<filter id="rimglow" x="-40%" y="-40%" width="180%" height="180%">'
            '<feGaussianBlur stdDeviation="9" result="b"/>'
            '<feMerge><feMergeNode in="b"/><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge></filter>']

def scene_mark_stack(mt):
    # Bloom and glow are filtered outside the mark's transform so their radii
    # stay in screen units instead of being scaled along with the path.
    return (['<g filter="url(#bloom)" opacity=".5"><g transform="%s"><use href="#m" fill="url(#neon)"/></g></g>' % mt,
             '<g filter="url(#glow)" opacity=".95"><g transform="%s"><use href="#m" fill="url(#neon)"/></g></g>' % mt]
            + mark_body(mt, MARK_W / S))


if __name__ == "__main__":
    print("outline bbox %.1f,%.1f %.1f,%.1f" % (X0, Y0, X1, Y1))
    build_scene()
    build_scene_mark()
    build_omega_neon()
    print("  scene mark %.1fcqw wide, feet at %.1fcqh; floorless copy centred"
          % (MARK_W / 10, (1000 - BASE_Y) / 10))
