#ifndef CED_TERMINAL_VIEW_H
#define CED_TERMINAL_VIEW_H

#include "terminal.h"
#include <cui/cui.h>

typedef struct {
    bool autofocus, focused, selecting, selected;
    int anchor, cursor;
} CedTerminalView;

void ced_terminal_view(cui_ctx *ui, CedTerminal *terminal, CedTerminalView *view, float size);

#endif
