# Bare roadmap

One milestone per session. Each one ends with: code written, headless tests written for everything testable without a
GPU, everything built, tests and the milestone's example run, failures fixed, `CLAUDE.md` "Current status" updated and
the milestone ticked here. Something visible works from M1.

Legend: `[ ]` not started, `[~]` in progress, `[x]` done.

---

## [x] M1: Window, SDL3 GPU, instanced SDF rounded rects

Done 2026-11-23. Not verified on this machine: DPI scales above 100% and live window resizing (see CLAUDE.md status).

**Goal:** the hello-world at the top of `bare.h` opens a window and draws a rounded box with a border and a soft shadow.

- CMake skeleton: `bare_core`, `bare`, `tests/`, `examples/`; options `BARE_BACKEND` (SDL3), `BARE_BUILD_TESTS`,
  `BARE_BUILD_EXAMPLES`; SDL3 via FetchContent at a pinned release tag, linked statically.
- Warnings: `-Wall -Wextra -Wpedantic` / `/W4`, as errors for `src/core`.
- `arena.c`, `id.c`, `tests/bare_test.h` (tiny test runner), first tests (arena, id hashing).
- `bare_backend.h` interfaces; `platform_sdl3.c` (window, events, time, DPI, blocking wait); `renderer_sdl3gpu.c`
  (device, swapchain, pipeline, instance buffer, batching, scissor, texture uploads even if unused yet).
- `shaders/bare.glsl`: SDF box with per-corner radius, border, Gaussian shadow, AA. Compile to SPIR-V with `glslc` and
  to DXIL/MSL with SDL_shadercross (installed in `tools/shadercross/`); commit blobs to `shaders/generated/`.
- `bare.c` + a stub layout: boxes with fixed sizes stack top to bottom from the window's top-left (real layout is M2).
- Emit `BARE_CMD_RECT` for boxes; clear colour; idle sleeping.

**Tests:** arena (alignment, growth, reset, high-water reuse), id (stable, distinct, index variants), emit of a fixed
box produces the expected `BareRectCmd` at 1× and 2× scale.
**Example:** `examples/hello.c` (the 8-line program) and `examples/shapes.c` (radii, borders, shadows side by side).
**Done when:** both examples render correctly on Windows; a window resize and a DPI change keep boxes sharp; idle CPU ~0%.

## [x] M1.5: Liquid glass (added by the user as Bare's first showcase)

Done 2026-11-23.

- `.glass` (`BareGlass`: refraction, depth, blur, dispersion, shine, merge, tint; `BARE_GLASS`, `BARE_GLASS_FROSTED`),
  `.offset` (unsnapped visual move, children included), `bare_stack` / `BARE_STACK` (layers).
- `BARE_CMD_GLASS` with up to 8 shapes; consecutive glass siblings with `.merge` melt into one surface (smooth min).
- Renderer draws into an offscreen scene; before each glass command it copies the scene to a mipmapped backdrop;
  `shaders/glass.glsl` bends it through a circular rim, with dispersion, frost, rim light and a merged shadow.
- `BARE_CAPTURE` / `BARE_CAPTURE_FRAMES` save a frame to .bmp for checking by eye.

**Example:** `examples/glass.c`. **Tests:** stack, offset, glass defaults/zero/merge/frost (in `test_emit.c`).

**Still to do to make it best in class** (fold into later milestones):
- Copy only the region behind each glass group (and share one copy between non-overlapping groups) instead of the
  whole frame; measure at 4K with many panels.
- ~~Springs from M4 for the motion~~ done after M4: glass settings animate, glass condenses and dissolves, `.zoom`
  (loupe) and `.light` (rim light direction); `examples/glass_motion/`. Still to do: glass that wobbles/stretches with
  velocity (liquid feel), rotation (M10) for tilted entrances.
- Light that responds to the content behind (adaptive tint and highlight), and optional pointer-driven light.
- ~~Glass inside clips and scroll containers (M6)~~ done: glass is scissored like everything else (`examples/scroll.c`). ~~Glass on text (M5)~~ done: text on glass and glass refracting
  text both work (`examples/text.c` loupe, labels in `glass_motion/` and the fluid panel).

## [x] M2: Flexbox layout and draw commands

Done 2026-11-23. Style resolution stayed in `tree.c` (no `style.c` yet); the fake text measurer moves to M5 with
text, and the clip push/pop pairing test to M6 with clips. Tests: `tests/test_layout.c` (23). Compared with HTML/CSS by
reasoning about the CSS algorithm, not side by side in a browser. One deliberate difference: pixel and percent sizes
never shrink.

**Goal:** real layout: rows, columns, padding, gap, grow/shrink, fit/fixed/percent, min/max, align, justify.

- `tree.c` (open/close/leaf, pre-order array, auto and explicit IDs, unclosed-container detection), `store.c`
  (retained node table with eviction), `style.c` (zero-means-default, `BARE_ZERO`, padding resolution), `layout.c`
  (the six passes in ARCHITECTURE §6), `bare_row` / `bare_column`.
- Text is not real yet: layout uses the `BareMeasureText` callback with a fake monospace measurer in tests and a
  placeholder (grey bar per character) in examples.
- `bare_core.h` headless context: `bare_ctx_create/begin/end/destroy`.

**Tests (headless):** fixed sizes; fit wraps children; grow splits by weight; shrink respects minimums; percent;
min/max clamps; padding forms; gap; every `align` and `justify`; nested rows in columns; `BARE_ZERO`; auto-ID
stability across frames and with loops; duplicate-ID detection; node eviction; draw-list order (pre-order, clip
push/pop pairing).
**Example:** `examples/layout.c`: a sidebar + header + content grid of cards that reflows on resize.
**Done when:** all layout tests pass, the example matches the same layout written in HTML/CSS (compare by eye).

## [x] M3: Basic interaction

Done 2026-11-23. Also pulled forward from M4: `.scale` as a real visual transform (about the centre, children
included, composed with `.offset`), drawn and hit-tested. Clicks follow the DOM rule (common ancestor of the down and up
targets, bubbling). An extra frame is drawn after a click or key press, and when the UI moves under a still mouse.
Checked in the real app by injecting SDL events from a scratch build (not committed) and capturing. Wheel offsets are
stored per scroll container, clamped only at 0 until M6.

**Goal:** elements know when they are hovered, pressed and clicked; the wheel produces scroll deltas.

- `input.c` (event application, key state, mouse state), `hit.c` (hit list, topmost-first search, clip and rounded
  corners, `.passthrough`), `BareState` for leaves and `bare_this()` for containers, `.hover` / `.press` looks applied
  instantly, `.cursor`, `bare_mouse`, `bare_key_down/pressed`, clipboard.
- Wheel events routed to the nearest scrollable ancestor (offsets stored, applied in M6).

**Tests (headless, synthetic events):** hover picks the topmost element; ancestors are hovered too; rounded corner
misses; passthrough; press→release inside clicks once; press inside, release outside doesn't click; drag out and back
in clicks; right click; key pressed is true for one frame only.
**Example:** `examples/buttons.c`: boxes that change colour on hover and press and count clicks.
**Done when:** tests pass and the example feels instant (no visible frame lag on click).

## [x] Canvas: your own GPU shaders (added by the user, with the Fluids port)

Done 2026-11-23. What `<canvas>` + WebGL are to a web page, as general features (nothing fluid-specific in Bare).

- `bare_canvas(.width, .height, .format, .nearest, .repeat)`: an image shaders draw into; RGBA8, RGBA16F, RG16F, R16F
  (narrow float formats fall back to wider ones where unsupported, as WebGL apps do). Starts transparent black.
- `bare_shader(&code)` / `bare_pass(.target, .shader, .inputs = {...}, BARE_PARAMS(p), .blend)`: a fragment shader
  over every pixel of the target (one fullscreen triangle, `shaders/pass.glsl`); up to 8 inputs; params as a std140
  block; blend NONE / ADD / OVER. Passes run on the GPU in call order before the frame is drawn.
- `bare_add_shaders()` (CMake): GLSL 450 → SPIR-V + DXIL + MSL + reflection, into a header of `BareShaderCode`
  values; falls back to the committed header without the tools.
- `.image` boxes (images and canvases) drawn over `.color`, rounded by `.radius`, under the border.
  `bare_image_size`, `bare_pixel_scale()` (devicePixelRatio).
- Core: GPU ops queued in call order (`src/core/gpu.c`), published with the frame's draw list; images and canvases
  share one id space; mistakes log file:line and are skipped.

**Example:** `examples/fluid/`: Pavel Dobryakov's WebGL fluid simulation (MIT) on canvases, 14 shaders, with a
frosted-glass settings panel built from Bare boxes; sleeps once the fluid settles.
**Tests:** `test_gpu.c` (op order, once-only delivery, no steady-state allocation, mistakes, id reuse, image box order).
**Benchmark:** `bench/` against the WebGL original in Edge: same GPU cost, ~3x less CPU, ~4-5x less memory, 0 when
idle; startup not yet a win. See bench/README.md.

**Still to do:** update an existing image's pixels (video, GIF previews); read a canvas back to the CPU; compute
shaders; passes into a sub-rectangle (scissor) of a canvas; vertex input for meshes.

## [x] M4: Animation system

Done 2026-11-23. `.scale` / `.offset` transforms had already arrived in M3. Channel sets are allocated only for
elements that ask for animation (a recycled pool), so static UI costs nothing. Beyond the plan: an element moved to
another parent glides from where it was; fading to or from transparent keeps the colour (as CSS's premultiplied
interpolation does); animating elements are drawn unsnapped; the first step after idle is one frame, not the whole
sleep. Tests: `tests/test_anim.c` (19). Checked by capturing frames mid-shuffle from a scratch build (not committed);
idle CPU measured at 0 after settling; the fluid benchmark is unchanged (8.1% of a core, 92 MB).

**Goal:** every animatable property moves toward its target with easing curves or springs, interruptibly.

- `easing.c` (cubic-bezier solve), `spring.c` (closed-form under/critical/over-damped step), `anim.c` (channels,
  retargeting, settle detection, active bits, relative-to-parent positions, `.animate` / `.animate_layout`),
  `.scale` / `.offset` visual transforms (emit and hit testing use them), idle detection feeds `bare_frame`'s sleep.

**Tests (headless, fixed dt):** bezier matches CSS reference values (ease, ease-in-out, custom); spring reaches target;
critical damping never overshoots; underdamped overshoots then settles; result independent of dt split (one 0.1 s step
≈ ten 0.01 s steps); retarget mid-flight keeps value (no jump) and, for springs, velocity; settling clears the active
bit; first appearance doesn't animate; a moving parent carries its children.
**Example:** `examples/cards.c`: the example from `bare.h` (cards + springy button), plus a "shuffle" button that
reorders cards with `BARE_IDI` ids so they glide to new places.
**Done when:** tests pass; hovering the button rapidly in and out never jumps; idle CPU returns to ~0% after settling.

## [x] M5: Text rendering

Done 2026-11-23. stb is fetched as a tarball of a pinned commit (SHA-256 checked). Inter 4.1 Regular is subset to
Latin plus common symbols with only its kerning kept (36 KB; `assets/fonts/README.md` has the recipe; its GPOS
extension lookups are unwrapped so stb_truetype reads the kerning). Glyphs are cached at three horizontal sub-pixel
positions, so still text is snapped to whole pixels without uneven spacing; moving or scaled text is drawn unsnapped.
New glyphs become one upload per frame (the bounding box of what changed); a full 2048² atlas starts over the next
frame and asks for that frame. Text elements size like CSS inline content in a flex item: as wide as the longest
line, at most the space they get (fit-content, which now applies to every content-sized cross-axis child), and a
row shrinks them down to their longest word. Default text colour is white (Bare's default background is dark).
Tests: `tests/test_text.c` (29, with a fake monospace font backend plus Inter itself). The fluid panel got real labels
on its frosted glass, `glass_motion` got "Play" and "Add". Not verified: 150%/200% on a real display (the 2x
glyph maths is tested; this display is 100%) and live window resizing (see Current status in CLAUDE.md); narrower
fixed-width captures rewrap as expected.

**Goal:** sharp, wrapped text with the embedded default font; images.

- FetchContent stb (pinned commit): `stb_truetype`, `stb_image`. Embed Inter (Latin subset) via a CMake
  file-to-header step. `font_stb.c` behind `BareFontBackend`; `text.c` (UTF-8, glyph cache, shelf-packed R8 atlas
  with growth, line breaking, `.align`, `.line_height`, `.nowrap`), `bare_text`, `bare_textf`, text colour
  inheritance, `bare_font_load/from_memory`, `BARE_CMD_TEXT` (glyph mode in the shader).
- Images: `bare_image_load/from_pixels/free`, `.image` on boxes, `BARE_CMD_IMAGE` with rounded mask.

**Tests (headless):** UTF-8 decoding incl. invalid bytes; line breaking (spaces, long words, empty lines); measured
widths match glyph advances; atlas packing never overlaps and reports uploads; a glyph is rasterized once.
**Example:** `examples/text.c`: paragraphs that rewrap on resize, sizes 10–48, alignment, an avatar image with
`.radius` = half its size.
**Done when:** text is crisp at 100% and 150%/200% DPI; resizing rewraps smoothly.

## [x] M6: Clipping and scroll containers

Done 2026-11-24. Scroll offsets are retained per node and stepped at the *start* of a frame (so `BareState.scroll`,
the visible-range helper and layout see the value drawn), and applied as a transform snapped to physical pixels,
not in layout pass 6: scrolled text stays sharp and `.animate_layout` children don't chase the scroll. A notch is
100 px (Chrome on Windows); SDL's fractional wheel values (touchpads) are followed directly. Hand-off follows browsers:
the first scroller in the chain that can move takes the whole delta. Shift + wheel scrolls sideways. Like CSS, a box
that clips or scrolls can be squeezed below its content. The scrollbar is two core-made elements (a draggable rail and
a bar that fades and widens via `.animate`), outside the flex flow; its fade-out uses a new core wake time
(`bare_ctx_wake_time`) instead of drawing frames while waiting. The helper is `bare_visible(i, count, size) { ... }`
(a loop; space for undeclared items is added by layout, so headers and footers work). Emit culls elements outside
their clip. Tests: `tests/test_scroll.c` (20). Checked by injecting SDL wheel, swipe and drag events from a scratch
build and capturing (D3D12 = Vulkan, GCC = MSVC). 10,000 rows: ~14 us of core time per frame while scrolling.
Not verified: a real touchpad and a real mouse wheel by hand. Clips are rectangles (rounded clipping stays later).

**Goal:** `.clip` and `.scroll` work, including nesting.

- Clip stack in emit and hit testing (rect intersection), `CLIP_PUSH/POP` as scissor, scroll offsets applied in layout
  pass 6, content size tracking, smooth scroll channel (spring), wheel and touchpad (`precise`) deltas, overscroll
  clamping, nested hand-off, a thin auto-hiding scrollbar drawn as a box (draggable).

**Tests (headless):** children outside a clip aren't hittable; clip intersection when nested; scroll clamps to content;
wheel goes to the inner container until it hits its end, then to the outer; scrolled elements' rects shift correctly.
**Example:** `examples/scroll.c`: a long list of 10,000 items in a scroll container (only visible ones declared, via a
helper computing the visible range) next to a nested horizontal scroller.
**Done when:** scrolling is smooth with mouse wheel and touchpad; 10,000-item list stays at frame rate.

## [ ] M7: Style system

**Goal:** reusable styles like CSS classes.

- `.style` base resolution (style → element → defaults → hover/press), styles referencing styles (one level of
  `.style` inside a style, cycles detected), a default theme (colours, radii, font sizes) as a set of exported styles
  (`bare_theme()->card`, etc.), `bare_background` default from the theme.

**Tests (headless):** element field beats style; style beats default; `BARE_ZERO` in an element overrides a style's
non-zero value; hover look on a style applies to elements using it; chained styles; cycle detection.
**Example:** `examples/styles.c`: one settings page in a light and a dark theme, switched by a button.
**Done when:** the cards example shrinks noticeably by using styles.

## [ ] M8: Core widgets

**Goal:** one-call widgets built only from public elements: proves the API is enough.

- `bare_button`, `bare_checkbox` (animated check), `bare_slider` (drag with press capture; keyboard arrows when
  hovered), `bare_list_item` (hover/selected look); each themeable through `.style` and overridable per call.
- Pointer capture for sliders (keeps receiving moves while pressed) added to `input.c` if missing.

**Tests (headless):** button click returns true once; checkbox toggles; slider maps mouse x to value with clamping and
keeps dragging outside its rect; list selection.
**Example:** `examples/widgets.c`: a settings panel with every widget, under 60 lines.
**Done when:** the example looks polished with no custom styling.

## [ ] M9: sokol backend

**Goal:** prove portability; open the way to WebAssembly.

- `platform_sapp.c` (sokol_app, callback-driven: implements `BarePlatform.run`), `renderer_sgfx.c` (sokol_gfx, same
  instance layout and batching), `bare.shdc.glsl` wrapper compiled with sokol-shdc, `-DBARE_BACKEND=SOKOL`.
- Any core change needed is an interface fix, not a special case. Emscripten build of `examples/cards.c` using
  `bare_run`.

**Tests:** the whole headless suite unchanged; every example built against both backends; screenshots of `shapes.c`
and `text.c` compared between backends.
**Example:** all examples with `bare_run`; `cards.c` in a browser.
**Done when:** examples look the same on both backends on Windows, and `cards.c` runs in a browser.

## [ ] M10: Web parity (looks and layout the web has that Bare doesn't yet)

Comes from the checklist below. Cheap layout items (align-self, aspect-ratio, absolute) can move earlier if an example
needs them.

- Layout: `.self` (align-self), `.aspect` (aspect-ratio), `.wrap` (flex-wrap, rows of cards), `.margin`, absolute
  positioning with insets (`.position = BARE_ABSOLUTE`, `.inset`).
- Looks: per-corner radii (`.radii`; the draw command already has four), per-side border widths and colours, outline +
  outline offset, linear / radial / conic gradients (`.gradient`), several shadows and inset shadows, dashed borders.
- Transforms: `.rotate`, `.origin` (transform-origin); skew and 3D perspective later.
- Filters: blur, brightness, contrast, saturate, grayscale on an element (`.filter`), blend modes.
- Text extras (after M5): letter spacing, underline/strike, text shadow, ellipsis on overflow, weight/italic.

---

## Web parity checklist (HTML + CSS + JS → Bare)

Bare replaces the web stack for native UI, so every common thing the web can do needs an answer. Honest status,
2026-11-23. **done** = works today; **Mx** = scheduled in that milestone; **C** = plain C already does it, no API
needed; **beyond** = something the web can't do as easily.

| Web | Bare | Status |
|---|---|---|
| **Layout** | | |
| `display: flex`, `flex-direction` | `bare_row`, `bare_column` | done |
| `padding`, `gap` | `.padding`, `.pad = {t, r, b, l}`, `.gap` | done |
| `flex: 1` / `flex-grow` / `flex-shrink` | `BARE_GROW` + `.grow`, `.shrink` | done |
| `width/height` px, %, `fit-content` | `300`, `BARE_PERCENT(50)`, `BARE_FIT` | done |
| `min-/max-width/height` | `.min_width` ... | done |
| `align-items`, `justify-content` (all 6) | `.align`, `.justify` | done |
| `position: absolute`, overlapping layers | `bare_stack` + `.offset` today; insets in M10 | partial |
| `z-index` | later siblings draw on top; floating layers later | partial |
| `align-self`, `aspect-ratio`, `flex-wrap`, `margin` | `.self`, `.aspect`, `.wrap`, `.margin` | M10 |
| `display: grid` | grid layout | later |
| `overflow: hidden / auto` | `.clip`, `.scroll` (smooth, nested, chaining, draggable overlay scrollbar) | done |
| virtualized long lists (react-window) | `bare_visible(i, count, size)` | done, beyond |
| `scrollTo`, `scrollIntoView`, keyboard scrolling | | later |
| `display: none`, `order` | don't declare it / declare in that order (immediate mode) | C |
| `vw`, `vh`, `calc()`, media queries | `bare_window_size()` and arithmetic, `if (win.x < 700)` | C |
| **Looks** | | |
| `background-color`, `opacity` | `.color`, `.opacity` (multiplies into children) | done |
| `border`, `border-radius` | `.border`, `.border_color`, `.radius` | done |
| `box-shadow` (one, outer) | `.shadow` with blur and spread, Gaussian like CSS | done |
| `backdrop-filter: blur()` | `.glass = BARE_GLASS_FROSTED` | done |
| Apple liquid glass (refraction, dispersion, merging) | `.glass`, `.merge` | beyond |
| per-corner radius, per-side borders, outline | `.radii`, per-side borders, `.outline` | M10 |
| gradients (linear, radial, conic) | `.gradient` | M10 |
| several / inset shadows | | M10 |
| `filter`, `mix-blend-mode` | `.filter`, blend modes | M10 |
| `clip-path`, `mask`, rounded overflow clip | rounded clipping | later |
| `<img>`, `object-fit` | `.image`, `bare_image_load` (PNG, JPEG, BMP, GIF, TGA); object-fit in M10 | done |
| `<canvas>` + WebGL / WebGL2 fragment shaders, render to texture | `bare_canvas`, `bare_shader`, `bare_pass` | done |
| float textures (`RGBA16F`, `RG16F`, `R16F`), additive blending | `.format`, `.blend = BARE_BLEND_ADD` | done |
| `devicePixelRatio` | `bare_pixel_scale()` | done |
| `texImage2D` updates every frame (video), `readPixels`, compute | | later |
| **Transforms and animation** | | |
| `transform: translate()` | `.offset` (sub-pixel smooth, children included) | done |
| `transform: scale()` | `.scale` (drawn and hit-tested) | done |
| `rotate()`, `transform-origin` | `.rotate`, `.origin` | M10 |
| `transition` with `cubic-bezier` | `.animate = BARE_EASE(0.3, BARE_EASE_OUT)` | done |
| spring physics, interruptible without jumps | `BARE_SPRING(response, damping)` | done, beyond |
| animate layout changes (FLIP by hand on the web) | `.animate_layout`: moved elements glide, even to another parent | done, beyond |
| `@keyframes`, looping animation | any value computed from `bare_time()` | C |
| enter / exit transitions | | later |
| **Text** | | |
| text, wrapping, size, colour (inherited), line-height, align, nowrap | `bare_text("...", .size = 20)`, `.text_color` | done |
| `@font-face` | `bare_font_load("x.ttf")`, `bare_font_from_memory` | done |
| letter-spacing, decoration, text-shadow, ellipsis | | M10 |
| complex scripts (Arabic, Indic), emoji | FreeType + HarfBuzz backend | later |
| `<input>`, `<textarea>`, selection | text input and editing | later (needed for ScreenPlus) |
| **Interaction (the JS part)** | | |
| `:hover`, `:active` | `.hover = {...}`, `.press = {...}` looks, animated by `.animate` | done |
| `onclick`, `oncontextmenu`, hover | `if (bare_box(...).clicked)`: the return value; clicks bubble | done |
| `cursor`, `pointer-events: none` | `.cursor`, `.passthrough` | done |
| keyboard events | `bare_key_down`, `bare_key_pressed` (layout-aware letters) | done |
| `dblclick`, mouse buttons 4/5, `mousedown` coordinates for drags | | M8 |
| clipboard | `bare_clipboard()`, `bare_set_clipboard()` | done (API) |
| state, `useState`, re-render | your C variables; every frame redraws from them | C |
| CSS classes, variables, themes | `.style = &card`, theme styles | M7 |
| buttons, checkboxes, sliders | `bare_button` ... | M8 |
| focus, tab order, drag and drop, touch, IME | | later |
| accessibility (screen readers) | platform a11y trees | later |
| runs in a browser | sokol backend + Emscripten | M9 |
| **Cost** | | |
| idle CPU and GPU | 0 (sleeps until input; a page's requestAnimationFrame loop never stops) | beyond |
| CPU and memory while animating | Fluids: ~3x less CPU, ~4-5x less memory than Edge (bench/README.md) | beyond |
| size | one ~4 MB exe with SDL3 static, no runtime | beyond |
| startup | ~0.6 s today, almost all GPU driver setup; target: first paint in tens of ms | not yet |

---

## Needed for the ScreenPlus port (`Wallpaper+\ScreenPlus`, ~9k lines of Direct2D/DirectWrite UI in `src/ui/`)

To be scheduled once M1–M9 land, after an inventory of ScreenPlus's screens: text input and editing (it has
`textedit.c`), flex-wrap or grid for the thumbnail library, textures updated every frame (GIF/video previews; canvases exist,
updating an image's pixels doesn't yet),
FreeType + HarfBuzz for DirectWrite-level text, Linux (SDL3 already covers it).

## Later (not scheduled)

Grid layout · text input and editing · keyboard focus and tab navigation · IME · drag and drop · touch gestures ·
multi-window (`bare_window_new`, `bare_use`) · floating layers (`.layer`) for popups and tooltips · exit/appear
transitions · rounded clipping · OKLab colour interpolation · FreeType + HarfBuzz backend · accessibility (platform
a11y trees) · scripting.
