# Bare

A UI framework in pure C11 for native apps that want what HTML + CSS + JS give you (flexible layout, rich styling,
smooth animation, interaction) without a browser: tiny binaries, millisecond startup, GPU rendering.

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

## Status

Early. Done: GPU renderer (SDL3), rounded boxes, borders and shadows, liquid glass, flexbox layout. Next: interaction,
animation, text. See [docs/ROADMAP.md](docs/ROADMAP.md).

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

Bare is created by CoolN. The code, shaders and documentation are written with
[Claude](https://www.anthropic.com/claude) (Anthropic's AI model), working from CoolN's design decisions and direction.
