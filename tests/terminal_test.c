#include "terminal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void tick(CedTerminal *t) {
    ced_terminal_poll(t);
    struct timespec pause = {.tv_nsec = 10000000}; nanosleep(&pause, NULL);
}

static bool contains(CedTerminal *t, const char *needle) {
    char text[8192];
    size_t n = vterm_screen_get_text(t->screen, text, sizeof text - 1, (VTermRect){0, t->rows, 0, t->columns});
    text[n] = 0;
    return strstr(text, needle) != NULL;
}

static void wait_for(CedTerminal *t, const char *text) {
    for (int i = 0; i < 500; i++) {
        tick(t);
        if (contains(t, text)) return;
        assert(t->alive);
    }
    fprintf(stderr, "Terminal output timed out: %s\n", text); abort();
}

int main(void) {
    char error[512], root[4096], temp[] = "/tmp/ced-terminal-XXXXXX";
    assert(mkdtemp(temp)); assert(realpath(temp, root));
    unsetenv("ENV");
    CedTerminal *t = ced_terminal_start(root, "/bin/sh", error, sizeof error);
    assert(t);
    ced_terminal_text(t, "printf '\\033[31mCED_%s\\033[0m\\n' TERM_OK; pwd\n", false);
    wait_for(t, "CED_TERM_OK"); wait_for(t, root);
    bool red = false;
    for (int row = 0; row < t->rows; row++) for (int col = 0; col < t->columns; col++) {
        VTermScreenCell cell;
        if (ced_terminal_cell(t, row, col, &cell) && cell.chars[0] == 'C') {
            vterm_screen_convert_color_to_rgb(t->screen, &cell.fg);
            red |= cell.fg.rgb.red > cell.fg.rgb.green;
        }
    }
    assert(red);
    ced_terminal_resize(t, 12, 64);
    ced_terminal_text(t, "stty size\n", false);
    wait_for(t, "12 64");
    ced_terminal_text(t, "cd /; printf 'PWD_%s_END\\n' \"$PWD\"\n", false);
    wait_for(t, "PWD_/_END");
    ced_terminal_text(t, "sleep 30\n", false);
    for (int i = 0; i < 10; i++) tick(t);
    vterm_keyboard_unichar(t->vt, 'c', VTERM_MOD_CTRL);
    ced_terminal_text(t, "printf 'AFTER_%s\\n' INTERRUPT\n", false);
    wait_for(t, "AFTER_INTERRUPT");
    const char *alt = "\033[?1049h\033[2J\033[HALTERNATE";
    vterm_input_write(t->vt, alt, strlen(alt)); vterm_screen_flush_damage(t->screen);
    assert(contains(t, "ALTERNATE"));
    vterm_input_write(t->vt, "\033[?1049l", 8); vterm_screen_flush_damage(t->screen);
    assert(contains(t, "AFTER_INTERRUPT"));
    for (int i = 0; i < 30; i++) vterm_input_write(t->vt, "history\r\n", 9);
    assert(t->history_count > 0); t->scroll = 1;
    VTermScreenCell cell; assert(ced_terminal_cell(t, 0, 0, &cell));
    /* Wrap the ring, then verify that equal-width lines reuse its storage. */
    for (int i = 0; i < CED_TERM_HISTORY + 20; i++) vterm_input_write(t->vt, "history\r\n", 9);
    assert(t->history_count == CED_TERM_HISTORY);
    VTermScreenCell *oldest = t->history[t->history_first].cells;
    vterm_input_write(t->vt, "history\r\n", 9);
    int newest = (t->history_first + t->history_count - 1) % CED_TERM_HISTORY;
    assert(t->history[newest].cells == oldest);
    ced_terminal_resize(t, 2000, 2000);
    assert(t->rows == 200 && t->columns == 500);
    ced_terminal_resize(t, 12, 64);
    ced_terminal_text(t, "exit\n", false);
    for (int i = 0; i < 100 && t->alive; i++) tick(t);
    assert(!t->alive && t->exit_status == 0);
    ced_terminal_stop(t);
    assert(!rmdir(root));
    puts("OK: real PTY, ANSI colors, persistent shell/cwd, resize, Ctrl-C, alternate screen, scrollback and cleanup");
    return 0;
}
