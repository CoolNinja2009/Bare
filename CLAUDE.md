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
cmake/BareShaders.cmake   bare_add_shaders(): apps' own GLSL -> a header of BareShaderCode (canvas passes)
assets/fonts/             default font (Inter Latin subset, OFL)
tests/                    headless tests; bare_test.h is the runner
examples/                 one small program per feature; fluid/ = canvas showcase with its own shaders
bench/                    benchmarks vs the browser (bench.ps1; README.md has the method and results)
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
`-DBARE_DEFAULT_FONT=ON`, `-DBARE_BUILD_BENCH=ON` (Release; see `bench/README.md`). Regenerate shader blobs: `cmake --build build --target bare_shaders`.
Both compilers must build warning-free; the API header is checked with `-Wall -Wextra -Wpedantic -Wshadow` and `/W4`.

## Session workflow

One roadmap milestone per session: write the code, write headless tests, build with both compilers when practical,
run tests and the milestone's example, fix failures, then update **Current status** below and tick the milestone in
`docs/ROADMAP.md`. After changes to the renderer, the frame loop or core hot paths, rerun `bench\bench.ps1` (mouse
off the screen) and update `bench/README.md` and the **Optimizations** list below. Keep `docs/GUIDE.md` (the user-facing tutorial) and the web parity checklist in the roadmap true.

**Commits:** author CoolN. Never add a `Co-Authored-By` trailer (the user's rule); the Claude credit lives in
`README.md` instead. Commit only when the user asks or a milestone is done.

## Current status

**2026-11-23: M1, M1.5 (liquid glass), M2 (flexbox layout), M3 (interaction) and Canvas (custom GPU shaders) done.**
Next: M4 (animation); the optimizations below can be interleaved.

- Works: window (hidden until the first frame), SDL3 GPU renderer (D3D12 and Vulkan give pixel-identical output),
  instanced SDF boxes with radius, border, Gaussian shadow, opacity; idle CPU 0 after startup; clean close.
  Examples: `hello.c` (the 8-line program), `shapes.c`, `glass.c`, `layout.c`, `buttons.c`, `fluid/`. 6 test
  programs pass on GCC and MSVC; everything builds warning-free on both. Bare also builds as a subproject
  (`add_subdirectory`), with tests and examples off by default there.
- Layout (M2): rows, columns, stacks; fit / fixed / grow (CSS flex: 1 with freezing at min/max) / percent / BARE_ZERO;
  min/max; shrink down to min-content (fixed and percent never shrink); padding, gap, all align and justify modes.
  Retained node store with eviction; automatic IDs from parent + call site + per-site counter; duplicate IDs logged
  once with both call sites. `BareState.rect` is last frame's drawn rect. `layout.c` example checked by capture.
- Liquid glass: `.glass` refraction through a curved rim, dispersion, frost, rim light, merging of `.merge` siblings;
  `.offset` (unsnapped) and `bare_stack` added for it. Checked by eye via captures, including 3x zooms of edges.
- Interaction (M3): hit list from the last drawn frame, hover/press/click/right-click chains (DOM bubbling), `.hover` /
  `.press` looks applied the same frame, `.cursor`, `.passthrough`, rounded-corner and clip-aware hits, keys
  (layout-aware), focus loss releases everything, wheel offsets stored per scroll container. `.scale` works
  (visual transform about the centre, composed with `.offset`, drawn and hit-tested).
- Canvas: `bare_canvas` (RGBA8 / RGBA16F / RG16F / R16F, nearest, repeat), `bare_shader`, `bare_pass` (up to 8
  inputs, std140 params, blend none/add/over), `bare_add_shaders()` in CMake, `.image` boxes, `bare_image_size`,
  `bare_image_from_pixels`, `bare_pixel_scale()`. GPU ops are queued in `src/core/gpu.c` and run by the renderer
  before the frame. `examples/fluid/` ports Pavel Dobryakov's WebGL fluid sim (MIT; Lively defaults) with a frosted
  settings panel made of Bare boxes (colour dots stand in for labels until M5); it sleeps once the fluid settles.
  Its dither is applied after gamma (the original's grain was a bug). Stirring, sliders and toggles were checked by
  injecting SDL events from a scratch build; captures checked on D3D12, Vulkan and an MSVC build.
- Benchmark (`bench/README.md`, 2026-11-23, RTX 5070, 200 Hz): Fluids in Bare vs the WebGL original in Edge, both at
  200 fps: CPU 7.8-8.2% of a core vs 22-37%; GPU equal (~6%); working set 90-153 MB vs 461-626 MB; 1 process vs
  8-18; idle 0% vs never idle; startup ~640 ms vs ~720 ms; one 4.2 MB exe.
- Stand-ins until their milestone: looks switch instantly until M4 animates them; text and image loading are not
  implemented (bare_text, bare_image_load etc. are declared but don't link yet); the text-measure hook in layout
  pass 3 arrives with text in M5; clip push/pop pairing is tested when clips exist (M6).
- Not verified: DPI scale above 100% (this display is 100%; the core's 1.5x/2x maths is unit-tested); live window
  resizing on Windows (SDL's modal resize loop may pause drawing while dragging: render from an event watch if so),
  so `layout.c`'s reflow was only checked at its starting size; canvases on Metal (MSL is generated, untested).
- User decisions: both loop styles (`bare_frame` loop + `bare_run`); dependencies via FetchContent; liquid glass was
  the first showcase; ScreenPlus (`..\Wallpaper+\ScreenPlus`) is the first real app to port later; Bare must reach
  full HTML/CSS/JS parity and beyond (tracked in the roadmap's web parity checklist and M10); Bare changes must be
  general features, never shaped around one app; example UIs are built with Bare only; the benchmarks drive
  optimization (rerun them, keep `bench/README.md` current).

## Optimizations (measured, not yet done)

Ordered by impact on the "beats the browser" claim. Numbers from `bench/` on the machine above.

1. **Startup (~640 ms; Edge ~720 ms).** `SDL_CreateGPUDevice` alone is 350-430 ms on D3D12 (Vulkan: ~200 ms for
   the device + ~170 ms claiming the window); SDL init + window ~25 ms; Bare's own setup and process launch ~150 ms
   (`bench/startup.c` breaks it down). Ideas: create the GPU device on a thread while the window appears at once
   with the background colour painted (perceived startup in tens of ms); find how much of the D3D12 time is the
   driver vs SDL's own setup (time a raw `D3D12CreateDevice`; SDL builds internal blit pipelines); create Bare's
   pipelines lazily or in parallel (glass only on first use); a pipeline cache.
2. **CPU per canvas pass.** 7.8% of a core at 200 fps with ~55 passes a frame is ~7 us per pass (render pass begin,
   pipeline, samplers, uniforms, draw, SDL's barriers). Profile it; skip redundant binds. Also measure Bare's own
   per-frame cost with a continuous `hello`.
3. **Memory (90-153 MB working set, 158-214 MB private; varies run to run).** Measure `hello`'s baseline; the
   offscreen scene + backdrop mip chain exist even without glass (allocate on first glass use); SDL's D3D12 staging
   and descriptor heaps; the fluid's own textures are ~40 MB.
4. **Binary size (4.2 MB stripped).** Mostly static SDL3 with every subsystem: build it without audio, joystick,
   haptic, camera, sensor, hidapi and SDL_Renderer; `-Os`, LTO, `--gc-sections`. Target: under 1.5 MB.
5. **Glass:** each glass command copies the whole frame and builds its mipmaps; copy only the region behind it and
   share one copy between non-overlapping groups.
6. **Showing a canvas** costs an extra full-screen draw (pass into a canvas, then the box samples it). Later: let a
   pass draw straight into a box's area of the frame.
7. **Canvas API gaps found by the port:** updating an image's pixels in place (video and GIF previews for
   ScreenPlus), reading a canvas back, compute shaders, passes into a sub-rectangle.
