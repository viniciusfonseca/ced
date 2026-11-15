#ifndef CED_COMMANDS_H
#define CED_COMMANDS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t document_id, version, selection_version;
    size_t anchor, cursor;
    bool has_document, editor_text_focus, workspace_trusted;
    const char *language_id;
} CedCommandContext;

typedef struct {
    unsigned mask, values;
    char language[64];
} CedCommandCondition;

enum { CED_WHEN_DOCUMENT = 1, CED_WHEN_FOCUS = 2, CED_WHEN_SELECTION = 4, CED_WHEN_TRUSTED = 8 };
typedef bool (*CedCommandHandler)(void *user, const char *id, const CedCommandContext *context, char *error);
typedef struct {
    char *id, *title, *category, *shortcut;
    CedCommandCondition when;
    const void *owner;
    CedCommandHandler handler;
    void *user;
} CedCommandEntry;
typedef struct { CedCommandEntry *entries; size_t count, capacity; } CedCommands;

bool ced_command_condition(const CedCommandCondition *when, const CedCommandContext *context);
bool ced_commands_add(CedCommands *commands, const char *id, const char *title,
    const char *category, const char *shortcut, CedCommandCondition when,
    const void *owner, CedCommandHandler handler, void *user, char *error);
const CedCommandEntry *ced_commands_find(const CedCommands *commands, const char *id);
bool ced_commands_execute(const CedCommands *commands, const char *id, const CedCommandContext *context, char *error);
void ced_commands_remove_owner(CedCommands *commands, const void *owner);
void ced_commands_free(CedCommands *commands);

#endif
