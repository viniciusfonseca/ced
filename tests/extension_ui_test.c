#include "extensions/extension_ui.h"
#include <cui/cui_render.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static CedCommandContext context(void *user) { (void)user; return (CedCommandContext){0}; }
static void render(cui_ctx *ui,cui_sw_renderer *sw,CedExtensions *m,const char *directory,const char *name) {
    for(int n=0;n<3;n++) {
        cui_begin_frame(ui,.1f);
        cui_column(ui,.width=CUI_FILL,.height=CUI_FILL) { cui_text(ui,"CED",.size=16); }
        ced_extensions_ui(ui,m); cui_end_frame(ui);
    }
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
    cui_destroy(ui); cui_sw_destroy(sw); ced_extension_record_free(&m->review); ced_extension_record_free(&m->instances[0].record); free(m);
    puts("OK: install review, manager and nested workspace trust dialogs rendered without a display"); return 0;
}
