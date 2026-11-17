#ifndef CED_EDITOR_BRIDGE_H
#define CED_EDITOR_BRIDGE_H

#include "documents.h"
#include <cui/cui.h>
bool ced_tree_begin(cui_ctx *ui, const char *label, bool *open, const cui_tree_opts *options);
void ced_editor_touch(cui_ctx *ui, const char *id);
/* Track effective native text changes, including undo/redo. */
cui_code_editor_result ced_editor_render(cui_ctx *ui, const char *id, CedDocument *document,
                                         const cui_code_editor_opts *options);

/* Invoke at the start of a frame, in the target editor's stable ID scope.
 * Inactive editors are supported after their CUI state has been initialized. */
bool ced_editor_commit(cui_ctx *ui, const char *id, CedDocument *document,
                       uint64_t expected_version, const CedEditTransaction *transaction, char *error);

#endif
