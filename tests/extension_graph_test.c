#include "extensions/graph_ui.h"
#include "cui_internal.h"
#include <cui/cui_render.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { WIDTH = 960, HEIGHT = 650 };

static cJSON *frame(cui_ctx *ui, CedExtensionGraphView *view, const CedExtensionPreview *preview, bool editable) {
    cui_begin_frame(ui, .016f);
    cJSON *edit = ced_extension_graph_body(ui, view, preview, editable, false);
    cui_end_frame(ui);
    return edit;
}

static cui_vec2 point(cui_ctx *ui, float x, float y) {
    cui_rect r = cui_get_rect(ui, cui_make_id(ui, "extension-graph"));
    const float h = CED_GRAPH_HEIGHT - CED_GRAPH_TOP;
    float z = r.w / CED_GRAPH_WIDTH < r.h / h ? r.w / CED_GRAPH_WIDTH : r.h / h;
    assert(z > 0);
    return (cui_vec2){r.x + (r.w - CED_GRAPH_WIDTH*z)*.5f + x*z,
                      r.y + (r.h - h*z)*.5f + (y-CED_GRAPH_TOP)*z};
}

static void move(cui_ctx *ui, float x, float y) {
    cui_vec2 p = point(ui, x, y);
    cui_input_mouse_move(ui, p.x, p.y);
}

static void click_zoom(cui_ctx *ui, CedExtensionGraphView *view, const CedExtensionPreview *preview,
                       const char *label, const char *icon) {
    cui_id id = cui__hash_str(icon, cui_make_id(ui, label));
    cui_rect button = cui_get_rect(ui, id);
    assert(button.w > 0 && button.h > 0);
    cui_input_mouse_move(ui, button.x + button.w * .5f, button.y + button.h * .5f);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    assert(!frame(ui, view, preview, true));
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    assert(!frame(ui, view, preview, true));
}

static cJSON *click_at(cui_ctx *ui, CedExtensionGraphView *view, const CedExtensionPreview *preview,
                       cui_vec2 point) {
    cui_input_mouse_move(ui, point.x, point.y);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    assert(!frame(ui, view, preview, true));
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    return frame(ui, view, preview, true);
}

static void settle_ui(cui_ctx *ui, CedExtensionGraphView *view, const CedExtensionPreview *preview) {
    for (int i = 0; i < 24; i++) assert(!frame(ui, view, preview, true));
}

static void open_menu(cui_ctx *ui, CedExtensionGraphView *view, const CedExtensionPreview *preview, float x, float y) {
    move(ui, x, y);
    cui_input_mouse_button(ui, CUI_MOUSE_RIGHT, true);
    assert(!frame(ui, view, preview, true));
    cui_input_mouse_button(ui, CUI_MOUSE_RIGHT, false);
    settle_ui(ui, view, preview);
}

static cui_rect item_rect(cui_ctx *ui, cui_id scope, const char *label) {
    cui_push_id_int(ui, (int)scope);
    cui_rect r = cui_get_rect(ui, cui_make_id(ui, label));
    cui_pop_id(ui);
    return r;
}

static cJSON *click_item(cui_ctx *ui, CedExtensionGraphView *view, const CedExtensionPreview *preview,
                        cui_id scope, const char *label) {
    cui_rect r = item_rect(ui, scope, label);
    assert(r.w > 0 && r.h > 0);
    return click_at(ui, view, preview, (cui_vec2){r.x + r.w * .5f, r.y + r.h * .5f});
}

static void test_menu(cui_ctx *ui, CedExtensionGraphView *view, const CedExtensionPreview *preview) {
    assert(!click_at(ui, view, preview, point(ui, 150, 170)));
    assert(view->selected_nodes[0]);
    cui_input_key(ui, CUI_KEY_NONE, false, CUI_MOD_SHIFT);
    assert(!click_at(ui, view, preview, point(ui, 560, 285)));
    assert(view->selected_nodes[0] && view->selected_nodes[1]);
    assert(!click_at(ui, view, preview, point(ui, 360, 227.5f)));
    assert(view->selected_links[0] && view->selected_nodes[0] && view->selected_nodes[1]);
    cui_input_key(ui, CUI_KEY_NONE, false, 0);

    cui_input_mouse_leave(ui);
    cui_input_key(ui, CUI_KEY_DELETE, true, 0);
    cJSON *edit = frame(ui, view, preview, true);
    cui_input_key(ui, CUI_KEY_DELETE, false, 0);
    assert(edit && !strcmp(ced_ext_json_string(edit, "type"), "removeElements"));
    assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(edit, "nodes")) == 2);
    assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(edit, "links")) == 1);
    cJSON_Delete(edit);

    cui_id menu = cui_make_id(ui, "graph-context") ^ 0xC7Cu;
    open_menu(ui, view, preview, 360, 227.5f);
    assert(view->menu_link == 0 && view->menu_node < 0);
    edit = click_item(ui, view, preview, menu, "Remove Link");
    assert(edit && !strcmp(ced_ext_json_string(edit, "type"), "removeElements"));
    assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(edit, "nodes")) == 0);
    assert(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(edit, "links"), 0)->valueint == 0);
    cJSON_Delete(edit);
    settle_ui(ui, view, preview);

    open_menu(ui, view, preview, 350, 420);
    assert(view->menu_node < 0);
    cui_rect add = item_rect(ui, menu, "Add Node");
    assert(add.w > 0 && add.w < 300);
    edit = click_item(ui, view, preview, menu, "Remove Selected Nodes");
    assert(edit && !strcmp(ced_ext_json_string(edit, "type"), "removeElements"));
    assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(edit, "nodes")) == 2);
    cJSON_Delete(edit);
    settle_ui(ui, view, preview);

    open_menu(ui, view, preview, 350, 420);
    edit = click_item(ui, view, preview, menu, "Add Node");
    assert(edit && !strcmp(ced_ext_json_string(edit, "type"), "addNode"));
    assert(cJSON_GetObjectItemCaseSensitive(edit, "x")->valueint == 350);
    assert(cJSON_GetObjectItemCaseSensitive(edit, "y")->valueint == 420);
    cJSON_Delete(edit);
    settle_ui(ui, view, preview);

    open_menu(ui, view, preview, 150, 170);
    assert(view->menu_node == 0 && item_rect(ui, menu, "Remove Node").w > 0);
    assert(!click_item(ui, view, preview, menu, "Edit Node…"));
    settle_ui(ui, view, preview);
    assert(view->editing && !strcmp(view->label, "A"));
    memcpy(view->label, "Renamed", sizeof "Renamed");
    view->node_width = 160;
    cui_id dialog = cui_make_id(ui, "Edit Node") ^ 0xD1A106u;
    edit = click_item(ui, view, preview, dialog, "Save");
    assert(edit && !strcmp(ced_ext_json_string(edit, "type"), "editNode"));
    assert(!strcmp(ced_ext_json_string(edit, "label"), "Renamed"));
    assert(cJSON_GetObjectItemCaseSensitive(edit, "width")->valueint == 160);
    cJSON_Delete(edit);
    settle_ui(ui, view, preview);

    open_menu(ui, view, preview, 150, 170);
    edit = click_item(ui, view, preview, menu, "Remove Node");
    assert(edit && !strcmp(ced_ext_json_string(edit, "type"), "removeElements"));
    assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(edit, "nodes")) == 1);
    cJSON_Delete(edit);
    settle_ui(ui, view, preview);
}

static void test_scroll(cui_ctx *ui, CedExtensionGraphView *view, const CedExtensionPreview *preview) {
    move(ui, 420, 270);
    cui_input_scroll(ui, 0, -1);
    assert(!frame(ui, view, preview, true));
    assert(view->pan.x == 0 && view->pan.y == 40 && view->zoom == 1);

    cui_input_key(ui, CUI_KEY_NONE, false, CUI_MOD_SHIFT);
    cui_input_scroll(ui, 0, -1);
    assert(!frame(ui, view, preview, true));
    assert(view->pan.x == 40 && view->pan.y == 40 && view->zoom == 1);

    cui_input_key(ui, CUI_KEY_NONE, false, CUI_MOD_PRIMARY);
    cui_input_scroll(ui, 0, 1);
    assert(!frame(ui, view, preview, true));
    assert(fabsf(view->zoom - 1.12f) < .001f);
    /* Before zooming, the pointer at (420, 270) is over world (460, 310). */
    assert(fabsf((460 - 480 - view->pan.x) * view->zoom + 480 - 420) < .001f);
    assert(fabsf((310 - 352 - view->pan.y) * view->zoom + 352 - 270) < .001f);
    cui_input_key(ui, CUI_KEY_NONE, false, 0);
    view->pan = (cui_vec2){0, 0};
    view->zoom = 1;
    assert(!frame(ui, view, preview, true));
}

static void test_resize(cui_ctx *ui, CedExtensionGraphView *view, const CedExtensionPreview *preview,
                        float x, float y, float dx, float dy, int left, int top, int width, int height) {
    move(ui, x, y);
    assert(!frame(ui, view, preview, true));
    assert(ui->cursor == CUI_CURSOR_RESIZE_EW);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    assert(!frame(ui, view, preview, true));
    move(ui, x + dx, y + dy);
    assert(!frame(ui, view, preview, true));
    assert(view->drag_x == left && view->drag_y == top && view->drag_w == width && view->drag_h == height);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    cJSON *edit = frame(ui, view, preview, true);
    assert(edit && !strcmp(ced_ext_json_string(edit, "type"), "resizeNode"));
    assert(cJSON_GetObjectItemCaseSensitive(edit, "x")->valueint == left);
    assert(cJSON_GetObjectItemCaseSensitive(edit, "y")->valueint == top);
    assert(cJSON_GetObjectItemCaseSensitive(edit, "width")->valueint == width);
    assert(cJSON_GetObjectItemCaseSensitive(edit, "height")->valueint == height);
    cJSON_Delete(edit);
}

static void test_refresh(void) {
    CedExtensions *manager = calloc(1, sizeof *manager);
    assert(manager);
    manager->trusted = true;
    manager->job.running = true; /* Hold the renderer's reply while drawing the prior graph. */
    CedExtensionInstance *instance = &manager->instances[0];
    instance->manager = manager;
    instance->record.granted = CED_EXT_READ | CED_EXT_EDIT;
    instance->document_grants[0] = 1;
    instance->grant_count = 1;
    CedExtensionRenderer renderer = {0};
    CedDocument document = {.id = 1, .version = 1};
    const CedExtensionPreview *preview = ced_extensions_preview(instance, &renderer, &document);
    CedExtensionGraph *ready = calloc(1, sizeof *ready);
    assert(ready);
    manager->preview.graph = ready;
    manager->preview.graph_version = document.version;
    manager->preview.serial = 1;

    uint64_t key = preview->key;
    document.version++;
    ced_extensions_document_changed(manager, &document, 1);
    preview = ced_extensions_preview(instance, &renderer, &document);
    assert(preview->graph == ready && preview->graph_version == 1 && preview->serial == 1);
    assert(preview->version == document.version && preview->key != key);
    cJSON *edit = cJSON_CreateObject();
    assert(edit && !ced_extensions_graph_edit(instance, &renderer, &document, edit));
    cJSON_Delete(edit);

    instance->grant_count = 0;
    assert(!ced_extensions_preview(instance, &renderer, &document)->graph);
    ced_extensions_preview_clear(manager);
    free(manager);
}

int main(void) {
    test_refresh();
    const char *json = "{\"nodes\":[{\"label\":\"A\",\"x\":100,\"y\":140,\"width\":120,\"height\":60},"
        "{\"label\":\"B\",\"x\":500,\"y\":250,\"width\":140,\"height\":70}],"
        "\"links\":[{\"from\":{\"node\":0,\"side\":\"right\",\"offset\":0.5},"
        "\"to\":{\"node\":1,\"side\":\"left\",\"offset\":0.5},\"style\":\"dashed\",\"direction\":\"both\"}]}";
    cJSON *body = cJSON_Parse(json);
    char error[CED_ERROR] = {0};
    CedExtensionGraph *graph = ced_extension_graph_decode(body, error);
    assert(graph && graph->node_count == 2 && graph->link_count == 1);
    cJSON *end = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(body, "links"), 0), "to");
    cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(end, "node"), 2);
    assert(!ced_extension_graph_decode(body, error));
    cJSON_Delete(body);

    CedExtensionPreview preview = {.key = 1, .graph = graph};
    CedExtensionGraphView view = {0};
    cui_sw_renderer *sw = cui_sw_create();
    cui_ctx *ui = cui_create(&(cui_ctx_desc){.renderer = cui_sw_iface(sw)});
    assert(sw && ui);
    cui_set_viewport(ui, WIDTH, HEIGHT, 1);
    cui_input_mouse_leave(ui);
    assert(!frame(ui, &view, &preview, true));
    assert(!frame(ui, &view, &preview, true));
    unsigned char *pixels = malloc(WIDTH * HEIGHT * 4); assert(pixels);
    cui_vec2 edge = point(ui, 158, 140);
    size_t pixel = ((size_t)(int)edge.y * WIDTH + (int)edge.x) * 4;
    cui_sw_render(sw, cui_get_draw_data(ui), pixels, WIDTH, HEIGHT, WIDTH*4, CUI_SW_RGBA);
    cui_rect canvas = cui_get_rect(ui, cui_make_id(ui, "extension-graph"));
    assert(canvas.w == WIDTH && canvas.y + canvas.h == HEIGHT);
    float old_top = canvas.y + (canvas.h - (CED_GRAPH_HEIGHT - CED_GRAPH_TOP)) * .5f;
    assert(old_top > canvas.y + 5);
    size_t margin = ((size_t)((int)canvas.y + 4) * WIDTH + 4) * 4;
    cui_color background = cui_color_of(ui, CUI_COLOR_SURFACE_MUTED);
    assert(!memcmp(pixels + margin, &background, sizeof background));

    click_zoom(ui, &view, &preview, "Zoom in", CUI_ICON_ZOOM_IN);
    assert(view.zoom == 1.25f);
    preview.key++;
    assert(!frame(ui, &view, &preview, true));
    assert(view.zoom == 1.25f);
    click_zoom(ui, &view, &preview, "Zoom out", CUI_ICON_ZOOM_OUT);
    assert(view.zoom == 1);
    cui_input_mouse_leave(ui);
    assert(!frame(ui, &view, &preview, true));
    cui_sw_render(sw, cui_get_draw_data(ui), pixels, WIDTH, HEIGHT, WIDTH*4, CUI_SW_RGBA);
    unsigned char resting[4];
    memcpy(resting, pixels + pixel, sizeof resting);

    move(ui, 150, 170);
    assert(!frame(ui, &view, &preview, true));
    cui_sw_render(sw, cui_get_draw_data(ui), pixels, WIDTH, HEIGHT, WIDTH*4, CUI_SW_RGBA);
    assert(memcmp(resting, pixels + pixel, sizeof resting));
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    assert(!frame(ui, &view, &preview, true));
    move(ui, 222, 220);
    assert(!frame(ui, &view, &preview, true));
    assert(view.pressed && view.moved && view.drag_x == 172 && view.drag_y == 190);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    cJSON *edit = frame(ui, &view, &preview, true);
    assert(edit && !strcmp(ced_ext_json_string(edit, "type"), "moveNode"));
    assert(cJSON_GetObjectItemCaseSensitive(edit, "node")->valueint == 0);
    assert(cJSON_GetObjectItemCaseSensitive(edit, "x")->valueint == 172);
    assert(cJSON_GetObjectItemCaseSensitive(edit, "y")->valueint == 190);
    cJSON_Delete(edit);

    move(ui, 130, 140);
    assert(!frame(ui, &view, &preview, true));
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    assert(!frame(ui, &view, &preview, true));
    move(ui, 605, 250);
    assert(!frame(ui, &view, &preview, true));
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    edit = frame(ui, &view, &preview, true);
    assert(edit && !strcmp(ced_ext_json_string(edit, "type"), "addLink"));
    cJSON *link = cJSON_GetObjectItemCaseSensitive(edit, "link");
    assert(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(link, "from"), "offset")->valuedouble == .25);
    assert(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(link, "to"), "offset")->valuedouble == .75);
    cJSON_Delete(edit);

    move(ui, 150, 170);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    assert(!frame(ui, &view, &preview, true));
    preview.key++;
    move(ui, 222, 220);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    assert(!frame(ui, &view, &preview, true));

    test_menu(ui, &view, &preview);
    test_scroll(ui, &view, &preview);
    test_resize(ui, &view, &preview, 100, 140, -30, -20, 70, 120, 150, 80);
    test_resize(ui, &view, &preview, 220, 140, 30, -20, 100, 120, 150, 80);
    test_resize(ui, &view, &preview, 100, 200, -30, 20, 70, 140, 150, 80);
    test_resize(ui, &view, &preview, 220, 200, 30, 20, 100, 140, 150, 80);

    move(ui, 150, 170);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    assert(!frame(ui, &view, &preview, true));
    move(ui, 150, 76);
    assert(!frame(ui, &view, &preview, true));
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    edit = frame(ui, &view, &preview, true);
    assert(edit && cJSON_GetObjectItemCaseSensitive(edit, "y")->valueint == 46);
    cJSON_Delete(edit);

    move(ui, 400, 400);
    cui_input_mouse_button(ui, CUI_MOUSE_MIDDLE, true);
    assert(!frame(ui, &view, &preview, true));
    move(ui, 450, 430);
    assert(!frame(ui, &view, &preview, true));
    cui_input_mouse_button(ui, CUI_MOUSE_MIDDLE, false);
    assert(!frame(ui, &view, &preview, true));
    assert(view.pan.x == -50 && view.pan.y == -30);

    graph->nodes[1].x = CED_GRAPH_MAX_COORDINATE;
    assert(!frame(ui, &view, &preview, true));
    free(pixels);
    cui_destroy(ui);
    cui_sw_destroy(sw);
    free(graph);
    puts("OK: native graph scroll, resize, selection and removal");
}
