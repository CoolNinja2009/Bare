/* image.c: decoding image files with stb_image (ARCHITECTURE §9).
 *
 * The decoded pixels go through bare_image_from_pixels, which copies them into
 * the GPU queue, so they are freed right away. */
#include "internal.h"
#include <stdlib.h>
#include <string.h>

/* stb_image's allocation hooks have no user pointer: they use the current
 * context's allocator (bare_image_load only runs with one). */
static const BareAllocator *stbi_alloc(void) {
    BareContext *ctx = bare_ctx_current();
    return ctx ? &ctx->alloc : NULL;
}

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_ONLY_TGA
#ifdef _WIN32
#define STBI_WINDOWS_UTF8   /* elsewhere its declaration has no body: an unused-function error */
#endif
#define STBI_MALLOC(sz)        bare__heap_alloc(stbi_alloc(), (sz))
#define STBI_REALLOC(p, newsz) bare__heap_realloc(stbi_alloc(), (p), (newsz))
#define STBI_FREE(p)           bare__heap_free(stbi_alloc(), (p))
#if defined(_MSC_VER)
#  pragma warning(push, 0)
#  pragma warning(disable : 4244 4245 4456 4457 4701 4702 4706)
#elif defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wpedantic"
#  pragma GCC diagnostic ignored "-Wunused-function"
#  pragma GCC diagnostic ignored "-Wsign-compare"
#  pragma GCC diagnostic ignored "-Wunused-parameter"
#  pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#include "stb_image.h"
#if defined(_MSC_VER)
#  pragma warning(pop)
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

BareImage bare_image_load(const char *path) {
    BareContext *ctx = bare_ctx_current();
    if (!ctx) { bare__log(NULL, "bare_image_load called before bare_window"); return 0; }
    int w = 0, h = 0, n = 0;
    unsigned char *pixels = path ? stbi_load(path, &w, &h, &n, 4) : NULL;
    if (!pixels) {
        bare__log(ctx, "bare_image_load: can't load \"%s\" (%s)", path ? path : "(null)",
                  path ? stbi_failure_reason() : "no path");
        return 0;
    }
    BareImage image = bare_image_from_pixels(w, h, pixels);
    stbi_image_free(pixels);
    return image;
}

BareImage bare_image_from_memory(const void *data, int size) {
    BareContext *ctx = bare_ctx_current();
    if (!ctx) { bare__log(NULL, "bare_image_from_memory called before bare_window"); return 0; }
    int w = 0, h = 0, n = 0;
    unsigned char *pixels = data && size > 0 ? stbi_load_from_memory(data, size, &w, &h, &n, 4) : NULL;
    if (!pixels) {
        bare__log(ctx, "bare_image_from_memory: can't decode %d bytes (%s)", size, data ? stbi_failure_reason() : "no data");
        return 0;
    }
    BareImage image = bare_image_from_pixels(w, h, pixels);
    stbi_image_free(pixels);
    return image;
}

unsigned char *bare_image_decode(const void *data, int size, int *width, int *height) {
    if (width) *width = 0;
    if (height) *height = 0;
    if (!bare_ctx_current() || !data || size <= 0) return NULL;
    int w = 0, h = 0, n = 0;
    unsigned char *pixels = stbi_load_from_memory(data, size, &w, &h, &n, 4);
    if (!pixels) return NULL;
    unsigned char *out = malloc((size_t)w * (size_t)h * 4);
    if (out) memcpy(out, pixels, (size_t)w * (size_t)h * 4);
    stbi_image_free(pixels);
    if (out && width) *width = w;
    if (out && height) *height = h;
    return out;
}
