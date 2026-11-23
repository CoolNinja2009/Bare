#include "bare.h"
int main(void) {
    bare_window(1920, 1080, "My App");
    while (bare_frame()) {
        bare_box(.width = 300, .height = 200, .radius = 16,
                 .color = BARE_RGB(40, 120, 255));
    }
    bare_quit();
}
