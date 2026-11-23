/* context.c: BareContext and the frame begin/end pair (ARCHITECTURE §3). */
#include "internal.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* The core's only global: which context the public bare_* calls act on. */
static BareContext *current;

static void log_stderr(void *user, const char *message) {
    (void)user;
    fprintf(stderr, "bare: %s\n", message);
}

void bare__log(BareContext *ctx, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (ctx && ctx->log) ctx->log(ctx->log_user, buf);
    else log_stderr(NULL, buf);
}

BareContext *bare_ctx_create(const BareConfig *config) {
    BareAllocator alloc = bare__default_allocator();
    size_t block = 0;
    if (config) {
        if (config->allocator.alloc) alloc = config->allocator;
        block = config->frame_arena_block;
    }
    const BareFontBackend *fonts = config ? config->fonts : NULL;
    BareContext *ctx = bare__alloc(&alloc, sizeof *ctx);
    if (!ctx) return NULL;
    memset(ctx, 0, sizeof *ctx);
    ctx->alloc = alloc;
    bare__arena_init(&ctx->frame, alloc, block);
    ctx->store.free_head = -1;
    ctx->anim_free = -1;
    ctx->font_backend = fonts;
    ctx->clear = BARE_RGB(24, 24, 28);
    ctx->scale = 1.0f;
    ctx->log = log_stderr;
    return ctx;
}

void bare_ctx_destroy(BareContext *ctx) {
    if (!ctx) return;
    if (current == ctx) current = NULL;
    bare__arena_free(&ctx->frame);
    bare__store_free(ctx);
    bare__anim_free(ctx);
    bare__text_free(ctx);
    bare__free(&ctx->alloc, ctx->elems, (size_t)ctx->elem_cap * sizeof *ctx->elems);
    bare__free(&ctx->alloc, ctx->cmds, (size_t)ctx->cmd_cap * sizeof *ctx->cmds);
    bare__free(&ctx->alloc, ctx->hits, (size_t)ctx->hit_cap * sizeof *ctx->hits);
    bare__gpu_free(ctx);
    BareAllocator alloc = ctx->alloc;
    bare__free(&alloc, ctx, sizeof *ctx);
}

void bare_ctx_make_current(BareContext *ctx) { current = ctx; }
BareContext *bare_ctx_current(void) { return current; }

void bare_ctx_set_clear(BareContext *ctx, BareColor color) { ctx->clear = color; }

void bare_ctx_set_log(BareContext *ctx, BareLogFn fn, void *user) {
    ctx->log = fn ? fn : log_stderr;
    ctx->log_user = user;
}

void bare_ctx_begin(BareContext *ctx, float width, float height, float scale, double time) {
    if (ctx->in_frame) {
        bare__log(ctx, "bare_ctx_begin called twice without bare_ctx_end");
        bare_ctx_end(ctx, NULL);
    }
    bare__arena_reset(&ctx->frame);
    ctx->width = width;
    ctx->height = height;
    ctx->scale = scale > 0 ? scale : 1.0f;
    ctx->time = time;
    ctx->in_frame = true;
    bare__input_begin(ctx);   /* before any element asks whether it's hovered */
    bare__tree_begin(ctx);
}

const BareDrawList *bare_ctx_end(BareContext *ctx, bool *still_animating) {
    if (still_animating) *still_animating = false;
    if (!ctx->in_frame) {
        bare__log(ctx, "bare_ctx_end called without bare_ctx_begin");
        return &ctx->list;
    }
    bare__tree_end(ctx);
    bare__layout(ctx);        /* targets */
    bare__animate(ctx);       /* what's drawn this frame */
    bare__transforms(ctx);
    bare__emit(ctx);
    bare__gpu_publish(ctx);   /* images, shaders and passes called since the last frame */

    /* Remember where everything was drawn: BareState.rect and the input that
     * arrives before the next frame both use it. Then forget elements that
     * weren't declared this frame. */
    bare__hits_build(ctx);
    for (int i = 0; i < ctx->elem_count; i++) {
        const BareElem *e = &ctx->elems[i];
        if (e->node >= 0) ctx->store.nodes[e->node].rect = bare__visual_rect(e);
    }
    bool more = bare__input_end(ctx) || ctx->anim_active || ctx->text_redraw;
    ctx->text_redraw = false;
    if (more && still_animating) *still_animating = true;
    bare__store_evict(ctx, ctx->frame_index);
    ctx->in_frame = false;
    ctx->frame_index++;
    return &ctx->list;
}
