#include "extensions/extension_ui.h"
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
        "\"actions\":[{\"id\":\"back\",\"label\":\"Sessions\"}],"
        "\"selectors\":["
        "{\"id\":\"config:model\",\"label\":\"Model\",\"placement\":\"composer\",\"selected\":\"model-1\",\"options\":[{\"id\":\"model-1\",\"label\":\"GPT-5.4\"},{\"id\":\"model-2\",\"label\":\"GPT-5.4 Mini\"}]},"
        "{\"id\":\"config:effort\",\"label\":\"Effort\",\"placement\":\"composer\",\"selected\":\"high\",\"options\":[{\"id\":\"high\",\"label\":\"High\"}]}],"
        "\"chat\":{\"author\":\"Codex\",\"messages\":["
        "{\"role\":\"user\",\"text\":\"Where are the editor settings stored?\"},"
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
    cui_push_id(ui, "send");
    cui_rect composer = cui_get_rect(ui, cui_make_id(ui, "tab-input"));
    cui_rect effort_picker = cui_get_rect(ui, cui_make_id(ui, "config:effort"));
    cui_push_id(ui, "tab-input");
    cui_rect model_picker = cui_get_rect(ui, cui_make_id(ui, "model"));
    cui_pop_id(ui);
    cui_pop_id(ui);
    cui_pop_id(ui);
    assert(composer.w > 0 && composer.h > 0);
    assert(model_picker.w > 0 && model_picker.y > composer.y);
    assert(effort_picker.w > 0 && effort_picker.x >= model_picker.x + model_picker.w);
    assert(effort_picker.y + effort_picker.h <= composer.y + composer.h);
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
    for (size_t n = 0; n < 2; n++) cJSON_Delete(instance->tabs[n].model);
    cui_destroy(ui); cui_sw_destroy(sw); ced_extension_record_free(&m->review); ced_extension_record_free(&m->instances[0].record); free(m);
    puts("OK: install review, manager, workspace trust and sidebar/terminal tabs rendered without a display"); return 0;
}
