# Benchmarks

Bare is meant to beat the browser at what browsers do. These benchmarks measure that on a real app
and are the baseline for optimizing Bare: rerun them after any change to the renderer, the frame
loop or the core, and update the numbers below.

## Fluids: Bare vs Microsoft Edge

The same app twice: [examples/fluid](../examples/fluid) (Bare canvases, GPU passes, a Bare
settings panel) and the WebGL original it was ported from (Pavel Dobryakov's WebGL Fluid
Simulation, as packaged for Lively Wallpaper in ScreenPlus's samples). Both run the same shaders at
the same resolutions (dye 1024, simulation 128, bloom 256, sunrays 196) with Lively's defaults.

```powershell
cmake -S . -B build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DBARE_BUILD_BENCH=ON
cmake --build build-rel
powershell -File bench\bench.ps1            # everything, ~3 min; or: start | base | bare | edge | edgestir
.\build-rel\bin\startup.exe                 # where Bare's startup time goes (SDL alone)
```

`-Sample <dir>` points at the original (default: `..\Wallpaper+\ScreenPlus\samples\0wj1biqk.f41`
next to this repo). **Keep the mouse off the screen while it runs**: moving it over Bare wakes the
simulation, and over Edge it stirs the fluid.

### Method

- **Bare active:** `fluid_bench`, which is `examples/fluid/fluid.c` unchanged plus
  [fluid_driver.h](fluid_driver.h) force-included. It moves the mouse every frame so the fluid never
  settles, hides the panel (the browser version has none), and shows fps in the title.
- **Bare idle:** the real `fluid.exe`, 15 s after launch, once the fluid has settled and Bare sleeps.
- **Edge:** a copy of the sample in `%TEMP%\bare-bench` with Lively's defaults applied through
  `livelyPropertyListener` (as Lively does), a frame counter in `document.title`, and a `?stir` mode
  that moves the mouse along the same path as `fluid_bench`. Opened with `--app`, a fresh profile,
  at 1280x720. Its processes are the browser process plus all its descendants (GPU, renderers,
  utilities).
- Both windows are pinned on top: a covered Edge window stops drawing and would measure as free.
- 5 s warm-up, then 15 one-second samples: CPU time, working set and private bytes,
  `\GPU Engine(*)\Utilization Percentage` for the app's own processes and for the whole adapter
  (which includes the compositor, dwm). Don't poll `nvidia-smi` while measuring: it stalled both apps
  to ~90 fps.
- Startup: launch until Bare's window appears (it shows at its first frame) / until the page's
  title says "ready" (after its second animation frame). Median of 5 warm runs.

### Results

2026-11-23. Ryzen 7 7700X (16 threads), RTX 5070, 200 Hz G-Sync display (so both run at 200 fps),
Windows 11, Edge 158, MinGW GCC Release build. Ranges are over the clean runs.

| | Bare | Edge, stirred the same way |
|---|---|---|
| Frame rate | 200 | 200 |
| CPU (% of one core) | **7.8-8.2** | 21.8-36.8 |
| GPU, own processes | 5.8-6.0% | 5.8-6.0% |
| GPU 3D, whole adapter | 9.2-10.0% | 9.3-11.3% |
| Working set | **90-153 MB** | 461-626 MB |
| Private memory | 158-214 MB | 286-378 MB |
| Processes | 1 | 8-18 |
| After the fluid settles | **0% CPU, 0% GPU** | unchanged: it never stops |
| Launch to first frame | ~640 ms | ~720 ms |
| Download | one 4.2 MB exe (static SDL3, stripped) | a browser |

Edge unstirred measured 11-115% of a core (its fresh profile does background work); its GPU use
matches the stirred run.

**Reading it:** the GPU work per frame is a tie, as it should be: same shaders, same pixels. Bare
wins on CPU (about 3x), memory (about 4-5x), process count, and above all on idle, where it does
nothing at all. Startup is not a win yet; see the optimizations in [CLAUDE.md](../CLAUDE.md).

### Where startup goes (`startup.exe`, SDL alone)

| Step | D3D12 | Vulkan |
|---|---|---|
| `SDL_Init` + hidden window | ~25 ms | ~25 ms |
| `SDL_CreateGPUDevice` | 350-430 ms | ~200 ms |
| `SDL_ClaimWindowForGPUDevice` | ~5-40 ms | ~170 ms |
| First frame | <10 ms | <10 ms |

Bare's own setup (pipelines, fonts, first layout) and process launch add roughly 150 ms more.

### History

- Texture cycling on canvas passes (SDL GPU `cycle = true`) gave every write to a ping-pong canvas
  a new copy of its memory: 418 MB private. Removed: 158 MB.
- The original's dither was added before gamma correction, which turned it into visible grain of up
  to 13/255 near black. Bare's port dithers after gamma.
