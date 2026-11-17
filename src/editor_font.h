#ifndef CED_EDITOR_FONT_H
#define CED_EDITOR_FONT_H
#include "preferences.h"
#include <cui/cui.h>

#define CED_FONT_CACHE_LIMIT 8
typedef struct { char path[CED_PATH]; unsigned char *data; cui_font font; bool monospace; } CedFontEntry;
typedef struct {
    cui_font default_font;
    bool initialized;
    size_t count;
    CedFontEntry entries[CED_FONT_CACHE_LIMIT];
} CedEditorFont;

bool ced_editor_font_apply(cui_ctx *ui, CedEditorFont *fonts, const CedPreferences *settings, char *error);
/* Font bytes must outlive the CUI context. Call after destroying that context. */
void ced_editor_font_free(CedEditorFont *fonts);
bool ced_font_find(const char *family, char *path, char *error);
#endif
