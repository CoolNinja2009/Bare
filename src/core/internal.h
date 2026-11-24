/* internal.h: shared by the core's files. Never included by apps or backends. */
#ifndef BARE_INTERNAL_H
#define BARE_INTERNAL_H

#include "bare/bare_core.h"
#include <stddef.h>

#ifndef BARE_ASSERT
#  include <assert.h>
#  define BARE_ASSERT(x) assert(x)
#endif

#define BARE__MIN(a, b) ((a) < (b) ? (a) : (b))
#define BARE__MAX(a, b) ((a) > (b) ? (a) : (b))

/* ---- allocation --------------------------------------------------------- */

BareAllocator bare__default_allocator(void);
void *bare__alloc(const BareAllocator *a, size_t size);
void *bare__realloc(const BareAllocator *a, void *ptr, size_t old_size, size_t new_size);
void  bare__free(const BareAllocator *a, void *ptr, size_t size);

/* Grows *items (each `item_size` bytes) so it holds at least `need`; doubles.
 * Used for persistent arrays that grow only when the UI grows. */
bool bare__grow(const BareAllocator *a, void **items, int *cap, int need, size_t item_size);

#ifdef BARE_HAS_DEFAULT_FONT
extern const unsigned char bare__default_font_data[];   /* generated from assets/fonts at build time */
extern const int bare__default_font_size;
#endif

/* malloc-style (no size on free), for stb: the size is kept in a header. */
void *bare__heap_alloc(const BareAllocator *a, size_t size);
void *bare__heap_realloc(const BareAllocator *a, void *ptr, size_t size);
void  bare__heap_free(const BareAllocator *a, void *ptr);

/* ---- arena -------------------------------------------------------------- */

typedef struct BareArenaBlock {
    struct BareArenaBlock *next;
    size_t size, used;
} BareArenaBlock;

typedef struct BareArena {
    BareAllocator   alloc;
    BareArenaBlock *first, *current;
    size_t          block_size;
    size_t          block_count;    /* blocks ever allocated (tests check reuse) */
} BareArena;

void  bare__arena_init(BareArena *arena, BareAllocator alloc, size_t block_size);
void *bare__arena_alloc(BareArena *arena, size_t size, size_t align);   /* zeroed */
char *bare__arena_strdup(BareArena *arena, const char *s);
void  bare__arena_reset(BareArena *arena);                              /* keeps blocks */
void  bare__arena_free(BareArena *arena);

/* ---- ids and maps ------------------------------------------------------- */

#define BARE__FNV_OFFSET 0xcbf29ce484222325ull
uint64_t bare__hash_bytes(uint64_t h, const void *data, size_t size);
uint64_t bare__hash_str(uint64_t h, const char *s);
uint64_t bare__hash_u64(uint64_t h, uint64_t v);

/* Per-frame map in the frame arena: key (never 0) -> int32. */
typedef struct BareMap {
    uint64_t  *keys;
    int32_t   *vals;
    int        cap, count;
    BareArena *arena;
} BareMap;

void     bare__map_init(BareMap *m, BareArena *arena, int cap);
/* Finds or inserts `key` (inserted values start at 0); NULL when out of memory. */
int32_t *bare__map_slot(BareMap *m, uint64_t key, bool *found);

/* ---- retained state ----------------------------------------------------- */

typedef struct BareNode {
    BareId   id;               /* 0 = free slot */
    int32_t  next_free;
    uint64_t last_frame;       /* frame it was last declared in */
    BareRect rect;             /* where it was drawn last frame (visual: offset and scale), logical pixels */
    /* Scroll containers (scroll.c). The offset is stepped at the start of a
     * frame, so the app and layout see the value this frame draws. */
    BareVec2 scroll;           /* drawn offset, logical pixels */
    BareVec2 scroll_to;        /* where it's heading (wheel, drag), within the content */
    BareVec2 scroll_v;         /* spring velocity */
    BareVec2 content;          /* last layout: content size, padding included */
    BareVec2 view;             /* last layout: box size */
    float    virt_start;       /* last layout: where a bare_visible run starts, from the box's start */
    double   scroll_seen;      /* last time it scrolled or the mouse moved over it (scrollbars show) */
    uint8_t  bars;             /* scrollbar rails present last frame: bit 0 = x, bit 1 = y */
    uint8_t  scroll_flags;     /* BARE__SCROLL_* */
    uint32_t anim;             /* animation set index + 1; 0 = never animated (anim.c) */
    struct BareEditState *edit;/* text fields: caret, selection, undo (edit.c); NULL until focused */
    bool     dup_warned;       /* a duplicate of this explicit ID was reported once */
} BareNode;

enum { BARE__SCROLL_MOVING = 1, BARE__SCROLL_POKED = 2 };

typedef struct BareStore {
    BareNode *nodes; int node_count, node_cap;
    int       free_head;
    uint64_t *keys;  int32_t *vals; int cap, used;   /* id -> node index */
} BareStore;

int  bare__store_get(BareContext *ctx, BareId id, bool *created);   /* node index, -1 on failure */
int  bare__store_find(const BareContext *ctx, BareId id);            /* node index, -1 if absent */
void bare__store_evict(BareContext *ctx, uint64_t frame);            /* drop nodes not seen in `frame` */
int  bare__store_live(const BareContext *ctx);
void bare__store_free(BareContext *ctx);

/* ---- text (text.c, font_stb.c) ------------------------------------------ */

/* A run of a text: the bytes from `start` to the next run's start, in one
 * face, size and colour (bare_span). Plain text is one run. */
typedef struct BareTextRun {
    int        start;
    BareFont   font;                        /* the face: family, weight and italic resolved */
    float      size, ascent, descent, lh;   /* pixels; lh: its line height */
    float      spacing;                     /* letter spacing */
    BareColor  color;                       /* alpha 0 = the element's (inherited) colour */
    BareColor  background;                  /* behind its glyphs; alpha 0 = none */
    bool       underline, strike;
} BareTextRun;

typedef struct BareSpanIn { int start; BareTextStyle style; } BareSpanIn;   /* a span as declared */

/* A line after wrapping. cut: ends in the ellipsis. y, h: its line box from
 * the text's top; asc: baseline from the line box's top. */
typedef struct BareTextLine { int first, count; float width; bool cut; float y, h, asc; } BareTextLine;

/* What a text field or editor line draws with its text (field.c): selection
 * and marks behind, carets and the IME underline in front. Byte offsets into
 * the shown text. */
typedef struct BareTextDeco {
    int        caret;   bool upstream, caret_on, block;
    int        sel_a, sel_b;            /* empty when equal */
    int        comp_a, comp_b;          /* IME composition, underlined */
    BareColor  caret_color, sel_color;
    const int *carets;  int caret_count;   /* more carets (editors); upstream never */
    const int *sels;    int sel_count;     /* more selections: pairs a, b */
    const int *marks;   int mark_count;    /* outlined ranges (find matches, brackets): pairs a, b */
    BareColor  mark_color;
    bool       whitespace;              /* dots for spaces, arrows for tabs */
    BareColor  whitespace_color;
} BareTextDeco;

/* A bare_text element's payload, in the frame arena. Shaped at its logical
 * size when declared; wrapped by layout pass 3; drawn by emit. */
typedef struct BareText {
    const char      *str;   int len;
    BareFont         font;                  /* the paragraph's face (ellipsis, tabs, the line strut) */
    float            size, line_height;     /* pixels; line_height already multiplied */
    float            ascent, descent;       /* of the paragraph's face at its size */
    BareAlign        align;
    bool             nowrap, ellipsis;
    bool             uniform;               /* every run has the paragraph's line metrics */
    int              max_lines;             /* 0 = no limit */
    BareColor        color;                 /* alpha 0 = inherited */
    BareShadow       shadow;
    BareTextRun     *runs;   int run_count;
    uint16_t        *grun;                  /* each glyph's run; NULL with one run */
    BareShapedGlyph  ell[3]; int ell_count; /* the ellipsis (U+2026, or "..." if the font lacks it) */
    float            ell_w;
    BareShapedGlyph *glyphs; int glyph_count;
    float            max_line;              /* widest line without wrapping */
    float            min_word;              /* widest run that can't be broken at a space or hyphen */
    BareTextLine    *lines;  int line_count;
    float            height;                /* of all lines, after wrapping */
    BareTextDeco    *deco;                  /* text fields only */
} BareText;

typedef struct BareFontRec {
    void   *handle;                         /* the backend's; NULL until loaded */
    void   *owned;  size_t owned_size;      /* file data read by bare_font_load */
    bool    alive, failed;
    BareFont family;                        /* the family's first face (itself for a root) */
    int16_t weight;                         /* 100..900 */
    bool    italic;
} BareFontRec;

/* Built-in faces: 0 Inter Regular, 1 JetBrains Mono, Inter SemiBold, Bold,
 * Italic, Bold Italic, mono Bold. Loaded faces come after them. */
#define BARE__FONT_BUILTIN 7
#ifdef BARE_HAS_DEFAULT_FONT
extern const unsigned char bare__font_mono_data[], bare__font_mono_bold_data[], bare__font_sans_semibold_data[],
    bare__font_sans_bold_data[], bare__font_sans_italic_data[], bare__font_sans_bold_italic_data[];
extern const int bare__font_mono_size, bare__font_mono_bold_size, bare__font_sans_semibold_size,
    bare__font_sans_bold_size, bare__font_sans_italic_size, bare__font_sans_bold_italic_size;
#endif

typedef struct BareGlyphSlot {              /* a rasterized glyph in the atlas */
    uint64_t key;                           /* 0 = empty */
    int16_t  x, y, w, h;                    /* atlas pixels */
    int16_t  left, top;                     /* from the pen on the baseline */
} BareGlyphSlot;

typedef struct BareShelf { int y, h, x; } BareShelf;

uint32_t bare__utf8_decode(const char *s, int len, int *i);   /* advances *i; U+FFFD for bad bytes */
const BareFontBackend *bare__font_backend(const BareContext *ctx);
void *bare__font_handle(BareContext *ctx, BareFont *id);       /* *id becomes the face that stood in; NULL: none */
BareFont bare__font_face(BareContext *ctx, BareFont font, int weight, bool italic);   /* nearest face in the family */
const BareGlyphSlot *bare__glyph_get(BareContext *ctx, void *font, BareFont id, uint32_t glyph, float px, int sub);
BareText *bare__text_new(BareContext *ctx, const char *str, int len, const BareTextStyle *style);
BareText *bare__text_runs(BareContext *ctx, const char *str, int len, const BareTextStyle *style,
                          const BareSpanIn *spans, int span_count);
void  bare__text_wrap(BareContext *ctx, BareText *t, float width);
float bare__text_height(const BareText *t);
int   bare__text_line_at_y(const BareText *t, float y);
void  bare__text_begin_emit(BareContext *ctx);                 /* starts the atlas over if it filled up */
void  bare__text_emit(BareContext *ctx, const BareText *t, BareRect r, float vscale, bool snap, BareColorF color,
                      float opacity);
void  bare__text_end_frame(BareContext *ctx);                  /* atlas uploads, uv normalisation */
void  bare__text_free(BareContext *ctx);

/* caret.c: positions in wrapped text (byte offsets; text-local logical pixels) */
int      bare__text_line_start(const BareText *t, int line);
int      bare__text_line_end(const BareText *t, int line, bool *upstream);
int      bare__text_line_of(const BareText *t, int offset, bool upstream);
float    bare__text_x_on_line(const BareText *t, int line, int offset, float box_w);
BareRect bare__text_caret(const BareText *t, int offset, bool upstream, float box_w);
int      bare__text_offset_at(const BareText *t, int line, float x, float box_w, bool *upstream);
int      bare__text_hit(const BareText *t, float x, float y, float box_w, bool *upstream);
int      bare__text_range_rects(const BareText *t, int a, int b, float box_w, BareRect *out, int cap);

/* ---- element tree ------------------------------------------------------- */

typedef struct BareElem {
    BareId      id;
    int32_t     node;               /* retained node index, -1 if none */
    int32_t     parent, first_child, last_child, next_sibling, child_count;
    const char *file;               /* call site, for messages */
    int         line;
    BareBox     box;                /* resolved: defaults applied, BARE_ZERO -> 0 */
    float       opacity;            /* effective (times ancestors) */
    float       vscale;             /* visual transform (offset and scale, with ancestors'): */
    BareVec2    vt;                 /*   drawn at rect * vscale + vt (layout.c, after the passes) */
    bool        glass;              /* box.glass is drawn (fields resolved; animated after bare__animate) */
    float       glass_amount;       /* 0..1: glass fading in or out */
    float       fit[2];             /* preferred (max-content) size per axis, layout passes 1/4 */
    float       minc[2];            /* min-content size per axis: shrinking stops here */
    bool        frozen;             /* layout scratch: size settled at a min/max while growing */
    bool        moving;             /* its position or size is animating (or an ancestor's): drawn unsnapped */
    BareRect    rect;               /* layout result, then (after bare__animate) the animated rect */
    BareRect    target;             /* the layout result, kept by bare__animate */
    BareText   *text;               /* bare_text payload (frame arena), NULL for boxes */
    /* Scroll containers (scroll.c). */
    uint8_t     overlay;            /* 1 / 2: the x / y scrollbar rail of its parent; outside the flex flow */
    int32_t     bar[2];             /* element index of its x / y rail, 0 = none */
    float       content[2];         /* content size per axis, padding included (layout) */
    uint8_t     focus;              /* BARE__FOCUS_* */
    float       scroll_pad;         /* added to the content (a text field's caret past the last glyph) */
    bool        no_bars;            /* a scroll box without scrollbars (single-line fields) */
    bool        virt;               /* holds a bare_visible run: */
    int         lead_at, tail_at;   /*   space for the items not declared goes before these children */
    float       lead, tail;
    int         virt_count, virt_last;
    float       virt_stride;
} BareElem;

#define BARE__MAX_DEPTH 256

/* ---- animation (easing.c, spring.c, anim.c) ------------------------------ */

/* One float per animatable property. x and y are relative to the parent, so a
 * moving parent carries its children. Colours are in 0..255 units. */
enum {
    BARE__CH_X, BARE__CH_Y, BARE__CH_W, BARE__CH_H,          /* layout: .animate_layout */
    BARE__CH_COLOR = 4,                                      /* r g b a, then the same for: */
    BARE__CH_BORDER_COLOR = 8,
    BARE__CH_TEXT_COLOR = 12,
    BARE__CH_SHADOW_COLOR = 16,
    BARE__CH_BORDER = 20, BARE__CH_RADIUS, BARE__CH_OPACITY, BARE__CH_SCALE,
    BARE__CH_OFFSET_X, BARE__CH_OFFSET_Y,
    BARE__CH_SHADOW_X, BARE__CH_SHADOW_Y, BARE__CH_SHADOW_BLUR, BARE__CH_SHADOW_SPREAD,
    BARE__CH_GLASS_TINT,                                     /* r g b a */
    BARE__CH_GLASS = BARE__CH_GLASS_TINT + 4,                /* amount: 1 = glass, 0 = none */
    BARE__CH_REFRACTION, BARE__CH_DEPTH, BARE__CH_GLASS_BLUR, BARE__CH_DISPERSION,
    BARE__CH_SHINE, BARE__CH_ZOOM, BARE__CH_LIGHT,
    BARE__CH_COUNT
};

typedef struct BareChannel {
    float value, velocity;      /* what's drawn; units per second */
    float target;
    float from, t;              /* easing: where it started and progress 0..1 */
} BareChannel;

typedef struct BareAnimSet {
    BareChannel ch[BARE__CH_COUNT];
    uint64_t    active;         /* bit per channel still moving */
    BareId      parent;         /* the parent x/y were relative to last frame */
    BareVec2    abs;            /* last animated position, window coordinates */
    bool        ready;          /* channels hold values (a new set snaps to its targets) */
    int32_t     next_free;
} BareAnimSet;

float bare__bezier(const float b[4], float x);                      /* CSS cubic-bezier: progress -> eased */
void  bare__spring_step(float *x, float *v, float response, float damping, float dt);
void  bare__animate(BareContext *ctx);                              /* layout targets -> visual values */
void  bare__anim_release(BareContext *ctx, uint32_t anim);          /* a node holding it was evicted */
void  bare__anim_free(BareContext *ctx);

/* ---- images, shaders and passes (gpu.c) ---------------------------------- */

typedef struct BareImageRec {
    int        width, height;
    BareFormat format;
    bool       alive, canvas;
    bool       internal;        /* Bare's own (the glyph atlas): not the app's to free */
    int32_t    next_free;
} BareImageRec;

typedef struct BareShaderRec {
    const BareShaderCode *code;
    bool                  alive;
    int32_t               next_free;
} BareShaderRec;

/* Ops in call order, with the bytes they point to (params, pixels) copied in.
 * While queued, pointers hold offsets into `data`; they become real when the
 * queue is handed to the renderer. */
typedef struct BareGpuQueue {
    BareGpuOp     *ops;  int count, cap;
    unsigned char *data; int data_size, data_cap;
} BareGpuQueue;

/* ---- interaction (hit.c, input.c) ---------------------------------------- */

/* One element as last drawn, for hit testing. Same order and indices as the
 * elements of the frame it was built from (paint order). */
typedef struct BareHit {
    BareId   id;
    int32_t  parent;
    int32_t  node;
    BareRect rect;              /* visual, logical pixels */
    float    radius;
    BareRect clip;              /* intersection of clipping ancestors */
    bool     passthrough;
    uint8_t  scroll;            /* BareScroll */
    uint8_t  bar;               /* 1 / 2: the x / y scrollbar rail of its parent */
    uint8_t  focus;             /* BARE__FOCUS_* */
} BareHit;

enum { BARE__FOCUS_ABLE = 1, BARE__FOCUS_FIELD = 2, BARE__FOCUS_TABS = 4, BARE__FOCUS_LINE = 8,
       BARE__FOCUS_EDITOR = 16 };   /* EDITOR: takes Tab and Shift+Tab; its own key bindings apply */

/* Keyboard input in arrival order, so typing, editing keys and IME text
 * interleave right (focus.c). Kept until the focused field consumes it. */
typedef struct BareKeyEv {
    uint8_t kind;               /* BARE__IN_* */
    uint8_t mods;               /* BARE_MOD_* */
    bool    repeat;
    int     key;                /* BareKey */
    int     text, len, cursor;  /* text: into ctx->in_text */
} BareKeyEv;

enum { BARE__IN_KEY, BARE__IN_TEXT, BARE__IN_COMPOSE, BARE__IN_ACTION };

typedef struct BareKeyBinding { int key; uint8_t mods, action, editor; } BareKeyBinding;   /* editor: bare_editor only */

/* A text field declared this frame (field.c), fixed up after layout. */
typedef struct BareFieldFrame {
    int      box, text;         /* element indices */
    BareId   id;
    bool     focused, multiline;
    bool     follow;            /* the caret moved: scroll it into view */
} BareFieldFrame;

/* An element and its ancestors (innermost first), by ID. */
typedef struct BareChain { BareId ids[BARE__MAX_DEPTH + 1]; int count; } BareChain;

enum { BARE__PRESS_LEFT, BARE__PRESS_RIGHT, BARE__PRESS_COUNT };

struct BareContext {
    BareAllocator alloc;
    BareArena     frame;            /* reset every frame */
    BareStore     store;            /* retained nodes */

    BareElem     *elems;   int elem_count, elem_cap;    /* pre-order */
    int           stack[BARE__MAX_DEPTH]; int depth;     /* open containers */
    int           overflow;         /* opens beyond MAX_DEPTH, ignored */
    BareMap       sites;            /* (parent, call site) -> times seen this frame */
    BareMap       ids;              /* id -> element index + 1, this frame */

    BareHit      *hits;    int hit_count, hit_cap;      /* last frame, for this frame's input */
    BareVec2      mouse;
    bool          mouse_in;          /* the pointer is over the window */
    bool          held[BARE__PRESS_COUNT];
    BareChain     press[BARE__PRESS_COUNT];   /* where each button went down */
    BareChain     hover;             /* computed at begin */
    BareChain     clicked[BARE__PRESS_COUNT]; /* since the last frame ended */
    BareId        hover_top;         /* innermost hovered element this frame (0 = none) */
    uint8_t       key_down[BARE_KEY_COUNT_], key_pressed[BARE_KEY_COUNT_];
    BareCursor    cursor;
    bool          mouse_moved;       /* since the last frame ended */
    BareVec2      press_pos;         /* where the left button last went down */
    double        press_time;
    int           click_count;       /* 1, 2 (double click), 3 (triple) for the last press */
    bool          press_new;         /* the left button went down since the last frame, */
    BareChain     press_began;       /*   on these (kept even if it's already released) */
    bool          window_focused;
    BareId        drag_id;           /* scroll container whose scrollbar thumb is held (0 = none) */
    int           drag_axis;
    float         drag_grab;         /* where along the thumb it was grabbed, logical pixels */

    bool          scroll_active;     /* some scroll offset is still moving toward its target */
    bool          scroll_started;    /* scroll_time is valid */
    double        scroll_time;       /* time of the last scroll step */
    float         scroll_frame;      /* last measured frame interval while scrolling */
    double        wake_at;           /* a frame is wanted at this time (negative = none) */

    BareImageRec  *images;  int image_count, image_cap, image_free;   /* by id - 1 */
    BareShaderRec *shaders; int shader_count, shader_cap, shader_free;
    BareGpuQueue   gpu_pending, gpu_published;

    BareAnimSet  *anims;   int anim_count, anim_cap, anim_free;   /* pool; nodes point in by index + 1 */
    bool          anim_active;      /* some channel was still moving at the end of the last frame */
    bool          anim_started;     /* anim_time is valid */
    double        anim_time;        /* time of the last animation step */
    float         anim_frame;       /* last measured frame interval while animating */

    const BareFontBackend *font_backend;
    BareFontRec   *fonts;   int font_count, font_cap;     /* [0] = the default font */
    BareGlyphSlot *glyphs;  int glyph_cap, glyph_used;   /* open addressing by key */
    unsigned char *atlas;   int atlas_w, atlas_h;         /* R8 copy of the GPU atlas */
    uint32_t       atlas_id;                              /* texture id (an image id), 0 until used */
    BareShelf     *shelves; int shelf_count, shelf_cap;
    int            dirty[4];                              /* x0 y0 x1 y1 to upload; x1 <= x0 = clean */
    bool           atlas_full;                            /* reset before the next frame */
    bool           text_redraw;                           /* glyphs were skipped: draw another frame */
    int            text_cmd_first;                        /* first text cmd of this frame's emit */
    int            raster_count;                          /* glyphs rasterized (tests) */
    BareTextureUpload upload;
    /* bare_paragraph being declared: its element, style and spans so far. */
    int            para_elem, para_depth;                 /* -1 = none */
    BareTextStyle  para_style;
    char          *para_buf;   int para_len, para_cap;
    BareSpanIn    *para_spans; int para_span_count, para_span_cap;
    const char    *para_file;  int para_line;

    BareDrawCmd  *cmds;    int cmd_count, cmd_cap;
    BareDrawList  list;

    float         width, height, scale;
    double        time;
    uint64_t      frame_index;
    bool          in_frame;
    BareColor     clear;
    bool          clear_set;        /* the app chose a background; else the theme's */
    BareTheme     theme;            /* the current theme (style.c) */
    const void   *style_warned[8];  /* broken style chains already reported */
    int           style_warned_count;

    /* Focus and text fields (focus.c, edit.c, field.c). */
    BareId        focus_id;          /* 0 = nothing focused */
    uint8_t       focus_kind;        /* BARE__FOCUS_* of the focused element, as last declared */
    uint8_t       focus_how;         /* 1 = mouse, 2 = keyboard (Tab), 3 = bare_focus */
    bool          focus_declared;    /* the focused element was declared this frame */
    BareKeyEv    *in;      int in_count, in_cap;       /* the keyboard queue */
    char         *in_text; int in_text_len, in_text_cap;
    int           in_take;           /* entries the focused field takes this frame */
    BareKeyBinding *binds; int bind_count, bind_cap;   /* NULL until first used: the defaults */
    bool          mac_keys;
    const char *(*clip_get)(void *user);
    void        (*clip_set)(void *user, const char *text);
    void         *clip_user;
    char         *clip_own;  size_t clip_own_size;       /* headless clipboard */
    BareFieldFrame *fields; int field_count, field_cap;
    BareTextInputKind ti_kind;       /* what the focused field wants from the platform */
    BareRect      ti_rect;

    BareLogFn     log;
    void         *log_user;
};

void bare__log(BareContext *ctx, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

/* style.c */
void bare__style_box(BareContext *ctx, const BareBox *box, BareBox *out, const char *file, int line);
void bare__style_text(BareContext *ctx, const BareTextStyle *st, BareTextStyle *out, const char *file, int line);
void bare__resolve_box(BareBox *b);         /* defaults for fields still 0; BARE_ZERO -> 0 */
bool bare__resolve_glass(BareGlass *g);     /* true when the box has glass */
void bare__apply_look(BareBox *b, const BareLook *l);

/* tree.c */
int  bare__tree_push(BareContext *ctx, const BareBox *box, const char *file, int line);
int  bare__text_push(BareContext *ctx, const char *str, int len, const BareTextStyle *style, const char *file, int line);
BareState bare__state_of(BareContext *ctx, int index);
void bare__tree_begin(BareContext *ctx);
void bare__tree_end(BareContext *ctx);

/* layout.c: the flexbox passes; then (after bare__animate) visual transforms (vscale, vt) */
void bare__layout(BareContext *ctx);
void bare__transforms(BareContext *ctx);

/* hit.c */
void bare__hits_build(BareContext *ctx);
int  bare__hit_find(const BareContext *ctx, BareVec2 p);              /* topmost hit index, -1 */
void bare__hit_chain(const BareContext *ctx, int index, BareChain *out);
bool bare__chain_has(const BareChain *c, BareId id);
BareRect bare__visual_rect(const BareElem *e);
BareRect bare__clip_rect(const BareElem *e);                         /* what a clipping box clips to */

/* gpu.c */
void bare__gpu_publish(BareContext *ctx);       /* pending ops -> the draw list */
void bare__gpu_free(BareContext *ctx);
const BareImageRec *bare__image(const BareContext *ctx, BareImage id);   /* NULL unless alive */
uint32_t bare__image_reserve(BareContext *ctx, int w, int h);           /* an id for an internal texture */

/* input.c */
void bare__input_begin(BareContext *ctx);       /* hover chain from the mouse */
bool bare__input_end(BareContext *ctx);         /* clears one-frame input; true = draw another frame */

/* emit.c */
void bare__emit(BareContext *ctx);
void bare__emit_fill(BareContext *ctx, BareRect physical, BareColorF color);

/* focus.c */
void bare__focus_begin(BareContext *ctx);       /* Tab / Escape at the head of the queue; this frame's share */
void bare__focus_end(BareContext *ctx);         /* consumed input dropped; focus on a vanished element cleared */
void bare__focus_event(BareContext *ctx, const BareEvent *e);   /* keys and text into the queue */
void bare__focus_press(BareContext *ctx, int hit);              /* left button down on a hit (-1: nothing) */
void bare__focus_set(BareContext *ctx, BareId id, int how);
void bare__focus_free(BareContext *ctx);
int  bare__mods(const BareContext *ctx);                         /* BARE_MOD_* held now */
const char *bare__clipboard_get(BareContext *ctx);
void bare__clipboard_set(BareContext *ctx, const char *text);
void bare__scroll_into_view(BareContext *ctx, int hit);           /* scroll ancestors so a hit shows */

/* edit.c: the editing engine of a field */
typedef struct BareEditState BareEditState;
BareEditAction bare__edit_lookup(BareContext *ctx, int key, int mods, bool *extend);
bool bare__edit_types(const BareContext *ctx, int key, int mods);       /* a key that types text (consumed by a focused field) */
void bare__edit_release(BareContext *ctx, BareEditState *ed);

/* field.c */
void bare__fields_layout(BareContext *ctx);     /* after layout: scroll to the caret, the IME rect */
void bare__field_emit_under(BareContext *ctx, const BareText *t, BareRect r, float k, bool snap, float opacity);
void bare__field_emit_over(BareContext *ctx, const BareText *t, BareRect r, float k, bool snap, BareColorF ink);

/* scroll.c */
#define BARE__BAR_HIT   14.0f   /* scrollbar rail thickness: the part that takes the mouse */
#define BARE__BAR_INSET 2.0f    /* gap between the bar and the box's edges */
bool  bare__scrolls(uint8_t scroll, int axis);
void  bare__scroll_begin(BareContext *ctx);                         /* steps offsets toward their targets */
void  bare__scroll_close(BareContext *ctx, int index);              /* a container closes: its scrollbars */
void  bare__scroll_fit(BareContext *ctx, int index);                /* layout: clamp, remember the content */
void  bare__scroll_thumb(float view, float content, float scroll, float other, float *pos, float *len);
float bare__scroll_drawn(const BareContext *ctx, const BareElem *e, int axis);   /* snapped to pixels */
void  bare__scroll_wheel(BareContext *ctx, float dx, float dy, bool precise);
void  bare__scroll_press(BareContext *ctx, int hit, BareVec2 p);    /* left button down on a hit */
void  bare__scroll_drag(BareContext *ctx, BareVec2 p);
void  bare__want_frame_at(BareContext *ctx, double time);

#endif /* BARE_INTERNAL_H */
