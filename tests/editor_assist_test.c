#include "editor_assist.h"
#include "cui_internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static cui_ctx *ui;
static CedEditorAssist assist;
static char text[8192] = "int main(void) {\n\té🙂 + answer;\n}\n";
static CedEditorGeometry geometry;
static cui_code_editor_result caret;
static float font_size = 14;
static bool complete;
static int chosen, jump = -1, hover;

static void frame(float dt) {
    cui_begin_frame(ui, dt);
    chosen = ced_assist_keys(ui, &assist);
    complete = ced_editor_completion_key(ui, "source");
    if (jump >= 0) { cui_code_editor_select(ui, "source", jump, jump); jump = -1; }
    caret = cui_code_editor(ui, "source", text, sizeof text, .height = 300, .size = font_size, .lang = "c", .autofocus = true);
    geometry = ced_editor_geometry(ui, "source", text, font_size);
    hover = ced_assist_hover(ui, &assist, 1, geometry, caret.changed, true);
    int clicked = ced_assist_draw(ui, &assist);
    if (clicked >= 0) chosen = clicked;
    cui_end_frame(ui);
}

static void release(cui_key key) { cui_input_key(ui, key, false, 0); frame(0.02f); }

int main(void) {
    ui = cui_create(NULL); assert(ui);
    cui_set_viewport(ui, 800, 500, 1);
    cui_input_focus(ui, true);
    frame(0.02f); frame(0.02f);
    int target = (int)(strstr(text, "answer") - text);
    jump = target; frame(0.02f);
    cui_rect anchor = geometry.caret;
    jump = 0; frame(0.02f);
    cui_input_mouse_move(ui, anchor.x + 2, anchor.y + anchor.h / 2);
    frame(0.02f); frame(0.02f);
    assert(geometry.hover_start == target && geometry.hover_end == target + 6);
    assert(caret.cursor == 0); /* hovering must not move the caret */
    frame(0.4f); assert(hover == target);
    ced_assist_show_hover(&assist, "```c\nint answer\n```");
    frame(0.02f); frame(0.02f);
    assert(assist.hover_open && ui->modal_layer == 0 && geometry.focused);
    assert(assist.hover_rect.w > 0 && assist.hover_rect.x >= 0 && assist.hover_rect.y >= 0);
    cui_input_mouse_move(ui, 790, 490); frame(0.3f);
    assert(!assist.hover_open);

    jump = target + 3; frame(0.02f);
    cui_input_key(ui, CUI_KEY_SPACE, true, CUI_MOD_CTRL);
    cui_input_text(ui, " "); /* some backends deliver text alongside the key */
    frame(0.02f);
    assert(complete && !strstr(text, "ans wer"));
    assert(caret.cursor == target + 3);
    release(CUI_KEY_SPACE);
    cui_input_key(ui, CUI_KEY_SPACE, true, 0); cui_input_text(ui, " "); frame(0.02f);
    assert(!complete && strstr(text, "ans wer")); release(CUI_KEY_SPACE);
    cui_input_key(ui, CUI_KEY_Z, true, CUI_MOD_PRIMARY); frame(0.02f); release(CUI_KEY_Z);
    assert(strstr(text, "answer"));

    static CedLspResult result;
    result.count = 2;
    strcpy(result.completions[0].label, "answer"); strcpy(result.completions[0].text, "answer");
    strcpy(result.completions[1].label, "another"); strcpy(result.completions[1].text, "another");
    result.completions[1].start = target; result.completions[1].end = target + 6;
    ced_assist_show_completions(&assist, &result);
    frame(0.02f);
    assert(assist.completion_open && ui->modal_layer == 0);
    int before = caret.cursor;
    cui_input_key(ui, CUI_KEY_DOWN, true, 0); frame(0.02f);
    assert(assist.selected == 1 && caret.cursor == before); release(CUI_KEY_DOWN);
    cui_input_key(ui, CUI_KEY_ENTER, true, 0); frame(0.02f);
    assert(chosen == 1 && !assist.completion_open && caret.cursor == before);
    release(CUI_KEY_ENTER);
    cui_begin_frame(ui, 0.02f);
    assert(ced_editor_replace(ui, "source", text, sizeof text, &result.completions[1]));
    cui_code_editor(ui, "source", text, sizeof text, .height = 300);
    cui_end_frame(ui);
    assert(strstr(text, "another"));
    cui_input_key(ui, CUI_KEY_Z, true, CUI_MOD_PRIMARY); frame(0.02f); release(CUI_KEY_Z);
    assert(strstr(text, "answer") && !strstr(text, "another"));
    cui_input_key(ui, CUI_KEY_Z, true, CUI_MOD_PRIMARY | CUI_MOD_SHIFT); frame(0.02f); release(CUI_KEY_Z);
    assert(strstr(text, "another"));

    /* The symbol hit test must follow zoom and the editor's own scrolling. */
    for (int i = 0; i < 60; i++) strcat(text, "int filler;\n");
    strcat(text, "int distant;\n");
    target = (int)(strstr(text, "distant") - text);
    font_size = 24; jump = target; frame(0.02f); frame(0.02f);
    anchor = geometry.caret;
    cui_input_mouse_move(ui, anchor.x + 2, anchor.y + anchor.h / 2);
    frame(0.02f); frame(0.02f);
    assert(geometry.hover_start == target);
    ced_assist_show_completions(&assist, &result);
    cui_input_key(ui, CUI_KEY_ESCAPE, true, 0); frame(0.02f);
    assert(!assist.completion_open); release(CUI_KEY_ESCAPE);
    cui_destroy(ui);
    puts("OK: mouse symbol hit testing, Unicode/tabs, hover debounce, nonmodal popover, Ctrl-Space, completion navigation, undo/redo, zoom and scroll");
    return 0;
}
