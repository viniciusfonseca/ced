#ifndef CED_DOCUMENTS_H
#define CED_DOCUMENTS_H

#include "workspace.h"
#include <stdint.h>

#define CED_REVISION_MAX UINT64_C(9007199254740991)
#define CED_EDIT_LIMIT 1000

typedef struct { size_t start, end; const char *text; } CedTextEdit;
typedef struct {
    char *text;
    size_t length, anchor, cursor;
    bool changed;
} CedEditTransaction;

bool ced_utf8_valid(const char *text, size_t length);
bool ced_utf8_boundary(const char *text, size_t length, size_t offset);
void ced_document_identify(CedDocument *document, uint64_t id);
bool ced_document_note_change(CedDocument *document);
/* Prepare without modifying the original buffer. Edits refer to that buffer,
 * may be unordered, and must not overlap. The caller commits via editor_bridge. */
bool ced_document_prepare_edit(const CedDocument *document, uint64_t expected_version,
    const CedTextEdit *edits, size_t count, size_t anchor, size_t cursor,
    bool explicit_selection, size_t selected_anchor, size_t selected_cursor,
    CedEditTransaction *transaction, char *error);
void ced_edit_transaction_free(CedEditTransaction *transaction);

#endif
