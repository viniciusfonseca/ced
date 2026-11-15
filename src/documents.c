#include "documents.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool ced_utf8_valid(const char *text, size_t length) {
    for (size_t i = 0; i < length;) {
        unsigned char c = (unsigned char)text[i++];
        if (!c) return false;
        if (c < 0x80) continue;
        unsigned count;
        uint32_t value, minimum;
        if (c >= 0xc2 && c <= 0xdf) { count = 1; value = c & 31; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { count = 2; value = c & 15; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { count = 3; value = c & 7; minimum = 0x10000; }
        else return false;
        if (length - i < count) return false;
        while (count--) {
            c = (unsigned char)text[i++];
            if ((c & 0xc0) != 0x80) return false;
            value = (value << 6) | (c & 63);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}

bool ced_utf8_boundary(const char *text, size_t length, size_t offset) {
    return offset <= length && (offset == length || ((unsigned char)text[offset] & 0xc0) != 0x80);
}

void ced_document_identify(CedDocument *document, uint64_t id) {
    document->id = id;
    document->version = 1;
    document->length = document->text ? strlen(document->text) : 0;
}

bool ced_document_note_change(CedDocument *document) {
    document->length = strlen(document->text);
    if (!document->id || document->version >= CED_REVISION_MAX) return false;
    document->version++;
    return true;
}

static bool fail(char *error, const char *message) {
    snprintf(error, CED_ERROR, "%s", message);
    return false;
}

static int edit_order(const void *a, const void *b) {
    const CedTextEdit *left = a, *right = b;
    return (left->start > right->start) - (left->start < right->start);
}

static size_t transform_position(size_t position, const CedTextEdit *edits, size_t count) {
    size_t previous = 0, output = 0;
    for (size_t i = 0; i < count; i++) {
        if (position < edits[i].start) return output + position - previous;
        output += edits[i].start - previous + strlen(edits[i].text);
        if (position < edits[i].end || (edits[i].start == edits[i].end && position == edits[i].end)) return output;
        previous = edits[i].end;
    }
    return output + position - previous;
}

bool ced_document_prepare_edit(const CedDocument *doc, uint64_t expected_version,
    const CedTextEdit *edits, size_t count, size_t anchor, size_t cursor,
    bool explicit_selection, size_t selected_anchor, size_t selected_cursor,
    CedEditTransaction *transaction, char *error) {
    memset(transaction, 0, sizeof *transaction);
    if (!doc || !doc->text || !doc->id) return fail(error, "Unavailable");
    if (doc->version != expected_version) return fail(error, "StaleDocument");
    if (count && !edits) return fail(error, "Invalid edits");
    if (count > CED_EDIT_LIMIT || doc->version >= CED_REVISION_MAX) return fail(error, "ResourceLimit");
    size_t length = strlen(doc->text), final_length = length;
    if (!ced_utf8_valid(doc->text, length)) return fail(error, "PermissionDenied");
    if (!ced_utf8_boundary(doc->text, length, anchor) || !ced_utf8_boundary(doc->text, length, cursor))
        return fail(error, "Invalid selection");
    CedTextEdit *ordered = count ? malloc(count * sizeof *ordered) : NULL;
    if (count && !ordered) return fail(error, "Out of memory");
    if (count) { memcpy(ordered, edits, count * sizeof *edits); qsort(ordered, count, sizeof *ordered, edit_order); }
    size_t added = 0, removed_total = 0;
    for (size_t i = 0; i < count; i++) {
        CedTextEdit *edit = &ordered[i];
        if (!edit->text || edit->start > edit->end ||
            !ced_utf8_boundary(doc->text, length, edit->start) || !ced_utf8_boundary(doc->text, length, edit->end) ||
            (i && (edit->start < ordered[i - 1].end || edit->start == ordered[i - 1].start ||
                (edit->start == ordered[i - 1].end && (edit->start == edit->end || ordered[i - 1].start == ordered[i - 1].end)))) ||
            !ced_utf8_valid(edit->text, strlen(edit->text)) || strchr(edit->text, '\r')) {
            free(ordered); return fail(error, "Invalid edits");
        }
        size_t inserted = strlen(edit->text), removed = edit->end - edit->start;
        removed_total += removed;
        if (inserted >= CED_TEXT_CAP || added > SIZE_MAX - inserted) {
            free(ordered); return fail(error, "ResourceLimit");
        }
        added += inserted;
    }
    final_length = length - removed_total;
    if (added >= CED_TEXT_CAP || final_length >= CED_TEXT_CAP - added) {
        free(ordered); return fail(error, "ResourceLimit");
    }
    final_length += added;
    char *text = malloc(final_length + 1);
    if (!text) { free(ordered); return fail(error, "Out of memory"); }
    size_t from = 0, to = 0;
    for (size_t i = 0; i < count; i++) {
        size_t copied = ordered[i].start - from, inserted = strlen(ordered[i].text);
        memcpy(text + to, doc->text + from, copied); to += copied;
        memcpy(text + to, ordered[i].text, inserted); to += inserted;
        from = ordered[i].end;
    }
    memcpy(text + to, doc->text + from, length - from + 1);
    size_t new_anchor = explicit_selection ? selected_anchor : transform_position(anchor, ordered, count);
    size_t new_cursor = explicit_selection ? selected_cursor : transform_position(cursor, ordered, count);
    free(ordered);
    if (!ced_utf8_boundary(text, final_length, new_anchor) || !ced_utf8_boundary(text, final_length, new_cursor)) {
        free(text); return fail(error, "Invalid selection");
    }
    *transaction = (CedEditTransaction){text, final_length, new_anchor, new_cursor,
        length != final_length || memcmp(doc->text, text, length) != 0};
    return true;
}

void ced_edit_transaction_free(CedEditTransaction *transaction) {
    free(transaction->text);
    memset(transaction, 0, sizeof *transaction);
}
