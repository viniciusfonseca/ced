#ifndef CED_EDITOR_ASSIST_H
#define CED_EDITOR_ASSIST_H

#include "lsp.h"
#include <cui/cui.h>

typedef struct {
    cui_id body;
    cui_rect content, caret, symbol;
    int hover_start, hover_end;
    bool focused;
} CedEditorGeometry;

typedef struct {
    CedEditorGeometry geometry;
    int editor_tab;
    bool completion_open;
    int completion_tab, completion_cursor, count, selected;
    CedCompletion completions[CED_LSP_COMPLETIONS];
    cui_id completion_layer;
    bool scroll_selection;
    bool hover_open, hover_requested, hover_keyboard;
    int hover_tab, hover_offset, hover_end;
    double hover_since, hover_last_inside;
    cui_vec2 hover_mouse;
    cui_rect hover_anchor, hover_rect;
    char hover[16384];
} CedEditorAssist;

/* Called in the same ID scope as the code editor. The adapter follows the
 * geometry and state prefix of the CUI revision pinned in CMake. */
CedEditorGeometry ced_editor_geometry(cui_ctx *ui, const char *id, const char *text, float size);
bool ced_editor_replace(cui_ctx *ui, const char *id, char *text, size_t cap, const CedCompletion *completion);
bool ced_editor_completion_key(cui_ctx *ui, const char *id);
void ced_assist_reset(CedEditorAssist *assist);
int ced_assist_keys(cui_ctx *ui, CedEditorAssist *assist);
/* Returns the byte offset to query after the mouse rests on a symbol. */
int ced_assist_hover(cui_ctx *ui, CedEditorAssist *assist, int tab, CedEditorGeometry geometry, bool changed, bool can_request);
void ced_assist_show_completions(CedEditorAssist *assist, const CedLspResult *result);
void ced_assist_show_hover(CedEditorAssist *assist, const char *text);
void ced_assist_hover_at_caret(cui_ctx *ui, CedEditorAssist *assist, int tab, int offset);
/* Draws nonmodal overlays; returns the index clicked in the completion list. */
int ced_assist_draw(cui_ctx *ui, CedEditorAssist *assist);

#endif
