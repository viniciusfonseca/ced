#ifndef CED_PREFERENCES_H
#define CED_PREFERENCES_H

#include <stdbool.h>
#include "workspace.h"

#define CED_FONT_SIZE_DEFAULT 14
#define CED_FONT_SIZE_MIN 8
#define CED_FONT_SIZE_MAX 32
#define CED_PREFERENCES_LIMIT (64 * 1024)

typedef struct {
    int font_size;
    bool terminal_right;
    char font_family[256], font_path[CED_PATH];
} CedPreferences;

void ced_preferences_defaults(CedPreferences *settings);
bool ced_preferences_path(char *path, char *error);
bool ced_preferences_load(CedPreferences *settings, char *error);
bool ced_preferences_validate(const char *text, size_t length, CedPreferences *settings, char *error);
bool ced_preferences_ensure(char *path, char *error);

int ced_font_size_load(void);
bool ced_font_size_save(int size);
bool ced_terminal_right_load(void);
bool ced_terminal_right_save(bool right);

#endif
