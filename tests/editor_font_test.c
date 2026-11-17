#include "editor_font.h"
#include "editor_assist.h"
#include "cui_internal.h"
#include <cui/cui_render.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc,char **argv) {
    assert(argc==4); char error[CED_ERROR];
    cui_sw_renderer *renderer=cui_sw_create(); assert(renderer);
    cui_ctx *ui=cui_create(&(cui_ctx_desc){.renderer=cui_sw_iface(renderer),.theme=CUI_THEME_DARK}); assert(ui);
    cui_set_viewport(ui,900,360,1); cui_input_focus(ui,true);
    CedEditorFont fonts={0}; CedPreferences settings; ced_preferences_defaults(&settings);
    int original=cui__resolve_font(ui,CUI_FONT_MONO);
    assert(ced_editor_font_apply(ui,&fonts,&settings,error));
    snprintf(settings.font_path,sizeof settings.font_path,"%s",argv[1]);
    assert(ced_editor_font_apply(ui,&fonts,&settings,error));
    assert(cui__resolve_font(ui,CUI_FONT_MONO)!=original);
    int count=ui->font_count,selected=cui__resolve_font(ui,CUI_FONT_MONO);
    for (int n=0;n<20;n++) assert(ced_editor_font_apply(ui,&fonts,&settings,error));
    assert(ui->font_count==count && fonts.count==1);
    snprintf(settings.font_path,sizeof settings.font_path,"%s",argv[2]);
    assert(!ced_editor_font_apply(ui,&fonts,&settings,error));
    assert(strstr(error,"monospaced") && cui__resolve_font(ui,CUI_FONT_MONO)==selected);
    snprintf(settings.font_path,sizeof settings.font_path,"/nonexistent/ced-font.ttf");
    assert(!ced_editor_font_apply(ui,&fonts,&settings,error) && cui__resolve_font(ui,CUI_FONT_MONO)==selected);
    char invalid[]="/tmp/ced-font-invalid-XXXXXX"; int fd=mkstemp(invalid); assert(fd>=0);
    assert(write(fd,"OTTO invalid font file",22)==22); close(fd);
    snprintf(settings.font_path,sizeof settings.font_path,"%s",invalid);
    assert(!ced_editor_font_apply(ui,&fonts,&settings,error)); assert(!unlink(invalid));
    *settings.font_path=0;
#ifdef __APPLE__
    snprintf(settings.font_family,sizeof settings.font_family,"Menlo");
    assert(ced_editor_font_apply(ui,&fonts,&settings,error));
    snprintf(settings.font_family,sizeof settings.font_family,"CED Missing Font 874391");
    assert(!ced_editor_font_apply(ui,&fonts,&settings,error));
#endif
    char text[1024]="int main(void) {\n\tint café = 42;\n\treturn café;\n}\n";
    for (int n=0;n<3;n++) {
        cui_begin_frame(ui,.02f);
        cui_code_editor_select(ui,"font-test",3,3);
        cui_code_editor(ui,"font-test",text,sizeof text,.height=340,.size=18,.lang="c",.autofocus=true);
        CedEditorGeometry geometry=ced_editor_geometry(ui,"font-test",text,18);
        float width=cui_measure_text(ui,CUI_FONT_MONO,18,"0000000000000000",16).width/16;
        if (n>0) assert(geometry.caret.w>0 && fabsf(geometry.caret.x-geometry.content.x-3*width)<0.1f);
        cui_end_frame(ui);
    }
    unsigned char *pixels=calloc(900*360,4); assert(pixels);
    cui_sw_render(renderer,cui_get_draw_data(ui),pixels,900,360,900*4,CUI_SW_RGBA);
    assert(cui_write_png(argv[3],pixels,900,360,900*4)); free(pixels);
    ced_preferences_defaults(&settings); assert(ced_editor_font_apply(ui,&fonts,&settings,error));
    assert(cui__resolve_font(ui,CUI_FONT_MONO)==original);
    cui_destroy(ui); ced_editor_font_free(&fonts); cui_sw_destroy(renderer);
    puts("OK: custom/font-family loading, cache reuse, invalid/proportional-font rejection, fallback, editor geometry and rendered screenshot"); return 0;
}
