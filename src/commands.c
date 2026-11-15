#include "commands.h"
#include "workspace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool ced_command_condition(const CedCommandCondition *when, const CedCommandContext *context) {
    unsigned values = (context->has_document ? CED_WHEN_DOCUMENT : 0) |
        (context->editor_text_focus ? CED_WHEN_FOCUS : 0) |
        (context->has_document && context->anchor != context->cursor ? CED_WHEN_SELECTION : 0) |
        (context->workspace_trusted ? CED_WHEN_TRUSTED : 0);
    return (values & when->mask) == when->values &&
        (!when->language[0] || (context->language_id && !strcmp(when->language, context->language_id)));
}

const CedCommandEntry *ced_commands_find(const CedCommands *commands, const char *id) {
    for (size_t i = 0; i < commands->count; i++) if (!strcmp(commands->entries[i].id, id)) return &commands->entries[i];
    return NULL;
}

static void entry_free(CedCommandEntry *entry) {
    free(entry->id); free(entry->title); free(entry->category); free(entry->shortcut);
}

bool ced_commands_add(CedCommands *commands, const char *id, const char *title,
    const char *category, const char *shortcut, CedCommandCondition when,
    const void *owner, CedCommandHandler handler, void *user, char *error) {
    if (!id || !*id || !title || !*title || !handler || ced_commands_find(commands, id) || commands->count == 1024) {
        snprintf(error, CED_ERROR, "Invalid or duplicate command: %.256s", id ? id : ""); return false;
    }
    CedCommandEntry entry = {strdup(id), strdup(title), strdup(category ? category : ""),
        strdup(shortcut ? shortcut : ""), when, owner, handler, user};
    if (!entry.id || !entry.title || !entry.category || !entry.shortcut) goto memory;
    if (commands->count == commands->capacity) {
        size_t capacity = commands->capacity ? commands->capacity * 2 : 32;
        CedCommandEntry *entries = realloc(commands->entries, capacity * sizeof *entries);
        if (!entries) goto memory;
        commands->entries = entries; commands->capacity = capacity;
    }
    commands->entries[commands->count++] = entry;
    return true;
memory:
    entry_free(&entry); snprintf(error, CED_ERROR, "Out of memory"); return false;
}

bool ced_commands_execute(const CedCommands *commands, const char *id, const CedCommandContext *context, char *error) {
    const CedCommandEntry *entry = ced_commands_find(commands, id);
    if (!entry || !ced_command_condition(&entry->when, context)) {
        snprintf(error, CED_ERROR, "Command unavailable: %.256s", id); return false;
    }
    return entry->handler(entry->user, id, context, error);
}

void ced_commands_remove_owner(CedCommands *commands, const void *owner) {
    size_t count = 0;
    for (size_t i = 0; i < commands->count; i++) {
        if (commands->entries[i].owner == owner) entry_free(&commands->entries[i]);
        else commands->entries[count++] = commands->entries[i];
    }
    commands->count = count;
}

void ced_commands_free(CedCommands *commands) {
    for (size_t i = 0; i < commands->count; i++) entry_free(&commands->entries[i]);
    free(commands->entries); memset(commands, 0, sizeof *commands);
}
