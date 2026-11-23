# Bare guide

How to build UIs with Bare, for people who know HTML and CSS. Everything here works today (M2) unless it's marked
*(coming in Mx)*. The full list of what the web has and where Bare stands is the
[web parity checklist](ROADMAP.md#web-parity-checklist-html--css--js--bare).

1. [Your first app](#1-your-first-app)
2. [How Bare thinks](#2-how-bare-thinks)
3. [Boxes and how they look](#3-boxes-and-how-they-look)
4. [Rows, columns and stacks](#4-rows-columns-and-stacks)
5. [Sizes](#5-sizes)
6. [Spacing and alignment](#6-spacing-and-alignment)
7. [Components are functions](#7-components-are-functions)
8. [Lists and IDs](#8-lists-and-ids)
9. [Movement](#9-movement)
10. [Liquid glass](#10-liquid-glass)
11. [CSS to Bare cheat sheet](#11-css-to-bare-cheat-sheet)
12. [Debugging](#12-debugging)
13. [What's next](#13-whats-next)

---

## 1. Your first app

Make a folder with two files next to your copy of Bare:

```cmake
# CMakeLists.txt
cmake_minimum_required(VERSION 3.24)
project(MyApp C)
add_subdirectory(../Bare bare)          # path to Bare
add_executable(myapp main.c)
target_link_libraries(myapp PRIVATE bare)
```

```c
/* main.c */
#include "bare.h"

int main(void) {
    bare_window(1280, 800, "My App");
    while (bare_frame()) {
        bare_box(.width = 300, .height = 200, .radius = 16, .color = BARE_RGB(40, 120, 255));
    }
    bare_quit();
}
```

Build and run (MinGW shown; with Visual Studio leave out `-G Ninja` and run `build\Debug\myapp.exe`):

```powershell
cmake -S . -B build -G Ninja
cmake --build build
.\build\myapp.exe
```

The first configure downloads SDL3 (about 2 minutes). Your app is one .exe with no DLLs next to it.

## 2. How Bare thinks

On the web you build a DOM once and then change it with JavaScript. In Bare there is no DOM to keep in sync: **every
frame, you describe the whole UI from your variables**, and Bare works out the layout, draws it, and remembers
whatever needs remembering (hover state, animations) by itself.

```c
int count = 0;                      /* your state: ordinary C variables */
bool dark = true;

while (bare_frame()) {              /* one frame */
    bare_column(.color = dark ? BARE_RGB(20, 20, 24) : BARE_WHITE) {
        for (int i = 0; i < count; i++) bare_box(.width = 40, .height = 40);
    }
}
```

What this means in practice:

- **To show something, call it. To hide it, don't.** `if (open) panel();` is the whole of `display: none`.
- **No re-render bugs.** The screen always matches your variables because it's drawn from them.
- **No wasted work.** `bare_frame()` sleeps until something happens (mouse, keys, a resize), so an idle app uses no CPU.
- **Order is drawing order.** Later elements are drawn on top of earlier ones.
- **Two loop styles.** `while (bare_frame()) { ... }` is the usual one. `bare_run(frame_fn, user)` calls your function
  once per frame instead; it works everywhere and is needed for the web build *(coming in M9)*.

## 3. Boxes and how they look

`bare_box` is Bare's `<div>`. Every setting is optional and named, and **leaving a setting out (or 0) means "the
default"**:

```c
bare_box(.width = 200, .height = 120,
         .color = BARE_HEX(0x6384FF),             /* background */
         .radius = 16,                            /* rounded corners */
         .border = 2, .border_color = BARE_RGBA(255, 255, 255, 60),
         .shadow = { .y = 12, .blur = 32, .color = BARE_RGBA(0, 0, 0, 90) },
         .opacity = 0.9f);
```

- **Colours:** `BARE_RGB(r, g, b)`, `BARE_RGBA(r, g, b, a)` (alpha 0-255), `BARE_HEX(0xRRGGBB)`, `BARE_WHITE`,
  `BARE_BLACK`. A colour with alpha 0 means "none".
- **Shadows** work like CSS `box-shadow`: `{ x, y, blur, spread, color }`. Presets: `BARE_SHADOW_SMALL`,
  `BARE_SHADOW_SOFT`, `BARE_SHADOW_LARGE`.
- **Borders** are drawn inside the box (like `box-sizing: border-box`).
- **Opacity** multiplies into everything inside, as in CSS.
- **When 0 isn't the default:** opacity defaults to 1, so `.opacity = 0` would mean "default". Write
  `.opacity = BARE_ZERO` for a real zero. The same goes for any field whose default isn't 0.
- `BARE_RGB` builds a value at run time, so it can't go in a `static` array; use a normal local array or
  `{ 99, 132, 255, 255 }` there.

`bare_background(color)` sets the window colour, `bare_title("...")` the title.

## 4. Rows, columns and stacks

Containers hold other elements. They're written like `if` blocks:

```c
bare_row(.gap = 12) {               /* children left to right: flex-direction: row */
    bare_box(.width = 50, .height = 50, .color = BARE_RGB(230, 80, 80));
    bare_box(.width = 50, .height = 50, .color = BARE_RGB(80, 200, 120));
}

bare_column(.gap = 12) {            /* top to bottom: flex-direction: column */
    ...
}

bare_stack(.align = BARE_CENTER) {  /* on top of each other, like position: absolute layers */
    bare_box(.width = 200, .height = 200, .color = BARE_RGB(40, 40, 50));
    bare_box(.width = 60, .height = 60, .color = BARE_WHITE);      /* centred over the first */
}
```

Containers are boxes too: they take every setting `bare_box` does (colour, radius, shadow...). The window itself is a
column, so top-level elements go top to bottom.

Don't `break`, `return` or `goto` out of a container block: it would stay open, and what follows would land inside it.
If it happens, Bare logs the container's file and line at the end of the frame and closes it.

## 5. Sizes

`.width` and `.height` take one of:

| Value | Means | CSS |
|---|---|---|
| leave it out (`BARE_FIT`) | as big as the content | `fit-content` |
| `240` | that many pixels | `240px` |
| `BARE_GROW` | share of the free space | `flex: 1` |
| `BARE_PERCENT(50)` | half of the parent's inside | `50%` |
| `BARE_ZERO` | really zero | `0` |

Plus `.min_width`, `.max_width`, `.min_height`, `.max_height` (0 = no limit).

**Grow** shares the space that's left after everything else is sized. `.grow` is the weight:

```c
bare_row(.width = BARE_GROW, .height = 60, .gap = 8) {
    bare_box(.width = 200);                         /* fixed sidebar */
    bare_box(.width = BARE_GROW);                   /* 1 share */
    bare_box(.width = BARE_GROW, .grow = 2);        /* 2 shares */
}
```

Like CSS, a growing element never gets smaller than its own content, and `.max_width` caps it (the rest goes to the
others).

**Shrink:** when content-sized children don't fit, they give space back in proportion to their size (and `.shrink`),
never below what their own content needs. Pixel and percent sizes don't shrink.

**Across the row** (height in a row, width in a column) `BARE_GROW` means "fill the parent".

**Filling the window:** `bare_column(.width = BARE_GROW, .height = BARE_GROW) { ... }` at the top level is
`height: 100vh`. For anything cleverer, `bare_window_size()` gives the size in pixels and C does the maths:

```c
BareVec2 win = bare_window_size();
bool narrow = win.x < 700;                          /* a media query */
bare_box(.width = win.x * 0.3f - 20);               /* calc(30vw - 20px) */
```

## 6. Spacing and alignment

```c
bare_row(.padding = 16,                 /* all sides */
         .pad = { 8, 16, 8, 16 },       /* or per side: top, right, bottom, left (CSS order) */
         .gap = 10,                     /* between children */
         .align = BARE_CENTER,          /* across: align-items */
         .justify = BARE_SPACE_BETWEEN) /* along: justify-content */
{ ... }
```

- `.align`: `BARE_START` (default), `BARE_CENTER`, `BARE_END`, `BARE_STRETCH` (content-sized children fill the cross
  axis).
- `.justify`: `BARE_START`, `BARE_CENTER`, `BARE_END`, `BARE_SPACE_BETWEEN`, `BARE_SPACE_AROUND`,
  `BARE_SPACE_EVENLY`.
- In a `bare_stack`, `.align` places the layers on both axes.
- **Spacer trick** (`margin-left: auto`): an empty growing box pushes everything after it to the end.

```c
bare_row(.width = BARE_GROW, .align = BARE_CENTER) {
    logo();
    bare_box(.width = BARE_GROW);       /* spacer */
    search_box();
    profile_button();
}
```

There's no `margin` yet *(coming in M10)*: use the parent's `.gap` and `.padding`, or a spacer.

## 7. Components are functions

A "component" is a C function that declares elements. Parameters are its props:

```c
static void stat_card(const char *label, BareColor accent) {
    bare_column(.width = BARE_GROW, .padding = 20, .gap = 12, .radius = 16,
                .color = BARE_RGB(31, 35, 46), .shadow = BARE_SHADOW_SOFT) {
        bare_box(.width = 28, .height = 28, .radius = 8, .color = accent);
        /* bare_text(label, .size = 20);   (coming in M5) */
    }
}

bare_row(.width = BARE_GROW, .gap = 20) {
    stat_card("Revenue", BARE_RGB(99, 132, 255));
    stat_card("Users", BARE_RGB(80, 200, 160));
}
```

No classes, no framework: call it in a loop, pass it state, return values from it. Reusable looks like CSS classes
(`.style = &card_style`) are *(coming in M7)*.

## 8. Lists and IDs

Bare gives every element an ID so it can remember things about it between frames (where it was, whether it's hovered,
its animations). It works them out from where in your code the element was declared, which is enough for most UIs,
including simple loops.

Give an explicit ID when the element is "the same thing" even if it moves around, like items of a list that can be
reordered, inserted or deleted:

```c
for (int i = 0; i < n; i++) {
    bare_row(.id = BARE_IDI("todo", todos[i].id)) { ... }     /* keyed by your data, like React's key= */
}
bare_box(.id = BARE_ID("save"), ...);                       /* a one-off name */
```

Using one ID twice in a frame is a mistake; Bare logs it with both file:line locations and keeps going.

Every element call returns a `BareState`. `.rect` is where it was drawn last frame. Hover and click flags are
*(coming in M3)*:

```c
BareState s = bare_box(.width = 120, .height = 40);
if (s.clicked) count++;             /* (coming in M3) */
```

## 9. Movement

Real transitions and springs are *(coming in M4)*: you'll set the new value and Bare animates to it:

```c
bare_box(.width = open ? 300 : 60, .animate = BARE_SPRING_SNAPPY);   /* (coming in M4) */
```

Today you can already move things smoothly yourself. `.offset` moves an element (and its children) without affecting
layout, like `transform: translate()`, and isn't rounded to whole pixels, so slow motion stays smooth:

```c
bare_continuous(true);              /* keep drawing every frame (otherwise Bare sleeps when idle) */
while (bare_frame()) {
    float t = (float)bare_time();   /* seconds since start; bare_dt() = seconds since the last frame */
    bare_box(.width = 80, .height = 80, .radius = 40, .offset = { sinf(t) * 200, 0 });
}
```

Turn `bare_continuous` off again when nothing moves, so the app goes back to using no CPU.

## 10. Liquid glass

Glass bends and frosts whatever was drawn **before** it (behind it):

```c
bare_stack() {
    my_wallpaper();                                                  /* first: what the glass sees */
    bare_box(.width = 520, .height = 96, .radius = 48, .glass = BARE_GLASS);
}
```

Presets: `BARE_GLASS` (clear, Apple style) and `BARE_GLASS_FROSTED` (blurry, like `backdrop-filter: blur()`). Tune it:

```c
.glass = { .refraction = 1.2f,      /* how much the rim bends (default 1) */
           .depth = 40,             /* width of the curved rim, px (default 20) */
           .blur = BARE_ZERO,       /* frost, px (default 2; BARE_ZERO = crystal clear) */
           .dispersion = 1,         /* rainbow fringe at the rim */
           .shine = 1,              /* rim highlight */
           .tint = BARE_RGBA(255, 255, 255, 20) }
```

**Merging:** glass siblings declared one after another with `.merge` melt into each other like drops of liquid when
they come within that many pixels. Up to 8 shapes merge into one surface.

```c
bare_box(.width = 300, .height = 90, .radius = 45, .glass = { .merge = 50 }, .offset = { x, y });
bare_box(.width = 90, .height = 90, .radius = 45, .glass = { .merge = 50 }, .offset = { x2, y2 });
```

Glass is the most expensive thing Bare draws (each glass group copies the screen), so use it for a few hero elements,
not for every list row. `examples/glass.c` is the showcase.

## 11. CSS to Bare cheat sheet

| CSS / HTML / JS | Bare |
|---|---|
| `<div>` | `bare_box(...)`, or `bare_row/column/stack(...) { }` with children |
| `display: flex; flex-direction: row` | `bare_row() { }` |
| `flex-direction: column` | `bare_column() { }` |
| `position: absolute` layers | `bare_stack() { }` + `.offset` |
| `width: 240px` / `50%` / `fit-content` | `.width = 240` / `BARE_PERCENT(50)` / leave it out |
| `flex: 1` / `flex: 2` | `.width = BARE_GROW` / `+ .grow = 2` |
| `min-width`, `max-width` | `.min_width`, `.max_width` |
| `padding: 8px 16px` | `.pad = { 8, 16, 8, 16 }` |
| `gap: 12px` | `.gap = 12` |
| `align-items: center` | `.align = BARE_CENTER` |
| `justify-content: space-between` | `.justify = BARE_SPACE_BETWEEN` |
| `margin-left: auto` | a `bare_box(.width = BARE_GROW)` spacer |
| `background: #6384ff` | `.color = BARE_HEX(0x6384FF)` |
| `border: 2px solid rgba(...)` | `.border = 2, .border_color = BARE_RGBA(...)` |
| `border-radius: 16px` | `.radius = 16` |
| `box-shadow: 0 12px 32px rgba(0,0,0,.35)` | `.shadow = { 0, 12, 32, 0, BARE_RGBA(0, 0, 0, 90) }` |
| `opacity: .5` | `.opacity = 0.5f` |
| `transform: translate(10px, 0)` | `.offset = { 10, 0 }` |
| `backdrop-filter: blur(18px)` | `.glass = BARE_GLASS_FROSTED` |
| `display: none` | `if (visible) ...` |
| `@media (max-width: 700px)` | `if (bare_window_size().x < 700)` |
| `calc(100vw - 240px)` | `bare_window_size().x - 240` |
| `key={item.id}` | `.id = BARE_IDI("item", item.id)` |
| `useState` | a C variable |
| `onclick` | `if (bare_box(...).clicked)` *(M3)* |
| `:hover { ... }` | `.hover = { .color = ... }` *(M3)* |
| `transition: all .3s ease-out` | `.animate = BARE_EASE(0.3f, BARE_EASE_OUT)` *(M4)* |
| text | `bare_text("Hello", .size = 20)` *(M5)* |
| `overflow: auto` | `.scroll = ...` *(M6)* |
| `class="card"` | `.style = &card` *(M7)* |
| `<button>`, `<input type=checkbox>`, `<input type=range>` | `bare_button`, `bare_checkbox`, `bare_slider` *(M8)* |

## 12. Debugging

- **Mistakes are logged, not crashes:** unclosed containers and duplicate IDs print a message with the file and line
  to stderr.
- **Screenshot a frame:** set `BARE_CAPTURE` and the app saves a frame as .bmp and quits. `BARE_CAPTURE_FRAMES=N`
  picks which frame (default 2).

  ```powershell
  $env:BARE_CAPTURE = "C:\temp\shot.bmp"; .\build\myapp.exe; Remove-Item Env:BARE_CAPTURE
  ```

- `SDL_GPU_DRIVER=vulkan` or `direct3d12` picks the GPU API (both draw the same pixels); `BARE_GPU_DEBUG=1` turns on
  the GPU validation layer.
- **Headless testing:** `#include "bare/bare_core.h"` gives a context with no window or GPU
  (`bare_ctx_create`, `bare_ctx_begin`, `bare_ctx_end`), which returns the list of draw commands. Bare's own tests in
  `tests/` work this way; your app's layout can be tested the same way.

## 13. What's next

Bare is being built one milestone at a time ([ROADMAP](ROADMAP.md)): interaction (M3), animation (M4), text and images
(M5), scrolling (M6), styles (M7), widgets (M8), a second backend and the browser (M9), then the rest of what CSS has
(M10: gradients, per-corner radii, rotation, filters, flex-wrap...). The examples in `examples/` grow with it:
`hello.c`, `shapes.c`, `glass.c` and `layout.c` today.
