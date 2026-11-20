#include "preferences.h"
#include "documents.h"
#include "cJSON.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool failure(char *error, const char *message) { snprintf(error,CED_ERROR,"%s",message); return false; }

static bool settings_paths(char *directory, char *path, char *error) {
    const char *home=getenv("HOME");
    if (!home || !*home || !ced_join(directory,CED_PATH,home,".ced") || !ced_join(path,CED_PATH,directory,"settings.json"))
        return failure(error,"Could not locate the user preferences.");
    char canonical[CED_PATH];
    if (realpath(path,canonical)) snprintf(path,CED_PATH,"%s",canonical);
    return true;
}

bool ced_preferences_path(char *path, char *error) { char directory[CED_PATH]; return settings_paths(directory,path,error); }

void ced_preferences_defaults(CedPreferences *settings) {
    memset(settings,0,sizeof *settings); settings->font_size=CED_FONT_SIZE_DEFAULT;
    snprintf(settings->font_family,sizeof settings->font_family,"Geist Mono");
}

static void legacy_load(const char *directory, CedPreferences *settings) {
    char path[CED_PATH],line[128]; if (!ced_join(path,sizeof path,directory,"settings.conf")) return;
    FILE *file=fopen(path,"r"); if (!file) return;
    while (fgets(line,sizeof line,file)) {
        if (!strchr(line,'\n') && !feof(file)) { int ch; while ((ch=fgetc(file))!='\n' && ch!=EOF) {} continue; }
        size_t length=strlen(line);
        while (length && isspace((unsigned char)line[length-1])) line[--length]=0;
        if (!strncmp(line,"font_size=",10)) {
            char *end; errno=0; long value=strtol(line+10,&end,10);
            if (!errno && end!=line+10 && !*end && value>=CED_FONT_SIZE_MIN && value<=CED_FONT_SIZE_MAX) settings->font_size=(int)value;
        } else if (!strcmp(line,"terminal_position=right")) settings->terminal_right=true;
        else if (!strcmp(line,"terminal_position=bottom")) settings->terminal_right=false;
    }
    fclose(file);
}

static bool valid_tree(const cJSON *value) {
    if (cJSON_IsNumber(value) && !isfinite(value->valuedouble)) return false;
    if (cJSON_IsObject(value)) for (const cJSON *a=value->child;a;a=a->next)
        for (const cJSON *b=a->next;b;b=b->next) if (!strcmp(a->string,b->string)) return false;
    for (const cJSON *child=value->child;child;child=child->next) if (!valid_tree(child)) return false;
    return true;
}

static bool text_field(const cJSON *root, const char *key, char *out, size_t capacity, char *error) {
    const cJSON *value=cJSON_GetObjectItemCaseSensitive(root,key);
    if (!value) return true;
    if (!cJSON_IsString(value) || strlen(value->valuestring)>=capacity) return failure(error,"Invalid editor font preference.");
    for (const unsigned char *p=(const unsigned char *)value->valuestring;*p;p++)
        if (*p<32 || *p==127) return failure(error,"Font preferences cannot contain control characters.");
    snprintf(out,capacity,"%s",value->valuestring); return true;
}

static cJSON *parse(const char *text, size_t length, CedPreferences *settings, char *error) {
    if (!length || length>CED_PREFERENCES_LIMIT || !ced_utf8_valid(text,length) || (length>=3 && !memcmp(text,"\xef\xbb\xbf",3))) goto invalid;
    bool quoted=false; unsigned depth=0,tokens=0;
    for (size_t n=0;n<length;n++) {
        unsigned char ch=(unsigned char)text[n];
        if (quoted) {
            if (ch<32) goto invalid;
            if (ch=='\\') {
                if (++n==length) goto invalid;
                if (text[n]=='u' && n+4<length && !memcmp(text+n+1,"0000",4)) goto invalid;
            } else if (ch=='"') quoted=false;
        } else if (ch=='"') { quoted=true; tokens++; }
        else if (ch=='{' || ch=='[') { if (++depth>16) goto invalid; tokens++; }
        else if (ch=='}' || ch==']') { if (!depth) goto invalid; depth--; }
        else if (ch==',' || ch==':') tokens++;
        else if (ch<32 && ch!='\t' && ch!='\n' && ch!='\r') goto invalid;
        else if (ch=='-' || (ch>='0' && ch<='9')) {
            size_t end=n;
            if (text[end]=='-' && ++end==length) goto invalid;
            if (text[end]=='0') end++;
            else {
                if (text[end]<'1' || text[end]>'9') goto invalid;
                while (end<length && text[end]>='0' && text[end]<='9') end++;
            }
            if (end<length && text[end]=='.') {
                size_t first=++end;
                while (end<length && text[end]>='0' && text[end]<='9') end++;
                if (first==end) goto invalid;
            }
            if (end<length && (text[end]=='e' || text[end]=='E')) {
                end++; if (end<length && (text[end]=='+' || text[end]=='-')) end++;
                size_t first=end;
                while (end<length && text[end]>='0' && text[end]<='9') end++;
                if (first==end) goto invalid;
            }
            if (end<length && !strchr(" \t\r\n,]}",text[end])) goto invalid;
            n=end-1; tokens++;
        }
        if (tokens>4096) goto invalid;
    }
    if (depth || quoted) goto invalid;
    const char *end=NULL; cJSON *root=cJSON_ParseWithLengthOpts(text,length,&end,false);
    while (end && end<text+length && isspace((unsigned char)*end)) end++;
    if (!root || end!=text+length || !cJSON_IsObject(root) || !valid_tree(root)) { cJSON_Delete(root); goto invalid; }
    CedPreferences next; ced_preferences_defaults(&next);
    const cJSON *size=cJSON_GetObjectItemCaseSensitive(root,"editor.fontSize");
    if (size && (!cJSON_IsNumber(size) || floor(size->valuedouble)!=size->valuedouble || size->valuedouble<CED_FONT_SIZE_MIN || size->valuedouble>CED_FONT_SIZE_MAX)) {
        failure(error,"editor.fontSize must be an integer between 8 and 32."); goto failed;
    }
    if (size) next.font_size=size->valueint;
    const cJSON *position=cJSON_GetObjectItemCaseSensitive(root,"terminal.position");
    if (position && (!cJSON_IsString(position) || (strcmp(position->valuestring,"bottom") && strcmp(position->valuestring,"right")))) {
        failure(error,"terminal.position must be \"bottom\" or \"right\"."); goto failed;
    }
    if (position) next.terminal_right=!strcmp(position->valuestring,"right");
    if (!text_field(root,"editor.fontFamily",next.font_family,sizeof next.font_family,error) ||
        !text_field(root,"editor.fontPath",next.font_path,sizeof next.font_path,error)) goto failed;
    bool absolute_font = *next.font_path=='/';
#ifdef _WIN32
    absolute_font |= strlen(next.font_path)>2 && isalpha((unsigned char)next.font_path[0]) && next.font_path[1]==':' &&
        (next.font_path[2]=='/' || next.font_path[2]=='\\');
#endif
    if (*next.font_path && !absolute_font && strncmp(next.font_path,"~/",2)) {
        failure(error,"editor.fontPath must be absolute or start with ~/."); goto failed;
    }
    *settings=next; return root;
failed:
    cJSON_Delete(root); return NULL;
invalid:
    failure(error,"Invalid settings.json: use a JSON object without duplicate keys (maximum 64 KiB)."); return NULL;
}

bool ced_preferences_validate(const char *text, size_t length, CedPreferences *settings, char *error) {
    cJSON *root=parse(text,length,settings,error); if (!root) return false; cJSON_Delete(root); return true;
}

static cJSON *load(CedPreferences *settings, char *error) {
    ced_preferences_defaults(settings); *error=0;
    char directory[CED_PATH],path[CED_PATH]; if (!settings_paths(directory,path,error)) return NULL;
    int fd=open(path,O_RDONLY|O_NONBLOCK|O_CLOEXEC);
    if (fd<0 && errno==ENOENT) {
        legacy_load(directory,settings); cJSON *root=cJSON_CreateObject();
        if (!root || !cJSON_AddNumberToObject(root,"editor.fontSize",settings->font_size) ||
            !cJSON_AddStringToObject(root,"editor.fontFamily",settings->font_family) || !cJSON_AddStringToObject(root,"editor.fontPath","") ||
            !cJSON_AddStringToObject(root,"terminal.position",settings->terminal_right ? "right" : "bottom")) { cJSON_Delete(root); failure(error,"Out of memory."); return NULL; }
        return root;
    }
    struct stat st;
    if (fd<0 || fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size<=0 || st.st_size>CED_PREFERENCES_LIMIT) {
        if (fd>=0) close(fd); failure(error,"Could not read settings.json (regular file, maximum 64 KiB)."); return NULL;
    }
    size_t length=(size_t)st.st_size,used=0; char *text=malloc(length+1);
    if (!text) { close(fd); failure(error,"Out of memory."); return NULL; }
    while (used<length) { ssize_t n=read(fd,text+used,length-used); if (n<0 && errno==EINTR) continue; if (n<=0) break; used+=(size_t)n; }
    close(fd); text[used]=0;
    cJSON *root=used==length ? parse(text,length,settings,error) : NULL; free(text);
    if (!root && !*error) failure(error,"Could not read settings.json."); return root;
}

bool ced_preferences_load(CedPreferences *settings, char *error) {
    cJSON *root=load(settings,error); if (!root) return false; cJSON_Delete(root); return true;
}

static bool save(const cJSON *settings, char *error) {
    char directory[CED_PATH],path[CED_PATH],temp[CED_PATH];
    if (!settings_paths(directory,path,error)) return false;
    if (mkdir(directory,0700) && errno!=EEXIST) return failure(error,"Could not create the preferences directory.");
    int length=snprintf(temp,sizeof temp,"%s.XXXXXX",path);
    if (length<0 || (size_t)length>=sizeof temp) return failure(error,"Preferences path is too long.");
    char *text=cJSON_Print(settings); if (!text) return failure(error,"Out of memory.");
    CedPreferences check;
    if (!ced_preferences_validate(text,strlen(text),&check,error)) { cJSON_free(text); return false; }
    int fd=mkstemp(temp); bool ok=fd>=0; size_t used=0,size=strlen(text);
    while (ok && used<size) { ssize_t n=write(fd,text+used,size-used); if (n<0 && errno==EINTR) continue; if (n<=0) ok=false; else used+=(size_t)n; }
    if (fd>=0) { if (fsync(fd)) ok=false; if (close(fd)) ok=false; }
    cJSON_free(text);
    if (ok && rename(temp,path)) ok=false;
    if (!ok) { unlink(temp); return failure(error,"Could not save settings.json; the previous file was preserved."); }
    return true;
}

bool ced_preferences_ensure(char *path, char *error) {
    if (!ced_preferences_path(path,error)) return false;
    struct stat st;
    if (!stat(path,&st)) return S_ISREG(st.st_mode) || failure(error,"Preferences path is not a regular file.");
    if (errno!=ENOENT) return failure(error,"Could not access settings.json.");
    CedPreferences settings; cJSON *root=load(&settings,error); if (!root) return false;
    bool ok=save(root,error); cJSON_Delete(root); return ok;
}

int ced_font_size_load(void) { CedPreferences settings; char error[CED_ERROR]; ced_preferences_load(&settings,error); return settings.font_size; }
bool ced_terminal_right_load(void) { CedPreferences settings; char error[CED_ERROR]; ced_preferences_load(&settings,error); return settings.terminal_right; }

static bool update(const char *key, cJSON *value) {
    CedPreferences settings; char error[CED_ERROR]; cJSON *root=load(&settings,error);
    if (!root || !value) { cJSON_Delete(root); cJSON_Delete(value); return false; }
    cJSON_DeleteItemFromObjectCaseSensitive(root,key);
    if (!cJSON_AddItemToObject(root,key,value)) { cJSON_Delete(root); cJSON_Delete(value); return false; }
    bool ok=save(root,error); cJSON_Delete(root); return ok;
}

bool ced_font_size_save(int size) { return size>=CED_FONT_SIZE_MIN && size<=CED_FONT_SIZE_MAX && update("editor.fontSize",cJSON_CreateNumber(size)); }
bool ced_terminal_right_save(bool right) { return update("terminal.position",cJSON_CreateString(right ? "right" : "bottom")); }

static const char recent_key[] = "workspace.recentFolders";

static bool same_folder(const char *first, const char *second) {
#ifdef _WIN32
    return _stricmp(first, second) == 0;
#else
    return strcmp(first, second) == 0;
#endif
}

static void append_recent_folder(CedRecentFolders *folders, const char *path) {
    size_t length = strlen(path);
    bool absolute = path[0] == '/';
#ifdef _WIN32
    absolute |= length > 2 && isalpha((unsigned char)path[0]) && path[1] == ':' && path[2] == '/';
#endif
    if (!absolute || length >= CED_PATH || !ced_utf8_valid(path, length) ||
        folders->count == CED_RECENT_FOLDERS_LIMIT) return;
    for (size_t i = 0; i < folders->count; i++)
        if (same_folder(path, folders->paths[i])) return;
    memcpy(folders->paths[folders->count++], path, length + 1);
}

static void read_recent_folders(const cJSON *root, CedRecentFolders *folders) {
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(root, recent_key);
    if (!cJSON_IsArray(list)) return;
    const cJSON *item;
    cJSON_ArrayForEach(item, list) {
        if (cJSON_IsString(item)) append_recent_folder(folders, item->valuestring);
    }
}

bool ced_recent_folders_load(CedRecentFolders *folders, char *error) {
    folders->count = 0;
    CedPreferences settings;
    cJSON *root = load(&settings, error);
    if (!root) return false;
    read_recent_folders(root, folders);
    cJSON_Delete(root);
    return true;
}

bool ced_recent_folder_record(const char *canonical_path) {
    CedRecentFolders folders = {0};
    append_recent_folder(&folders, canonical_path);
    if (!folders.count) return false;
    CedPreferences settings;
    char error[CED_ERROR];
    cJSON *root = load(&settings, error);
    if (!root) return false;
    read_recent_folders(root, &folders);
    cJSON *list = cJSON_CreateArray();
    bool ok = list != NULL;
    for (size_t i = 0; ok && i < folders.count; i++)
        ok = cJSON_AddItemToArray(list, cJSON_CreateString(folders.paths[i]));
    if (ok) {
        cJSON_DeleteItemFromObjectCaseSensitive(root, recent_key);
        if (cJSON_AddItemToObject(root, recent_key, list)) {
            list = NULL;
            ok = save(root, error);
        } else ok = false;
    }
    cJSON_Delete(list);
    cJSON_Delete(root);
    return ok;
}
