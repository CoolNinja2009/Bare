/* doc.h: documents (doc.c), their editing commands (doc_edit.c) and the
 * editor view (doc_view.c). See ARCHITECTURE §8. */
#ifndef BARE_DOC_H
#define BARE_DOC_H

#include "internal.h"

/* A line, without its line break. `cap` 0: the bytes belong to the loaded
 * file (doc->orig) and are copied on the first change. */
typedef struct BareLine {
    char    *s;
    int      len, cap;
    int      hl_end;        /* the highlighter's state at the line's end (valid below doc->hl_valid) */
    uint32_t rows_key;      /* the wrap settings `rows` was measured with (0 = not measured) */
    uint16_t rows;          /* screen rows when wrapped */
} BareLine;

typedef struct BareSel {
    BareTextPos anchor, caret;
    float       goal_x;     /* ↑ ↓ keep this x (pixels; columns without a layout) */
    bool        has_goal;
    bool        upstream;   /* drawn at the end of the upper wrapped row */
} BareSel;

/* One change: `text` (in the pool) was inserted at `at`, or removed from there. */
typedef struct BareDocEdit { BareTextPos at; size_t text, len; bool insert; } BareDocEdit;

/* One undo step: its edits and the selections before and after (in sel_pool). */
typedef struct BareDocGroup {
    int      first, count;          /* edits */
    int      sels_before, nbefore, sels_after, nafter;
    uint8_t  kind;                  /* BARE__UNDO_* from edit.h (what may merge) */
    uint64_t id;                    /* the document state after it */
} BareDocGroup;

typedef struct BareFindMatch { int line, col, len; } BareFindMatch;

/* The editor's view of a document, kept on the document (one view each). */
typedef struct BareDocView {
    BareId     area;                /* the editor's text area (its scroll box) */
    double     blink;               /* the carets last moved */
    bool       focused, follow, overwrite;
    uint8_t    drag;                /* mouse selection by 1 characters, 2 words, 3 lines; 4 a column; 5 Alt held */
    BareTextPos drag_a, drag_b;     /* the word or line first clicked */
    float      drag_x, drag_y;      /* where a column drag started (content coordinates) */
    float      max_w;               /* widest line drawn so far (no-wrap content width) */
    char      *comp; int comp_len, comp_cap, comp_cursor;   /* IME composition at the main caret */
    /* Find, replace and go to line. */
    bool       find_open, replace_open, goto_open, was_in_bar;
    uint8_t    focus_bar;           /* 1 find, 2 replace, 3 go to: focus it this frame */
    char       query[256], replace[256], goto_line[16];
    int        find_flags;
    BareTextPos find_from;          /* where the search started (typing searches from here) */
    uint64_t   find_version, find_hash;   /* what the matches were found for */
    BareFindMatch *matches; int match_count, match_cap;
    bool       matches_capped;
    BareId     find_field, replace_field, goto_field;
} BareDocView;

struct BareDoc {
    BareAllocator alloc;
    BareLine     *lines;  int count, cap;
    char         *orig;   size_t orig_size;   /* the loaded file; lines point in until changed */
    bool          crlf;                        /* saves with \r\n */
    bool          bom;                         /* the file began with a UTF-8 BOM (kept on save) */
    uint64_t      version;                     /* every change */
    uint64_t      state, saved_state, next_state;   /* undo-aware: modified = state != saved */
    BareSel      *sels;   int sel_count, sel_cap, main;
    /* undo */
    BareDocEdit  *edits;  int edit_count, edit_cap;
    BareDocGroup *groups; int group_count, group_cap, undo_pos;
    BareSel      *sel_pool; int sel_pool_len, sel_pool_cap;
    char         *pool;   size_t pool_len, pool_cap;
    int           open;                        /* a group is being recorded (nesting depth) */
    int           open_group;                  /* its index (-1: not recording, out of memory) */
    bool          merge;                       /* the last group may still grow (a typing run) */
    bool          replaying;                   /* undo / redo: changes aren't recorded */
    uint64_t      base_state;                  /* the state before the oldest step kept */
    /* caches */
    int           hl_valid;                    /* lines below this have a valid hl_end */
    BareHighlighter hl_fn; void *hl_user;      /* the highlighter the states are from */
    int          *row_sum; int row_sum_cap; bool rows_dirty; uint32_t rows_key;   /* prefix sums of rows */
    char         *text;   size_t text_cap; uint64_t text_version; int text_len;   /* bare_doc_text */
    int           longest;                     /* bytes in the longest line (no-wrap width estimate) */
    bool          longest_dirty;
    char         *scratch; size_t scratch_cap;   /* bare__doc_scratch */
    char         *seltext; size_t seltext_cap;   /* bare_doc_selected */
    uint64_t      line_clip;                   /* hash of whole lines we copied (they paste as lines) */
    BareDocView   view;
};

/* doc.c: changes (each records itself for undo and moves the selections). */
BareTextPos bare__doc_insert(BareDoc *d, BareTextPos at, const char *s, int n);   /* returns the end */
void  bare__doc_delete(BareDoc *d, BareTextPos a, BareTextPos b);
void  bare__doc_begin(BareDoc *d, int kind);          /* groups changes into one undo step */
void  bare__doc_end(BareDoc *d);
bool  bare__doc_undo(BareDoc *d, bool redo);
BareTextPos bare__doc_clamp(const BareDoc *d, BareTextPos p);
int   bare__pos_cmp(BareTextPos a, BareTextPos b);
char *bare__doc_range(BareDoc *d, BareArena *arena, BareTextPos a, BareTextPos b, int *len);   /* '\n' breaks */
size_t bare__doc_range_size(const BareDoc *d, BareTextPos a, BareTextPos b);   /* a <= b, both valid */
void  bare__doc_range_copy(const BareDoc *d, BareTextPos a, BareTextPos b, char *out);
char *bare__doc_scratch(BareDoc *d, size_t n);       /* a buffer of the document's, valid until the next call */
bool  bare__doc_find_in_line(const char *s, int len, int from, const char *q, int qn, int flags, bool back,
                             int *at);
void  bare__doc_sels_normalize(BareDoc *d);           /* sorted, overlapping ones merged */
bool  bare__doc_rows(BareDoc *d, uint32_t key);       /* prefix sums current for key; false: out of memory */
void  bare__doc_view_free(BareDoc *d);

/* doc_edit.c: commands on every selection. */
typedef struct BareDocCmd {
    BareContext *ctx;
    BareDoc     *doc;
    BareText *(*layout)(void *user, int line);   /* the line as shown, wrapped; NULL: one row, columns */
    void        *user;
    float        width;              /* what `layout` wrapped at */
    int          page;               /* rows in a page */
    bool         read_only, spaces, overwrite;
    int          tab_size;
    const char  *comment, *auto_close;
    int        (*indent)(void *user, const char *line, int len, int col, char *out, int cap);
    void        *indent_user;
    bool         changed, moved;
    bool         blur;               /* Escape with nothing left to collapse */
} BareDocCmd;

void bare__doc_run(BareDocCmd *c, BareEditAction a, bool extend);
void bare__doc_type(BareDocCmd *c, const char *s, int n);
void bare__doc_paste(BareDocCmd *c, const char *s, bool as_typed);
void bare__doc_toggle(BareDoc *d, const char *prefix);           /* comment / uncomment the selected lines */
void bare__doc_command(BareDoc *d, BareEditAction a, bool extend);   /* without a view (tests, scripts) */

#endif
