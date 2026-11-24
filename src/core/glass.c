/* Reusable glass looks. Uses only public Bare elements and style resolution. */
#include "bare.h"

static const BareBox presets[] = {
    [BARE_GLASS_CLEAR] = { .glass = { .refraction = 1.0f }, .shadow = { 0, 8, 24, 0, { 0, 0, 0, 70 } },
                            .animate = { .kind = BARE_ANIM_EASE, .duration = 0.2f, .bezier = { BARE_EASE_OUT } } },
    [BARE_GLASS_FROST] = { .glass = { .refraction = 0.6f, .blur = 18.0f, .tint = { 255, 255, 255, 30 } }, .shadow = { 0, 8, 24, 0, { 0, 0, 0, 70 } },
                            .animate = { .kind = BARE_ANIM_EASE, .duration = 0.2f, .bezier = { BARE_EASE_OUT } } },
    [BARE_GLASS_LENS] = { .glass = { .refraction = 1.2f, .zoom = 1.25f, .blur = BARE_ZERO,
                                      .shine = 1.2f }, .shadow = { 0, 8, 24, 0, { 0, 0, 0, 70 } },
                            .animate = { .kind = BARE_ANIM_SPRING, .response = 0.20f, .damping = 0.85f } },
    [BARE_GLASS_DOCK] = { .glass = { .refraction = 1.0f, .merge = 56.0f,
                                      .tint = { 255, 255, 255, 14 } },
                            .shadow = { 0, 20, 48, -4, { 0, 0, 0, 90 } }, .animate = { .kind = BARE_ANIM_SPRING, .response = 0.20f, .damping = 0.85f } },
    [BARE_GLASS_DROP] = { .glass = { .refraction = 1.0f, .merge = 56.0f,
                                      .tint = { 255, 255, 255, 14 } },
                            .animate = { .kind = BARE_ANIM_SPRING, .response = 0.20f, .damping = 0.85f } },
};

static BareBox surface(BareGlassPreset preset, const BareBox *box) {
    BareBox b = *box;
    if (preset < BARE_GLASS_CLEAR || preset > BARE_GLASS_DROP) preset = BARE_GLASS_CLEAR;
    if (!b.style) b.style = &presets[preset];
    return b;
}

BareState bare_glass_surface_(BareGlassPreset preset, const BareBox *box, const char *file, int line) {
    BareBox b = surface(preset, box);
    return bare_box_(&b, file, line);
}

int bare_glass_layer_(BareGlassPreset preset, const BareBox *box, const char *file, int line) {
    BareBox b = surface(preset, box);
    return bare_open_(&b, file, line);
}

