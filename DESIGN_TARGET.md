# DESIGN_TARGET.md — what the d-os renderer can actually draw

> **Why this file exists.**  A visual design is being produced in Claude Design
> and will arrive here as a handoff package (tokens + widget spec).  That design
> is being authored against a renderer nobody on the design side can see.  This
> file states what the renderer *is*, so the spec is written against the real
> target instead of against a browser — and so a token that cannot survive the
> trip is refused at design time rather than silently approximated at draw time.
>
> Every claim here is read out of the tree, not remembered.  Sources are named.

## 1. The renderer in one paragraph

A **software framebuffer painter in C**, ring 0.  32-bit XRGB pixels in a
surface (`struct gfx_surface`), one back buffer per window plus the
compositor's; the compositor blits damage rectangles and the display presents
by page flip (x86) or a virtio-gpu transfer (aarch64).  There is no GPU, no
shader, no display list, no retained scene — a widget's `draw` op writes pixels
into a surface and that is the whole pipeline.

## 2. The complete primitive set

This is the entire drawing API (`kernel/includes/gfx.h`).  Not a summary — the
list:

| Call | What it does |
|---|---|
| `gfx_fill` | solid rectangle, hard edges |
| `gfx_line` | 1-pixel line, hard edges |
| `gfx_blit` | copy a rectangle between surfaces |
| `gfx_move_within` | copy a rectangle *inside* one surface (§4.61's drag path) |
| `gfx_blend_fill` | rectangle at **constant** alpha over the destination |
| `gfx_vgradient` | **vertical** gradient, per-row interpolation |
| `gfx_text` | 8×8 bitmap glyphs, transparent background |
| `gfx_set_clip` / `gfx_clear_clip` | one rectangular clip, **replaces** (never nests — §M65) |

**There is no curve primitive of any kind.**  No circle, no ellipse, no arc, no
rounded rectangle, no bezier, no arbitrary path.  `icons.c` says so in its own
header and works around it by stacking three rectangles into a shape that
"reads as a rounded square" — that workaround is the state of the art here.

There is also no shadow primitive.  Window shadows are the compositor's, painted
as blended rectangles.

## 3. Anti-aliasing — **the answer is no**

Asked in the design tool as `aa`.  Recorded as "nem tudom"; the answer is:

**Nothing a widget draws is anti-aliased.**  `gfx_fill`, `gfx_line` and
`gfx_text` all produce hard pixels.  A diagonal is a staircase.  A 1-pixel
border is exactly one pixel.

**One exception, and it is not reachable from a widget in practice:**
`kernel/gui/vpath.c` is a real anti-aliased scanline rasteriser — 4× vertical
subsampling, coverage accumulated per pixel, one src-over blend
(`SUBSAMPLES 4`, `blend()` at vpath.c:32).  It exists for **the boot logo** and
nothing else, is integer-only, and takes flattened polygons produced at build
time by `scripts/svg2paths.py`.  It is a proof that AA is *possible* here, not
evidence that the toolkit has it.

The only alpha available to a widget is `gfx_blend_fill`, and it is a
**constant** alpha across a rectangle — not a per-pixel ramp, not a mask.

### What that forbids in the design

Each of these is a browser habit that has no implementation on the other side:

- **`border-radius` > 0** — a rounded corner must be drawn as visible steps, or
  it is not drawn.  At 8-pixel type sizes a stepped corner is *loud*.
- **`box-shadow` with a blur** — there is no blur.  A drop shadow can only be a
  hard offset rectangle at constant alpha.
- **Feathered / soft edges, glows, vignettes** — no per-pixel alpha.
- **Gradient borders, gradient text, radial or angled gradients** — the only
  gradient is top-to-bottom over a rectangle.
- **`opacity` on a subtree** — alpha applies per fill call, not to a composed
  group.

## 4. Fonts — **there is exactly one, and it is 8×8**

Asked in the design tool as `fonts`.  Recorded as "nem tudom még"; the answer is
already fixed by the tree:

`font8x8[256][8]` in `kernel/drivers/terminal/fb_terminal.c:81` — **one bit per
pixel**, 8 pixels wide, 8 pixels tall, **every glyph the same width**.  Exported
to layout code as `GFX_GLYPH_W` / `GFX_GLYPH_H` so nothing hard-codes the 8.

- `0x00`–`0x7F`: derived from the public-domain CP437 ROM font.
- `0x80`–`0xFF`: **ISO-8859-2 (Latin-2)**, not Latin-1 — forced, because **ő and
  ű do not exist in Latin-1** and a Latin-1 font would silently drop the two
  vowels most characteristic of Hungarian (§4.66).  18 accented glyphs are
  derived from their base letters so they stay the same typeface.

There is **no second font, no proportional metrics, no kerning, no hinting, no
font weight, and no font size.**  The only scaling that exists is **integer
multiplication** of the bitmap (what the boot splash does), and 2× is visibly
blocky.

### What that forbids, and what replaces it

A `font-family` / `font-weight` / `font-size` token cannot be honoured and must
not appear in the spec.  Realistically the type scale is **8 px, and 16 px where
a heading earns the blockiness**.

So **typographic hierarchy has to come from somewhere else**, and the design
should say explicitly which of these it is using where:

- **colour** (foreground contrast against the plate)
- **spacing** — the single strongest tool available
- **rules and plates** (a filled band behind a title, a 1-px separator)
- **CASE** (upper-case for section labels)

§M65 already measures layout in **cells, not pixels**, for exactly this reason —
one fixed font, no DPI.  A grid unit of **8 px** is not a preference here, it is
the glyph cell.

## 5. What the toolkit already has

Nine registered widget classes (`WIDGET_CLASS()`, §M65):

`button` · `label` · `textinput` · `checkbox` · `radio` · `slider` · `combo` ·
`listview` · `menubar`

Plus two that are their own thing: `w_editor` (multiline text) and `w_itemview`
(the §M64 model/view — grid, list and table layouts over one model).

Containers and layout: `UI_GRID` (one shared label column), `UI_SCROLL`
(viewport with an offset column), `UI_WRAP_COMPACT` (a row becomes a column at
the small size class).  Three size classes, measured in cells.

A window has **one popup slot** (menus and combos share it) and a Tab focus
cycle at the window level.

## 6. Tokens that survive the trip

For the handoff package — the shortlist that maps onto something real:

**Keep:** colour (as `RRGGBB`), spacing (px, prefer multiples of 8), border
width (integer px, realistically 1), fill vs. plate assignments, per-state
colours (rest / hover / pressed / focused / disabled), the vertical-gradient
pair for a plate, constant-alpha values for overlays and shadows.

**Drop:** `border-radius`, `box-shadow` blur/spread, `font-family`,
`font-weight`, `font-size`, `letter-spacing`, `line-height` as a ratio (it is
an integer pixel advance here), subtree `opacity`, any non-vertical gradient,
any transition or animation curve (the compositor repaints damage rects; there
is no animation clock in the toolkit).

## 7. Open questions to send back to the design side

1. **Does the "Console Plate" direction depend on rounded corners or soft
   shadows?**  If yes, it needs a stepped/hard-edged variant, and the step size
   should be a design decision rather than something the implementation invents.
2. **Where does hierarchy come from** (§4), given one 8×8 font?  Naming it in
   the spec prevents the toolkit from guessing differently in nine places.
3. **Are the state colours specified for all five states of every control?**
   The toolkit has no computed-tint helper, so a missing state is a colour
   somebody picks at implementation time.
