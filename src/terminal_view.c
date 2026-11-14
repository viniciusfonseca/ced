/* The custom terminal widget uses CUI's internal focus/input hooks. Keep
 * this adapter isolated; CUI is pinned to a specific revision in CMake. */
#include "terminal_view.h"
#include "cui_internal.h"

typedef struct { CedTerminal *term; CedTerminalView *view; float size, cw, ch; } Paint;

static int utf8(char *out, uint32_t c) {
    if (c < 0x80) { out[0] = (char)c; return 1; }
    if (c < 0x800) { out[0] = (char)(0xc0 | (c >> 6)); out[1] = (char)(0x80 | (c & 63)); return 2; }
    if (c < 0x10000) { out[0] = (char)(0xe0 | (c >> 12)); out[1] = (char)(0x80 | ((c >> 6) & 63)); out[2] = (char)(0x80 | (c & 63)); return 3; }
    out[0] = (char)(0xf0 | (c >> 18)); out[1] = (char)(0x80 | ((c >> 12) & 63)); out[2] = (char)(0x80 | ((c >> 6) & 63)); out[3] = (char)(0x80 | (c & 63)); return 4;
}

static void copy_selection(cui_ctx *ui, CedTerminal *t, CedTerminalView *v) {
    if (!v->selected || !ui->platform.set_clipboard) return;
    int a = CUI_MIN(v->anchor, v->cursor), b = CUI_MAX(v->anchor, v->cursor);
    size_t cap = (size_t)(b - a + 1) * (4 * VTERM_MAX_CHARS_PER_CELL + 1) + 1;
    char *text = malloc(cap);
    if (!text) return;
    size_t n = 0;
    for (int at = a; at <= b; at++) {
        VTermScreenCell cell;
        if (ced_terminal_cell(t, at / t->columns, at % t->columns, &cell)) {
            if (!cell.chars[0]) text[n++] = ' ';
            for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i] && cell.chars[i] != UINT32_MAX; i++) n += (size_t)utf8(text + n, cell.chars[i]);
        }
        if (at % t->columns == t->columns - 1 && at < b) {
            while (n && text[n - 1] == ' ') n--;
            text[n++] = '\n';
        }
    }
    text[n] = 0;
    ui->platform.set_clipboard(ui->platform.user, text);
    free(text);
}

static void paint(cui_ctx *ui, cui_node *node, void *user) {
    Paint *p = user;
    CedTerminal *t = p->term;
    cui_rect r = node->rect;
    int rows = (int)((r.h - 12) / p->ch), columns = (int)((r.w - 12) / p->cw);
    if (rows != t->rows || columns != t->columns) p->view->selected = false;
    ced_terminal_resize(t, rows, columns);
    cui_push_clip(ui, r);
    cui_draw_rect(ui, r, CUI_CORNERS(0), (cui_color){23, 24, 27, 255});
    int a = CUI_MIN(p->view->anchor, p->view->cursor), b = CUI_MAX(p->view->anchor, p->view->cursor);
    for (int row = 0; row < t->rows; row++) {
        for (int col = 0; col < t->columns; col++) {
            VTermScreenCell cell;
            if (!ced_terminal_cell(t, row, col, &cell)) continue;
            vterm_screen_convert_color_to_rgb(t->screen, &cell.fg);
            vterm_screen_convert_color_to_rgb(t->screen, &cell.bg);
            cui_color fg = {cell.fg.rgb.red, cell.fg.rgb.green, cell.fg.rgb.blue, 255};
            cui_color bg = {cell.bg.rgb.red, cell.bg.rgb.green, cell.bg.rgb.blue, 255};
            if (cell.attrs.reverse) { cui_color swap = fg; fg = bg; bg = swap; }
            int index = row * t->columns + col;
            if (p->view->selected && index >= a && index <= b) bg = (cui_color){56, 80, 122, 255};
            cui_rect cr = {r.x + 6 + col * p->cw, r.y + 6 + row * p->ch, p->cw * (cell.width > 0 ? cell.width : 1), p->ch};
            cui_draw_rect(ui, cr, CUI_CORNERS(0), bg);
            if (cell.width == 0 || cell.chars[0] == UINT32_MAX || cell.attrs.conceal) continue;
            char bytes[VTERM_MAX_CHARS_PER_CELL * 4]; int n = 0;
            for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; i++) n += utf8(bytes + n, cell.chars[i]);
            if (n) cui_draw_text(ui, CUI_FONT_MONO, p->size, (cui_vec2){cr.x, cr.y}, fg, bytes, n);
            if (cell.attrs.underline) cui_draw_line(ui, (cui_vec2){cr.x, cr.y + p->ch - 2}, (cui_vec2){cr.x + cr.w, cr.y + p->ch - 2}, 1, fg);
        }
    }
    if (t->cursor_visible && !t->scroll && p->view->focused) {
        VTermPos cursor; vterm_state_get_cursorpos(vterm_obtain_state(t->vt), &cursor);
        cui_draw_rect_border(ui, (cui_rect){r.x + 6 + cursor.col * p->cw, r.y + 6 + cursor.row * p->ch, p->cw, p->ch},
                             CUI_CORNERS(0), CUI_TRANSPARENT, (cui_color){220, 223, 228, 255}, 1);
    }
    cui_pop_clip(ui);
}

void ced_terminal_view(cui_ctx *ui, CedTerminal *t, CedTerminalView *v, float size) {
    cui_id id = cui_make_id(ui, "terminal-screen");
    cui_interact r = cui__interact(ui, id, CUI_IF_FOCUSABLE | CUI_IF_DRAG | CUI_IF_TEXT);
    bool autofocus = v->autofocus;
    if (autofocus) { ui->focus = id; v->autofocus = false; }
    v->focused = ui->focus == id && (r.keyboard_focused || autofocus);
    float cw = cui_measure_text(ui, CUI_FONT_MONO, size, "M", 1).width;
    if (cw <= 0) cw = size * 0.6f;
    float ch = size * 1.45f;
    cui_rect rect = cui_get_rect(ui, id);
    bool started = r.active && ui->mpressed[CUI_MOUSE_LEFT];
    if (started) { v->selecting = true; v->selected = false; }
    if (v->selecting) {
        int row = CUI_CLAMP((int)((ui->my - rect.y - 6) / ch), 0, t->rows - 1);
        int col = CUI_CLAMP((int)((ui->mx - rect.x - 6) / cw), 0, t->columns - 1);
        if (started) v->anchor = row * t->columns + col;
        v->cursor = row * t->columns + col;
        v->selected = v->cursor != v->anchor;
        if (!ui->mdown[CUI_MOUSE_LEFT]) v->selecting = false;
    }
    if (r.hovered && ui->scroll_dy) {
        t->scroll = CUI_CLAMP(t->scroll + (int)(ui->scroll_dy * 3), 0, t->history_count);
        v->selected = false;
        ui->scroll_dy = 0;
    }
    if (v->focused) {
        ui->tab_owner = id;
        bool copy = cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_C) || cui_shortcut(ui, CUI_MOD_CTRL | CUI_MOD_SHIFT, CUI_KEY_C);
        bool paste = cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_V) || cui_shortcut(ui, CUI_MOD_CTRL | CUI_MOD_SHIFT, CUI_KEY_V);
#ifndef __APPLE__
        if (!v->selected && cui_shortcut(ui, CUI_MOD_CTRL, CUI_KEY_C)) copy = false;
#endif
        if (copy) copy_selection(ui, t, v);
        else if (paste && ui->platform.get_clipboard) ced_terminal_text(t, ui->platform.get_clipboard(ui->platform.user), true);
        else if (cui_shortcut(ui, CUI_MOD_SHIFT, CUI_KEY_PAGE_UP) || cui_shortcut(ui, CUI_MOD_SHIFT, CUI_KEY_PAGE_DOWN)) {
            int direction = cui_key_pressed(ui, CUI_KEY_PAGE_UP) ? 1 : -1;
            t->scroll = CUI_CLAMP(t->scroll + direction * (t->rows - 1), 0, t->history_count);
        } else if (!(ui->mods & CUI_MOD_SUPER)) {
            VTermModifier mods = (VTermModifier)(((ui->mods & CUI_MOD_SHIFT) ? VTERM_MOD_SHIFT : 0) |
                ((ui->mods & CUI_MOD_CTRL) ? VTERM_MOD_CTRL : 0) | ((ui->mods & CUI_MOD_ALT) ? VTERM_MOD_ALT : 0));
            static const struct { cui_key cui; VTermKey vt; } keys[] = {
                {CUI_KEY_ENTER, VTERM_KEY_ENTER}, {CUI_KEY_TAB, VTERM_KEY_TAB}, {CUI_KEY_BACKSPACE, VTERM_KEY_BACKSPACE},
                {CUI_KEY_ESCAPE, VTERM_KEY_ESCAPE}, {CUI_KEY_UP, VTERM_KEY_UP}, {CUI_KEY_DOWN, VTERM_KEY_DOWN},
                {CUI_KEY_LEFT, VTERM_KEY_LEFT}, {CUI_KEY_RIGHT, VTERM_KEY_RIGHT}, {CUI_KEY_HOME, VTERM_KEY_HOME},
                {CUI_KEY_END, VTERM_KEY_END}, {CUI_KEY_DELETE, VTERM_KEY_DEL}, {CUI_KEY_PAGE_UP, VTERM_KEY_PAGEUP}, {CUI_KEY_PAGE_DOWN, VTERM_KEY_PAGEDOWN}};
            for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
                if (cui_key_pressed(ui, keys[i].cui)) ced_terminal_key(t, keys[i].vt, mods);
            for (int k = CUI_KEY_F1; k <= CUI_KEY_F12; k++)
                if (cui_key_pressed(ui, (cui_key)k)) ced_terminal_key(t, VTERM_KEY_FUNCTION(k - CUI_KEY_F1 + 1), mods);
            if (ui->mods & CUI_MOD_CTRL) {
                for (int k = CUI_KEY_A; k <= CUI_KEY_Z; k++)
                    if (cui_key_pressed(ui, (cui_key)k)) vterm_keyboard_unichar(t->vt, (uint32_t)('a' + k - CUI_KEY_A), mods);
                if (cui_key_pressed(ui, CUI_KEY_SPACE)) vterm_keyboard_unichar(t->vt, ' ', mods);
            } else if (ui->text_in_len) {
                char text[sizeof ui->text_in + 1];
                memcpy(text, ui->text_in, (size_t)ui->text_in_len); text[ui->text_in_len] = 0;
                ced_terminal_text(t, text, false);
            }
        }
        ui->text_in_len = 0;
        memset(ui->kpressed, 0, sizeof ui->kpressed);
    }
    int node = cui__node_new(ui, id);
    ui->nodes[node].w = CUI_FILL; ui->nodes[node].h = CUI_FILL;
    cui__mark_interactive(ui, node, CUI_IF_FOCUSABLE | CUI_IF_DRAG | CUI_IF_TEXT);
    Paint *p = cui__frame_alloc(ui, sizeof *p);
    *p = (Paint){t, v, size, cw, ch};
    ui->nodes[node].paint = paint; ui->nodes[node].paint_data = p;
    ui->last_id = id;
}
