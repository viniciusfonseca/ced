#include "editor_font.h"
#include "cui_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FONT_FILE_LIMIT (32 * 1024 * 1024)
static bool failure(char *error, const char *message) { snprintf(error,CED_ERROR,"%s",message); return false; }
static uint32_t be32(const unsigned char *p) { return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3]; }

/* Bound the SFNT directory before handing user-selected bytes to CUI's font
 * loader. Collections use their first face, matching CUI's public API. */
static bool font_directory(const unsigned char *data, size_t size) {
    if (size<12) return false;
    size_t offset=0;
    if (!memcmp(data,"ttcf",4)) { if (size<16 || !be32(data+8)) return false; offset=be32(data+12); }
    if (offset>size-12) return false;
    uint32_t signature=be32(data+offset);
    if (signature!=0x00010000 && signature!=0x4f54544f && signature!=0x74727565) return false;
    size_t tables=(size_t)data[offset+4]*256+data[offset+5];
    if (!tables || tables>256 || tables>(size-offset-12)/16) return false;
    unsigned required=0;
    for (size_t n=0;n<tables;n++) {
        const unsigned char *table=data+offset+12+n*16;
        size_t start=be32(table+8),length=be32(table+12);
        if (start>size || length>size-start) return false;
        if (!memcmp(table,"head",4) && length>=54) required|=1;
        if (!memcmp(table,"hhea",4) && length>=36) required|=2;
        if (!memcmp(table,"hmtx",4) && length>=4) required|=4;
        if (!memcmp(table,"maxp",4) && length>=6) required|=8;
        if (!memcmp(table,"cmap",4) && length>=4) required|=16;
        if (!memcmp(table,"glyf",4) || !memcmp(table,"CFF ",4)) required|=32;
    }
    return required==63;
}

static unsigned char *read_font(const char *path, size_t *size, char *error) {
    int fd=open(path,O_RDONLY|O_NONBLOCK|O_CLOEXEC); struct stat st;
    if (fd<0 || fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size<12 || st.st_size>FONT_FILE_LIMIT) {
        if (fd>=0) close(fd); failure(error,"Font must be a readable TTF/OTF/TTC file, at most 32 MiB."); return NULL;
    }
    *size=(size_t)st.st_size; unsigned char *bytes=malloc(*size);
    if (!bytes) { close(fd); failure(error,"Out of memory loading the font."); return NULL; }
    size_t used=0;
    while (used<*size) { ssize_t n=read(fd,bytes+used,*size-used); if (n<0 && errno==EINTR) continue; if (n<=0) break; used+=(size_t)n; }
    close(fd);
    if (used!=*size || !font_directory(bytes,*size)) { free(bytes); failure(error,"Invalid or unsupported font file."); return NULL; }
    return bytes;
}

bool ced_editor_font_apply(cui_ctx *ui, CedEditorFont *fonts, const CedPreferences *settings, char *error) {
    *error=0;
    if (!fonts->initialized) {
        /* CUI's pinned adapter maps concrete font IDs to 16 + internal index. */
        fonts->default_font=16+cui__resolve_font(ui,CUI_FONT_MONO); fonts->initialized=true;
    }
    if (!*settings->font_path && (!*settings->font_family || !strcmp(settings->font_family,"Geist Mono"))) {
        cui_font_set_role(ui,CUI_FONT_MONO,fonts->default_font); return true;
    }
    char path[CED_PATH],canonical[CED_PATH];
    if (*settings->font_path) {
        if (!strncmp(settings->font_path,"~/",2)) {
            const char *home=getenv("HOME");
            if (!home || !ced_join(path,sizeof path,home,settings->font_path+2)) return failure(error,"Could not expand the font path.");
        } else snprintf(path,sizeof path,"%s",settings->font_path);
    } else if (!ced_font_find(settings->font_family,path,error)) return false;
    if (!realpath(path,canonical)) return failure(error,"The configured font file is unavailable; the previous font was kept.");
    CedFontEntry *entry=NULL;
    for (size_t n=0;n<fonts->count;n++) if (!strcmp(fonts->entries[n].path,canonical)) entry=&fonts->entries[n];
    if (!entry) {
        if (fonts->count==CED_FONT_CACHE_LIMIT) return failure(error,"Restart CED before loading additional font files.");
        size_t size; unsigned char *bytes=read_font(canonical,&size,error); if (!bytes) return false;
        cui_font font=cui_font_load_memory(ui,bytes,size,false);
        if (font<0) { free(bytes); return failure(error,"CUI could not load this font; the previous font was kept."); }
        entry=&fonts->entries[fonts->count++]; entry->data=bytes; entry->font=font;
        snprintf(entry->path,sizeof entry->path,"%s",canonical);
        float narrow=cui_measure_text(ui,font,16,"iiiiiiii",8).width;
        float wide=cui_measure_text(ui,font,16,"WWWWWWWW",8).width;
        float space=cui_measure_text(ui,font,16,"        ",8).width;
        entry->monospace=narrow>0 && fabsf(narrow-wide)<0.1f && fabsf(narrow-space)<0.1f;
        if (entry->monospace) cui_font_add_fallback(ui,font,fonts->default_font);
    }
    if (!entry->monospace) return failure(error,"Choose a monospaced editor font; the previous font was kept.");
    cui_font_set_role(ui,CUI_FONT_MONO,entry->font); return true;
}

void ced_editor_font_free(CedEditorFont *fonts) {
    for (size_t n=0;n<fonts->count;n++) free(fonts->entries[n].data);
    memset(fonts,0,sizeof *fonts);
}
