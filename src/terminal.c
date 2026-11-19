#include "terminal.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/ioctl.h>
#include <sys/wait.h>
#endif
#include <time.h>
#include <unistd.h>
#if defined(_WIN32)
#elif defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

#ifndef _WIN32
extern char **environ;
#endif

static void output(const char *bytes, size_t length, void *user) {
    ced_terminal_send(user, bytes, length);
}

static int property(VTermProp prop, VTermValue *value, void *user) {
    CedTerminal *t = user;
    if (prop == VTERM_PROP_CURSORVISIBLE) t->cursor_visible = value->boolean;
    return 1;
}

static int push_line(int columns, const VTermScreenCell *cells, void *user) {
    CedTerminal *t = user;
    int at = (t->history_first + t->history_count) % CED_TERM_HISTORY;
    CedTermLine *line = &t->history[at];
    if (!line->cells || line->columns != columns) {
        VTermScreenCell *copy = realloc(line->cells, sizeof *copy * (size_t)columns);
        if (!copy) return 0;
        line->cells = copy;
        line->columns = columns;
    }
    memcpy(line->cells, cells, sizeof *cells * (size_t)columns);
    if (t->history_count == CED_TERM_HISTORY)
        t->history_first = (t->history_first + 1) % CED_TERM_HISTORY;
    else t->history_count++;
    if (t->scroll > 0 && t->scroll < t->history_count) t->scroll++;
    return 1;
}

static int pop_line(int columns, VTermScreenCell *cells, void *user) {
    CedTerminal *t = user;
    if (!t->history_count) return 0;
    CedTermLine *line = &t->history[(t->history_first + --t->history_count) % CED_TERM_HISTORY];
    memset(cells, 0, sizeof *cells * (size_t)columns);
    memcpy(cells, line->cells, sizeof *cells * (size_t)(columns < line->columns ? columns : line->columns));
    free(line->cells);
    line->cells = NULL;
    if (t->scroll > t->history_count) t->scroll = t->history_count;
    return 1;
}

CedTerminal *ced_terminal_start(const char *root, const char *shell, char *error, size_t cap) {
#ifdef _WIN32
    if (!shell || !*shell) shell=getenv("COMSPEC");
    if (!shell || !*shell) shell="cmd.exe";
#else
    if (!shell || !*shell) shell = "/bin/sh";
#endif
    CedTerminal *t = calloc(1, sizeof *t);
    if (!t) { snprintf(error, cap, "Out of memory."); return NULL; }
    t->fd = -1; t->rows = 24; t->columns = 80; t->cursor_visible = true;
    t->vt = vterm_new(t->rows, t->columns);
    if (!t->vt) { free(t); snprintf(error, cap, "Could not create terminal."); return NULL; }
    vterm_set_utf8(t->vt, 1);
    t->screen = vterm_obtain_screen(t->vt);
    static const VTermScreenCallbacks callbacks = {.settermprop = property, .sb_pushline = push_line, .sb_popline = pop_line};
    vterm_screen_set_callbacks(t->screen, &callbacks, t);
    vterm_screen_enable_altscreen(t->screen, 1);
    vterm_output_set_callback(t->vt, output, t);
    VTermColor fg, bg;
    vterm_color_rgb(&fg, 220, 223, 228); vterm_color_rgb(&bg, 23, 24, 27);
    vterm_state_set_default_colors(vterm_obtain_state(t->vt), &fg, &bg);
    vterm_screen_reset(t->screen, 1);
#ifdef _WIN32
    const char *args[]={shell,NULL};
    if (!ced_windows_spawn(&t->process,args,root,true,false,NULL)) {
        snprintf(error,cap,"Could not start the Windows pseudoconsole (Windows 10 1809+ required).");
        ced_terminal_stop(t); return NULL;
    }
    t->alive=true;
#else
    size_t count = 0;
    while (environ[count]) count++;
    char **env = calloc(count + 3, sizeof *env);
    if (!env) { ced_terminal_stop(t); snprintf(error, cap, "Out of memory."); return NULL; }
    size_t used = 0;
    for (size_t i = 0; i < count; i++)
        if (strncmp(environ[i], "TERM=", 5) && strncmp(environ[i], "COLORTERM=", 10)) env[used++] = environ[i];
    env[used++] = "TERM=xterm-256color";
    env[used] = "COLORTERM=truecolor";
    struct winsize size = {.ws_row = (unsigned short)t->rows, .ws_col = (unsigned short)t->columns};
    t->pid = forkpty(&t->fd, NULL, NULL, &size);
    if (!t->pid) {
        char *const args[] = {(char *)shell, "-i", NULL};
        if (chdir(root) == 0) execve(shell, args, env);
        static const char message[] = "Could not start shell.\r\n";
        write(STDERR_FILENO, message, sizeof message - 1);
        _exit(127);
    }
    free(env);
    if (t->pid < 0) {
        snprintf(error, cap, "Could not start terminal: %s", strerror(errno));
        ced_terminal_stop(t); return NULL;
    }
    t->alive = true;
    fcntl(t->fd, F_SETFD, FD_CLOEXEC);
    fcntl(t->fd, F_SETFL, O_NONBLOCK);
#endif
    return t;
}

bool ced_terminal_send(CedTerminal *t, const char *data, size_t length) {
    if (!t || !t->alive || length > sizeof t->pending - t->pending_length) return false;
    memcpy(t->pending + t->pending_length, data, length);
    t->pending_length += length;
    t->scroll = 0;
    return true;
}

bool ced_terminal_poll(CedTerminal *t) {
    if (!t) return false;
    bool changed = false;
    bool writable=t->fd>=0;
#ifdef _WIN32
    writable=t->alive;
#endif
    if (t->pending_length && writable) {
#ifdef _WIN32
        ssize_t n=ced_windows_write(&t->process,t->pending,t->pending_length);
#else
        ssize_t n = write(t->fd, t->pending, t->pending_length);
#endif
        if (n > 0) {
            memmove(t->pending, t->pending + n, t->pending_length - (size_t)n);
            t->pending_length -= (size_t)n;
        }
    }
    char bytes[8192];
    for (int i = 0; i < 8; i++) {
#ifdef _WIN32
        ssize_t n=ced_windows_read(&t->process,bytes,sizeof bytes);
#else
        if (t->fd<0) break;
        ssize_t n = read(t->fd, bytes, sizeof bytes);
#endif
        if (n <= 0) break;
        vterm_input_write(t->vt, bytes, (size_t)n);
        changed = true;
    }
    vterm_screen_flush_damage(t->screen);
    if (t->alive) {
#ifdef _WIN32
        if (ced_windows_exited(&t->process,&t->exit_status)) { t->alive=false; changed=true; }
#else
        int status;
        pid_t result = waitpid(t->pid, &status, WNOHANG);
        if (result == t->pid || (result < 0 && errno == ECHILD)) {
            t->alive = false;
            t->exit_status = result > 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            changed = true;
        }
#endif
    }
    return changed;
}

void ced_terminal_resize(CedTerminal *t, int rows, int columns) {
    if (!t || rows < 2 || columns < 2) return;
    if (rows > 200) rows = 200;
    if (columns > 500) columns = 500;
    if (rows == t->rows && columns == t->columns) return;
    t->rows = rows; t->columns = columns;
    vterm_set_size(t->vt, rows, columns);
#ifdef _WIN32
    if (t->process.console) ResizePseudoConsole(t->process.console,(COORD){(SHORT)columns,(SHORT)rows});
#else
    struct winsize size = {.ws_row = (unsigned short)rows, .ws_col = (unsigned short)columns};
    if (t->fd >= 0) ioctl(t->fd, TIOCSWINSZ, &size);
#endif
}

void ced_terminal_key(CedTerminal *t, VTermKey key, VTermModifier mods) {
    if (t && t->alive) vterm_keyboard_key(t->vt, key, mods);
}

void ced_terminal_text(CedTerminal *t, const char *text, bool paste) {
    if (!t || !t->alive || !text) return;
    if (paste) vterm_keyboard_start_paste(t->vt);
    /* Bracketed paste protects commands when the shell enables it. */
    ced_terminal_send(t, text, strlen(text));
    if (paste) vterm_keyboard_end_paste(t->vt);
}

bool ced_terminal_cell(CedTerminal *t, int row, int column, VTermScreenCell *cell) {
    memset(cell, 0, sizeof *cell);
    if (row < 0 || column < 0 || column >= t->columns || row >= t->rows) return false;
    int history_row = t->history_count - t->scroll + row;
    if (history_row < t->history_count) {
        if (history_row < 0) return false;
        CedTermLine *line = &t->history[(t->history_first + history_row) % CED_TERM_HISTORY];
        if (column >= line->columns) return false;
        *cell = line->cells[column]; return true;
    }
    return vterm_screen_get_cell(t->screen, (VTermPos){row - t->scroll, column}, cell) != 0;
}

void ced_terminal_stop(CedTerminal *t) {
    if (!t) return;
#ifdef _WIN32
    ced_windows_process_close(&t->process);
#else
    if (t->alive) {
        pid_t foreground = tcgetpgrp(t->fd);
        if (foreground > 0 && foreground != getpgrp()) kill(-foreground, SIGHUP);
        kill(-t->pid, SIGHUP);
    }
    if (t->fd >= 0) close(t->fd);
    if (t->alive) {
        int status;
        for (int i = 0; i < 20; i++) {
            if (waitpid(t->pid, &status, WNOHANG) != 0) { t->alive = false; break; }
            struct timespec pause = {.tv_nsec = 10000000}; nanosleep(&pause, NULL);
        }
        if (t->alive) { kill(-t->pid, SIGKILL); while (waitpid(t->pid, &status, 0) < 0 && errno == EINTR) {} }
    }
#endif
    for (int i = 0; i < t->history_count; i++) free(t->history[(t->history_first + i) % CED_TERM_HISTORY].cells);
    if (t->vt) vterm_free(t->vt);
    free(t);
}
