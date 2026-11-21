#include "extensions/extension_ui.h"
#include "cui_internal.h"
#include <cui/cui_render.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static CedCommandContext context(void *user) { (void)user; return (CedCommandContext){0}; }
static void draw_frame(cui_ctx *ui, CedExtensions *m) {
    cui_begin_frame(ui,.1f);
    cui_column(ui,.width=CUI_FILL,.height=CUI_FILL) {
        cui_text(ui,"CED",.size=16);
        if (ced_extension_has_tabs(m, false) && ced_extension_has_tabs(m, true)) {
            char sidebar[257] = "example.uppercase-selection.side";
            char terminal[257] = "example.uppercase-selection.term";
            cui_row(ui, .width = CUI_FILL, .height = CUI_FILL) {
                cui_column(ui, .width = CUI_PX(250), .height = CUI_FILL) {
                    cui_row(ui, .gap = 0) { ced_extension_tab_buttons(ui, m, false, sidebar); }
                    ced_extension_tab_body(ui, m, sidebar);
                }
                cui_column(ui, .width = CUI_FILL, .height = CUI_FILL) {
                    cui_row(ui, .gap = 0) { ced_extension_tab_buttons(ui, m, true, terminal); }
                    ced_extension_tab_body(ui, m, terminal);
                }
            }
        }
    }
    ced_extensions_ui(ui,m); cui_end_frame(ui);
}

static void render(cui_ctx *ui,cui_sw_renderer *sw,CedExtensions *m,const char *directory,const char *name) {
    for (int n = 0; n < 3; n++) draw_frame(ui, m);
    const cui_draw_data *data=cui_get_draw_data(ui); assert(data);
    unsigned char *pixels=calloc(1000*800,4); assert(pixels);
    cui_sw_render(sw,data,pixels,1000,800,4000,CUI_SW_RGBA);
    char path[CED_PATH]; assert(ced_join(path,sizeof path,directory,name)); assert(cui_write_png(path,pixels,1000,800,4000)); free(pixels);
}

static void append_chat_message(cJSON *messages, const char *role, const char *text) {
    cJSON *item = cJSON_CreateObject();
    assert(item && cJSON_AddStringToObject(item, "role", role) && cJSON_AddStringToObject(item, "text", text));
    assert(cJSON_AddItemToArray(messages, item));
}

static void test_chat_typewriter(cui_ctx *ui, cui_sw_renderer *sw, CedExtensions *m, const char *directory) {
    CedExtensionTabState *tab = &m->instances[0].tabs[1];
    cJSON *messages = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(tab->model, "chat"), "messages");
    const char *first = "**Preferences** are stored in `preferences.json`."
        "\n\nThe workspace retains its own settings so the editor can restore the same view next time."
        "\n\nOpen the settings file to review or update its values."
        "\n\nA new session will pick up any changes automatically.";
    char combined[1024];
    snprintf(combined, sizeof combined, "%s\n\n- First check the workspace.\n- Then inspect the global settings. 🙂", first);
    cui_push_id(ui, "example.uppercase-selection.term");
    const cui_scroll_state *scroll = cui_state(ui, cui_make_id(ui, "chat"), sizeof *scroll);
    cui_pop_id(ui);
    tab->chat_animate = true;
    append_chat_message(messages, "agent", first);
    cJSON *reply = cJSON_GetArrayItem(messages, cJSON_GetArraySize(messages) - 1);
    assert(cJSON_AddStringToObject(reply, "id", "1:3"));
    render(ui, sw, m, directory, "extension-typewriter.png");
    float first_height = scroll->content_h;
    assert(cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(reply, "text"), combined));
    draw_frame(ui, m);
    assert(scroll->content_h >= first_height);
    tab->chat_animate = false;
    for (int n = 0; n < 30; n++) draw_frame(ui, m);
    float complete_height = scroll->content_h;
    assert(complete_height > first_height);
    assert(cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(reply, "id"), "1:4"));
    draw_frame(ui, m);
    assert(scroll->content_h == complete_height);
    cJSON_DeleteItemFromArray(messages, cJSON_GetArraySize(messages) - 1);
    draw_frame(ui, m);
}

static void test_chat_controls(cui_ctx *ui, cui_sw_renderer *sw, CedExtensions *m, const char *directory) {
    CedExtensionInstance *instance = &m->instances[0];
    CedExtensionTabState *tab = &instance->tabs[1];
    cJSON *chat = cJSON_GetObjectItemCaseSensitive(tab->model, "chat");
    cJSON *messages = cJSON_GetObjectItemCaseSensitive(chat, "messages");
    for (int n = 0; n < 32; n++)
        append_chat_message(messages, n % 2 ? "agent" : "user", "A previous message in the conversation.");

    cui_push_id(ui, "example.uppercase-selection.term");
    cui_id scroll_id = cui_make_id(ui, "chat");
    cui_id jump_id = cui_make_id(ui, "chat-jump");
    cui_push_id(ui, "send");
    cui_id send_id = cui_make_id(ui, "tab-input") ^ 0x5E7Du;
    cui_pop_id(ui);
    cui_pop_id(ui);
    for (int n = 0; n < 15; n++) draw_frame(ui, m);
    const cui_scroll_state *scroll = cui_state(ui, scroll_id, sizeof *scroll);
    assert(tab->chat_following && scroll->y > 0 && scroll->content_h - scroll->view_h - scroll->y < 1);

    float previous = scroll->y;
    append_chat_message(messages, "agent", "New message while following.");
    for (int n = 0; n < 15; n++) draw_frame(ui, m);
    assert(tab->chat_following && scroll->y > previous && scroll->content_h - scroll->view_h - scroll->y < 1);

    cui_rect viewport = cui_get_rect(ui, scroll_id);
    cui_input_mouse_move(ui, viewport.x + 30, viewport.y + viewport.h / 2);
    cui_input_scroll(ui, 0, 5);
    for (int n = 0; n < 15; n++) draw_frame(ui, m);
    assert(!tab->chat_following && scroll->y < previous);
    previous = scroll->y;
    append_chat_message(messages, "user", "New message while reading older history.");
    for (int n = 0; n < 5; n++) draw_frame(ui, m);
    assert(!tab->chat_following && scroll->y == previous);
    cui_rect jump = cui_get_rect(ui, jump_id);
    assert(jump.w > 0 && jump.y + jump.h < viewport.y + viewport.h);

    cJSON *input = cJSON_GetObjectItemCaseSensitive(tab->model, "input");
    assert(cJSON_AddStringToObject(input, "stop", "cancel:1") && cJSON_AddBoolToObject(input, "disabled", true));
    cui_input_mouse_move(ui, jump.x + jump.w / 2, jump.y + jump.h / 2);
    render(ui, sw, m, directory, "extension-chat-controls.png");
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    draw_frame(ui, m);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    for (int n = 0; n < 15; n++) draw_frame(ui, m);
    assert(tab->chat_following && scroll->content_h - scroll->view_h - scroll->y < 1);

    cui_input_mouse_move(ui, viewport.x + 30, viewport.y + viewport.h / 2);
    cui_input_scroll(ui, 0, 4);
    draw_frame(ui, m);
    assert(!tab->chat_following);
    cui_input_scroll(ui, 0, -10);
    for (int n = 0; n < 15; n++) draw_frame(ui, m);
    assert(tab->chat_following && scroll->content_h - scroll->view_h - scroll->y < 1);

    // Refuse the request at the manager boundary after the native Stop is clicked.
    *m->error = 0;
    instance->manager = m;
    instance->state = EXT_FAILED;
    instance->record.manifest.commands[0].when = (CedCommandCondition){0};
    cui_rect send = cui_get_rect(ui, send_id);
    assert(send.w > 0);
    cui_input_mouse_move(ui, send.x + send.w / 2, send.y + send.h / 2);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    draw_frame(ui, m);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    draw_frame(ui, m);
    assert(!strcmp(m->error, "Restart the failed extension in Manage Extensions."));
    assert(!tab->submitted && !strcmp(tab->input, "First line\nSecond line"));
}

int main(int argc,char **argv) {
    assert(argc==3); CedExtensions *m=calloc(1,sizeof *m); assert(m);
    m->host.context=context; snprintf(m->workspace,sizeof m->workspace,"/example/workspace");
    char error[CED_ERROR]; assert(ced_extension_inspect(argv[1],&m->review,error));
    cui_sw_renderer *sw=cui_sw_create(); assert(sw);
    cui_ctx *ui=cui_create(&(cui_ctx_desc){.renderer=cui_sw_iface(sw),.theme=CUI_THEME_DARK}); assert(ui);
    cui_set_viewport(ui,1000,800,1); m->review_open=true; render(ui,sw,m,argv[2],"extension-install.png");
    m->review_open=false; m->manage_open=true;
    assert(ced_extension_inspect(argv[1],&m->instances[0].record,error)); m->count=1; m->instances[0].record.enabled=true;
    render(ui,sw,m,argv[2],"extension-manager.png");
    m->trust_open=true; render(ui,sw,m,argv[2],"extension-trust.png");
    cui_destroy(ui);
    ui = cui_create(&(cui_ctx_desc){.renderer=cui_sw_iface(sw),.theme=CUI_THEME_DARK});
    assert(ui); cui_set_viewport(ui,1000,800,1);
    m->manage_open = m->trust_open = false;
    m->trusted = true;
    CedExtensionInstance *instance = &m->instances[0];
    instance->record.manifest.tab_count = 2;
    instance->record.manifest.tabs[0] = (CedExtensionTab){.id = "example.uppercase-selection.side", .title = "Tools"};
    instance->record.manifest.tabs[1] = (CedExtensionTab){.id = "example.uppercase-selection.term", .title = "Agents", .terminal = true};
    instance->tabs[0].model = cJSON_Parse(
        "{\"heading\":\"Agents\",\"text\":\"\","
        "\"selectors\":[{\"id\":\"harness\",\"label\":\"Harness\",\"selected\":\"codex\","
        "\"options\":[{\"id\":\"codex\",\"label\":\"Codex\"},{\"id\":\"claude\",\"label\":\"Claude Code\"},{\"id\":\"devin\",\"label\":\"Devin\"}]}],"
        "\"actions\":[{\"id\":\"session:1\",\"label\":\"Review editor settings\"},{\"id\":\"new:codex\",\"label\":\"New Session\"}]}");
    instance->tabs[1].model = cJSON_Parse(
        "{\"heading\":\"Review editor settings · Codex\",\"text\":\"\","
        "\"actions\":[],"
        "\"selectors\":["
        "{\"id\":\"config:model\",\"label\":\"Model\",\"placement\":\"composer\",\"selected\":\"model-1\",\"options\":[{\"id\":\"model-1\",\"label\":\"GPT-5.4\"},{\"id\":\"model-2\",\"label\":\"GPT-5.4 Mini\"}]},"
        "{\"id\":\"config:effort\",\"label\":\"Effort\",\"placement\":\"composer\",\"selected\":\"high\",\"options\":["
        "{\"id\":\"low\",\"label\":\"Low\"},{\"id\":\"medium\",\"label\":\"Medium\"},{\"id\":\"high\",\"label\":\"High\"}]}],"
        "\"chat\":{\"author\":\"Codex\",\"back\":\"back\",\"messages\":["
        "{\"role\":\"user\",\"text\":\"Where are the editor settings stored?\"},"
        "{\"role\":\"reasoning\",\"id\":\"1:1\",\"text\":\"**Check how preferences are loaded**\\n\\n"
        "- Inspect `preferences.json`.\\n- Confirm the *default path*.\"},"
        "{\"role\":\"tool\",\"id\":\"1:2\",\"text\":\"Read editor settings\",\"status\":\"done\","
        "\"input\":\"{\\\"path\\\":\\\"preferences.json\\\"}\",\"output\":\"Loaded editor preferences.\"},"
        "{\"role\":\"agent\",\"text\":\"CED loads preferences from `~/.ced/preferences.json`.\"}]},"
        "\"input\":{\"id\":\"send\",\"label\":\"Send\",\"placeholder\":\"Message\"}}");
    for (size_t n = 0; n < 2; n++) {
        instance->tabs[n].requested = true;
        assert(instance->tabs[n].model);
    }
    assert(ced_extension_tab_find(m, "example.uppercase-selection.side", NULL, NULL));
    assert(ced_extension_tab_find(m, "example.uppercase-selection.term", NULL, NULL));
    render(ui,sw,m,argv[2],"extension-tabs.png");
    cui_push_id(ui, "example.uppercase-selection.term");
    cui_rect back = cui_get_rect(ui, cui_make_id(ui, "chat-back"));
    const char *heading = ced_ext_json_string(instance->tabs[1].model, "heading");
    size_t heading_length = strlen(heading);
    cui_rect title = {0};
    for (int n = 0; n < ui->node_count; n++) {
        const cui_node *node = &ui->nodes[n];
        if (node->text && node->text_len == (int)heading_length && !memcmp(node->text, heading, heading_length))
            title = node->rect;
    }
    assert(back.w > 0 && title.x >= back.x + back.w);
    cui_push_id(ui, "chat-activity");
    cui_id reasoning_id = cui_make_id(ui, "1:1");
    cui_id tool_id = cui_make_id(ui, "1:2");
    cui_push_id(ui, "1:1");
    cui_id reasoning_markdown = cui_make_id(ui, "markdown");
    cui_pop_id(ui);
    cui_pop_id(ui);
    cui_push_id(ui, "send");
    cui_rect composer = cui_get_rect(ui, cui_make_id(ui, "tab-input"));
    cui_push_id(ui, "tab-input");
    cui_rect model_picker = cui_get_rect(ui, cui_make_id(ui, "model"));
    cui_push_id(ui, "secondary-picker");
    cui_id effort_id = cui_make_id(ui, "config:effort");
    cui_rect effort_picker = cui_get_rect(ui, effort_id);
    /* The native picker scopes menu items by popup ID and row index. */
    cui_push_id_int(ui, (int)(effort_id ^ 0x30DEu));
    cui_push_id_int(ui, 1);
    cui_id medium_effort = cui_make_id(ui, "Medium");
    cui_pop_id(ui);
    cui_pop_id(ui);
    cui_pop_id(ui);
    cui_pop_id(ui);
    cui_pop_id(ui);
    cui_pop_id(ui);
    assert(composer.w > 0 && composer.h > 0);
    assert(model_picker.w > 0 && model_picker.y > composer.y);
    assert(model_picker.x == composer.x + 48);
    assert(effort_picker.w > 0 && effort_picker.x == model_picker.x + model_picker.w + 6);
    assert(effort_picker.y + effort_picker.h <= composer.y + composer.h);
    assert(effort_picker.h == model_picker.h && effort_picker.y == model_picker.y);
    cui_input_mouse_move(ui, effort_picker.x + effort_picker.w / 2, effort_picker.y + effort_picker.h / 2);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    draw_frame(ui, m);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    render(ui,sw,m,argv[2],"extension-effort-picker.png");
    cui_rect effort_option = cui_get_rect(ui, medium_effort);
    assert(effort_option.w > 0 && effort_option.y < effort_picker.y);
    cui_input_key(ui, CUI_KEY_ESCAPE, true, 0);
    draw_frame(ui, m);
    cui_input_key(ui, CUI_KEY_ESCAPE, false, 0);
    for (int n = 0; n < 8; n++) draw_frame(ui, m);
    cui_rect reasoning = cui_get_rect(ui, reasoning_id);
    cui_rect tool = cui_get_rect(ui, tool_id);
    assert(reasoning.w > 0 && tool.y > reasoning.y && tool.w > 0);
    cui_input_mouse_move(ui, reasoning.x + 10, reasoning.y + reasoning.h / 2);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    draw_frame(ui, m);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    for (int n = 0; n < 8; n++) draw_frame(ui, m);
    cui_rect expanded = cui_get_rect(ui, tool_id);
    assert(expanded.y > tool.y);
    cui_rect markdown = cui_get_rect(ui, reasoning_markdown);
    assert(markdown.w > 0 && markdown.y > reasoning.y && markdown.y + markdown.h <= expanded.y);
    cui_input_mouse_move(ui, expanded.x + 10, expanded.y + expanded.h / 2);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    draw_frame(ui, m);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    render(ui,sw,m,argv[2],"extension-activity.png");
    cui_input_mouse_move(ui, composer.x + 24, composer.y + 22);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    draw_frame(ui, m);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    draw_frame(ui, m);
    cui_input_text(ui, "First line");
    draw_frame(ui, m);
    cui_input_key(ui, CUI_KEY_ENTER, true, CUI_MOD_SHIFT);
    draw_frame(ui, m);
    cui_input_key(ui, CUI_KEY_ENTER, false, 0);
    cui_input_text(ui, "Second line");
    draw_frame(ui, m);
    assert(!strcmp(instance->tabs[1].input, "First line\nSecond line"));
    cJSON *model_selector = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(instance->tabs[1].model, "selectors"), 0);
    cJSON *options = cJSON_GetObjectItemCaseSensitive(model_selector, "options");
    for (int n = 3; n <= 9; n++) {
        char name[20];
        snprintf(name, sizeof name, "Model %d", n);
        cJSON *item = cJSON_CreateObject();
        assert(item && cJSON_AddStringToObject(item, "id", name) && cJSON_AddStringToObject(item, "label", name));
        assert(cJSON_AddItemToArray(options, item));
    }
    draw_frame(ui, m);
    draw_frame(ui, m);
    cui_push_id(ui, "example.uppercase-selection.term");
    cui_push_id(ui, "send");
    cui_id browse_state = cui_make_id(ui, "browse-models");
    cui_push_id(ui, "tab-input");
    cui_id native_picker = cui_make_id(ui, "model");
    model_picker = cui_get_rect(ui, native_picker);
    /* The pinned picker scopes its menu items by the popup ID and row index. */
    cui_push_id_int(ui, (int)(native_picker ^ 0x30DEu));
    cui_push_id_int(ui, 1);
    cui_id browse_item = cui_make_id(ui, "Browse all models…");
    for (int n = 0; n < 5; n++) cui_pop_id(ui);
    cui_input_mouse_move(ui, model_picker.x + model_picker.w / 2, model_picker.y + model_picker.h / 2);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    draw_frame(ui, m);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    for (int n = 0; n < 3; n++) draw_frame(ui, m);
    cui_rect browse = cui_get_rect(ui, browse_item);
    assert(browse.w > 0 && browse.y < model_picker.y);
    cui_input_mouse_move(ui, browse.x + browse.w / 2, browse.y + browse.h / 2);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    draw_frame(ui, m);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    draw_frame(ui, m);
    assert(*(bool *)cui_state(ui, browse_state, sizeof(bool)));
    cui_input_text(ui, "Model 9");
    draw_frame(ui, m);
    assert(!strcmp(instance->tabs[1].input, "First line\nSecond line"));
    cui_input_key(ui, CUI_KEY_ESCAPE, true, 0);
    draw_frame(ui, m);
    cui_input_key(ui, CUI_KEY_ESCAPE, false, 0);
    for (int n = 0; n < 8; n++) draw_frame(ui, m);
    test_chat_typewriter(ui, sw, m, argv[2]);
    test_chat_controls(ui, sw, m, argv[2]);
    draw_frame(ui, m);
    *m->error = 0;
    cui_input_mouse_move(ui, back.x + back.w / 2, back.y + back.h / 2);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    draw_frame(ui, m);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    draw_frame(ui, m);
    assert(!strcmp(m->error, "Restart the failed extension in Manage Extensions."));
    for (size_t n = 0; n < 2; n++) cJSON_Delete(instance->tabs[n].model);
    cui_destroy(ui); cui_sw_destroy(sw); ced_extension_record_free(&m->review); ced_extension_record_free(&m->instances[0].record); free(m);
    puts("OK: install review, manager, workspace trust and sidebar/terminal tabs rendered without a display"); return 0;
}
