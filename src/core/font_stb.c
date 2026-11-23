/* font_stb.c: the default font backend, stb_truetype (ARCHITECTURE §13).
 *
 * "Shaping" here is a code point -> glyph map plus pair kerning (GPOS or
 * kern table); HarfBuzz can do real shaping behind the same interface. */
#include "internal.h"
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#define STBTT_malloc(x, u) bare__heap_alloc((const BareAllocator *)(u), (x))
#define STBTT_free(x, u)   bare__heap_free((const BareAllocator *)(u), (x))
#if defined(_MSC_VER)
#  pragma warning(push, 0)
#  pragma warning(disable : 4244 4245 4701 4702 4706)
#elif defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wpedantic"
#  pragma GCC diagnostic ignored "-Wunused-function"
#  pragma GCC diagnostic ignored "-Wsign-compare"
#  pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include "stb_truetype.h"
#if defined(_MSC_VER)
#  pragma warning(pop)
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

typedef struct StbFont {
    stbtt_fontinfo info;
    BareAllocator  alloc;
    int            ascii[128];          /* glyph per ASCII code point, -1 = not looked up */
    unsigned char *bitmap;  int bitmap_cap;
} StbFont;

static void *stb_load(const BareAllocator *alloc, const void *data, int size) {
    const unsigned char *bytes = data;
    int offset = stbtt_GetFontOffsetForIndex(bytes, 0);
    if (size < 12 || offset < 0) return NULL;
    StbFont *f = bare__alloc(alloc, sizeof *f);
    if (!f) return NULL;
    memset(f, 0, sizeof *f);
    f->alloc = *alloc;
    if (!stbtt_InitFont(&f->info, bytes, offset)) {
        bare__free(alloc, f, sizeof *f);
        return NULL;
    }
    f->info.userdata = &f->alloc;
    for (int i = 0; i < 128; i++) f->ascii[i] = -1;
    return f;
}

static void stb_metrics(void *font, float px, float *ascent, float *descent, float *line_gap) {
    StbFont *f = font;
    int a, d, g;
    stbtt_GetFontVMetrics(&f->info, &a, &d, &g);
    float s = stbtt_ScaleForMappingEmToPixels(&f->info, px);   /* px is the em size, as in CSS */
    *ascent = (float)a * s;
    *descent = (float)-d * s;
    *line_gap = (float)g * s;
}

static int glyph_of(StbFont *f, uint32_t cp) {
    if (cp < 128) {
        if (f->ascii[cp] < 0) f->ascii[cp] = stbtt_FindGlyphIndex(&f->info, (int)cp);
        return f->ascii[cp];
    }
    return stbtt_FindGlyphIndex(&f->info, (int)cp);
}

static int stb_shape(void *font, float px, const char *utf8, int len, BareShapedGlyph *out, int cap) {
    StbFont *f = font;
    float s = stbtt_ScaleForMappingEmToPixels(&f->info, px);
    bool kerns = f->info.gpos || f->info.kern;
    int n = 0, i = 0;
    while (i < len && n < cap) {
        int at = i;
        uint32_t cp = bare__utf8_decode(utf8, len, &i);
        int g = glyph_of(f, cp == '\t' ? ' ' : cp);
        int adv, lsb;
        stbtt_GetGlyphHMetrics(&f->info, g, &adv, &lsb);
        if (cp == '\n' || cp == '\r') adv = 0;
        if (n > 0 && kerns && out[n - 1].advance > 0)
            out[n - 1].advance += (float)stbtt_GetGlyphKernAdvance(&f->info, (int)out[n - 1].glyph, g) * s;
        out[n].glyph = (uint32_t)g;
        out[n].advance = (float)adv * s;
        out[n].x_offset = out[n].y_offset = 0;
        out[n].cluster = (uint32_t)at;
        n++;
    }
    return n;
}

static bool stb_rasterize(void *font, uint32_t glyph, float px, float shift_x, BareGlyphBitmap *out) {
    StbFont *f = font;
    float s = stbtt_ScaleForMappingEmToPixels(&f->info, px);
    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBoxSubpixel(&f->info, (int)glyph, s, s, shift_x, 0, &x0, &y0, &x1, &y1);
    memset(out, 0, sizeof *out);
    int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0 || stbtt_IsGlyphEmpty(&f->info, (int)glyph)) return true;   /* blank */
    if (w * h > f->bitmap_cap) {
        unsigned char *b = bare__realloc(&f->alloc, f->bitmap, (size_t)f->bitmap_cap, (size_t)(w * h));
        if (!b) return false;
        f->bitmap = b;
        f->bitmap_cap = w * h;
    }
    stbtt_MakeGlyphBitmapSubpixel(&f->info, f->bitmap, w, h, w, s, s, shift_x, 0, (int)glyph);
    out->width = w;
    out->height = h;
    out->left = x0;
    out->top = y0;
    out->pixels = f->bitmap;
    out->stride = w;
    return true;
}

static void stb_unload(void *font) {
    StbFont *f = font;
    if (!f) return;
    BareAllocator a = f->alloc;
    bare__free(&a, f->bitmap, (size_t)f->bitmap_cap);
    bare__free(&a, f, sizeof *f);
}

const BareFontBackend *bare_font_backend_stb(void) {
    static const BareFontBackend backend = { stb_load, stb_metrics, stb_shape, stb_rasterize, stb_unload };
    return &backend;
}
