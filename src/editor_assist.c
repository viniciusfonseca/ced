#include "editor_assist.h"
#include "cui_internal.h"
#include <ctype.h>

#define EDITOR_SALT 0xED17C0DEu
#define EDITOR_BODY 0xED1B0D1u
typedef struct { cui_edit_state edit; double sy; float sx; } EditorStatePrefix;

static bool inside(cui_rect r, cui_vec2 p) {
    return r.w > 0 && r.h > 0 && p.x >= r.x && p.y >= r.y && p.x < r.x + r.w && p.y < r.y + r.h;
}

static bool identifier(unsigned char c) { return isalnum(c) || c == '_' || c >= 128; }

static int column(const char *text, int a, int b) {
    int col = 0;
    for (int i = a; i < b; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\t') col = (col / 4 + 1) * 4;
        else if ((c & 0xc0) != 0x80) col++;
    }
    return col;
}

CedEditorGeometry ced_editor_geometry(cui_ctx *ui, const char *id, const char *text, float size) {
    CedEditorGeometry g = {.hover_start = -1, .hover_end = -1};
    cui_id key = cui_make_id(ui, id);
    cui_state_entry *entry = cui__state_entry(ui, key ^ EDITOR_SALT);
    if (!entry->data || entry->size < sizeof(EditorStatePrefix)) return g;
    const EditorStatePrefix *state = entry->data;
    g.body = key ^ EDITOR_BODY;
    cui_rect body = cui_get_rect(ui, g.body);
    g.focused = ui->focus == g.body;
    float fs = cui__fs(ui, size), lh = roundf(fs * 1.6f);
    float cw = cui__text_width(ui, CUI_FONT_MONO, fs, "0000000000000000", 16, true) / 16;
    if (body.w <= 0 || body.h <= 0 || cw <= 0) return g;
    int length = (int)strlen(text);
    int lines = 1, caret_line = 0, caret_start = 0, cursor = CUI_CLAMP(state->edit.cursor, 0, length);
    for (int at = 0; text[at]; at++) if (text[at] == '\n') {
        lines++;
        if (at < cursor) { caret_line++; caret_start = at + 1; }
    }
    int digits = 2;
    for (int n = lines / 100; n > 0; n /= 10) digits++;
    float gutter = 26 + digits * cw;
    g.content = (cui_rect){body.x + gutter + 8, body.y + 6, body.w - gutter - 20, body.h - 18};
    float x0 = g.content.x - state->sx, y0 = g.content.y - (float)state->sy;
    g.caret = (cui_rect){x0 + column(text, caret_start, cursor) * cw, y0 + caret_line * lh, 1, lh};
    cui_vec2 mouse = cui_mouse_pos(ui);
    if (!inside(g.content, mouse) || ui->hot != g.body || ui->mdown[0]) return g;
    int line = (int)floorf((mouse.y - y0) / lh), start = 0;
    if (line < 0) return g;
    for (int i = 0; i < line; i++) {
        const char *next = strchr(text + start, '\n');
        if (!next) return g;
        start = (int)(next - text) + 1;
    }
    int col = 0, at = start;
    float target = (mouse.x - x0) / cw;
    while (text[at] && text[at] != '\n') {
        int next_col = text[at] == '\t' ? (col / 4 + 1) * 4 : col + 1;
        if (target < next_col) break;
        at = cui__utf8_next(text, length, at); col = next_col;
    }
    if (!identifier((unsigned char)text[at]) || !text[at]) return g;
    int a = at, b = at;
    while (a > start && identifier((unsigned char)text[a - 1])) a--;
    while (text[b] && identifier((unsigned char)text[b])) b++;
    if (isdigit((unsigned char)text[a])) return g;
    g.hover_start = a; g.hover_end = b;
    g.symbol = (cui_rect){x0 + column(text, start, a) * cw, y0 + line * lh, column(text, a, b) * cw, lh};
    return g;
}

bool ced_editor_completion_key(cui_ctx *ui, const char *id) {
    if (ui->focus != (cui_make_id(ui, id) ^ EDITOR_BODY)) return false;
    if (!cui_shortcut(ui, CUI_MOD_CTRL, CUI_KEY_SPACE) &&
        !cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_SHIFT, CUI_KEY_SPACE)) return false;
    ui->kpressed[CUI_KEY_SPACE] = 0;
    if (ui->text_in_len == 1 && ui->text_in[0] == ' ') ui->text_in_len = 0;
    return true;
}

void ced_assist_reset(CedEditorAssist *a) {
    a->completion_open = a->hover_open = a->hover_requested = a->hover_keyboard = false;
    a->hover_offset = -1; a->hover_tab = -1;
}

int ced_assist_keys(cui_ctx *ui, CedEditorAssist *a) {
    if (!a->completion_open) return -1;
    if ((ui->focus != a->geometry.body && ui->hot_layer != a->completion_layer) ||
        (ui->mpressed[0] && ui->hot_layer != a->completion_layer)) {
        a->completion_open = false; return -1;
    }
    if (cui_shortcut(ui, 0, CUI_KEY_ESCAPE)) {
        a->completion_open = false; ui->kpressed[CUI_KEY_ESCAPE] = 0; return -1;
    }
    int direction = cui_shortcut(ui, 0, CUI_KEY_DOWN) ? 1 : cui_shortcut(ui, 0, CUI_KEY_UP) ? -1 : 0;
    if (direction && a->count > 0) {
        a->selected = (a->selected + direction + a->count) % a->count;
        a->scroll_selection = true;
        ui->kpressed[CUI_KEY_UP] = ui->kpressed[CUI_KEY_DOWN] = 0;
    }
    if (cui_shortcut(ui, 0, CUI_KEY_ENTER) || cui_shortcut(ui, 0, CUI_KEY_TAB)) {
        ui->kpressed[CUI_KEY_ENTER] = ui->kpressed[CUI_KEY_TAB] = 0;
        a->completion_open = false; return a->selected;
    }
    return -1;
}

int ced_assist_hover(cui_ctx *ui, CedEditorAssist *a, int tab, CedEditorGeometry g, bool changed, bool can_request) {
    if (a->editor_tab != tab || changed) ced_assist_reset(a);
    a->editor_tab = tab; a->geometry = g;
    cui_vec2 mouse = cui_mouse_pos(ui);
    bool over_popover = a->hover_open && inside(a->hover_rect, mouse);
    if (a->completion_open || (ui->mpressed[0] && !over_popover) || ui->text_in_len || !ui->window_focused) {
        a->hover_open = false; a->hover_requested = false; a->hover_offset = -1; return -1;
    }
    if (cui_key_pressed(ui, CUI_KEY_ESCAPE)) {
        a->hover_open = false; a->hover_requested = true; return -1;
    }
    if (a->hover_keyboard && fabsf(mouse.x - a->hover_mouse.x) < 3 && fabsf(mouse.y - a->hover_mouse.y) < 3) return -1;
    a->hover_keyboard = false;
    if (over_popover) { a->hover_last_inside = ui->time; return -1; }
    if (g.hover_start < 0) {
        if (a->hover_open && ui->time - a->hover_last_inside < 0.18) return -1;
        a->hover_open = a->hover_requested = false; a->hover_offset = -1; return -1;
    }
    a->hover_last_inside = ui->time;
    if (a->hover_tab != tab || a->hover_offset != g.hover_start || a->hover_end != g.hover_end) {
        a->hover_open = a->hover_requested = false;
        a->hover_tab = tab; a->hover_offset = g.hover_start; a->hover_end = g.hover_end;
        a->hover_since = ui->time;
    }
    a->hover_anchor = g.symbol;
    if (!a->hover_requested && can_request && ui->time - a->hover_since >= 0.35) {
        a->hover_requested = true; return a->hover_offset;
    }
    if (!a->hover_requested) cui_request_wakeup(ui, 0.05f);
    return -1;
}

void ced_assist_show_completions(CedEditorAssist *a, const CedLspResult *r) {
    a->count = r->count; a->selected = 0; a->scroll_selection = true;
    memcpy(a->completions, r->completions, sizeof *a->completions * (size_t)r->count);
    a->completion_open = r->count > 0; a->hover_open = false;
}

void ced_assist_show_hover(CedEditorAssist *a, const char *text) {
    snprintf(a->hover, sizeof a->hover, "%s", text);
    a->hover_open = *text != 0;
}

void ced_assist_hover_at_caret(cui_ctx *ui, CedEditorAssist *a, int tab, int offset) {
    a->hover_tab = tab; a->hover_offset = offset;
    a->hover_requested = a->hover_keyboard = true;
    a->hover_mouse = cui_mouse_pos(ui); a->hover_anchor = a->geometry.caret;
}

static void panel_begin(cui_ctx *ui, cui_id id, cui_rect anchor, float width, float height) {
    cui_rect previous = cui_get_rect(ui, id);
    float h = previous.h > 0 ? previous.h : height;
    float y = anchor.y + anchor.h + 4;
    if (y + h > ui->vh - 8 && anchor.y - h - 4 >= 8) y = anchor.y - h - 4;
    int node = cui__float_begin(ui, id, 2200, 0, CUI__PLACE_POINT, 0);
    cui_node *n = &ui->nodes[node];
    n->fx = anchor.x; n->fy = y;
    n->w = CUI_PX(CUI_MIN(width, ui->vw - 16)); n->max_h = 300;
    n->pad[0] = n->pad[1] = n->pad[2] = n->pad[3] = 6;
    n->bg = CUI_C(ui, SURFACE_RAISED); n->border = 1; n->border_color = CUI_C(ui, BORDER);
    n->radius = CUI_CORNERS(8); n->elevation = CUI_ELEVATION_FLOATING; n->elevation_alpha = 1;
    n->flags |= CUI_NF_HIT_OPAQUE | CUI_NF_BLOCKER;
}

int ced_assist_draw(cui_ctx *ui, CedEditorAssist *a) {
    int selected = -1;
    if (a->completion_open) {
        cui_id id = cui_make_id(ui, "completion-popup"); a->completion_layer = id;
        float height = (float)CUI_MIN(a->count, 8) * 32;
        panel_begin(ui, id, a->geometry.caret, 420, height + 12);
        cui_push_id(ui, "completion-popup");
        if (a->scroll_selection) { cui_scroll_to(ui, "suggestions", a->selected * 32.0f); a->scroll_selection = false; }
        cui_scroll(ui, .id = "suggestions", .height = CUI_PX(height)) {
            for (int i = 0; i < a->count; i++) {
                cui_push_id_int(ui, i);
                if (cui_nav_item(ui, a->completions[i].label, .icon = CUI_ICON_CODE, .selected = i == a->selected)) selected = i;
                cui_tooltip(ui, a->completions[i].detail);
                cui_pop_id(ui);
            }
        }
        cui_pop_id(ui); cui__float_end(ui);
    } else if (a->hover_open) {
        cui_id id = cui_make_id(ui, "symbol-popover");
        a->hover_rect = cui_get_rect(ui, id);
        panel_begin(ui, id, a->hover_anchor, 500, 160);
        cui_box(ui, .id = "symbol-content", .max_height = 270, .scroll_y = true, .clip = true, .width = CUI_FILL, .pad = 6) {
            cui_markdown(ui, "symbol-markdown", a->hover, .compact = true);
        }
        cui__float_end(ui);
    }
    return selected;
}
