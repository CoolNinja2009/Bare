/* edit.h: the editing engine (edit.c) as used by text fields (field.c). */
#ifndef BARE_EDIT_H
#define BARE_EDIT_H

#include "internal.h"

/* One undoable change: at `at`, `del` bytes became `ins` bytes. The removed
 * then the inserted text sit at `bytes` in the state's byte pool. */
typedef struct BareUndo {
    int     at, del, ins, bytes;
    int     caret0, anchor0, caret1, anchor1;   /* before / after */
    uint8_t kind;                               /* BARE__UNDO_* (what can merge) */
} BareUndo;

enum { BARE__UNDO_OTHER, BARE__UNDO_TYPE, BARE__UNDO_BACK, BARE__UNDO_FORWARD };

/* Retained per field, on its node (allocated when it's first focused). */
struct BareEditState {
    int      caret, anchor;     /* byte offsets; the selection is between them */
    bool     upstream;          /* the caret sits at the end of a wrapped line, not the next one's start */
    float    goal_x;            /* ↑ ↓ keep this column */
    bool     has_goal;
    bool     overwrite;
    bool     focused;           /* had focus last frame */
    double   blink;             /* when the caret last moved: it shows solid, then blinks */
    uint64_t hash;  int len;    /* the buffer as the field left it: anything else was the app */
    char    *comp;  int comp_len, comp_cap, comp_cursor;   /* IME composition, shown at the caret */
    BareUndo *undo; int undo_count, undo_cap, undo_pos;    /* undo_pos: records before it are done */
    char    *bytes; int bytes_len, bytes_cap;
    bool     merge;             /* the last record may still grow (a typing run) */
    uint8_t  drag;              /* selecting with the mouse by 1 characters, 2 words, 3 lines */
    int      drag_a, drag_b;    /* the word or line first clicked */
};

typedef struct BareEditCtx BareEditCtx;
struct BareEditCtx {             /* one field, this frame */
    BareContext   *ctx;
    BareEditState *ed;
    char          *buf;  int len, cap;    /* cap: bytes the text may use (the NUL excluded) */
    bool           multiline, password, tabs, read_only;
    BareText    *(*layout)(BareEditCtx *e);   /* the text as shown, wrapped at box_w; NULL if unknown */
    void          *user;
    float          box_w;       /* text width, logical pixels */
    float          page_h;      /* visible height, for PageUp / PageDown */
    bool           changed, submitted, moved;
    int            gen;         /* counts changes to the text (layout caches by it) */
};

uint64_t bare__edit_hash(const char *s, int len);
int  bare__edit_next(const char *s, int len, int at);   /* the next code point boundary */
int  bare__edit_prev(const char *s, int at);
void bare__edit_word_at(const char *s, int len, int at, int *a, int *b);   /* the run (word, spaces, ...) under at */
int  bare__edit_word_left(const char *s, int at);                    /* Ctrl+Left from at (stops at a line start) */
int  bare__edit_word_right(const char *s, int len, int at, bool mac);  /* Ctrl+Right (macOS: Option+Right) */
void bare__edit_run(BareEditCtx *e, BareEditAction action, bool extend);
void bare__edit_type(BareEditCtx *e, const char *text, int n);
void bare__edit_select(BareEditCtx *e, int anchor, int caret, bool upstream);
void bare__edit_forget(BareEditState *ed);   /* the app replaced the text: no undo, no composition */
void bare__edit_set_comp(BareEditCtx *e, const char *text, int n, int cursor);

#endif
