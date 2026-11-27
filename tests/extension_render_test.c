#include "extension_temp.h"
#include "extensions/extension_ui.h"
#include "file_operations.h"
#include <cui/cui_render.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static CedCommandContext context(void *user) { (void)user; return (CedCommandContext){0}; }

static bool document(void *user, size_t index, CedExtensionDocument *out) {
    if (index) return false;
    *out = (CedExtensionDocument){.document = user, .language = "text"};
    return true;
}

static void settle(CedExtensions *manager) {
    double deadline = ced_ext_now() + 8;
    for (;;) {
        ced_extensions_tick(manager);
        bool busy = manager->job.running || manager->reload;
        for (size_t i = 0; i < manager->count; i++) busy |= *manager->instances[i].pending != 0;
        if (!busy) break;
        assert(ced_ext_now() < deadline);
        struct timespec delay = {.tv_nsec = 5000000};
        nanosleep(&delay, NULL);
    }
}

static void copy(const char *from, const char *to) {
    FILE *in = fopen(from, "rb"), *out = fopen(to, "wb");
    assert(in && out);
    char buffer[8192];
    size_t length;
    while ((length = fread(buffer, 1, sizeof buffer, in)))
        assert(fwrite(buffer, 1, length, out) == length);
    assert(!ferror(in));
    fclose(in);
    assert(!fclose(out));
#ifndef _WIN32
    assert(!chmod(to, 0755));
#endif
}

static void test_decode(void) {
    CedExtensionImage image = {0};
    char error[CED_ERROR];
    const char *cases[] = {
        "{\"width\":1,\"height\":1,\"rgba\":\"AQID/w==\"}",
        "{\"width\":1,\"height\":1,\"rgba\":\"AQID/x==\"}",
        "{\"width\":1,\"height\":1,\"rgba\":\"AQID\"}",
        "{\"width\":1,\"height\":1025,\"rgba\":\"AQID/w==\"}",
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        cJSON *value = cJSON_Parse(cases[i]);
        assert(value && ced_extension_image_decode(value, &image, error) == (i == 0));
        if (!i) assert(image.width == 1 && image.height == 1 && image.pixels[3] == 255);
        ced_extension_image_free(&image);
        cJSON_Delete(value);
    }
    assert(ced_rpc_memory_used() == 0);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    test_decode();
    char root[CED_PATH], parent[CED_PATH], package[CED_PATH], profile[CED_PATH], path[CED_PATH], error[CED_ERROR];
    extension_temp("ced-asln-render-XXXXXX", root, parent);
    assert(ced_join(package, sizeof package, root, "package") && !mkdir(package, 0700));
    assert(ced_join(profile, sizeof profile, root, "profile"));
#ifdef _WIN32
    const char *executable = "fixture.exe";
#else
    const char *executable = "fixture";
#endif
    assert(ced_join(path, sizeof path, package, executable));
    copy(argv[1], path);
    assert(ced_join(path, sizeof path, package, "ced-extension.json"));
    FILE *file = fopen(path, "wb"); assert(file);
    fprintf(file, "{\"manifestVersion\":1,\"id\":\"fixture.renderer\",\"name\":\"Diagram fixture\",\"version\":\"1.0.0\","
        "\"ced\":{\"minVersion\":\"0.2.0\",\"apiMajor\":1},\"kind\":\"executable\",\"entrypoints\":{\"%s\":\"%s\"},"
        "\"untrustedWorkspaces\":false,\"capabilities\":[\"documents.read\"],\"activationEvents\":[\"onCommand:fixture.renderer.draw\"],"
        "\"contributes\":{\"commands\":[{\"id\":\"fixture.renderer.draw\",\"title\":\"Draw Diagram\"}],"
        "\"documentRenderers\":[{\"extension\":\".asln\",\"title\":\"Diagram\",\"command\":\"fixture.renderer.draw\"}]}}",
        ced_extension_platform(), executable);
    assert(!fclose(file));
    CedExtensionRecord approved = {0};
    assert(ced_extension_inspect(package, &approved, error));
    assert(ced_extension_install(profile, &approved, error));
    ced_extension_record_free(&approved);

    assert(ced_join(path, sizeof path, root, "workflow.ASLN"));
    file = fopen(path, "wb"); assert(file);
    fputs("{\"steps\":[\"start\"]}", file); assert(!fclose(file));
    CedDocument doc = {0};
    assert(ced_document_open(&doc, path, error));
    ced_document_identify(&doc, 1);
    CedCommands commands = {0};
    CedExtensions *manager = ced_extensions_create(&commands, (CedExtensionHost){
        .user = &doc, .context = context, .document = document}, profile, root);
    assert(manager);
    settle(manager);
    CedExtensionInstance *owner;
    const CedExtensionRenderer *renderer = ced_extensions_renderer(manager, doc.path, &owner);
    assert(renderer && owner && !strcmp(renderer->title, "Diagram"));
    const CedExtensionPreview *preview = ced_extensions_preview(owner, renderer, &doc);
    assert(!preview->image.pixels && !owner->process);

    assert(ced_extensions_job(manager, EXT_TRUST, NULL));
    settle(manager);
    renderer = ced_extensions_renderer(manager, doc.path, &owner);
    assert(renderer && manager->trusted);
    strcpy(doc.text, "{\"steps\":[\"start\",\"finish\"]}");
    assert(ced_document_note_change(&doc));
    preview = ced_extensions_preview(owner, renderer, &doc);
    settle(manager);
    if (*preview->error) fprintf(stderr, "%s\n", preview->error);
    assert(preview->image.pixels && preview->image.width == 256 && preview->image.height == 96 && preview->image.interactive);
    size_t pixel = (32 * (size_t)preview->image.width + 138) * 4;
    size_t between = (32 * (size_t)preview->image.width + 128) * 4;
    assert(preview->image.pixels[pixel] == 52 && preview->image.pixels[between] == 18); /* Two steps in unsaved buffer. */

    cui_sw_renderer *software = cui_sw_create();
    cui_ctx *ui = cui_create(&(cui_ctx_desc){.renderer = cui_sw_iface(software)});
    assert(software && ui);
    cui_set_viewport(ui, 320, 240, 1);
    CedExtensionPreviewView view = {0};
    for (int frame = 0; frame < 2; frame++) {
        cui_begin_frame(ui, .016f);
        ced_extension_preview_body(ui, &view, owner, renderer, &doc);
        cui_end_frame(ui);
    }
    assert(view.texture);
    const cui_draw_data *draw = cui_get_draw_data(ui);
    assert(draw);
    unsigned char framebuffer[320 * 240 * 4];
    cui_sw_render(software, draw, framebuffer, 320, 240, 320 * 4, CUI_SW_RGBA);
    assert(framebuffer[(100 * 320 + 172) * 4] == 52);
    /* The image fits to 320x120 and is centered in the 320x240 surface. */
    cui_input_mouse_move(ui, 172, 100);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    cui_begin_frame(ui, .016f); ced_extension_preview_body(ui, &view, owner, renderer, &doc); cui_end_frame(ui);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    cui_begin_frame(ui, .016f); ced_extension_preview_body(ui, &view, owner, renderer, &doc); cui_end_frame(ui);
    settle(manager);
    assert(preview->image.pixels[pixel] == 240);

    assert(ced_extensions_preview_click(owner, renderer, &doc, 138, 32));
    strcpy(doc.text, "{\"steps\":[\"start\"]}");
    assert(ced_document_note_change(&doc));
    settle(manager);
    assert(!preview->image.pixels); /* A frame for the previous revision cannot be published. */
    ced_extensions_preview(owner, renderer, &doc);
    settle(manager);
    assert(preview->image.pixels && preview->image.pixels[between] == 52); /* The wider first node fills this pixel. */
    ced_extensions_document_closed(manager, doc.id);
    assert(!preview->image.pixels);
    ced_extension_preview_view_clear(ui, &view);
    cui_destroy(ui); cui_sw_destroy(software);
    ced_extensions_destroy(manager);
    ced_commands_free(&commands);
    ced_document_free(&doc);
    assert(!ced_rpc_memory_used() && ced_file_delete(parent, root, error));
    puts("OK: installed .asln renderer, workspace trust, unsaved source, native display, click and stale rendering");
    return 0;
}
