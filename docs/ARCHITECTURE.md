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
    tree.c             element tree built while the app declares UI (open/close/leaf), IDs,
                       style resolution (defaults, BARE_ZERO; .style and looks join in M3/M7)
    layout.c           flexbox passes
    easing.c           cubic-bezier evaluation
    spring.c           closed-form damped spring step
    anim.c             animation channels per node, retargeting, idle detection
    input.c            event queue -> mouse/keys state, press/click/scroll bookkeeping
    hit.c              hit list and hit testing (clip + paint order)
    text.c             UTF-8, line breaking, glyph cache and atlas (font backend below)
    font_stb.c         stb_truetype implementation of the font backend
    image.c            image handles and pending texture uploads (decoding via stb_image)
    emit.c             tree -> BareDrawList
    context.c          BareContext, frame begin/end, the current-context pointer
    widgets.c          (M8) button, checkbox, slider, list item: built only from public elements
  bare.c               the simple API: bare_window / bare_frame / bare_run / bare_quit on top of
                       the core context and the backend chosen at build time
  backend/
    sdl3/platform_sdl3.c     BarePlatform on SDL3
    sdl3/renderer_sdl3gpu.c  BareRenderer on the SDL3 GPU API
    sokol/                   (M9) platform_sapp.c, renderer_sgfx.c
shaders/
  bare.glsl            the one shader source (vertex + fragment)
  generated/           compiled blobs as C headers (SPIR-V, DXIL, MSL), committed
assets/fonts/          the default font (Inter, Latin subset, OFL), embedded at build time
tests/                 headless unit tests (no SDL)
examples/              hello.c, cards.c, ... one feature per example
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
 │    4. hit list      visual rects + clips in paint order    -> kept for frame N
 │    5. emit          tree + visual values                   -> BareDrawList
 │    6. render        renderer->render(draw list)            (backend)
 │    7. evict         drop nodes not declared in frame N-1
 └─ begin frame N
      8. wait/poll     platform->poll_events; block if idle (see §11)
      9. input         apply events; hit-test the mouse against the frame N-1 hit list;
                       update hovered / pressed / clicked / scroll per node
     10. reset         frame arena; open the root element (window size, column)
     11. return true   ... the app declares UI; each element reads its BareState now
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
    BareVec2  scroll, scroll_target;
    BareAnimSet anim;            /* animation channels, see §7 */
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

## 5. Styles (resolved in `tree.c`; may move to `style.c` when M7 makes it bigger)

For every element, in this order: start from zero → copy non-zero fields of `.style` (M7) → copy non-zero fields of
the element → apply built-in defaults to fields still at 0 → if hovered, overlay non-zero `.hover` fields; if pressed,
overlay `.press` (on top of hover) → resolve `BARE_ZERO` to 0. Inherited values (text colour, font) come from the
nearest ancestor that sets them, else the theme default. The result is the element's **target** look; animation moves
the visual look toward it.

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
| 6 | top-down | **position**: padding, `.justify` along the main axis (start/center/end/space-*), `.align` across, gaps, minus the scroll offset of scroll containers |

Widths are fully resolved before heights because text height depends on width. Rects are in logical pixels, window
coordinates; snapping to physical pixels happens in `emit.c`.

Text measuring goes through a callback held by the context (`BareMeasureText`), so layout tests in M2 use a fake
monospace measurer and never load a font.

## 7. Animation (`easing.c`, `spring.c`, `anim.c`)

### What animates

Each node has a fixed set of **channels**, each one float:

| Group | Channels | Spec used |
|---|---|---|
| layout | x, y (offset relative to the parent's visual position), width, height | `.animate_layout`, else `.animate` |
| look | background rgba, border rgba, text rgba, border width, radius, opacity, scale, offset x/y, shadow x/y/blur/spread/rgba | `.animate` |
| scroll | scroll x/y | built-in smooth spring (M6) |

Positions animate relative to the parent, so when a parent moves its children move with it rigidly instead of each
lagging on its own. Scale and offset are visual-only transforms (like CSS `transform`): they never change layout, they
apply to the element and its children, and hit testing uses the transformed rects.

```c
typedef struct BareChannel {
    float value, velocity;   /* current visual value; velocity in units/second */
    float target;
    float from, t;           /* easing: start value and progress 0..1 */
} BareChannel;

typedef struct BareAnimSet {
    BareChannel ch[BARE_CHANNEL_COUNT];
    uint64_t    active;      /* bit per channel still moving; 0 = node idle */
} BareAnimSet;
```

### Each frame

For every declared node: compare each new target with the channel's target. If it changed, **retarget**; then advance
every active channel by `dt` (clamped to 1/20 s so a stall can't blow anything up).

- **No animation** (`kind == BARE_ANIM_NONE`, the default): value = target.
- **Easing**: retarget sets `from = value`, `t = 0`. Each frame `t += dt / duration`, `value = lerp(from, target,
  bezier(t))`. Interrupting starts from where the value *is*, so there is never a jump in position (velocity may change
  abruptly; use springs for fully smooth interruption). Cubic-bezier is solved for x with Newton steps plus a bisection
  fallback, as browsers do.
- **Spring**: parameterised like SwiftUI: `response` (period in seconds) and `damping` ratio (1 = critical, < 1 bounces).
  Stiffness `k = (2π / response)²`, damping `c = 4π·damping / response`, mass 1. Each frame advances `(value - target,
  velocity)` with the **closed-form** solution for the under-, critically- or over-damped case, so it is exact and
  stable at any `dt`. Retargeting keeps value *and* velocity: interruption is smooth in both.
- A channel settles when it is within 0.01 units of its target with near-zero velocity: it snaps and clears its bit.

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

**Wheel.** Goes to the nearest scroll-container ancestor of what's under the mouse; its node's scroll offset moves (40
px per notch). M6 adds clamping to the content, hand-off between nested scrollers at their ends, and the spring.

**Cursor.** The innermost hovered element's `.cursor` (`bare_ctx_cursor`), sent to the platform after each frame.

**Z-order.** Paint order is declaration order. Floating layers (popups, tooltips drawn above everything) are deferred,
and will be a `.layer` field that sorts the hit list and the draw list.

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
    const BareTextureUpload *uploads; int upload_count;   /* process first */
    const BareDrawCmd       *cmds;    int cmd_count;
    int        width, height;          /* framebuffer, physical pixels */
    float      scale;                  /* physical / logical */
    BareColorF clear;
} BareDrawList;
```

Emission walks the tree in pre-order: for each element, one `RECT` (background, border and shadow together, skipped if
all invisible), `IMAGE` if it has one, `CLIP_PUSH` if it clips, then its children, then `CLIP_POP`. Text elements emit
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

**Shader toolchain.** `shaders/bare.glsl` and `shaders/glass.glsl` (GLSL 450) are the sources. For SDL3: `glslc` → SPIR-V (Vulkan) and
`SDL_shadercross` → DXIL (D3D12) and MSL (Metal). For sokol (M9): a small `bare.shdc.glsl` wrapper `@include`s the
same source for `sokol-shdc`. The compiled blobs are committed in `shaders/generated/` as C headers, so building Bare
never needs a shader compiler; the `bare_shaders` CMake target regenerates them when the tools are installed.

## 11. Idle, time and threads

- When no channel is animating, no scroll is settling and `bare_continuous(false)` (default), `bare_frame` blocks in
  `poll_events` until input arrives or `bare_wake()` is called from another thread. Idle apps use no CPU and no GPU.
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

- **Default font**: a Latin subset of Inter (SIL OFL), embedded at build time (≈ 100 KB), so `bare_text` works with no
  setup. `-DBARE_DEFAULT_FONT=OFF` drops it for size-critical builds.
- **Font backend interface** (internal, in `bare_core.h`), shaped so FreeType + HarfBuzz can replace stb_truetype
  without touching layout or emit:

```c
typedef struct BareShapedGlyph { uint32_t glyph; float advance, x_offset, y_offset; uint32_t cluster; } BareShapedGlyph;

typedef struct BareFontBackend {
    void *(*load)(const void *data, int size);
    void  (*metrics)(void *font, float px, float *ascent, float *descent, float *line_gap);
    int   (*shape)(void *font, float px, const char *utf8, int len, BareShapedGlyph *out, int cap);
    bool  (*rasterize)(void *font, uint32_t glyph, float px, BareGlyphBitmap *out);
    void  (*unload)(void *font);
} BareFontBackend;
```

  The stb version "shapes" by mapping code points to glyphs with kerning. HarfBuzz will do real shaping.
- **Glyph cache**: keyed by (font, glyph, physical pixel size), packed into an R8 atlas (1024² growing to 2048²) with a
  shelf packer; new glyphs become `BareTextureUpload`s. Text is rasterized at physical size, so it's sharp at any DPI.
  Scaled elements (`.scale` animations) stretch existing glyphs during the animation.
- **Line breaking**: at spaces and after hyphens, falling back to breaking inside a word that doesn't fit. Full UAX #14
  comes with HarfBuzz.

## 14. Headless use and testing

`bare_core.h` exposes the context directly:

```c
BareContext *bare_ctx_create(const BareConfig *config);   /* allocator, measure-text, font backend */
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
cross-backend comparison in M9. Multi-window support later
(`bare_window_new`, `bare_use`) is one context per window, switched by `bare_use`.
