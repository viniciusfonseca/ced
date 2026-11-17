#include "editor_bridge.h"
#include "cui_internal.h"
#include <stdio.h>
#include <stdlib.h>

#define EDITOR_SALT 0xED17C0DEu
typedef struct { cui_edit_state edit; double sy; float sx; int preferred_column; } EditorStatePrefix;

bool ced_tree_begin(cui_ctx *ui, const char *label, bool *open, const cui_tree_opts *options) {
    int row = ui->node_count;
    bool expanded = cui_tree_begin(ui, label, open, options);
    /* The pinned CUI creates the row first and fixes its height at 30 px.
     * Adjust its layout so drawing and hit testing both use the compact height. */
    ui->nodes[row].h = CUI_PX(24);
    return expanded;
}

void ced_editor_touch(cui_ctx *ui, const char *id) {
    cui__state_entry(ui, cui_make_id(ui, id) ^ EDITOR_SALT);
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
