# Bare

Bare is a UI framework in pure C11 that gives native apps the flexibility and polish of HTML + CSS + JS (flexbox
layout, rich styling, smooth animation, interaction) with none of the browser's overhead: tiny binaries, millisecond
startup, GPU rendering. Design: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). Plan: [docs/ROADMAP.md](docs/ROADMAP.md).
Public API: [include/bare.h](include/bare.h).



## Simplicity is the #1 rule

Bare must be as easy as raylib. This must always compile and work:

```c
#include "bare.h"
int main(void) {
    bare_window(1920, 1080, "My App");
    while (bare_frame()) {
        bare_box(.width = 300, .height = 200, .radius = 16,
                 .color = BARE_RGB(40, 120, 255));
    }
    bare_quit();
}
```

- One window is current implicitly; no handles in the common case. Multi-window (`bare_window_new`, `bare_use`) later.
- Elements take named, optional parameters (macro + C11 designated initializers). **0 always means "default"**;
  `BARE_ZERO` (-0.0f) is a real zero for fields whose default isn't 0. Never pre-fill defaults inside a macro
  (GCC/Clang warn on overridden initializers, and `_Pragma` can't silence it inside an expression).
- Backend selection is build-time only, never in user code.
- Users never see draw commands, renderers, arenas or GPU types unless they include `bare/bare_core.h` or
  `bare/bare_backend.h`.
- Before adding API: can it be one call with good defaults? If an example needs more than ~10 lines to show a
  feature, simplify the API.
- Two loop styles: `while (bare_frame())` (default) and `bare_run(frame_fn, user)` (required on sokol_app and the
  web, works everywhere). Both go through `bare_ctx_begin` / `bare_ctx_end`.

## Decisions (don't change without asking the user)

- C11, no C++. Shaders are the only non-C code.
- Prefixes: `bare_` functions, `Bare` types, `BARE_` macros and enum values.
- Layers: **core** (layout, styles, animation, interaction → flat draw commands: rect, text, image, clip push/pop)
  never includes SDL/sokol/GPU headers; **renderer** (`BareRenderer`) turns draw commands into GPU calls;
  **platform** (`BarePlatform`) does window, input, time, clipboard.
- Backends: SDL3 with the SDL3 GPU API first; sokol (sokol_gfx + sokol_app) later. Interfaces stay backend-agnostic.
- Immediate-mode API with retained state keyed by stable element IDs (`BARE_ID("save")`, `BARE_IDI("row", i)`,
  automatic from parent + call site otherwise).
- Flexbox layout (direction, padding, gap, grow/shrink, fixed/fit/grow/percent, alignment). Grid later.
- Rounded rects, borders, soft shadows via an instanced SDF fragment shader. One shader source (`shaders/bare.glsl`),
  cross-compiled per backend (glslc + SDL_shadercross; sokol-shdc later). Compiled blobs committed.
- Animation: every animatable property (position, size, colour, opacity, radius, shadow, scale) transitions with easing
  (linear, ease-in/out, cubic-bezier) or springs (response + damping ratio); interruptible without jumps.
- Memory: per-frame arena; no malloc in the steady-state frame.
- Interaction: hit testing respecting clip and z-order, hover/press/click per ID, scroll wheel.
- Deferred: text input/editing, keyboard focus/tab, drag and drop, touch gestures, IME, scripting, grid.
- Dependencies via CMake FetchContent at pinned versions (SDL3, stb). Nothing vendored except the default font.
- Core logic (layout, easing, springs, arena, IDs, hit testing, emit) is unit-tested headlessly: no GPU, no window.
- If a backend needs something the core lacks, fix the abstraction; no backend-specific special cases in the core.

## Layout

```
include/bare.h            public API (most apps include only this)
include/bare/             advanced headers: bare_core.h (headless ctx, draw list), bare_backend.h (interfaces)
src/core/                 the core; no platform headers (see ARCHITECTURE §2 for the file list)
src/bare.c                simple API on top of the core + the build-time backend
src/backend/sdl3/         SDL3 platform + SDL3 GPU renderer
src/backend/sokol/        (M9)
shaders/                  bare.glsl (boxes, glyphs, images), glass.glsl (liquid glass); generated/ = compiled blobs
cmake/compile_shaders.cmake  the bare_shaders target's script (glslc + shadercross -> generated/bare_shaders.h)
assets/fonts/             default font (Inter Latin subset, OFL)
tests/                    headless tests; bare_test.h is the runner
examples/                 one small program per feature
docs/                     ARCHITECTURE.md, ROADMAP.md
```

## Conventions

- 4-space indent, braces on the same line, `snake_case` functions and variables, `PascalCase` types.
- Public: `bare_name`. Internal across core files: `bare__name` (double underscore) declared in
  `src/core/internal.h`. File-local: `static`, no prefix needed. Macro internals: `BARE__NAME`.
- Public functions that macros expand to end in `_` (`bare_box_`) and are documented as not for direct use.
- No globals in `src/core/` except the current-context pointer in `context.c`; everything else hangs off
  `BareContext`. App state and the backend pair live in `src/bare.c`.
- Draw-command unions are named `as` (`cmd.as.rect`), not anonymous.
- Floats for geometry, in logical pixels; physical pixels only in the draw list and backends.
- No allocation in per-frame paths except from the frame arena. All heap use goes through `BareAllocator`.
- User mistakes (unclosed container, duplicate ID) log a clear message with file:line and recover; they don't crash.
  `BARE_ASSERT` is for Bare's own invariants.
- Keep files small and readable (aim under ~600 lines); comments explain *why*, not what.
- Every testable change gets a headless test in `tests/`.

## Build and test (Windows)

Toolchains present: MinGW-w64 GCC + Ninja (`J:\msys\mingw64\bin`) and MSVC Build Tools 2022 (CMake bundled).
Shader tools: `glslc` (GLSL → SPIR-V, on PATH) and SDL_shadercross 3.0.0 (SPIR-V → DXIL / MSL / reflection JSON) at
`tools\shadercross\SDL3_shadercross-3.0.0-windows-VC-x64\bin\shadercross.exe`. That build came from the official repo's
CI on 2026-09-03 (commit `1ff05bec57`, there are no releases), so `tools/` is local-only: never commit it. Only the
generated shader blobs are committed. Usage: `shadercross in.spv -s SPIRV -d DXIL -t fragment -o out.dxil`.

MinGW is not on PATH by default: prepend `J:\msys\mingw64\bin` for the build (in bash, do it in a subshell: it
shadows the Windows `python` that has PIL).

```powershell
$env:PATH = "J:\msys\mingw64\bin;$env:PATH"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=gcc
cmake --build build
ctest --test-dir build --output-on-failure
.\build\bin\glass.exe
```

Executables land in `build\bin\` (MSVC: `build-msvc\bin\Debug\`). The first configure downloads SDL 3.4.20 (~2 min).

**Checking rendering by eye:** `BARE_CAPTURE=C:\path\out.bmp` (+ `BARE_CAPTURE_FRAMES=N`) saves frame N and quits;
convert with PIL and look at it. `SDL_GPU_DRIVER=direct3d12|vulkan` forces a GPU driver (both must match pixel for
pixel). `BARE_GPU_DEBUG=1` enables the GPU validation layer.

MSVC (no developer prompt needed with the VS generator):

```powershell
cmake -S . -B build-msvc -G "Visual Studio 17 2022"
cmake --build build-msvc --config Debug
ctest --test-dir build-msvc -C Debug --output-on-failure
```

Options: `-DBARE_BACKEND=SDL3|SOKOL`, `-DBARE_BUILD_TESTS=ON`, `-DBARE_BUILD_EXAMPLES=ON`,
`-DBARE_DEFAULT_FONT=ON`. Regenerate shader blobs: `cmake --build build --target bare_shaders`.
Both compilers must build warning-free; the API header is checked with `-Wall -Wextra -Wpedantic -Wshadow` and `/W4`.

## Session workflow

One roadmap milestone per session: write the code, write headless tests, build with both compilers when practical,
run tests and the milestone's example, fix failures, then update **Current status** below and tick the milestone in
`docs/ROADMAP.md`. Keep `docs/GUIDE.md` (the user-facing tutorial) and the web parity checklist in the roadmap true.

**Commits:** author CoolN. Never add a `Co-Authored-By` trailer (the user's rule); the Claude credit lives in
`README.md` instead. Commit only when the user asks or a milestone is done.

## Current status

**2026-11-23: M1, M1.5 (liquid glass), M2 (flexbox layout) and M3 (interaction) done.** Next: M4 (animation).

- Works: window (hidden until the first frame), SDL3 GPU renderer (D3D12 and Vulkan give pixel-identical output),
  instanced SDF boxes with radius, border, Gaussian shadow, opacity; idle CPU 0 after startup; clean close.
  Examples: `hello.c` (the 8-line program), `shapes.c`, `glass.c`, `layout.c`, `buttons.c`. 5 test programs / 67
  tests pass on GCC and MSVC; everything builds warning-free on both. Bare also builds as a subproject (`add_subdirectory`), with
  tests and examples off by default there.
- Layout (M2): rows, columns, stacks; fit / fixed / grow (CSS flex: 1 with freezing at min/max) / percent / BARE_ZERO;
  min/max; shrink down to min-content (fixed and percent never shrink); padding, gap, all align and justify modes.
  Retained node store with eviction; automatic IDs from parent + call site + per-site counter; duplicate IDs logged
  once with both call sites. `BareState.rect` is last frame's drawn rect. `layout.c` example checked by capture.
- Liquid glass: `.glass` refraction through a curved rim, dispersion, frost, rim light, merging of `.merge` siblings;
  `.offset` (unsnapped) and `bare_stack` added for it. Checked by eye via captures, including 3x zooms of edges.
- Interaction (M3): hit list from the last drawn frame, hover/press/click/right-click chains (DOM bubbling), `.hover` /
  `.press` looks applied the same frame, `.cursor`, `.passthrough`, rounded-corner and clip-aware hits, keys
  (layout-aware), focus loss releases everything, wheel offsets stored per scroll container. `.scale` works now
  (visual transform about the centre, composed with `.offset`, drawn and hit-tested). Verified in the real app by
  injecting SDL events from a scratch build and capturing.
- Stand-ins until their milestone: looks switch instantly until M4 animates them; text, images, keys are not implemented
  (bare_text etc. are declared but don't link yet); the text-measure hook in layout pass 3 arrives with text in M5;
  clip push/pop pairing is tested when clips exist (M6).
- Not verified: DPI scale above 100% (this display is 100%; the core's 1.5x/2x maths is unit-tested); live window
  resizing on Windows (SDL's modal resize loop may pause drawing while dragging: render from an event watch if so),
  so `layout.c`'s reflow was only checked at its starting size.
- Known cost: each glass command copies the whole frame and builds its mipmaps (see ROADMAP M1.5 to-dos).
- User decisions: both loop styles (`bare_frame` loop + `bare_run`); dependencies via FetchContent; liquid glass was
  the first showcase; ScreenPlus (`..\Wallpaper+\ScreenPlus`) is the first real app to port later; Bare must reach
  full HTML/CSS/JS parity and beyond (tracked in the roadmap's web parity checklist and M10).
