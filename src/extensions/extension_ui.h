#ifndef CED_EXTENSION_UI_H
#define CED_EXTENSION_UI_H
#include "manager.h"
#include <cui/cui.h>
/* Returns true when the user requests to install an extension. */
bool ced_extensions_panel(cui_ctx *ui, const CedExtensions *manager);
void ced_extensions_ui(cui_ctx *ui, CedExtensions *manager);
#endif
