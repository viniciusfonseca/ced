#ifndef CED_EXTENSION_UI_H
#define CED_EXTENSION_UI_H
#include "manager.h"
#include <cui/cui.h>
/* Returns true when the user requests to install an extension. */
bool ced_extensions_panel(cui_ctx *ui, const CedExtensions *manager);
const CedExtensionTab *ced_extension_tab_find(CedExtensions *manager, const char *id,
    CedExtensionInstance **owner, size_t *index);
bool ced_extension_tab_buttons(cui_ctx *ui, CedExtensions *manager, bool terminal, char *selected);
bool ced_extension_has_tabs(const CedExtensions *manager, bool terminal);
void ced_extension_tab_body(cui_ctx *ui, CedExtensions *manager, const char *selected);
void ced_extensions_ui(cui_ctx *ui, CedExtensions *manager);
#endif
