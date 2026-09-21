# Brand assets

Vector marks for Omega Stick. Every mark here is true vector: the omega outline
is 95 cubic Bezier curves with no straight segments, and the wordmark is drawn
rather than set -- straight lines and arcs, stroked with round caps -- so
nothing here depends on a typeface being installed. The folder also holds
two build scripts and three bitmaps, all noted below.

## Files

| File | Use |
| --- | --- |
| `omega.svg` | Primary mark, with outer glow. 96px and up |
| `omega-flat.svg` | Same mark without the blur filter. Below 96px |
| `omega-favicon.svg` | Silhouette only, no rim. Browser tab, 16px and up |
| `omega-mono.svg` | Single colour via `currentColor`. Silkscreen, laser, one-colour print |
| `wordmark.svg` | OMEGA STICK, neon gradient. Generated |
| `wordmark-mono.svg` | OMEGA STICK, single colour via `currentColor`. Generated |
| `lockup.svg` | Mark above wordmark. Default for headers and README. Generated |
| `scene.svg` | The scene as a still. Poster and hero art. Generated |
| `scene-mark.svg` | The same canvas and sky with the floor removed. Generated |
| `omega-neon.svg` | The mark alone with the neon-tube treatment, transparent. Dark grounds only. Generated |
| `scene.html` | The scene as a live hero: constructed mark, lit grid, power-on |
| `build-scene.py` | Regenerates the three scene files above from the constants `scene.html` uses |
| `build-wordmark.py` | Draws the alphabet and regenerates the two wordmarks and the lockup |
| `source-silhouette.png` | Two-colour bitmap the outline was traced from. Kept so the trace is reproducible |
| `source-scene.png` | The render the scene was matched to. Provenance, not a spec |
| `source-scene-knockout.png` | The same render with its background knocked out |

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
  `color` on the parent rather than editing the file. That only works when the
  SVG is inlined in the page. Referenced through `<img>` it is a separate
  document, inherits nothing, and comes out black -- so on a dark ground reach
  for `wordmark.svg`, or inline the file.
- **The wordmark is stroked, not filled.** One path of straight lines and
  arcs, `stroke-linecap` and `stroke-linejoin` both `round`, at
  `stroke-width` 17% of the cap height. That is the whole letterform: the round
  terminal is the same tube the mark reads as, which is what makes the two look
  like one drawing in the lockup rather than a symbol next to some type. It
  also means the file stays under 2 kB and the weight is one number.
  **For laser, CNC or vinyl, expand the stroke to outlines first** -- those
  toolpaths follow a stroked path as a centreline and will cut the skeleton
  rather than the letters.
- **The S is the one letter that is not plainly circles and lines.** Each half
  is walked out from the waist as a chain of tangent arcs whose radius tightens
  into the left extreme and opens back out over the top, so the curvature falls
  towards zero at the inflection instead of the letter carrying a straight
  segment through its middle. Its proportions are measured off Century Gothic,
  a geometric face with a truly circular O: there the S is 0.56 of the O's
  width, and this one is 0.62 because a much heavier stroke needs the extra
  room for its counters. The docstring in `build-wordmark.py` records two
  constructions that looked reasonable and were not, so they do not get tried
  again.
- **The S's lower half is wider than its upper half, deliberately.** Draw the
  two congruent and the letter measures symmetrical but reads top-heavy, the
  usual illusion in S, B, X and 8. Every face measured corrects it, by 1.06 in
  Corbel and Verdana up to 1.16 in Century Gothic; this sits at 1.095. The
  waist is nudged left of centre to buy that, and up a little to put the
  inflection where the references put it, both through one `WAIST` constant.
  If the S ever looks wrong at the bottom, check that before the curves: the
  bug people expect is the two halves having drifted apart, and the bug that
  actually shows is them being identical.
- **Nothing in the wordmark is a declared width.** The letters are drawn in a
  100-unit cap height; each one's ink box and both its sidebearings are then
  measured off the shape. A side gets a full bearing where its silhouette is
  flat and less of one the further that silhouette recedes, so `T` tucks under
  its neighbour and `O` sits closer than `I` without anybody kerning pairs by
  hand. Change the weight and the spacing re-solves with it.
- **The round letters are bigger than the cap height, on purpose.** `O`, `C`,
  `G` and `S` overshoot the cap line and the baseline by 1.3 units in 100, and
  the points of `A` and `M` do the same; a circle that stops exactly on the cap
  line reads as too small beside a flat-topped letter. `O`, `C` and `G` stay
  true circles at any weight because their radius is derived from the overshoot
  rather than typed in.
- **The traced outline lives in one place.** All eight SVGs that draw the mark
  carry it byte for byte, and both build scripts read it out of `omega-mono.svg`
  at build time rather than keeping a copy of their own.
- **Mind which space a gradient is in.** The hand-drawn marks use
  `userSpaceOnUse`, and there the rule is: keep the geometry in one coordinate
  space. Wrap parts of a mark in their own transformed groups and each group
  restarts the gradient in its own space, so every piece renders one flat
  colour. The three scene files cannot do that -- the mark and its
  reflection have to sit in differently transformed groups -- so their neon
  gradient is `objectBoundingBox` instead, which is measured per element and
  survives the transform. Copy the right one for the situation.
- `scene.svg` and `scene.html` carry the grid and are deliberately the only
  files that do. The grid is decoration; the mark has to work without it.
- **The wordmark files are generated too; edit `build-wordmark.py`, not them.**
  Run `python assets/logo/build-wordmark.py` to rebuild `wordmark.svg`,
  `wordmark-mono.svg` and `lockup.svg`. It reads the traced outline out of
  `omega-mono.svg` for the lockup, the same way `build-scene.py` does, so the
  trace still lives in exactly one place. The lockup sizes the mark and the gap
  in wordmark cap heights, measured off the mark's real ink box rather than its
  viewBox -- the mark files carry a margin for their glow, and laying out
  against that would put the mark further away than the number says.
- **Three scene files are generated; edit `build-scene.py`, not them.** Run
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
- **The scene was matched to `source-scene.png` by eye, not by measurement,**
  and it cannot be measured. The reference is a soft, glow-heavy render with
  no crisp edge anywhere on the mark: threshold it five different ways and
  the mark's width comes out anywhere from 37% to 55% of the frame, because
  near the feet its bloom floods almost the full width and the horizon band
  below it is brighter than the mark's own rim. `scene.html` settles on
  38.4%, which sits at the sane end of that range. The constants at the top
  of `scene.html` are the authority for framing; these two PNGs are kept as
  provenance, so do not re-derive geometry from them.
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
