# Brand assets

Vector marks for Omega Stick. Every mark here is true vector: the omega outline
is 95 cubic Bezier curves with no straight segments, and the wordmark is built
from geometric primitives rather than an outlined font, so nothing here depends
on a typeface being installed. The folder also holds one build script and one
bitmap, both noted below.

## Files

| File | Use |
| --- | --- |
| `omega.svg` | Primary mark, with outer glow. 96px and up |
| `omega-flat.svg` | Same mark without the blur filter. Below 96px |
| `omega-favicon.svg` | Silhouette only, no rim. Browser tab, 16px and up |
| `omega-mono.svg` | Single colour via `currentColor`. Silkscreen, laser, one-colour print |
| `wordmark.svg` | OMEGA STICK, neon gradient |
| `wordmark-mono.svg` | OMEGA STICK, single colour |
| `lockup.svg` | Mark above wordmark. Default for headers and README |
| `scene.svg` | The scene as a still. Poster and hero art. Generated |
| `scene-mark.svg` | The same canvas and sky with the floor removed. Generated |
| `omega-neon.svg` | The mark alone with the neon-tube treatment, transparent. Dark grounds only. Generated |
| `scene.html` | The scene as a live hero: constructed mark, lit grid, power-on |
| `build-scene.py` | Regenerates the three generated files above from the constants `scene.html` uses |
| `source-silhouette.png` | Two-colour bitmap the outline was traced from. Kept so the trace is reproducible |

## Palette

| Stop | Hex |
| --- | --- |
| 0% | `#FF12E8` |
| 24% | `#C022FF` |
| 50% | `#7A3BFF` |
| 76% | `#2A8CFF` |
| 100% | `#25D8F2` |

Background used in the scene: `#0B0A2E`. Every scene file lays a sky gradient
over it rather than using it flat -- `#0A0730` overhead to `#1B0A5E` at the
horizon in `scene.html` and `scene.svg`, a shorter ramp in `scene-mark.svg`
which has no horizon -- so sampling a pixel out of a render will not give you
`#0B0A2E`. Use it as the surrounding page colour, not as a colour to match.

## Usage notes

- **Do not scale `omega.svg` below about 96px.** Its Gaussian blur turns to
  mush at small sizes. Use `omega-flat.svg` or `omega-favicon.svg` instead.
  This was checked by rendering both, not assumed.
- **`omega-mono.svg` and `wordmark-mono.svg` inherit `currentColor`**, so set
  `color` on the parent rather than editing the file.
- **The traced outline lives in one place.** All eight SVGs that draw the mark
  carry it byte for byte, and `build-scene.py` reads it out of `omega-mono.svg`
  at build time rather than keeping its own copy.
- **Mind which space a gradient is in.** The hand-drawn marks use
  `userSpaceOnUse`, and there the rule is: keep the geometry in one coordinate
  space. Wrap parts of a mark in their own transformed groups and each group
  restarts the gradient in its own space, so every piece renders one flat
  colour. The three generated files cannot do that -- the mark and its
  reflection have to sit in differently transformed groups -- so their neon
  gradient is `objectBoundingBox` instead, which is measured per element and
  survives the transform. Copy the right one for the situation.
- `scene.svg` and `scene.html` carry the grid and are deliberately the only
  files that do. The grid is decoration; the mark has to work without it.
- **Three files here are generated; edit `build-scene.py`, not them.** Run
  `python assets/logo/build-scene.py` to rebuild `scene.svg`, `scene-mark.svg`
  and `omega-neon.svg`. The camera, framing and palette constants at the top of
  that script mirror the custom properties at the top of `scene.html`, so
  `scene.svg` and `scene.html` stay the same picture. They drifted
  badly once -- the SVG had the mark at 55.5% of the frame width against the
  HTML's 38.9%, with the feet on the horizon rather than in front of it and the
  grid at half the pitch -- which is why the geometry is now derived rather than
  drawn. Change one, change the other.
- **The two still differ in the mark outline, on purpose.** `scene.svg` uses the
  traced outline shared with `omega.svg` and the rest of this folder;
  `scene.html` constructs its own from ellipses. They part company slightly
  around the feet. Everything else -- camera, framing, colour ramp, glow, haze,
  reflection, vignette -- matches to a mean of 14/255 per pixel across the
  frame, measured by rendering both.
- **`scene.html` draws the mark from construction, not from the trace.** Outer
  contour: one ellipse. Counter: two half-ellipses sharing a radius, the lower
  one longer. Feet: axis-aligned slabs. It is symmetric about its centre, which
  the traced outline in the other files is not.
- **`scene.html` is switched by data attributes on `.omega-scene`**, and the
  tunable dials are custom properties documented at the top of the file.

  | Attribute | Effect |
  | --- | --- |
  | `data-bg="transparent"` | Drop the sky, stars and vignette, for compositing |
  | `data-motion="calm"` | Still grid, rest of the scene alive. **The default** |
  | `data-motion="on"` | Add the grid drift back on top of the default |
  | `data-motion="off"` | Freeze everything, as `prefers-reduced-motion` does |
  | `data-intro="off"` | Skip the power-on, keep the idle breathe and twinkle |
  | `data-stars="off"` | Drop the starfield, back to flat negative space |
  | `data-crt="on"` | Scanlines and film grain over the whole frame |

- **The grid holds still by default, and that is deliberate.** Perspective
  squares the line weight, so a depth line lands on screen at
  `--lw-z * cos(tilt) * (1 - u)^2` and falls under a pixel wide past `u = 0.61`.
  A sub-pixel line cannot move without its coverage stepping, which reads as
  flicker. Softening the line profile (`--soft-z`), fading the far field where
  lines stop being resolvable, and low-passing the floor together cut the
  measured shimmer by about half -- not enough, so the drift is off unless asked
  for. `data-motion="on"` restores it and `--drift` sets its speed.
- **`omega-neon.svg` needs a dark ground; `omega.svg` does not.** The neon
  treatment is a white rim plus an inner tube highlight and an outer halo, and
  all three of those depend on the background being darker than they are. On a
  near-white card the rim disappears into the page and the halo does nothing,
  leaving a shape that reads as cut out rather than lit. Checked by rendering
  both marks over ink, near-white and mid-violet. Use `omega.svg` on light
  backgrounds and `omega-neon.svg` on dark ones; they share a viewBox and
  intrinsic size, so swapping one for the other moves nothing in a layout.
- **`scene-mark.svg` is the scene with the floor taken out**, not a crop of it.
  The mark recentres, and the sky loses the two light pools anchored to the
  horizon in favour of one centred on the mark -- otherwise the frame reads as
  a scene whose floor failed to load. It is the file to reach for when the
  README note above about the grid being decoration needs demonstrating.
- **Set `data-intro="off"` before screenshotting `scene.html`.** The power-on
  runs for 3.2s from load and its first keyframe is a black frame, so a headless
  capture -- which freezes CSS animations at t = 0 -- otherwise gets the unlit
  scene. This was hit while rendering the file, not guessed at.
- **The plane's colour stops are pre-distorted and are tied to `--depth`.** The
  gradient runs down a plane that is then foreshortened, so evenly spaced stops
  would pile up against the horizon. The stops in the file invert that map for
  the current `--depth`; change the depth and they have to be re-solved, or the
  floor's cyan-to-magenta ramp lands in the wrong place. `--fade` is tied to
  `--depth` the same way, and is called out where it is set.
