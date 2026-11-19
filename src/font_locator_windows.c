#include "editor_font.h"
#include <string.h>

bool ced_font_find(const char *family, char *path, char *error) {
    const HKEY roots[]={HKEY_CURRENT_USER,HKEY_LOCAL_MACHINE};
    for (size_t r=0;r<2;r++) {
        HKEY key;
        if (RegOpenKeyExW(roots[r],L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts",0,KEY_READ,&key)) continue;
        for (DWORD index=0;;index++) {
            wchar_t name[512],value[4096]; DWORD names=512,bytes=sizeof value,type;
            LONG result=RegEnumValueW(key,index,name,&names,NULL,&type,(BYTE *)value,&bytes);
            if (result==ERROR_NO_MORE_ITEMS) break;
            if (result || type!=REG_SZ || bytes>=sizeof value) continue;
            value[bytes/sizeof(wchar_t)]=0;
            char *label=ced_windows_utf8(name), *file=ced_windows_utf8(value);
            if (label && file) {
                char *suffix=strstr(label," ("); if (suffix) *suffix=0;
                if (!_stricmp(label,family)) {
                    bool ok=false;
                    if ((strlen(file)>2 && file[1]==':') || file[0]=='/' || file[0]=='\\') {
                        ok=strlen(file)<CED_PATH; if (ok) snprintf(path,CED_PATH,"%s",file);
                    } else {
                        wchar_t windows[32768]; UINT n=GetWindowsDirectoryW(windows,32768);
                        char *base=n && n<32768 ? ced_windows_utf8(windows) : NULL;
                        char fonts[CED_PATH]; ok=base && ced_join(fonts,sizeof fonts,base,"Fonts") && ced_join(path,CED_PATH,fonts,file); free(base);
                    }
                    free(label); free(file); RegCloseKey(key);
                    if (ok) return true;
                    snprintf(error,CED_ERROR,"Font path is too long."); return false;
                }
            }
            free(label); free(file);
        }
        RegCloseKey(key);
    }
    snprintf(error,CED_ERROR,"Installed font not found: %.255s. Try editor.fontPath.",family); return false;
}
