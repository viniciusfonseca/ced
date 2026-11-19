#ifndef CED_TERMINAL_H
#define CED_TERMINAL_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include <vterm.h>
#ifdef _WIN32
#include "windows_process.h"
#endif

#define CED_TERM_HISTORY 2000
typedef struct { int columns; VTermScreenCell *cells; } CedTermLine;
typedef struct {
#ifdef _WIN32
    CedWindowsProcess process;
#endif
    int fd, rows, columns, scroll;
    pid_t pid;
    bool alive, cursor_visible;
    int exit_status;
    VTerm *vt;
    VTermScreen *screen;
    CedTermLine history[CED_TERM_HISTORY];
    int history_first, history_count;
    char pending[256 * 1024];
    size_t pending_length;
} CedTerminal;

CedTerminal *ced_terminal_start(const char *root, const char *shell, char *error, size_t cap);
void ced_terminal_stop(CedTerminal *term);
bool ced_terminal_poll(CedTerminal *term);
bool ced_terminal_send(CedTerminal *term, const char *data, size_t length);
void ced_terminal_resize(CedTerminal *term, int rows, int columns);
void ced_terminal_key(CedTerminal *term, VTermKey key, VTermModifier mods);
void ced_terminal_text(CedTerminal *term, const char *text, bool paste);
bool ced_terminal_cell(CedTerminal *term, int row, int column, VTermScreenCell *cell);

#endif
