#ifndef CED_EDITOR_BRIDGE_H
#define CED_EDITOR_BRIDGE_H

#include "documents.h"
#include <cui/cui.h>
bool ced_tree_begin(cui_ctx *ui, const char *label, bool *open, const cui_tree_opts *options, bool ignored);
bool ced_list_item_ex(cui_ctx *ui, const char *label, const cui_list_item_opts *opts);
bool ced_nav_item_ex(cui_ctx *ui, const char *label, const cui_nav_opts *opts);
bool ced_menu_item_ex(cui_ctx *ui, const char *label, const cui_menu_item_opts *opts);
int ced_command_palette(cui_ctx *ui, bool *open, const cui_command *commands, int count);
bool ced_aux_panel_button(cui_ctx *ui, const char *id, bool right, bool selected, const char *tooltip);
bool ced_git_tab_button(cui_ctx *ui, int count, bool truncated, bool selected);
#define ced_list_item(ui, label, ...) ced_list_item_ex((ui), (label), CUI__OPTS(cui_list_item_opts, __VA_ARGS__))
#define ced_nav_item(ui, label, ...) ced_nav_item_ex((ui), (label), CUI__OPTS(cui_nav_opts, __VA_ARGS__))
#define ced_menu_item(ui, label, ...) ced_menu_item_ex((ui), (label), CUI__OPTS(cui_menu_item_opts, __VA_ARGS__))
enum { CED_TAB_MENU_NONE = -2, CED_TAB_MENU_CLOSE_ALL = -1 };
/* Close Other Tabs returns the index of the tab to keep. */
int ced_editor_tabs_close_menu(cui_ctx *ui, const char *id, const cui_doc_tab *tabs, int count);
/* Anchor a menu to the preceding widget. Open it with cui_popover_open. */
bool ced_anchored_menu_begin(cui_ctx *ui, const char *id, float width);
void ced_editor_touch(cui_ctx *ui, const char *id);
/* Track effective native text changes, including undo/redo. */
cui_code_editor_result ced_editor_render(cui_ctx *ui, const char *id, CedDocument *document,
                                         const cui_code_editor_opts *options);

/* Invoke at the start of a frame, in the target editor's stable ID scope.
 * Inactive editors are supported after their CUI state has been initialized. */
bool ced_editor_commit(cui_ctx *ui, const char *id, CedDocument *document,
                       uint64_t expected_version, const CedEditTransaction *transaction, char *error);

#endif
