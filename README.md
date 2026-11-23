# Bare

A UI framework in pure C11 for native apps that want what HTML + CSS + JS give you (flexible layout, rich styling,
smooth animation, interaction, your own GPU shaders) without a browser: one small exe, a fraction of the CPU and
memory, nothing at all when idle, GPU rendering.

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

It includes liquid glass with real refraction:

```c
bare_box(.width = 520, .height = 96, .radius = 48, .glass = BARE_GLASS);
```

And canvases for your own shaders, like `<canvas>` + WebGL: [examples/fluid](examples/fluid) is a GPU fluid
simulation with a frosted settings panel built from Bare boxes.

## Status

Early. Done: GPU renderer (SDL3), rounded boxes, borders and shadows, liquid glass, flexbox layout, hover / press /
click / keys, canvases and custom shaders. Next: animation, text. See [docs/ROADMAP.md](docs/ROADMAP.md).

Measured against the same fluid simulation running in Microsoft Edge, both at 200 fps
([bench/README.md](bench/README.md)): about 3x less CPU, 4-5x less memory, 1 process instead of 8-18, and 0% CPU and
GPU once the fluid settles (the browser never stops). GPU cost per frame is the same, and startup (~0.6 s, mostly GPU
driver setup) is not yet faster than the browser's: that's next on the optimization list.

- How to use it: [docs/GUIDE.md](docs/GUIDE.md)
- How it works: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)

## Build (Windows)

Needs CMake 3.24+ and either MinGW-w64 GCC + Ninja or Visual Studio 2022. SDL3 is downloaded on the first configure.

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
.\build\bin\glass.exe
```

## Credits

Bare is created by CoolN and [Claude](https://www.anthropic.com/news/introducing-claude)

`examples/fluid` is a port of [WebGL Fluid Simulation](https://github.com/PavelDoGreat/WebGL-Fluid-Simulation)
by Pavel Dobryakov (MIT licence, see [examples/fluid/LICENSE.txt](examples/fluid/LICENSE.txt)).
