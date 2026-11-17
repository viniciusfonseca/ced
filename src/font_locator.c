#include "editor_font.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

bool ced_font_find(const char *family, char *path, char *error) {
    const char *args[]={"fc-match","--format=%{family}\n%{file}\n","--",family,NULL}; char *output=NULL;
    if (ced_command(args,&output,NULL) || !output) { free(output); snprintf(error,CED_ERROR,"Install fontconfig or set editor.fontPath to a font file."); return false; }
    char *file=strchr(output,'\n'); bool match=!strcasecmp(family,"monospace");
    if (file) {
        *file++=0;
        for (char *part=output;part && *part;) {
            char *comma=strchr(part,','); if (comma) *comma=0;
            if (!strcasecmp(part,family)) match=true;
            part=comma ? comma+1 : NULL;
        }
        file[strcspn(file,"\r\n")]=0;
    }
    bool ok=match && file && *file=='/' && strlen(file)<CED_PATH;
    if (ok) snprintf(path,CED_PATH,"%s",file);
    else snprintf(error,CED_ERROR,"Installed font not found: %.255s",family);
    free(output); return ok;
}
