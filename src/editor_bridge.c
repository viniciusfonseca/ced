#include "editor_bridge.h"
#include "cui_internal.h"
#include <stdio.h>
#include <stdlib.h>

#define EDITOR_SALT 0xED17C0DEu
typedef struct { cui_edit_state edit; double sy; float sx; int preferred_column; } EditorStatePrefix;

bool ced_tree_begin(cui_ctx *ui, const char *label, bool *open, const cui_tree_opts *options, bool ignored) {
    int row = ui->node_count;
    bool expanded = cui_tree_begin(ui, label, open, options);
    /* The pinned CUI creates the row first and fixes its height at 30 px.
     * Adjust its layout so drawing and hit testing both use the compact height. */
    ui->nodes[row].h = CUI_PX(24);
    ui->nodes[row].radius = CUI_CORNERS(0);
    ui->nodes[row].opacity = ignored ? 0.5f : 1.0f;
    return expanded;
}

bool ced_list_item_ex(cui_ctx *ui, const char *label, const cui_list_item_opts *opts) {
    int row = ui->node_count;
    bool clicked = cui_list_item_ex(ui, label, opts);
    ui->nodes[row].radius = CUI_CORNERS(0);
    return clicked;
}

/* CUI paints the animated navigation selection separately from the row. */
extern void cui__paint_nav_highlight(cui_ctx *ui, cui_node *node, void *data);

static void paint_square_nav_highlight(cui_ctx *ui, cui_node *node, void *data) {
    if (node->abs_anchor < 0) return;
    cui_rect rect = node->rect;
    rect.w = ui->nodes[node->abs_anchor].rect.w;
    cui_draw_rect(ui, rect, CUI_CORNERS(0), *(const cui_color *)data);
}

bool ced_nav_item_ex(cui_ctx *ui, const char *label, const cui_nav_opts *opts) {
    int row = ui->node_count;
    bool clicked = cui_nav_item_ex(ui, label, opts);
    ui->nodes[row].radius = CUI_CORNERS(0);
    for (int i = row + 1; i < ui->node_count; i++) {
        cui_node *node = &ui->nodes[i];
        if (node->paint != cui__paint_nav_highlight) continue;
        cui_color *color = CUI_PAINT_DATA(ui, cui_color);
        *color = CUI_C(ui, ACTIVE);
        node->paint_data = color;
        node->paint = paint_square_nav_highlight;
    }
    return clicked;
}

bool ced_menu_item_ex(cui_ctx *ui, const char *label, const cui_menu_item_opts *opts) {
    int row = ui->node_count;
    bool clicked = cui_menu_item_ex(ui, label, opts);
    ui->nodes[row].radius = CUI_CORNERS(0);
    return clicked;
}

static void paint_aux_panel_icon(cui_ctx *ui, cui_node *node, void *data) {
    const bool right = *(const bool *)data;
    const float size = 12, divider = 8, stroke = 1.2f;
    const cui_rect rect = {node->rect.x + (node->rect.w - size) * 0.5f,
                           node->rect.y + (node->rect.h - size) * 0.5f, size, size};
    const cui_color ink = CUI_C(ui, FOREGROUND);
    cui_draw_rect_border(ui, rect, CUI_CORNERS(1.8f), (cui_color){0}, ink, stroke);
    if (right)
        cui_draw_line(ui, (cui_vec2){rect.x + divider, rect.y}, (cui_vec2){rect.x + divider, rect.y + size}, stroke, ink);
    else
        cui_draw_line(ui, (cui_vec2){rect.x, rect.y + divider}, (cui_vec2){rect.x + size, rect.y + divider}, stroke, ink);
}

bool ced_aux_panel_button(cui_ctx *ui, const char *id, bool right, bool selected, const char *tooltip) {
    int button = ui->node_count;
    bool clicked = cui__icon_button_id(ui, cui_make_id(ui, id), "",
        &(cui_button_opts){.size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST, .selected = selected,
            .tooltip = tooltip});
    // The pinned CUI icon font only contains the left panel glyph.
    bool *position = CUI_PAINT_DATA(ui, bool);
    *position = right;
    ui->nodes[button].paint = paint_aux_panel_icon;
    ui->nodes[button].paint_data = position;
    return clicked;
}

int ced_command_palette(cui_ctx *ui, bool *open, const cui_command *commands, int count) {
    int first = ui->node_count;
    int selected = cui_command_palette(ui, open, commands, count);
    /* The pinned palette builds command rows directly in its scroll area. */
    for (int i = first; i < ui->node_count; i++) {
        cui_node *node = &ui->nodes[i];
        if ((node->flags & CUI_NF_INTERACTIVE) && node->parent >= first &&
            (ui->nodes[node->parent].flags & CUI_NF_SCROLL_Y))
            node->radius = CUI_CORNERS(0);
    }
    return selected;
}

bool ced_anchored_menu_begin(cui_ctx *ui, const char *id, float width) {
    return cui__popup_begin(ui, cui_make_id(ui, id), cui_last_id(ui),
        CUI_PLACE_BOTTOM_START, width, 6, 0, true);
}

void ced_editor_touch(cui_ctx *ui, const char *id) {
    cui__state_entry(ui, cui_make_id(ui, id) ^ EDITOR_SALT);
}

int ced_editor_tabs_close_menu(cui_ctx *ui, const char *id, const cui_doc_tab *tabs, int count) {
    cui_id previous = ui->last_id;
    ui->last_id = 0;
    cui_push_id(ui, id);
    cui_id *target = cui_state(ui, cui_make_id(ui, "tab-context-target"), sizeof *target);
    int keep = -1;
    for (int i = 0; i < count; i++) {
        /* The pinned CUI scopes tab IDs by the strip ID and derives the
         * close button ID from the tab title in cui__dtf_build_tab. */
        cui_id tab = cui_make_id(ui, tabs[i].title);
        if (ui->mpressed[CUI_MOUSE_RIGHT] && (ui->hot == tab || ui->hot == (tab ^ 0xC105Eu))) {
            ui->last_id = ui->hot;
            *target = tab;
            keep = i;
            break;
        }
        if (*target == tab) keep = i;
    }
    int action = CED_TAB_MENU_NONE;
    cui_context_menu(ui, "tab-context") {
        if (ced_menu_item_ex(ui, "Close All Tabs", NULL)) action = CED_TAB_MENU_CLOSE_ALL;
        if (ced_menu_item(ui, "Close Other Tabs", .disabled = count < 2 || keep < 0)) action = keep;
    }
    cui_pop_id(ui);
    ui->last_id = previous;
    return action;
}

cui_code_editor_result ced_editor_render(cui_ctx *ui, const char *id, CedDocument *doc,
                                         const cui_code_editor_opts *options) {
    bool input = ui->text_in_len > 0;
    for (int key = 0; !input && key < CUI_KEY_COUNT; key++) input = ui->kpressed[key] != 0;
    char *before = input ? strdup(doc->text) : NULL;
    cui_code_editor_opts opts = *options;
    /* Refuse edits if their revision cannot be tracked; navigation still works. */
    opts.read_only |= (input && !before) || doc->version >= CED_REVISION_MAX;
    cui_code_editor_result result = cui_code_editor_ex(ui, id, doc->text, CED_TEXT_CAP, &opts);
    if (result.changed && before && !strcmp(before, doc->text)) result.changed = false;
    if (result.changed) ced_document_note_change(doc);
    free(before);
    return result;
}

bool ced_editor_commit(cui_ctx *ui, const char *id, CedDocument *doc,
                       uint64_t expected_version, const CedEditTransaction *transaction, char *error) {
    if (doc->version != expected_version) { snprintf(error, CED_ERROR, "StaleDocument"); return false; }
    cui_state_entry *entry = cui__state_entry(ui, cui_make_id(ui, id) ^ EDITOR_SALT);
    if (!entry->data || entry->size < sizeof(EditorStatePrefix)) {
        snprintf(error, CED_ERROR, "Unavailable"); return false;
    }
    cui_edit_state *state = entry->data; /* CUI's code editor begins with its edit state. */
    if (!transaction->changed) return true;
    if (transaction->length >= CED_TEXT_CAP || doc->version >= CED_REVISION_MAX) {
        snprintf(error, CED_ERROR, "ResourceLimit"); return false;
    }
    /* Prepare the checkpoint before touching either history or text. CUI's
     * void undo-push API does not report allocation failures. */
    size_t length = strlen(doc->text);
    /* cui__alloc aborts on OOM; use the same allocator directly so this
     * external transaction can fail before changing text or history. */
    char *checkpoint = ui->alloc.alloc(length + 1, ui->alloc.user);
    if (!checkpoint) { snprintf(error, CED_ERROR, "Out of memory"); return false; }
    memcpy(checkpoint, doc->text, length + 1);
    for (int i = state->undo_pos; i < state->undo_count; i++) {
        cui__free(ui, state->undo[i]); state->undo[i] = NULL;
    }
    state->undo_count = state->undo_pos;
    if (state->undo_count == CUI__UNDO_MAX) {
        cui__free(ui, state->undo[0]);
        memmove(state->undo, state->undo + 1, sizeof state->undo[0] * (CUI__UNDO_MAX - 1));
        memmove(state->undo_cursor, state->undo_cursor + 1, sizeof state->undo_cursor[0] * (CUI__UNDO_MAX - 1));
        state->undo_count--;
    }
    state->undo[state->undo_count] = checkpoint;
    state->undo_cursor[state->undo_count++] = state->cursor;
    state->undo_pos = state->undo_count;
    state->last_kind = 3; state->last_edit = ui->time;
    memcpy(doc->text, transaction->text, transaction->length + 1);
    state->anchor = (int)transaction->anchor; state->cursor = (int)transaction->cursor;
    state->last_cursor = state->cursor;
    ((EditorStatePrefix *)entry->data)->preferred_column = -1;
    ced_document_note_change(doc);
    /* Do not call code_editor_select: it would steal focus from another tab. */
    cui_request_wakeup(ui, 0);
    return true;
}
