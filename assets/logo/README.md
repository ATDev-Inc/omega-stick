# Brand assets

Vector marks for Omega Stick. Every file is true vector: the omega outline is
95 cubic Bezier curves with no straight segments, and the wordmark is built
from geometric primitives rather than an outlined font, so nothing here
depends on a typeface being installed.

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
| `scene.svg` | Mark on the perspective grid. Poster and hero art only |
| `source-silhouette.png` | Two-colour bitmap the outline was traced from. Kept so the trace is reproducible |

## Palette

| Stop | Hex |
| --- | --- |
| 0% | `#FF12E8` |
| 24% | `#C022FF` |
| 50% | `#7A3BFF` |
| 76% | `#2A8CFF` |
| 100% | `#25D8F2` |

Background used in the scene: `#0B0A2E`.

## Usage notes

- **Do not scale `omega.svg` below about 96px.** Its Gaussian blur turns to
  mush at small sizes. Use `omega-flat.svg` or `omega-favicon.svg` instead.
  This was checked by rendering both, not assumed.
- **`omega-mono.svg` and `wordmark-mono.svg` inherit `currentColor`**, so set
  `color` on the parent rather than editing the file.
- **The gradient is `userSpaceOnUse`.** If you wrap parts of a mark in their own
  transformed groups, each group restarts the gradient in its own coordinate
  space and every piece renders the same colour. Keep the geometry in one
  space, as it is here.
- `scene.svg` carries the grid and is deliberately the only file that does.
  The grid is decoration; the mark has to work without it.
