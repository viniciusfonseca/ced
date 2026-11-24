#ifndef CED_PREFERENCES_H
#define CED_PREFERENCES_H

#include <stdbool.h>
#include "workspace.h"

#define CED_FONT_SIZE_DEFAULT 14
#define CED_FONT_SIZE_MIN 8
#define CED_FONT_SIZE_MAX 32
#define CED_PREFERENCES_LIMIT (64 * 1024)
#define CED_RECENT_FOLDERS_LIMIT 10
#define CED_TERMINAL_TAB_CAPACITY 257

typedef struct {
    size_t count;
    char paths[CED_RECENT_FOLDERS_LIMIT][CED_PATH];
} CedRecentFolders;

typedef struct {
    char pom[CED_PATH], maven_settings[CED_PATH], gradle[CED_PATH], gradle_java[CED_PATH];
    char jdtls_path[CED_PATH], kotlin_lsp_path[CED_PATH];
} CedJvmSettings;

typedef struct {
    int font_size;
    bool terminal_right, terminal_visible;
    char terminal_tab[CED_TERMINAL_TAB_CAPACITY];
    char font_family[256], font_path[CED_PATH];
    CedJvmSettings jvm;
} CedPreferences;

void ced_preferences_defaults(CedPreferences *settings);
bool ced_preferences_path(char *path, char *error);
bool ced_preferences_project(const char *workspace, const CedJvmSettings *defaults, CedJvmSettings *project, char *error);
bool ced_preferences_load(CedPreferences *settings, char *error);
bool ced_preferences_validate(const char *text, size_t length, CedPreferences *settings, char *error);
bool ced_preferences_ensure(char *path, char *error);
bool ced_recent_folders_load(CedRecentFolders *folders, char *error);
bool ced_recent_folder_record(const char *canonical_path);

int ced_font_size_load(void);
bool ced_font_size_save(int size);
bool ced_terminal_right_load(void);
bool ced_terminal_right_save(bool right);
bool ced_terminal_state_save(bool visible, const char *tab);

#endif
