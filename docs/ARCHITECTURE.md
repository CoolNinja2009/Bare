# Bare architecture

Bare is a UI framework in C11 that gives native apps flexbox layout, CSS-like styling, smooth animation and
interaction, drawn on the GPU, with a tiny binary and millisecond startup. This document is the design; the public API
is in [`include/bare.h`](../include/bare.h) and the build order in [ROADMAP.md](ROADMAP.md).

## 1. Design rules

1. **As simple as raylib.** The smallest program is a window, a `while (bare_frame())` loop and one `bare_box(...)`.
   Users never see draw commands, renderers, arenas or GPU types unless they include the advanced headers. Before
   adding API, ask whether it can be one call with good defaults.
2. **Named, optional parameters.** Elements take a struct through a macro: `bare_box(.width = 300, .radius = 16)`.
   **0 always means "default"**, so no macro needs to pre-fill anything (pre-filling and overriding fields makes GCC and
   Clang warn under `-Wextra`, and pragmas can't silence that inside an expression). Where the default isn't 0
   (opacity, scale, grow, shrink, text size), `BARE_ZERO` (`-0.0f`, told apart with `signbit`) means a real zero.
3. **Layered.** The core never includes SDL, sokol or GPU headers. It turns declared UI into a flat list of draw
   commands. Backends turn those into pixels and feed input back.
4. **Immediate API, retained state.** The app declares its UI every frame; Bare keeps per-element state (animations,
   hover/press, scroll) across frames, keyed by stable element IDs.
5. **No malloc in the steady-state frame.** Transient data lives in a per-frame arena. Retained tables grow only when
   the UI grows, then stay at their high-water mark.
6. **Headless-testable core.** Layout, easing, springs, arenas, IDs, hit testing and draw-command output run in unit
   tests with no window or GPU.
7. **Fix the abstraction, not the backend.** If a backend needs something the core doesn't give, change the interface
   instead of adding a special case.

## 2. Modules

```
include/
  bare.h               public API (the only header most apps include)
  bare/bare_core.h     advanced: headless context, draw list types, font backend interface
  bare/bare_backend.h  advanced: BareRenderer, BarePlatform, BareEvent
src/
  core/                no platform headers, ever
    arena.c            bump allocator: per-frame arena and persistent arena
    id.c               FNV-1a hashing for BARE_ID / BARE_IDI / automatic IDs
    store.c            retained node table: id -> BareNode (open addressing); per-frame maps
    tree.c             element tree built while the app declares UI (open/close/leaf), IDs
    style.c            .style chains, defaults, hover/press looks, glass resolution, the themes
    layout.c           flexbox passes
    easing.c           cubic-bezier evaluation
    spring.c           closed-form damped spring step
    anim.c             animation channels per node, retargeting, idle detection
    input.c            event queue -> mouse/keys state, press/click/scroll bookkeeping
    hit.c              hit list and hit testing (clip + paint order)
    focus.c            keyboard focus, Tab order, the ordered key / text queue, clipboard hooks
    edit.c, edit.h     the editing engine: actions, the binding table, words, undo / redo
    field.c            bare_input: the field box, mouse selection, caret blink, IME composition
    caret.c            positions in wrapped text: offset <-> caret rect, hit testing, selection rects
    scroll.c           scroll boxes: wheel routing and hand-off, the offset spring, scrollbars, bare_visible
    text.c             UTF-8, line breaking, rich text runs, glyph cache and atlas (font backend below)
    font.c             font handles and families: the built-in faces, bare_font_add_face, .weight / .italic
    font_stb.c         stb_truetype implementation of the font backend
    doc.c, doc.h       BareDoc: lines (copy on write from the loaded file), selections, undo groups, find
    doc_edit.c         the editor's commands on every caret: typing, pairs, indent, line moves, paste
    doc_view.c         bare_editor: visible lines only, wrapped rows, the gutter, find bar, mouse
    image.c            image handles and pending texture uploads (decoding via stb_image)
    emit.c             tree -> BareDrawList
    gpu.c              images, canvases, shaders and passes: GPU ops queued in call order
    context.c          BareContext, frame begin/end, the current-context pointer
    widgets.c          button, checkbox, slider, list item: built only from public elements
  bare.c               the simple API: bare_window / bare_frame / bare_run / bare_quit on top of
                       the core context and the backend chosen at build time
  backend/
    sdl3/platform_sdl3.c     BarePlatform on SDL3
    sdl3/renderer_sdl3gpu.c  BareRenderer on the SDL3 GPU API
    sokol/                   (M9) platform_sapp.c, renderer_sgfx.c
shaders/
  bare.glsl            boxes, glyphs and images (vertex + fragment)
  glass.glsl           liquid glass
  pass.glsl            the vertex half of every canvas pass (one fullscreen triangle)
  generated/           compiled blobs as C headers (SPIR-V, DXIL, MSL), committed
assets/fonts/          the default font (Inter, Latin subset, OFL), embedded at build time
tests/                 headless unit tests (no SDL)
examples/              hello.c, cards.c, ... one feature per example; fluid/ has its own shaders
bench/                 benchmarks against the browser (bench/README.md)
cmake/                 BareShaders.cmake: bare_add_shaders() for apps' own shaders
```

CMake targets: `bare_core` (static library, core only, no dependencies beyond libc/libm) and `bare` (core + `bare.c` +
the selected backend). Tests link `bare_core` only.

## 3. The frame lifecycle

```
bare_frame() #N
 ├─ end frame N-1 (skipped on the first call)
 │    1. close the root; report unclosed containers (file:line where opened)
 │    2. layout        flexbox passes over the tree           -> target rects
 │    3. animate       move every channel toward its target   -> visual values
 │                     then .offset / .scale transforms        -> drawn rects
 │    4. hit list      visual rects + clips in paint order    -> kept for frame N
 │    5. emit          tree + visual values                   -> BareDrawList
 │    6. publish       GPU ops queued this frame (canvases)   -> list.ops
 │    7. render        renderer->render(draw list): ops first (backend)
 │    8. evict         drop nodes not declared in frame N-1
 └─ begin frame N
      9. wait/poll     platform->poll_events; block if idle, at most until the wake time (see §11)
     10. input         apply events; hit-test the mouse against the frame N-1 hit list;
                       update hovered / pressed / clicked / scroll targets per node;
                       step scroll offsets toward their targets (so the app sees this frame's)
     11. reset         frame arena; open the root element (window size, column)
     12. return true   ... the app declares UI; each element reads its BareState now
```

`bare_run(frame_fn, user)` runs exactly the same steps, but the platform calls `frame_fn` (sokol_app, the web). Both
forms go through one internal pair, `bare_ctx_begin` / `bare_ctx_end`, so they can't diverge.

Why hit testing straddles the frame boundary: the app needs `clicked` *while declaring* the element, before this frame's
layout exists. So input is resolved at the start of the frame against what the user actually saw: last frame's
*visual* (animated) rects. Clicks therefore have no added latency. A hover that results from layout moving under a still
mouse shows one frame later (16 ms), which is not perceptible.

## 4. Element IDs and retained state

### IDs

`BareId` is 64-bit. Sources, in order of priority:

| Source | How it's made |
|---|---|
| `.id = BARE_ID("save")` | `hash(name)`, global: the same name anywhere is the same element |
| `.id = BARE_IDI("row", i)` | `hash(name, i)`, for list items |
| automatic (`.id` = 0) | `hash(parent_id, file, line, n)`, where `n` counts how many times that call site has run under this parent this frame |

Automatic IDs are stable as long as the UI structure is stable, which covers static UI. Elements in data-driven lists,
or shown conditionally *before* siblings from the same call site, should get explicit IDs; the docs and examples say so.
Debug builds detect a duplicate ID within a frame and log both call sites.

### Retained nodes

`store.c` holds a hash table `BareId -> BareNode`, open addressing with linear probing and backward-shift deletion (no
tombstones). It lives in the persistent arena and doubles only when the load factor passes 0.7.

```c
typedef struct BareNode {
    BareId    id;
    uint64_t  last_frame;        /* frame it was last declared in; evicted when stale */
    BareRect  layout_rect;       /* last target rect (window coordinates) */
    BareRect  visual_rect;       /* last drawn rect (after animation and scale) */
    uint32_t  flags;             /* hovered, pressed, clicked, right_clicked, ... */
    BareVec2  scroll, scroll_to, scroll_v;   /* drawn offset, target, spring velocity (scroll.c) */
    BareVec2  content, view;                 /* last layout: content and box size, for input between frames */
    uint32_t  anim;              /* index + 1 of its animation channels (§7); 0 = never animated */
} BareNode;
```

A node is created the first time its ID is declared, with every animation channel already at its target (nothing flies
in from 0). It is evicted at the end of a frame in which it wasn't declared. Exit transitions (keeping a node alive to
animate out) are deferred; the eviction step is where they will hook in.

### The per-frame tree

While the app declares UI, `tree.c` appends `BareElem` records to an array in the frame arena, **in pre-order**:

```c
typedef struct BareElem {
    BareId    id;
    uint32_t  node;                      /* index of the retained BareNode */
    int32_t   parent, first_child, next_sibling, child_count;
    BareBox   box;                       /* resolved copy: defaults, style, looks applied */
    BareText *text;                      /* text payload or NULL (arena) */
    float     min_w, min_h;              /* intrinsic sizes (layout pass 1/4) */
    BareRect  rect;                      /* layout result */
} BareElem;
```

Pre-order matters: every child comes after its parent, so **iterating the array backwards visits children before
parents** (bottom-up passes) and forwards visits parents first (top-down passes). Layout and emit need no recursion and
no explicit stack. Strings are copied into the frame arena, so the app's buffers only need to live for the call.

## 5. Styles and themes (`style.c`)

For every element, in this order: start from zero → walk its `.style` chain to the base, then lay the styles from the
base up (each non-zero field overwrites) → lay the element's own non-zero fields on top → apply built-in defaults to
fields still at 0 → if hovered, overlay non-zero `.hover` fields; if pressed, overlay `.press` (on top of hover) →
resolve `BARE_ZERO` to 0. "Non-zero" means any bit set, so `BARE_ZERO` (-0.0f), `BARE_CLEAR` (transparent white),
`BARE_START` (1), `BARE_CURSOR_ARROW` and `BARE_INSTANT` (an ease of duration `BARE_ZERO`) are values an element can
use to cancel a style's. Inherited values (text colour, font) come from the nearest ancestor that sets them, else the
theme. The result is the element's **target** look; animation moves the visual look toward it.

Merge units: most fields one by one; groups that only make sense together are taken whole from the most specific
place that sets any of them: padding (`.padding` and `.pad` as one unit, so an element's `.padding` replaces a style's
`.pad`), `.shadow`, `.offset`, `.glass`, `.animate`, `.animate_layout`. `.hover` and `.press` are merged field by
field, so a style's hover colour and an element's hover scale combine. `.id`, `.dir` and `.style` itself always come
from the element. Text styles (`BareTextStyle`) follow the same rules. Chains are walked up to 8 deep; a cycle (or a
deeper chain) is cut where it repeats and logged once per style with the element's file:line.

Themes: `BareTheme` is colours plus ready-made styles (panel, card, button, primary, row; title, heading, body,
caption). The current theme is a copy inside the context, so `&bare_theme()->card` is a stable address whose values
change when `bare_set_theme` is called; elements using it then animate to the new values, because theme styles carry
`.animate` (0.2 s ease-out on looks, instant layout). The theme's background is the clear colour unless the app called
`bare_background`, and its text colour is the root text colour.

Padding: `.pad` (per side) wins if any side is non-zero, else `.padding` on all sides.

Group opacity is approximated: an element's opacity multiplies into its own colours and its children's. Overlapping
translucent children can show through each other where a browser would composite the group first.

## 6. Layout (`layout.c`)

Flexbox, single-line (no wrap of children, which comes with grid later). `BARE_STACK` (`bare_stack`) is the third
direction: children share the content box as layers (later ones in front), placed by `.align` on both axes.
`.offset` is visual and never affects layout. Main axis = `.dir`, cross axis = the other.
Size values: fixed pixels, `BARE_FIT` (content), `BARE_GROW` (share of free space by `.grow`), `BARE_PERCENT(p)`
(encoded as a negative float ≤ -2, decoded in `layout.c`). Min/max clamp every result.

`BARE_GROW` behaves like CSS `flex: 1`: basis 0, a share of the free space by `.grow`, never below the element's
min-content size; a child clamped at its min or max is frozen and the rest is re-split (the CSS algorithm). Shrinking
applies to content-sized and growing children only. Unlike CSS (where a `width: 200px` flex item still shrinks down to
its min-content), pixel and percent sizes never shrink: in Bare a number is a promise. Across the main axis, `BARE_GROW` fills the
parent, and `BARE_STRETCH` makes content-sized children fill it.

Passes (each a straight loop over the pre-order array):

| # | Direction | Pass |
|---|---|---|
| 1 | bottom-up | **fit widths**: each element's content width (text: widest unbreakable run as minimum, unwrapped line as preferred); containers sum children along a row (+ gaps + padding) or take the max in a column |
| 2 | top-down | **resolve widths**: fixed and percent first; then distribute the free space of each row among `BARE_GROW` children by `.grow`, or take overflow back from shrinkable children by `.shrink` × size, never below their minimum; in a column, `BARE_GROW` / `BARE_STRETCH` fill the cross axis |
| 3 | top-down | **wrap text** at the final widths → text heights |
| 4 | bottom-up | **fit heights** (as pass 1, with the axes swapped) |
| 5 | top-down | **resolve heights** (as pass 2) |
| 6 | top-down | **position**: padding, `.justify` along the main axis (start/center/end/space-*), `.align` across, gaps; scroll boxes clamp their offset to the content and place their scrollbars |

Widths are fully resolved before heights because text height depends on width. Rects are in logical pixels, window
coordinates; snapping to physical pixels happens in `emit.c`.

Text is shaped when it's declared (`bare__text_new`), so pass 1 knows its unwrapped width and its longest unbreakable
run; pass 3 wraps it at its final width. Shaping goes through the context's font backend (§13), so layout tests use a
fake monospace backend and never need a real font.

**Absolute children** (`.position = BARE_ABSOLUTE`) are linked to their parent but kept out of its sibling chain
(like scrollbar rails), so they take no room and don't count for gaps. Passes 2/5 size them from the parent's box
inside its border (fixed, percent of it, `BARE_GROW` fills it, content gets fit-content); pass 6 places them at
`.anchor` (one of nine points, the same point of parent and child) plus `.x`, `.y`. They paint in declaration order.

A content-sized (`BARE_FIT`) child across its parent's main axis, or in a stack, gets CSS's fit-content:
`min(preferred, max(min-content, available))`. That is what makes a paragraph in a column wrap at the column's width
instead of overflowing it.

## 7. Animation (`easing.c`, `spring.c`, `anim.c`)

### What animates

An element animates when it sets `.animate` (and optionally `.animate_layout`). Its node then gets a set of
**channels**, one float each, from a pool in the context (sets are recycled when nodes are evicted, so most elements,
which never animate, cost nothing):

| Group | Channels | Spec used |
|---|---|---|
| layout | x, y (relative to the parent), width, height | `.animate_layout`, else `.animate` |
| look | background rgba, border rgba, text rgba, shadow rgba (0..255), border width, radius, opacity, scale, offset x/y, shadow x/y/blur/spread | `.animate` |
| glass | amount (1 = glass, 0 = none), refraction, depth, blur, dispersion, shine, zoom, light, tint rgba | `.animate` |
| scroll | scroll x/y | built-in critically damped spring (`scroll.c`, stepped at frame begin, not a channel set) |

`merge` doesn't animate. Positions animate relative to the parent: the target is the element's layout
position minus its parent's, and the drawn position is the parent's *drawn* position plus the channel. So when a parent
moves, its children move with it rigidly instead of each lagging on its own, and elements that don't animate at all are
still carried by an animating parent. Scale and offset are visual-only transforms (like CSS `transform`): they never
change layout, apply to the element and its children, and hit testing uses the transformed rects.

```c
typedef struct BareChannel {
    float value, velocity;   /* what's drawn; units per second */
    float target;
    float from, t;           /* easing: where it started and progress 0..1 */
} BareChannel;

typedef struct BareAnimSet {
    BareChannel ch[BARE__CH_COUNT];   /* 30 */
    uint64_t    active;               /* bit per channel still moving */
    BareId      parent;               /* the parent x/y were relative to last frame */
    BareVec2    abs;                  /* last drawn position, window coordinates */
    bool        ready;
} BareAnimSet;
```

### Each frame

After layout, `anim.c` walks the elements top-down. The layout rect and the resolved looks (hover and press included)
are the targets; the element's rect and looks are overwritten with the animated values, and the layout rect is kept in
`BareElem.target`. Transforms, hit testing and emit only ever see the animated values.

- **A new set** (first appearance, or the first frame an element asks for animation) snaps every channel to its target:
  nothing flies in from 0.
- **No animation** (`kind == BARE_ANIM_NONE`): value = target.
- **Retarget**: when a target differs from the channel's, the channel starts from wherever its value *is*.
- **Easing**: `t += dt / duration`, `value = lerp(from, target, bezier(t))`. Interrupting restarts from the current
  value, so there's never a jump in position (velocity may change abruptly; springs avoid even that). The bezier is
  solved for x with Newton steps plus a bisection fallback, as browsers do. Defaults: 0.25 s, CSS `ease`
  (0.25, 0.1, 0.25, 1). The ease's velocity is tracked too, so a spring can take over smoothly.
- **Spring**: parameterised like SwiftUI: `response` (period in seconds, default 0.35) and `damping` ratio (default 1 =
  critical, < 1 bounces; clamped to ≥ 0.02 so it always settles). Stiffness `k = (2π / response)²`, mass 1. Each frame
  advances `(value - target, velocity)` with the **closed-form** solution for the under-, critically- or over-damped
  case, so it is exact and stable at any `dt` (one 0.1 s step equals ten 0.01 s steps). Retargeting keeps value *and*
  velocity: interruption is smooth in both.
- **Settling**: a spring channel closer to its target than its threshold (0.01 px, 0.25 colour units, 0.001 opacity,
  0.0001 scale) and slower than 10 thresholds per second snaps there and clears its bit; an ease lands when `t` reaches 1.
- **BARE_ZERO** duration or response means instant.
- **Transparent ends**: fading a colour to alpha 0 animates only its alpha, and fading in from alpha 0 starts from the
  new colour (CSS interpolates premultiplied colours, which has the same effect): red fading out stays red. A shadow's
  colour governs its geometry the same way, so a shadow fades in place.
- **Glass coming and going**: turning glass off fades its *amount* (the shader draws `glass * amount`, the shadow
  separately) while refraction, frost, fringes, zoom and tint fade with it and the rim's depth, shine and light stay put,
  so it dissolves into the backdrop; it's drawn until the amount reaches 0. Turning it on from nothing starts with the new
  shape and light and no bending, so it condenses.
- **Moving to another parent** (same explicit id): the channel is re-expressed relative to the new parent from the
  last drawn position, so the element glides from where it was.
- **Snapping**: an element whose position or size is animating (or an ancestor's) is drawn unsnapped, like scaled ones,
  so slow motion glides instead of stepping whole pixels; it lands on the pixel grid when it settles.
- **Time**: `dt` is the frame interval, clamped to 1/20 s. Coming out of idle the clock has run on while nothing moved,
  so the first step is one typical frame (the last measured animating interval, else 1/60 s), not the whole sleep.

Colours animate per component in sRGB (as CSS does); OKLab interpolation can come later without API change.

`bare_ctx_end` reports whether any channel is still active, which drives idle sleeping (§11).

## 8. Interaction (`input.c`, `hit.c`)

**Hit list.** At the end of each frame, `hit.c` records every element as drawn (after `.offset` and `.scale`), in
paint order, with the same indices as that frame's elements: `{id, parent, node, visual rect, radius, clip rect,
passthrough, scroll}`. The clip rect is the intersection of all clipping (or scrolling) ancestors. It is a persistent
buffer reused every frame.

**Hit testing.** Input events (`bare_ctx_event`) arrive between frames and are tested against that list. Walk it
**backwards** (topmost first) and take the first entry that isn't `.passthrough` whose rect and clip contain the
point. Rounded corners count. Passthrough removes only the element itself; its children can still be hit. The result
is a *chain*: the element and all its ancestors, by ID.

**Hover.** At the start of a frame (`bare__input_begin`) the chain under the mouse is the hover chain: every element in
it is `hovered` (like CSS `:hover`). Looks (`.hover`, then `.press` on top) are applied while the element is declared,
so they show in the same frame.

**Press and click.** On button down, the chain under the mouse is remembered; its elements are `pressed` while the
button is held, wherever the mouse goes. On button up, the elements in both that chain and the chain under the mouse
now are `clicked` (the DOM rule: the nearest common ancestor of the down and up targets, bubbling up). One click per
press; dragging off and back on still clicks, dragging off and releasing doesn't. The right button sets
`right_clicked` the same way. Losing window focus releases all keys and buttons.

**Extra frames.** `bare_ctx_end` asks for another frame after a click or key press (the app may have declared
elements before reacting to it) and when the element under a still mouse changed (layout moved), so hover is never
stale while idle.

**Wheel (`scroll.c`).** Each axis goes to the innermost scroll box in the chain under the mouse that can still move
that way; one at its end passes it on (browsers' scroll chaining). Shift turns a vertical wheel sideways. A notch is
100 px (`BARE_WHEEL_NOTCH`, as Chrome on Windows) and moves only the node's target: the drawn offset follows on a
critically damped spring (response 0.15 s), stepped at the *start* of the next frame so `BareState.scroll`,
`bare_visible` and layout all see the value that frame draws. `precise` deltas (touchpads: SDL reports fractions of a
notch) move target and offset together, so the content follows the fingers. Targets are clamped to
`content - view` from the last layout; layout clamps again when the content shrinks.

**Scroll offsets are a transform, not layout.** Layout positions a scroll box's children unscrolled;
`bare__transforms` translates them by the offset rounded to whole physical pixels (so scrolled text stays sharp and
never shimmers). Children with `.animate_layout` therefore don't chase the scroll on their springs.

**Scroll boxes in layout.** Like CSS, a box that clips or scrolls has a min-content size of just its padding, so a
column can squeeze it to the space left and it scrolls the rest; along a scroll axis its children never shrink.

**Scrollbars.** When a scroll box closes, Bare appends for each overflowing axis a *rail* (the thumb's hit area,
14 px thick) holding a *bar* (what shows). The rail is linked to the box as its parent but left out of the sibling
chain, so the flex passes never see it; layout sizes it as the thumb and pins it to the box's far edge, and transforms
don't scroll it. Being real elements, they are drawn and hit on top of the content and get hover and press for free.
The bar fades and widens through `.animate`. It shows for a second after the box scrolls or the mouse moves over it
(the core asks for a frame at that time: `bare_ctx_wake_time`), and while hovered or dragged. Pressing a rail grabs
the thumb; mouse moves then set offset and target directly.

**Long lists (`bare_visible`).** A loop macro: it reads the box's offset and last view, declares items
`first..last` (one spare at each end) and sets `lead` / `tail` on the box: the length of the undeclared items before
and after the run, which layout adds to the fit size, the free space and the position cursor. So positions and the
content size equal those of the full list, and the scrollbar is right. Cost while scrolling 10,000 rows: ~14 us of core
time per frame (vs ~8.5 ms declaring them all).

**Cursor.** The innermost hovered element's `.cursor` (`bare_ctx_cursor`), sent to the platform after each frame.

**Z-order.** Paint order is declaration order. Floating layers (popups, tooltips drawn above everything) are deferred,
and will be a `.layer` field that sorts the hit list and the draw list.

### Keyboard focus and text editing (`focus.c`, `edit.c`, `field.c`, `caret.c`)

Typing is built in: an app passes a buffer to `bare_input` and never maps a key itself.

**Focus.** One id has focus (`focus_id`). A press focuses the innermost `.focusable` element in the pressed chain
(scrollbars excepted) or clears focus; `bare_focus(id)` sets it. Tab / Shift+Tab walk the focusable entries of last
frame's hit list, which is in pre-order, i.e. declaration order, and scroll the new one into view. Escape blurs.
Each focusable element reports itself while declared; a focus whose element wasn't declared is dropped at frame end.
`.focus` is a look like `.hover`, applied between hover and press.

**The key queue.** Keys, text (`BARE_EVENT_TEXT`), IME composition and `bare_edit` actions go into one ordered
queue, so "type, Ctrl+Z, type" replays in order. When a field has focus it takes the queue (`in_take` entries) while
it's declared; `bare_key_pressed` no longer sees keys the field used, so shortcuts underneath don't fire. A Tab in
the middle splits the queue: the rest goes to the next field next frame. With no field focused only plain keys stay.

**Bindings.** `edit.c` has the default shortcut table as data (`BareKeyBinding`: key, modifiers, action), tagged for
all platforms, PC or macOS; `BARE_MOD_SHORTCUT` is Ctrl or Cmd. Lookup tries an exact match, then Shift + a
movement action extends the selection. `bare_edit_bind` adds or replaces entries; `bare_ctx_set_mac_keys` switches
the table (tests run both).

**The engine** works on the app's buffer in place, byte offsets on code point boundaries: caret, anchor, a goal
column for up / down, and affinity (`upstream`) for the two caret positions at a soft wrap. Every change goes through
one `replace()`, which records undo: records live in a byte pool, typing / Backspace / Delete runs merge until a word
starts, at most 1000 records (the oldest half dropped). Paste turns CRLF into LF, and line breaks into spaces in a
single-line field; `cap` is respected without splitting a code point.

**The field** (`field.c`) is a box (`theme.input` look, focusable, scrolling sideways or down without scrollbars)
holding one text element. Its state (`BareEditState`) is allocated on first focus and lives on the node. Password
fields lay out bullets and map offsets between shown and real text; the IME composition is spliced into the shown
text and underlined. Layout is cached by a generation counter. After layout, the focused field scrolls instantly to
keep the caret in view and tells the platform where the caret is (`bare_ctx_text_input`: kind + rect, used for
`SDL_StartTextInput` and the IME window). Emit draws selection rects under the glyphs and the caret over them.
The caret blinks 0.53 s on / off by wake times, not continuous frames, and stops blinking after 10 s idle, so a
focused field costs nothing.

**Text geometry** (`caret.c`) is shared by every text element: offset to caret rect (half-leading, ascent +
descent tall), point to offset, line of an offset, selection rects per line. Clicks, drags (by character, word on
double click, line on triple click) and arrows all use it.

## 9. Draw commands (`emit.c`)

The core's only output. Coordinates are **physical pixels**, already snapped, so backends never deal with DPI.

```c
typedef enum BareCmdKind { BARE_CMD_RECT, BARE_CMD_TEXT, BARE_CMD_IMAGE,
                           BARE_CMD_CLIP_PUSH, BARE_CMD_CLIP_POP } BareCmdKind;

typedef struct BareColorF { float r, g, b, a; } BareColorF;  /* sRGB, straight alpha, opacity applied */

typedef struct BareRectCmd {
    BareRect   rect;
    BareColorF fill, border_color;
    float      radius[4];          /* top-left, top-right, bottom-right, bottom-left */
    float      border;
    BareRect   shadow_offset_blur; /* x, y, blur, spread */
    BareColorF shadow_color;
} BareRectCmd;

typedef struct BareGlyph { BareRect dst, uv; } BareGlyph;
typedef struct BareTextCmd  { uint32_t atlas; BareColorF color; const BareGlyph *glyphs; int count; } BareTextCmd;
typedef struct BareImageCmd { uint32_t texture; BareRect rect, uv; float radius[4]; BareColorF tint; } BareImageCmd;
typedef struct BareClipCmd  { BareRect rect; } BareClipCmd;  /* already intersected with the enclosing clip */

typedef struct BareGlassShape { BareRect rect; float radius; } BareGlassShape;
typedef struct BareGlassCmd {                 /* liquid glass, see §10 */
    const BareGlassShape *shapes; int count;  /* up to 8, melted together over `merge` px */
    float refraction, depth, blur, dispersion, shine, merge;
    BareColorF tint; float shadow[4]; BareColorF shadow_color; float opacity;
} BareGlassCmd;

typedef struct BareDrawCmd {
    BareCmdKind kind;
    union { BareRectCmd rect; BareTextCmd text; BareImageCmd image; BareClipCmd clip; BareGlassCmd glass; } as;
} BareDrawCmd;

typedef struct BareTextureUpload {
    uint32_t texture;              /* core-assigned id */
    int      width, height;        /* full texture size (create or resize when new) */
    int      format;               /* BARE_TEX_R8 (glyph atlas) or BARE_TEX_RGBA8 */
    BareRect region;               /* part of the texture to upload */
    const void *pixels; int stride;
    bool     destroy;              /* free the texture instead */
} BareTextureUpload;

typedef struct BareDrawList {
    const BareGpuOp         *ops;     int op_count;       /* images, shaders, canvas passes and draws: run first, in order */
    const BareTextureUpload *uploads; int upload_count;   /* then these (glyph atlas) */
    const BareDrawCmd       *cmds;    int cmd_count;
    int        width, height;          /* framebuffer, physical pixels */
    float      scale;                  /* physical / logical */
    BareColorF clear;
} BareDrawList;
```

Emission walks the tree in pre-order: for each element, one `RECT` (background, border and shadow together, skipped if
all invisible), `IMAGE` if it has one, `CLIP_PUSH` if it clips, then its children, then `CLIP_POP`. A clip is the box
inside its border (CSS clips to the padding box), rounded to whole physical pixels and intersected with the enclosing
clip; it is popped after the box's last descendant (pre-order: the subtree of `i` is `i..end[i]`). An element whose
rect, grown by its shadow, is entirely outside the current clip emits nothing (its children are still visited: they
can overflow it); a clip with no area skips its whole subtree. Text elements emit
one `TEXT` per run. A glass element emits `GLASS` (which draws its shadow) and then a `RECT` for any `.color` /
`.border` on top; consecutive glass siblings with `.merge` share one `GLASS` command so they melt together.

Rects are snapped to whole physical pixels at their *layout* position, then `.offset` (accumulated from ancestors) is
added unsnapped, so static UI stays crisp and moving UI glides instead of stepping a pixel at a time.

**Textures are data, not calls.** The core never calls the renderer. It assigns texture IDs itself and lists the
uploads a frame needs (new glyphs in the atlas, newly created images, frees) at the front of the draw list. The renderer
keeps an `id → GPU texture` map. This keeps the core pure (and tests can assert on uploads), and keeps renderers simple.
Rounded clipping is deferred: clips are rectangles (scissor).

## 10. Backend interfaces (`include/bare/bare_backend.h`)

A backend is a **platform + renderer pair** chosen at build time (`-DBARE_BACKEND=SDL3` or `SOKOL`). The renderer
may ask its platform for native handles (the SDL window, the sokol swapchain), which is why they are paired.

```c
typedef enum BareEventKind {
    BARE_EVENT_MOUSE_MOVE, BARE_EVENT_MOUSE_BUTTON, BARE_EVENT_WHEEL,
    BARE_EVENT_KEY, BARE_EVENT_RESIZE, BARE_EVENT_SCALE, BARE_EVENT_FOCUS, BARE_EVENT_QUIT
} BareEventKind;

typedef struct BareEvent {
    BareEventKind kind;
    double time;
    union {
        struct { float x, y; } mouse;                     /* logical pixels */
        struct { int button; bool down; float x, y; } button;
        struct { float dx, dy; bool precise; } wheel;     /* precise = touchpad pixels, else lines */
        struct { int key; bool down, repeat; int mods; } key;
        struct { float w, h, scale; } size;
        bool focused;
    };
} BareEvent;

typedef struct BarePlatform {
    void *self;
    bool   (*open_window)(void *self, int w, int h, const char *title);
    void   (*close_window)(void *self);
    /* Fills `out` with up to `max` events; blocks up to `wait` seconds if none (0 = poll). */
    int    (*poll_events)(void *self, BareEvent *out, int max, double wait);
    void   (*wake)(void *self);                          /* thread-safe: unblock poll_events */
    void   (*window_size)(void *self, float *w, float *h, float *scale);
    double (*time)(void *self);                          /* seconds, monotonic */
    void   (*set_cursor)(void *self, int cursor);
    void   (*set_title)(void *self, const char *title);
    const char *(*clipboard_get)(void *self);
    void   (*clipboard_set)(void *self, const char *text);
    void  *(*native)(void *self, int what);              /* e.g. BARE_NATIVE_SDL_WINDOW */
    /* Platforms that own the main loop (sokol_app, web) set this; others leave it NULL. */
    void   (*run)(void *self, void (*frame)(void *ctx), void *ctx);
} BarePlatform;

typedef struct BareRenderer {
    void *self;
    bool (*init)(void *self, const BarePlatform *platform);
    void (*render)(void *self, const BareDrawList *list);  /* uploads, draw, present */
    void (*shutdown)(void *self);
} BareRenderer;
```

The renderer interface is three functions because the draw list carries everything else. Plain `bare_frame()` on a
platform whose `run` is set fails with a clear message telling the user to call `bare_run`.

### The SDF renderer

One shader program, **instanced**: every rect, glyph and image is one instance of a unit quad (instance-rate vertex
attributes, not storage buffers, so the same design runs on WebGL2 through sokol).

Instance data (≈ 112 bytes): rect, per-corner radii, fill, border colour and width, shadow (offset, blur, spread,
colour), uv rect, and a mode (0 = SDF box, 1 = glyph from the R8 atlas, 2 = image with rounded mask).

- The vertex shader grows the quad to cover the shadow (`blur·3 + spread + |offset|`).
- The fragment shader evaluates the rounded-box SDF (per-corner radius) for fill and border, anti-aliased over one
  pixel with `fwidth`; the shadow is the closed-form Gaussian-blurred rounded rect (erf-based approximation, as in
  Evan Wallace's method), composited under the box in the same instance.
- Consecutive commands that share a texture become one draw call; only a texture change or a clip (scissor) splits a
  batch. A screen of cards and text is a handful of draw calls.
- Blending is in sRGB space on a non-sRGB swapchain, matching how browsers blend.

### Liquid glass

The renderer always draws into an offscreen **scene** texture and copies it to the window at the end. Before each
`GLASS` command it ends the render pass, copies the scene into a **backdrop** texture and generates its mipmaps, then
draws the glass with a second pipeline (`shaders/glass.glsl`, one quad per command, shapes and settings in a fragment
uniform block). The shader:

- evaluates the merged distance field (polynomial smooth-min of up to 8 rounded boxes) and its gradient (the normal);
- treats the outer `depth` pixels as a quarter-circle rim and shifts the backdrop sample inward by the rim's drop
  (`1 - sqrt(1 - x²)`): clear in the middle, bending toward the edge, like a convex lens;
- samples red, green and blue with slightly different shifts (dispersion), at a mip level for `blur` (frost);
- lifts saturation, applies the tint, adds a rim highlight from a fixed top-left light plus a bevel line;
- draws the merged shape's shadow outside the glass.

Cost: one full-frame copy + mip chain per glass command. Copying only the covered region is on the roadmap.

### Canvases and passes (`gpu.c`)

Images, canvases, app shaders and passes don't belong to the element tree: the app calls them whenever it likes,
and their order matters (a simulation is a chain of passes). So `gpu.c` appends each call to a **pending queue** of
`BareGpuOp`s (create / upload / destroy image, create / destroy shader, pass), copying params and pixels into the
queue's data buffer. At the end of the frame `bare__gpu_publish` swaps the pending and published queues, turns data
offsets into pointers and hands the published ops to the renderer as `list.ops`, which runs them in order before
drawing the frame. Both queues keep their memory, so the steady state allocates nothing; ops made between frames go
out with the next frame.

- Images and canvases share one id space (`BareImage`); ids are reused through a free list. The core validates every
  call (live target and shader, inputs not the target, input count against the shader's sampler count, params size)
  and logs file:line instead of queueing a bad op.
- A pass draws one triangle covering the target (`shaders/pass.glsl` gives `v_uv`, top-left origin) with the app's
  fragment shader. The renderer keeps a pipeline per (shader, target format, blend). `BLEND_NONE` passes don't load
  the old contents (DONT_CARE). They don't cycle the texture either: a ping-pong canvas written dozens of times a
  frame would get a new copy of its memory for every write (418 MB in the Fluids benchmark before this was removed).
- App shaders come from `bare_add_shaders()` (`cmake/BareShaders.cmake` + `bare_shader_header.cmake`): glslc, then
  shadercross for DXIL, MSL and a reflection JSON whose sampler and uniform-buffer counts go into `BareShaderCode`.

**Shader toolchain.** `shaders/bare.glsl` and `shaders/glass.glsl` (GLSL 450) are the sources. For SDL3: `glslc` → SPIR-V (Vulkan) and
`SDL_shadercross` → DXIL (D3D12) and MSL (Metal). For sokol (M9): a small `bare.shdc.glsl` wrapper `@include`s the
same source for `sokol-shdc`. The compiled blobs are committed in `shaders/generated/` as C headers, so building Bare
never needs a shader compiler; the `bare_shaders` CMake target regenerates them when the tools are installed.

## 11. Idle, time and threads

- When no channel is animating, no scroll is settling and `bare_continuous(false)` (default), `bare_frame` blocks in
  `poll_events` until input arrives or `bare_wake()` is called from another thread. Idle apps use no CPU and no GPU.
- Things timed to change while idle (a scrollbar fading a second after scrolling) set the context's wake time
  (`bare_ctx_wake_time`); `bare_frame` waits no longer than that, so one frame starts the change instead of drawing
  frames until it's due.
- `bare_time()` and `bare_dt()` come from the platform's monotonic clock. Animations use the clamped `dt`.
- Bare is single-threaded: call it from the thread that created the window. Only `bare_wake` is thread-safe.

## 12. Memory (`arena.c`)

- **Frame arena**: a bump allocator reset at the start of every frame. Holds the element tree, copied strings, glyph
  runs, the draw list. It's a chain of blocks; when a frame outgrows it, a block is added once and kept, so the steady
  state allocates nothing.
- **Persistent arena / tables**: node table, hit list, glyph cache, image registry. Grown by doubling only when the UI
  grows.
- Allocation goes through `BareAllocator` (malloc/realloc/free function pointers plus user pointer) in `BareConfig`,
  defaulting to libc.
- Debug builds poison the frame arena on reset to catch use of last frame's data.

## 13. Text (`text.c`, `font_stb.c`)

- **Default font**: a Latin subset of Inter 4.1 Regular (SIL OFL), 36 KB, embedded at build time by
  `cmake/embed_file.cmake`, so `bare_text` works with no setup. `-DBARE_DEFAULT_FONT=OFF` drops it for size-critical
  builds (then text needs `bare_font_load`). `assets/fonts/README.md` has the subsetting recipe.
- **Font backend interface** (in `bare_core.h`, chosen with `BareConfig.fonts`), shaped so FreeType + HarfBuzz can
  replace stb_truetype without touching layout or emit:

```c
typedef struct BareShapedGlyph { uint32_t glyph; float advance, x_offset, y_offset; uint32_t cluster; } BareShapedGlyph;
typedef struct BareGlyphBitmap { int width, height, left, top; const unsigned char *pixels; int stride; } BareGlyphBitmap;

typedef struct BareFontBackend {
    void *(*load)(const BareAllocator *alloc, const void *data, int size);
    void  (*metrics)(void *font, float px, float *ascent, float *descent, float *line_gap);
    int   (*shape)(void *font, float px, const char *utf8, int len, BareShapedGlyph *out, int cap);
    bool  (*rasterize)(void *font, uint32_t glyph, float px, float shift_x, BareGlyphBitmap *out);
    void  (*unload)(void *font);
} BareFontBackend;
```

  The stb version "shapes" by mapping code points to glyphs, with pair kerning from GPOS or `kern`. Sizes are em
  sizes, as in CSS. HarfBuzz will do real shaping.
- **A text element** is a leaf `BareElem` with a `BareText` payload in the frame arena: the string (copied), its
  shaped glyphs at the logical size, ascent and descent, the unwrapped width and the longest unbreakable run; layout
  pass 3 adds the lines. Line boxes are `size × line_height` tall with the font's box centred in them (CSS
  half-leading); `.align` places each line inside the element's width.
- **Line breaking** (greedy): at spaces, which hang off the line's end and are skipped at the next line's start; after
  hyphens; inside a word only when no break fits. `
` forces a break. Full UAX #14 comes with HarfBuzz.
- **Formatting**: `.transform` changes the copied string's case (ASCII and Latin-1, same byte length, so offsets
  hold); `.letter_spacing` is added to every advance after shaping, so carets agree; `.max_lines` drops lines after
  wrapping, and `.ellipsis` cuts the last kept line (or, with `.nowrap`, any line wider than the box) and marks it
  to end in "…" (shaped once; "..." if the font lacks it). A `.nowrap` + `.ellipsis` text can shrink to the ellipsis.
  `.underline` / `.strike` are rect commands after the glyphs, on whole pixels, in the text's colour.
- **Glyph cache**: open addressing keyed by (font, glyph, physical size in 1/64 px, sub-pixel phase). Still text is
  snapped to whole physical pixels with one of three horizontal sub-pixel phases, so it's sharp without uneven
  spacing; moving or scaled text (`.scale` animations stretch existing glyphs) is placed unsnapped at phase 0.
- **Atlas**: one R8 texture, 1024² growing to 2048² (old rows copied), packed by shelves (best-fitting shelf, else a
  new one) with a 1-pixel gutter so bilinear sampling never bleeds. Its texture id is an internal image id, so it
  never collides with the app's images. The core keeps the pixels; each frame's new glyphs become **one**
  `BareTextureUpload` covering their bounding box (the whole atlas when it's new or grew). Glyph uvs are emitted in
  pixels and normalised at the end of emit, because the atlas may grow mid-frame. When a 2048² atlas is full, the
  frame skips the glyphs that don't fit, logs once, and asks for another frame; the atlas starts over at the start
  of that frame.
- **Colour**: emit walks the tree once, carrying the nearest `.text_color` down (white at the top), so animated or
  hovered `.text_color` reaches the text inside.
- **Images** (`image.c`): stb_image decodes PNG, JPEG, BMP, GIF (first frame) and TGA into RGBA8, then
  `bare_image_from_pixels` queues them like any image (§9). stb's allocations go through the context's allocator.

## 14. Headless use and testing

`bare_core.h` exposes the context directly:

```c
BareContext *bare_ctx_create(const BareConfig *config);   /* allocator, frame arena block, font backend */
void         bare_ctx_make_current(BareContext *ctx);      /* the public bare_* calls act on this */
void         bare_ctx_begin(BareContext *ctx, float w, float h, float scale, double time,
                            const BareEvent *events, int event_count);
const BareDrawList *bare_ctx_end(BareContext *ctx, bool *still_animating);
void         bare_ctx_destroy(BareContext *ctx);
```

Tests create a context, feed synthetic events, declare UI with the **same public calls apps use**, and assert on
element rects, node state and the draw list. Time is passed in, so animation tests are deterministic. The test runner is
a single header (`tests/bare_test.h`) with no dependencies, one executable per area, registered with CTest.

Globals: the core has exactly one, the current-context pointer in `context.c` (so the public element calls work in
headless tests too). `bare.c` owns the app state and the backend pair.

**Capture.** With `BARE_CAPTURE=out.bmp` (and optionally `BARE_CAPTURE_FRAMES=N`, default 2) the SDL3 backend never
sleeps, saves frame N to the file and quits. It is how rendering is checked without screenshots, and the basis of the
cross-backend comparison in M9.

**Media.** `src/backend/media_win32.c` holds the system's decoders (Media Foundation, WIC, the shell's thumbnail
cache): `bare_picture` (`src/picture.c`), `bare_sound_load` (a whole file's sound to float samples) and
`bare_video_load` (`src/video.c`: one thread per video reading a frame at a time, shrinking each to the size asked
for, keeping a ring of a few frames just ahead of the time the app last asked for; a jump back or far ahead seeks).
Sounds play through `src/backend/sdl3/sound_sdl3.c`: each voice is fed from the sound's samples by SDL's get callback
(no second copy) and knows its position; on a fixed clock (`BARE_FIXED_DT`, `BARE_RECORD`) plays are kept on that
clock instead of being heard, so `bare_sound_position` follows `bare_time` and recordings stay in sync.
`src/thread.h` has the lock, condition and thread helpers both loaders use.

**Recording.** With `BARE_RECORD=out.mp4` `bare.c` runs the app on a fixed clock (`BARE_RECORD_FPS`, default 60:
frame n is at n / fps seconds, whatever the wall clock says), never sleeps, asks the renderer for every frame
(`BareRenderer.read_frames`: a `BareFrameSink` called with each presented frame's pixels) and quits after
`BARE_RECORD_SECONDS` (default 10). The pixels go to `src/backend/record_win32.c`, which encodes H.264 into an MP4
with Media Foundation's sink writer (it converts to YUV and uses the GPU's encoder when there is one). Because the
clock is the video's, a slow frame never drops or stutters, and the same app records the same video every run.
Other systems have no encoder yet (opening fails with a message).

Multi-window support later
(`bare_window_new`, `bare_use`) is one context per window, switched by `bare_use`.
