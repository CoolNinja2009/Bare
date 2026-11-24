/* md.h: Markdown for the Write example: a highlighter and an indenter for
 * bare_editor, and a live preview made of Bare elements. */
#ifndef WRITE_MD_H
#define WRITE_MD_H

#include "bare.h"

typedef struct MdLook {
    BareColor text, muted, accent, code, code_bg, rule;
    float     size;             /* body text size */
    BareFont  font;             /* body font (code is always BARE_MONO) */
} MdLook;

/* bare_editor's .highlight (user: an MdLook) and .indent (continues lists and quotes). */
int  md_highlight(void *user, const char *line, int len, int *state, BareHighlight *out, int cap);
int  md_indent(void *user, const char *line, int len, int col, char *out, int cap);

/* The document rendered, at the current place in the tree. Clicking a task
 * box ticks it in the document. */
void md_preview(BareDoc *doc, const MdLook *look);

#endif
