# Bare guide

How to build UIs with Bare, for people who know HTML and CSS. Everything here works today (through M7: styles and themes, plus M7.5's text fields) unless it's marked
*(coming in Mx)*. The full list of what the web has and where Bare stands is the
[web parity checklist](ROADMAP.md#web-parity-checklist-html--css--js--bare).

1. [Your first app](#1-your-first-app)
2. [How Bare thinks](#2-how-bare-thinks)
3. [Boxes and how they look](#3-boxes-and-how-they-look)
4. [Rows, columns and stacks](#4-rows-columns-and-stacks)
5. [Sizes](#5-sizes)
6. [Spacing and alignment](#6-spacing-and-alignment)
7. [Text and images](#7-text-and-images)
8. [Components and styles](#8-components-and-styles)
9. [Lists and IDs](#9-lists-and-ids)
10. [Clicks, hover and keys](#10-clicks-hover-and-keys)
    - [Text fields and keyboard focus](#text-fields-and-keyboard-focus)
11. [Movement](#11-movement)
12. [Liquid glass](#12-liquid-glass)
13. [Canvases and your own shaders](#13-canvases-and-your-own-shaders)
14. [CSS to Bare cheat sheet](#14-css-to-bare-cheat-sheet)
15. [Debugging](#15-debugging)
16. [What's next](#16-whats-next)

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

`bare_background(color)` sets the window colour (default: the theme's background, see section 8), `bare_title("...")`
the title.

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

**Taking an element out of the flow** (CSS `position: absolute`): `.position = BARE_ABSOLUTE` places any element,
text included, on its parent instead of after its siblings. It takes no room. `.anchor` picks which point of the
parent and of the element meet (`BARE_TOP_LEFT` by default, `BARE_TOP`, `BARE_TOP_RIGHT`, `BARE_LEFT`,
`BARE_MIDDLE`, `BARE_RIGHT`, `BARE_BOTTOM_LEFT`, `BARE_BOTTOM`, `BARE_BOTTOM_RIGHT`), and `.x`, `.y` move it from
there (right and down). Sizes work as usual: `BARE_GROW` fills the parent, a percent is of the parent. Declare it
last to draw it on top:

```c
bare_row(.style = &t->button) {
    bare_text("Inbox");
    bare_row(.position = BARE_ABSOLUTE, .anchor = BARE_TOP_RIGHT, .x = 8, .y = -8,   /* a badge on the corner */
             .pad = { 2, 6, 2, 6 }, .radius = 8, .color = BARE_RGB(255, 90, 110)) {
        bare_text("3", .size = 11);
    }
}
```

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

## 7. Text and images

```c
bare_text("Hello");                                          /* 16 px, Inter, white */
bare_text("Big and blue", .size = 32, .color = BARE_RGB(90, 140, 255));
bare_textf("Clicked %d times", clicks);                      /* printf-style */
```

Text is an element like a box. It's as wide as its longest line, but no wider than its parent lets it be: then it
wraps, at spaces and after hyphens (a word longer than the line breaks where it must). In a row it shrinks down to
its longest word before anything overflows, like inline text in a flex item. `\n` starts a new line.

- `.size` (default 16), `.line_height` (a multiple of the size, default 1.3), `.font`, `.color`.
- `.weight = BARE_BOLD` (or `BARE_MEDIUM`, `BARE_SEMIBOLD`, any 100-900) and `.italic = true`.
- `.shadow = { 0, 2, 6, 0, BARE_RGBA(0, 0, 0, 120) }`: x, y, blur and colour, like CSS `text-shadow`.
- `.align = BARE_CENTER` or `BARE_END` aligns each wrapped line inside the text's box. Give the text the width to
  align in: `.align = BARE_STRETCH` on a column, or a parent of fixed width.
- `.nowrap = true` keeps it on one line.
- `.max_lines = 2` cuts it after two lines; `.ellipsis = true` ends the cut with "…" (CSS `line-clamp`). With
  `.nowrap`, `.ellipsis` cuts a line that doesn't fit its box (`text-overflow: ellipsis`), and the text can then be
  squeezed narrower than its content.
- `.underline`, `.strike` (line-through), `.letter_spacing = 2` (pixels after each character),
  `.transform = BARE_UPPERCASE` or `BARE_LOWERCASE`, `.opacity = 0.5f`.
- **Colour is inherited**, like CSS `color`: set `.text_color` on any container and the text inside takes it, unless
  the text sets its own `.color`. `.hover = { .text_color = ... }` works, and `.animate` fades it.
- Strings are UTF-8 and copied, so a temporary buffer is fine.

```c
bare_column(.width = 320, .gap = 6, .text_color = BARE_RGB(200, 205, 220)) {
    bare_text("Sunset Hills", .size = 22, .color = BARE_WHITE);
    bare_text("Wraps at 320 px; inherits its colour from the column.");
}
```

**Fonts.** The default font is Inter (Latin and common symbols), built in, so text works with no setup. Load others
with `BareFont f = bare_font_load("fonts/Display.ttf");` once, then `.font = f`. `bare_font_from_memory(data, size)`
uses a font you've embedded (the data must stay alive). Text is rasterized at the screen's real pixel size, so it's
sharp at 150% and 200%. `BARE_MONO` is the built-in monospaced font (JetBrains Mono), for code.

**Weights and italics** come from a font's family: the built-in Inter has regular, semibold, bold, italic and bold
italic, and `.weight` picks the nearest one the way CSS does. Give a font you loaded more faces with
`bare_font_add_face(family, face, BARE_BOLD, false)`.

**Mixed styles in one paragraph** (HTML's `<b>`, `<a>` and `<span>`): spans wrap together as one flow.

```c
bare_paragraph(.size = 18) {
    bare_span("Read the ");
    bare_span("guide", .color = t->accent, .underline = true);
    bare_span(" before you ", .italic = true);
    bare_span("ship", .weight = BARE_BOLD);
}
```

**Images.** `bare_image_load("photo.png")` (PNG, JPEG, BMP, GIF's first frame, TGA) once, then show it with `.image`
on any box. It stretches to the box and is cut to its `.radius`, so a round avatar is:

```c
BareImage avatar = bare_image_load("avatar.png");            /* before the loop */
...
bare_box(.width = 96, .height = 96, .radius = 48, .image = avatar);
```

`bare_image_size(img)` gives its pixel size (for an aspect ratio), `bare_image_from_pixels(w, h, rgba)` makes one
from memory, and `bare_image_free(img)` frees it. `examples/text.c` shows all of this, plus a liquid glass loupe that
magnifies the text under the mouse. `.image_fit = BARE_FIT_COVER` fills the box keeping the picture's shape, like CSS
`object-fit: cover`.

**Pictures from files, many of them, or moving.** For a gallery, call `bare_picture(path, .width, .height)` every
frame instead, like any other part of the UI. It loads on background threads (0 until then), shrinks to the size you
ask (cropped from the middle), keeps it while you draw it and frees it 10 s after you stop. Everything shows something
quickly: small files first, then big ones and videos (on Windows, Explorer's cached thumbnail shows meanwhile, and a
moving picture's first frame before the rest). GIFs play every frame; on Windows it also reads WebP, HEIC and the rest
WIC knows, and shows a few looping seconds of a video (Media Foundation, decoded and shrunk on the GPU).
`.still = true` keeps only the first frame.

```c
bare_box(.width = 240, .height = 135, .radius = 12, .image = bare_picture(path, .width = 480, .height = 270));
```

## 8. Components and styles

A "component" is a C function that declares elements. Parameters are its props:

```c
static void stat_card(const char *label, BareColor accent) {
    bare_column(.width = BARE_GROW, .padding = 20, .gap = 12, .radius = 16,
                .color = BARE_RGB(31, 35, 46), .shadow = BARE_SHADOW_SOFT) {
        bare_box(.width = 28, .height = 28, .radius = 8, .color = accent);
        bare_text(label, .size = 20);
    }
}

bare_row(.width = BARE_GROW, .gap = 20) {
    stat_card("Revenue", BARE_RGB(99, 132, 255));
    stat_card("Users", BARE_RGB(80, 200, 160));
}
```

No classes, no framework: call it in a loop, pass it state, return values from it.

### Styles: reusable looks (CSS classes)

A style is a plain `BareBox` (or `BareTextStyle` for text). Elements point at it with `.style`; what the element sets
itself wins, everything it leaves at 0 comes from the style:

```c
const BareBox card = { .padding = 20, .radius = 16, .color = BARE_RGB(36, 38, 46),
                       .shadow = BARE_SHADOW_SOFT, .hover = { .offset = { 0, -4 } },
                       .animate = BARE_SPRING_SNAPPY };
const BareTextStyle label = { .size = 20, .color = BARE_RGB(220, 224, 235) };

bare_column(.style = &card, .width = 300) { bare_text("Revenue", .style = &label); }
bare_column(.style = &card, .color = BARE_RGB(60, 40, 40)) { ... }     /* a red card */
```

- **Styles build on styles:** `const BareBox danger = { .style = &card, .color = BARE_RGB(120, 30, 40) };`.
  A loop (`a` → `b` → `a`) is logged once and cut.
- **Cancelling a style's value:** 0 means "not set", so use the deliberate nothings: `BARE_ZERO` (a real 0),
  `BARE_CLEAR` (no colour), `BARE_START` (start alignment), `BARE_CURSOR_ARROW`, `BARE_INSTANT` (no animation).
- **Some things go together:** padding (`.padding` and `.pad`), `.shadow`, `.offset`, `.glass` and the animations
  are taken whole from the element if it sets any part of them. `.hover` and `.press` mix field by field: the
  style's hover colour plus the element's hover scale.
- A style is just a value: a local (before the loop, or built in the frame when it depends on state), a global or a
  `static`. Presets like `BARE_RGB`, `BARE_SHADOW_SOFT` and `BARE_SPRING_SNAPPY` are compound literals, which C
  doesn't allow in a `static` or global: write those values out in braces there.

### Themes

`bare_theme()` is the current theme: colours (`background`, `surface`, `text`, `muted`, `accent`, `border`) and
ready-made styles (`panel`, `card`, `button`, `primary`, `row`; text: `title`, `heading`, `body`, `caption`). The
theme also sets the window background and the default text colour. Bare starts dark.

```c
const BareTheme *t = bare_theme();
bare_column(.style = &t->card) {
    bare_text("Storage", .style = &t->heading);
    bare_row(.style = &t->primary) { bare_text("Upgrade", .style = &t->body); }
}
if (clicked) bare_set_theme(bare_theme_light());     /* everything styled from the theme fades over */
```

Your own theme is a tweaked copy (like CSS variables): `BareTheme mine = *bare_theme_dark(); mine.accent = ...;
mine.primary.color = mine.accent; bare_set_theme(&mine);`. `examples/styles.c` is a settings page in both themes.

## 9. Lists and IDs

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

### Scrolling and clipping

`.clip = true` hides whatever overflows a box (CSS `overflow: hidden`); `.scroll` makes it scroll
(`overflow: auto`). Scrolling clips too.

```c
bare_column(.height = BARE_GROW, .gap = 8, .scroll = BARE_SCROLL_Y) {   /* or BARE_SCROLL_X, BARE_SCROLL_BOTH */
    for (int i = 0; i < n; i++) message(&msgs[i]);
}
```

- The wheel scrolls the innermost scroll box under the mouse that can still move that way; one at its end passes the
  wheel to the next one out. A mouse wheel glides (100 px a notch), a touchpad follows your fingers, Shift + wheel
  scrolls sideways.
- A thin scrollbar appears while scrolling or when the mouse moves over the box, fades a second later, widens under
  the mouse and can be dragged.
- A box that clips or scrolls is happy to be smaller than its content, as in CSS: give it `BARE_GROW` (or let a column
  squeeze it) and it takes the space there is and scrolls the rest. Children of a scroll box keep their size along the
  scroll direction instead of shrinking.
- `bare_this().scroll` (inside the box) is how far it's scrolled this frame.
- Clips are rectangles: a box with `.radius` doesn't round its children's corners yet.

**Long lists.** Declaring 10,000 rows every frame costs milliseconds; `bare_visible` declares only the ones on screen
and keeps room for the rest, so the scrollbar and scrolling behave as if they were all there. Items must all be the same
length along the box's direction (the box's `.gap` is added for you):

```c
bare_column(.height = BARE_GROW, .scroll = BARE_SCROLL_Y) {
    bare_visible(i, 10000, 44) {                       /* items 44 px tall */
        bare_row(.id = BARE_IDI("row", i), .height = 44) { bare_textf("Row %d", i + 1); }
    }
}
```

It's a loop: one element per item, no `break`. Headers and footers can go before and after it in the same box.
`examples/scroll.c` has a 10,000-row list next to a page of sideways-scrolling strips.

## 10. Clicks, hover and keys

Every element call returns a `BareState`: what happened to it. There are no callbacks or event listeners. You ask:

```c
if (bare_box(.width = 120, .height = 40, .color = BARE_RGB(70, 110, 245)).clicked) count++;
```

| Field | True when |
|---|---|
| `.hovered` | the mouse is over it or over one of its children (like `:hover`) |
| `.pressed` | the left button went down on it and is still held, even if the mouse has moved off (like `:active`) |
| `.clicked` | the left button went down and came back up over it, this frame only |
| `.right_clicked` | the same with the right button |
| `.rect` | where it was drawn last frame (after `.offset` and `.scale`) |

Clicks bubble like in the DOM: clicking the icon inside a button clicks the icon *and* the button, so test the
button. Dragging off a button and letting go doesn't click; dragging back on first does.

Containers are blocks, not expressions, so inside one ask with `bare_this()`:

```c
bare_row(.padding = 12, .radius = 10) {
    if (bare_this().clicked) open = !open;
    bare_box(.width = 16, .height = 16);
}
```

**Looks for hover and press** (`:hover` and `:active`) change any of colour, border colour, shadow, opacity, scale,
radius, or nudge the offset. They apply in the same frame, with no lag:

```c
bare_box(.width = 150, .height = 52, .radius = 14, .color = BARE_RGB(70, 110, 245),
         .cursor = BARE_CURSOR_HAND,
         .hover = { .color = BARE_RGB(92, 132, 255), .scale = 1.04f, .shadow = BARE_SHADOW_LARGE },
         .press = { .scale = 0.96f });
```

A look field left at 0 means "unchanged" (`BARE_ZERO` for a real zero). The press look goes on top of the hover look.

- `.scale` (and the looks' `.scale`) grows or shrinks the element about its centre, children included, without moving
  anything else, like `transform: scale()`. The mouse hits what's drawn, so a scaled-up button is clickable on its
  bigger edge.
- `.cursor`: `BARE_CURSOR_HAND`, `BARE_CURSOR_TEXT`, `BARE_CURSOR_MOVE`, `BARE_CURSOR_RESIZE_EW`, `_NS`,
  `BARE_CURSOR_NOT_ALLOWED`. The innermost hovered element with a cursor wins.
- `.passthrough = true` lets the mouse go through an element (`pointer-events: none`), for overlays and decorations.
  Its children still get the mouse.
- Rounded corners count: the mouse in the empty corner of a round button misses it.

**Keys:** `bare_key_down(BARE_KEY_SHIFT)` is true while held; `bare_key_pressed(BARE_KEY_S)` is true in the frame it
goes down (and again on auto-repeat, like `keydown`). Letters follow the user's keyboard layout. A shortcut:

```c
if (bare_key_down(BARE_KEY_CTRL) && bare_key_pressed(BARE_KEY_S)) save();
```

`bare_mouse()` is the mouse position.

### Text fields and keyboard focus

`bare_input` edits a `char` buffer you own, in place (UTF-8, always NUL-terminated, never past its size):

```c
static char name[64], bio[1024];
bare_input(name, sizeof name, .placeholder = "Your name", .width = BARE_GROW);
bare_input(bio, sizeof bio, .multiline = true, .lines = 5, .width = BARE_GROW);
if (bare_input(search, sizeof search, .placeholder = "Search").changed) refilter();
```

Everything a native text box does is built in, with nothing to map: click, drag, double-click a word, triple-click a
line; arrows, Ctrl+arrows by word, Home / End, PageUp / PageDown; Shift with any of them selects; Ctrl+A; Backspace /
Delete by character or (with Ctrl) by word; cut, copy, paste; undo and redo (Ctrl+Z, Ctrl+Y / Ctrl+Shift+Z);
Insert for overwrite; IME composition for Chinese, Japanese and Korean. On macOS the keys are the Mac ones (Cmd,
Option). The field scrolls to keep the caret in view, and a focused field uses no CPU while you're not typing.

- It returns a `BareState` with `.focused`, `.changed` (the text changed this frame) and `.submitted` (Enter in a
  one-line field).
- Options: `.placeholder`, `.multiline` (Enter makes a new line), `.lines` (its height, multiline), `.password`
  (shows dots), `.read_only` (select and copy only), `.tabs` (Tab types a tab instead of moving focus), `.width`,
  `.height`, `.size`, `.font`, `.color`, `.caret` and `.selection` colours, and `.style` (default: the theme's
  `input` look) or `.text_style`.
- Give it an `.id` when its position in the code can change (in a list, say): the caret and undo history belong to
  the id.

**Focus.** Clicking a field focuses it; Tab and Shift+Tab move through focusable elements in the order they're
declared; Escape or clicking elsewhere leaves. Any element can take focus with `.focusable = true`, and `.focus` is a
look like `.hover` (a focus ring). `bare_focus(BARE_ID("email"))` moves focus from code, `bare_focused()` says
which id has it. While a field has focus it takes the keys it uses, so `bare_key_pressed` shortcuts don't fire
under it.

```c
bare_row(.style = &t->primary, .focusable = true, .focus = { .shadow = { 0, 0, 0, 3, BARE_RGBA(40, 120, 255, 90) } }) {
    bare_text("Save");
    BareState b = bare_this();
    if (b.clicked || (b.focused && bare_key_pressed(BARE_KEY_ENTER))) save();
}
```

**Your own shortcuts and buttons.** `bare_edit_bind(BARE_KEY_D, BARE_MOD_SHORTCUT, BARE_EDIT_DELETE_LINE_LEFT)`
adds or remaps a key (`BARE_MOD_SHORTCUT` is Ctrl, or Cmd on a Mac); `bare_edit(BARE_ID("bio"), BARE_EDIT_UNDO)`
runs any editing action from a menu or toolbar button. `examples/editor.c` is a sign-up form and a search box
filtering 5,000 rows.

### The editor: code, notes and long documents

For more than a field, `bare_editor` edits a `BareDoc`: a document Bare owns, with its own undo history and any
number of carets. Only the lines on screen are laid out, so a million-line file opens and scrolls at once.

```c
static BareDoc *doc;
if (!doc) doc = bare_doc_load("notes.md");                   /* or bare_doc_create() */
bare_editor(doc, .line_numbers = true, .wrap = true);
if (bare_key_down(BARE_KEY_SHORTCUT) && bare_key_pressed(BARE_KEY_S)) bare_doc_save(doc, "notes.md");
```

On top of every field shortcut: Ctrl+D selects the next match (one caret each), Ctrl+Shift+L all of them, Alt+click
adds a caret, Alt+drag selects a column; Alt+Up / Down move lines, Shift+Alt+Up / Down copy them, Ctrl+Shift+K
deletes one, Tab / Shift+Tab indent; Ctrl+F finds, Ctrl+H replaces, Ctrl+G goes to a line. The file keeps its line
endings and byte order mark, and `bare_doc_modified` knows when undo brings it back to what was saved.

- Options: `.font` (default `BARE_MONO`; `BARE_SANS` for prose), `.size`, `.line_height`, `.wrap`, `.line_numbers`,
  `.highlight_line`, `.whitespace`, `.ruler = 80`, `.tab_size`, `.spaces` (Tab types spaces), `.read_only`,
  `.comment = "// "` (Ctrl+/ toggles it), `.auto_close = "()[]{}\"\""` (pairs typed together), `.style` (its box;
  the padding insets the text).
- `.highlight` colours lines: a function that reads one line and writes coloured spans (bold, italic, underline,
  backgrounds too). It gets the state the previous line ended in, so block comments and fenced code work, and Bare
  re-runs it only from an edit down. `.indent` decides what Enter puts on the next line (continuing a list, say).
- Edit from code as the user would: `bare_doc_insert`, `bare_doc_wrap(doc, "**", "**")`, `bare_doc_select`,
  `bare_doc_undo`; read with `bare_doc_text`, `bare_doc_line`, `bare_doc_caret`.

`examples/code.c` is a C editor with highlighting and a 1M-line log; `examples/write/` is a Markdown and plain text
editor with a live preview.

### Widgets

One call each, themed, focusable (Tab, then Space or Enter):

```c
if (bare_button("Save", .style = &bare_theme()->primary).clicked) save();
bare_checkbox("Wrap lines", &wrap);                          /* true when toggled */
bare_slider(&volume, 0, 100, .width = 240);                  /* drag it, or the arrow keys */
for (int i = 0; i < n; i++)
    if (bare_list_item(names[i], i == sel, .id = BARE_IDI("item", i)).clicked) sel = i;
```

They take any box option (`.width`, `.style`, `.hover`...), and they're built only from the elements above, so
copying one into your app to change it is fine. `examples/widgets.c` has them all.

### Opening and saving files

`bare_file_dialog` shows the system's open, save or folder dialog. It doesn't block: the app keeps drawing, and the
answer comes back in a later frame from `bare_file_chosen` (`""` if the user cancelled).

```c
if (bare_button("Open").clicked)
    bare_file_dialog(BARE_ID("open"), BARE_FILE_OPEN, "Markdown:md;markdown|Text:txt", NULL);
const char *path = bare_file_chosen(BARE_ID("open"));
if (path && *path) open_file(path);
```

## 11. Movement

Set the new value and say how to get there; Bare animates to it:

```c
bare_box(.width = open ? 300 : 60, .height = 40, .animate = BARE_SPRING_SNAPPY);
```

`.animate` covers everything that can change: position, size, colours, opacity, radius, border, shadow, `.scale` and
`.offset`, including the `.hover` and `.press` looks. Two kinds:

- **Springs**, `BARE_SPRING(response, damping)`: `response` is roughly how long it takes (seconds), `damping` 1 settles
  without overshoot and less than 1 bounces. `BARE_SPRING_SMOOTH`, `BARE_SPRING_SNAPPY` and `BARE_SPRING_BOUNCY` are
  ready-made. Interrupt a spring halfway (hover in and out quickly) and it turns around smoothly, keeping its speed.
- **Easing**, `BARE_EASE(seconds, curve)`, like a CSS transition: curves `BARE_LINEAR`, `BARE_EASE_IN`,
  `BARE_EASE_OUT`, `BARE_EASE_IN_OUT` or `BARE_BEZIER(x1, y1, x2, y2)`. `{ .kind = BARE_ANIM_EASE }` alone is 0.25 s
  of CSS's `ease`.

`.animate_layout` overrides `.animate` for position and size, so a card can lift on hover with a snappy spring but
glide to a new place with a smooth one:

```c
for (int k = 0; k < count; k++) {
    int i = order[k];                                   /* shuffle `order` and the cards glide */
    bare_column(.id = BARE_IDI("card", i), .width = 300, .padding = 20, .radius = 16,
                .hover = { .offset = { 0, -6 } },
                .animate = BARE_SPRING_SNAPPY, .animate_layout = BARE_SPRING_SMOOTH) { ... }
}
```

What to know:

- **Give moving things an id.** Bare tracks an element across frames by its id (section 9). A card that changes place
  in a list needs `BARE_IDI("card", i)` with `i` belonging to the card, not to its place, or Bare sees a different card
  appear in each slot instead of one card moving. The same id under another parent glides there too.
- **Children ride along.** Positions animate relative to the parent, so when a card moves its contents move with it,
  even if they don't animate themselves. Sizes don't ride along: give children `.animate_layout` too if their size
  follows the parent's (otherwise they jump to their new size while the parent is still growing).
- **Nothing flies in.** An element's first frame is drawn where it belongs; only later changes animate.
- **Fading out**: animate `.opacity` to `BARE_ZERO`, or a colour to alpha 0; a colour fading to transparent keeps its
  hue. (Animating elements out after you stop declaring them is for later.)
- **Idle is still free.** Bare draws frames while something moves and goes back to sleep when everything has settled.

You can also move things yourself, frame by frame. `.offset` moves an element (and its children) without affecting
layout, like `transform: translate()`, and isn't rounded to whole pixels, so slow motion stays smooth:

```c
bare_continuous(true);              /* keep drawing every frame (otherwise Bare sleeps when idle) */
while (bare_frame()) {
    float t = (float)bare_time();   /* seconds since start; bare_dt() = seconds since the last frame */
    bare_box(.width = 80, .height = 80, .radius = 40, .offset = { sinf(t) * 200, 0 });
}
```

Turn `bare_continuous` off again when nothing moves, so the app goes back to using no CPU.

## 12. Liquid glass

For reusable glass surfaces, use `bare_glass_stack` and the preset surfaces:

```c
bare_glass_stack(.width = BARE_GROW, .height = BARE_GROW) {
    draw_background();
    bare_glass_layer(BARE_GLASS_FROST, .width = 320, .padding = 24, .radius = 28) {
        bare_text("Settings");
    }
    bare_glass_surface(BARE_GLASS_LENS, .width = 96, .height = 96, .radius = 48);
}
```

`BARE_GLASS_CLEAR`, `BARE_GLASS_FROST`, `BARE_GLASS_LENS`, `BARE_GLASS_DOCK`, and
`BARE_GLASS_DROP` supply a look and default animation. All `BareBox` options work;
`.glass` can replace a preset's glass settings. A custom `.style` replaces the
preset look. Dock and drop presets have matching merge distance, so declare
them next to each other to make them join. A glass layer holds controls and
text; a surface is a leaf. Give moving or conditional surfaces stable `.id` values.


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
           .zoom = 1.5f,            /* magnify what's behind, like a loupe (default 1) */
           .light = 90,             /* turn the light catching the rim, degrees clockwise */
           .tint = BARE_RGBA(255, 255, 255, 20) }
```

**Glass in motion:** with `.animate`, every glass setting animates like the rest of the look, and so does turning glass
on and off: it condenses out of what's behind it and dissolves back into it. A control can turn to glass only while it's
held, the way a switch's knob lifts off its track:

```c
bool held = bare_this().pressed;
bare_box(.width = held ? 120 : 90, .height = held ? 80 : 56, .radius = 40,
         .color = held ? BARE_RGBA(255, 255, 255, 0) : BARE_WHITE,    /* solid white fades out... */
         .glass = held ? (BareGlass){ .refraction = 1, .zoom = 1.15f } : (BareGlass){ 0 },   /* ...glass condenses */
         .animate = BARE_SPRING(0.38f, 0.72f));
```

Change the tint, size and radius together and a round lens turns into a red capsule button.
`examples/glass_motion/` plays five such moments on a loop: a lens forming and dissolving, a capsule with a turning
light, a loupe that lifts off a colour wheel and magnifies it, the loupe morphing into a tinted button over a
checkerboard, and a switch whose knob turns to glass while held (`glass_motion 4.4` starts 4.4 s in).

**Merging:** glass siblings declared one after another with `.merge` melt into each other like drops of liquid when
they come within that many pixels. Up to 8 shapes merge into one surface.

```c
bare_box(.width = 300, .height = 90, .radius = 45, .glass = { .merge = 50 }, .offset = { x, y });
bare_box(.width = 90, .height = 90, .radius = 45, .glass = { .merge = 50 }, .offset = { x2, y2 });
```

Glass is the most expensive thing Bare draws (each glass group copies the region it bends), so use it for a few hero
elements, not for every list row. `examples/glass.c` and `examples/glass_motion/` are the showcases.

**A window of glass (Windows):** `bare_window_backdrop()` is the live desktop behind the window, as an image the size
of the window that updates by itself. Draw it first, behind everything, and every `.glass` above it bends what is
really behind the window rather than a blur of it. Pair it with a frameless window and your own title bar:

```c
bare_window_with(.width = 1200, .height = 800, .title = "Notes", .frameless = true, .min_width = 640, .min_height = 420);
while (bare_frame()) {
    BareVec2 size = bare_window_size();
    bare_stack(.width = size.x, .height = size.y, .image = bare_window_backdrop()) {
        bare_row(.width = BARE_GROW, .height = 44, .window_drag = true) {   /* drags the window */
            bare_box(.width = BARE_GROW);
            if (bare_button("x").clicked) bare_window_close();            /* buttons stay clickable */
        }
        /* ... glass panels ... */
    }
}
```

To see what's behind itself the window is left out of screen capture, but only while it moves or resizes: at rest
it shows in screenshots, recordings and screen shares, and the backdrop keeps its last picture. It returns 0 where it isn't available (other systems, Windows before 10 2004): draw a background
of your own then. `bare_image_update` refreshes any image of the same size from new pixels in the same way.

## 13. Canvases and your own shaders

What `<canvas>` and WebGL are to a web page. A **canvas** is an image your shaders draw into; a **pass** runs a
fragment shader once for every pixel of a canvas. Show a canvas with `.image` like any picture, or feed it into the
next pass: that's how blurs, generated backgrounds and whole simulations are built.

```c
#include "bare.h"
#include "my_shaders.h"                                  /* generated from shaders/*.glsl at build time */

BareImage sky = bare_canvas(.width = 512, .height = 512);
BareShader clouds = bare_shader(&clouds_shader);

while (bare_frame()) {
    struct { float time; } p = { (float)bare_time() };   /* the shader's params */
    bare_pass(.target = sky, .shader = clouds, BARE_PARAMS(p));
    bare_box(.width = BARE_GROW, .height = BARE_GROW, .image = sky, .radius = 24);
}
```

**Writing a shader** (GLSL 450, the fragment half only; Bare supplies the rest):

```glsl
#version 450
layout(location = 0) in vec2 v_uv;                        // 0..1 across the canvas, (0, 0) top-left
layout(location = 0) out vec4 o_color;                    // straight alpha, like every Bare image
layout(set = 2, binding = 0) uniform sampler2D u_prev;    // .inputs[0], then binding 1 = .inputs[1] ...
layout(set = 3, binding = 0) uniform Params { float time; };   // .params, std140 layout

void main() { o_color = vec4(v_uv, 0.5 + 0.5 * sin(time), 1.0); }
```

Compile shaders with one CMake line; the header holds every GPU's format (SPIR-V, DXIL, Metal) and how many inputs
and params each shader takes. Commit it, and people without the shader tools can still build:

```cmake
bare_add_shaders(myapp OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/generated/my_shaders.h SOURCES shaders/clouds.glsl)
```

- `.format`: `BARE_RGBA8` (default), or half floats for HDR colour and simulation state: `BARE_RGBA16F`,
  `BARE_RG16F` (e.g. velocity), `BARE_R16F` (e.g. pressure). `.nearest = true` turns off filtering;
  `.repeat = true` wraps sampling outside 0..1.
- `.blend`: `BARE_BLEND_NONE` (default: replace), `BARE_BLEND_ADD`, `BARE_BLEND_OVER`.
- A pass can't read the canvas it writes. Ping-pong between two canvases instead, swapping them after each pass.
- Size canvases in pixels: `bare_window_size()` times `bare_pixel_scale()` (`devicePixelRatio`) for a sharp
  full-window canvas.
- Passes run on the GPU in the order you call them, before the frame is drawn. Skip them when nothing changes and
  your app costs nothing, which a page's endless `requestAnimationFrame` loop never does.
- std140 in C: floats and `vec2`s pack as in C, but a `vec3` / `vec4` starts on a 16-byte boundary: use `float[4]`.

**Drawing your own triangles** (WebGL's `drawArrays` / `drawArraysInstanced`): `bare_draw` runs a *program*, a
`.glsl` file with a vertex half as well, over your vertex data, into a canvas:

```c
typedef struct { float pos[4], color[4]; } Vert;   /* each attribute is a vec4 slot */
bare_draw(.target = sky, .shader = ribbon, .strip = true, BARE_VERTICES(verts, n), BARE_PARAMS(p));
bare_draw(.target = sky, .shader = dots, .vertex_count = 6, BARE_INSTANCES(dots, 300),
          .blend = BARE_BLEND_ALPHA, BARE_PARAMS(p));
```

```glsl
#version 450
layout(set = BARE_PARAMS_SET, binding = 0) uniform Params { vec4 res; };   // both halves may read it
#ifdef BARE_VERTEX
layout(location = 0) in vec2 a_pos;                       // the vertex's attributes, then the instance's
layout(location = 1) in vec4 a_color;
layout(location = 0) out vec4 v_color;
void main() { v_color = a_color; gl_Position = vec4(a_pos / res.xy * 2.0 - 1.0, 0.0, 1.0); }
#else
layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 o_color;
void main() { o_color = v_color; }
#endif
```

- Attributes are 16 bytes each (declare them `float` to `vec4`), so `.vertex_size` and `.instance_size` are multiples
  of 16. Without `.vertices` the program makes its vertices from `gl_VertexIndex` (and `gl_InstanceIndex`).
- A draw keeps what its triangles don't cover; `.clear = true, .clear_color = {...}` clears the targets first.
- `.strip = true` for a triangle strip. `BARE_BLEND_ALPHA` is WebGL's `blendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA)`.
- Clip space is WebGL's: `y = 1` is the top of the picture, in a `.bottom_up` canvas too, so ported vertex shaders
  work unchanged. `examples/draw/` shows a strip and instanced dots.

`examples/fluid/` is the full showcase: a GPU fluid simulation in 14 passes (Screen+'s Fluids wallpaper), and `bench/` measures it against the same simulation running in Edge.

## 14. CSS to Bare cheat sheet

| CSS / HTML / JS | Bare |
|---|---|
| `<div>` | `bare_box(...)`, or `bare_row/column/stack(...) { }` with children |
| `<canvas>` + WebGL shaders | `bare_canvas(...)`, `bare_pass(...)`, `bare_draw(...)`, `.image = canvas` |
| `devicePixelRatio` | `bare_pixel_scale()` |
| `display: flex; flex-direction: row` | `bare_row() { }` |
| `flex-direction: column` | `bare_column() { }` |
| `position: absolute; top: 0; right: 0` | `.position = BARE_ABSOLUTE, .anchor = BARE_TOP_RIGHT` (+ `.x`, `.y`) |
| overlapping layers | `bare_stack() { }` |
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
| `onclick` | `if (bare_box(...).clicked)` |
| `:hover { ... }`, `:active` | `.hover = { .color = ... }`, `.press = { ... }` |
| `transform: scale(1.05)` | `.scale = 1.05f` |
| `cursor: pointer` | `.cursor = BARE_CURSOR_HAND` |
| `keydown` | `bare_key_pressed(BARE_KEY_ENTER)` |
| `transition: all .3s ease-out` | `.animate = BARE_EASE(0.3f, BARE_EASE_OUT)` |
| FLIP animations of layout changes | `.animate_layout = BARE_SPRING_SMOOTH` and stable ids |
| text, `font-size`, `color`, `line-height` | `bare_text("Hello", .size = 20, .color = ..., .line_height = 1.5f)` |
| `color` on a parent (inherited) | `.text_color = ...` on the container |
| `text-align: center` | `.align = BARE_CENTER` on the text |
| `white-space: nowrap` | `.nowrap = true` |
| `text-overflow: ellipsis` / `line-clamp: 2` | `.nowrap = true, .ellipsis = true` / `.max_lines = 2, .ellipsis = true` |
| `text-decoration: underline / line-through` | `.underline = true` / `.strike = true` |
| `letter-spacing: 2px`, `text-transform: uppercase` | `.letter_spacing = 2`, `.transform = BARE_UPPERCASE` |
| `<input>`, `<textarea>`, `type=password` | `bare_input(buf, sizeof buf, ...)`, `.multiline = true`, `.password = true` |
| `tabindex="0"`, `:focus`, `.focus()` | `.focusable = true`, `.focus = { ... }`, `bare_focus(id)` |
| `@font-face` | `bare_font_load("x.ttf")`, then `.font = f` |
| `<img src="a.png">`, `border-radius: 50%` | `.image = bare_image_load("a.png")`, `.radius = size / 2` |
| `overflow: hidden` / `overflow: auto` | `.clip = true` / `.scroll = BARE_SCROLL_Y` |
| virtual list (react-window) | `bare_visible(i, count, item_size) { ... }` |
| `class="card"` | `.style = &card` (a `BareBox`; text: a `BareTextStyle`) |
| CSS variables, light/dark themes | `bare_theme()->accent`, `bare_set_theme(bare_theme_light())` |
| `<button>`, `<input type=checkbox>`, `<input type=range>` | `bare_button`, `bare_checkbox`, `bare_slider` |
| `<b>`, `<i>`, `<span>`, `font-weight`, `text-shadow` | `bare_paragraph` + `bare_span`, `.weight`, `.italic`, `.shadow` |
| CodeMirror / Monaco | `bare_editor(doc, ...)` on a `BareDoc` |
| `<input type=file>` | `bare_file_dialog` + `bare_file_chosen` |

## 15. Debugging

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

## 16. What's next

Bare is being built one milestone at a time ([ROADMAP](ROADMAP.md)): next a second backend and the browser (M9), then
the rest of what CSS has (M10: gradients, per-corner radii, rotation, filters, flex-wrap...). The examples in
`examples/` grow with it: `hello.c`, `shapes.c`, `glass.c`, `layout.c`, `buttons.c`, `cards.c`, `text.c`,
`scroll.c`, `styles.c`, `editor.c`, `widgets.c`, `code.c`, `write/`, `glass_motion/`, `fluid/` and `draw/` today.
