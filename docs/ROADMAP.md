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
- Springs from M4 for the motion; glass that wobbles/stretches with velocity (liquid feel).
- Light that responds to the content behind (adaptive tint and highlight), and optional pointer-driven light.
- Glass inside clips and scroll containers (M6), glass on text (M5).

## [ ] M2: Flexbox layout and draw commands

Keep from M1/M1.5: `BARE_STACK` layers and `.offset` (visual, ignored by layout).

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

## [ ] M3: Basic interaction

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

## [ ] M4: Animation system

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

## [ ] M5: Text rendering

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

## [ ] M6: Clipping and scroll containers

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

---

## Needed for the ScreenPlus port (`Wallpaper+\ScreenPlus`, ~9k lines of Direct2D/DirectWrite UI in `src/ui/`)

To be scheduled once M1–M9 land, after an inventory of ScreenPlus's screens: text input and editing (it has
`textedit.c`), flex-wrap or grid for the thumbnail library, textures updated every frame (GIF/video previews),
FreeType + HarfBuzz for DirectWrite-level text, Linux (SDL3 already covers it).

## Later (not scheduled)

Grid layout · text input and editing · keyboard focus and tab navigation · IME · drag and drop · touch gestures ·
multi-window (`bare_window_new`, `bare_use`) · floating layers (`.layer`) for popups and tooltips · exit/appear
transitions · rounded clipping · OKLab colour interpolation · FreeType + HarfBuzz backend · accessibility (platform
a11y trees) · scripting.
