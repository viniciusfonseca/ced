#include "workspace.h"
#include "documents.h"
#include "preferences.h"
#include "file_operations.h"
#include "process_memory.h"
#include "windows_process.h"
#include "lsp.h"
#include "terminal.h"
#include <assert.h>
#include <errno.h>
#include <string.h>

static char error[CED_ERROR];

int main(int argc, char **argv) {
    ced_windows_init();
    if (argc>1 && !strcmp(argv[1],"--args")) {
        for (int i=2;i<argc;i++) { fwrite(argv[i],1,strlen(argv[i]),stdout); fputc(0,stdout); }
        return 0;
    }
    if (argc>1 && !strcmp(argv[1],"--stdin")) {
        char buffer[8192]; size_t n;
        while ((n=fread(buffer,1,sizeof buffer,stdin))) assert(fwrite(buffer,1,n,stdout)==n);
        return 0;
    }
    if (argc>1 && !strcmp(argv[1],"--large")) {
        char bytes[8192]; memset(bytes,'x',sizeof bytes);
        for (int i=0;i<256;i++) assert(fwrite(bytes,1,sizeof bytes,stdout)==sizeof bytes);
        return 0;
    }
    wchar_t module[32768],temp[MAX_PATH];
    assert(GetModuleFileNameW(NULL,module,32768)); assert(GetTempPathW(MAX_PATH,temp));
    char *exe=ced_windows_utf8(module), *directory=ced_windows_utf8(temp);
    assert(exe && directory);
    char root[CED_PATH],native[CED_PATH];
    snprintf(native,sizeof native,"%sced-test-%lu-é",directory,GetCurrentProcessId());
    assert(!mkdir(native,0700));
    assert(realpath(native,root));
    assert(!_putenv_s("HOME",root));

    char created[CED_PATH],renamed[CED_PATH],folder[CED_PATH];
    assert(ced_file_create(root,root,"café.txt",created,error));
    assert(!ced_file_create(root,root,"café.txt",renamed,error));
    assert(!ced_file_create(root,root,"CON.txt",renamed,error));
    assert(!ced_file_create(root,root,"../escape",renamed,error));
    CedDocument doc={0}; assert(ced_document_open(&doc,created,error));
    strcpy(doc.text,"hello\nworld\n"); doc.crlf=true;
    assert(ced_document_save(&doc,created,false,error)); ced_document_free(&doc);
    assert(ced_document_open(&doc,created,error)); assert(doc.crlf && !strcmp(doc.text,"hello\nworld\n")); ced_document_free(&doc);
    assert(ced_file_rename(root,created,"renamed.txt",renamed,error));
    assert(ced_directory_create(root,root,"folder",folder,error));
    assert(ced_file_create(root,folder,"child.txt",created,error));
    assert(!ced_file_delete(root,root,error));
    char traversal[CED_PATH]; assert(ced_join(traversal,sizeof traversal,root,"folder/.."));
    assert(!ced_file_create(root,traversal,"escape.txt",created,error));
    assert(ced_file_delete(root,folder,error));
    assert(ced_preferences_ensure(created,error));
    assert(ced_font_size_save(19)); assert(ced_terminal_right_save(true));
    CedPreferences settings; assert(ced_preferences_load(&settings,error));
    assert(settings.font_size==19 && settings.terminal_right);
    CedWorkspace ws={0}; assert(ced_workspace_open(&ws,root,error)); assert(ws.count>=3); ced_workspace_free(&ws);
    assert(ced_memory_usage_bytes()>0);
    const char *source="C:/Projects/café file.c";
    char *uri=ced_lsp_uri(source); assert(uri && !strncmp(uri,"file:///C:/",11));
    assert(ced_lsp_path(uri,created,sizeof created) && !strcmp(created,source)); free(uri);

    /* Wine can check filesystem/process code, but a Windows VM must exercise
     * the real ConPTY implementation and its integration with libvterm. */
    if (!GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"wine_get_version")) {
        CedTerminal *terminal=ced_terminal_start(root,NULL,error,sizeof error);
        if (!terminal) fprintf(stderr,"ConPTY startup: %s\n",error);
        assert(terminal);
        const char command[]="echo CED_%PROCESSOR_ARCHITECTURE%_CONPTY_OK\r";
        assert(ced_terminal_send(terminal,command,sizeof command-1));
        const char *arch=getenv("PROCESSOR_ARCHITECTURE"); assert(arch);
        char expected_terminal[128]; snprintf(expected_terminal,sizeof expected_terminal,"CED_%s_CONPTY_OK",arch);
        bool found=false; ULONGLONG deadline=GetTickCount64()+10000;
        while (!found && GetTickCount64()<deadline) {
            ced_terminal_poll(terminal);
            for (int row=0;row<terminal->rows;row++) {
                char line[501]; int column=0;
                for (;column<terminal->columns && column<500;column++) {
                    VTermScreenCell cell;
                    line[column]=ced_terminal_cell(terminal,row,column,&cell) && cell.chars[0]<128 && cell.chars[0] ? (char)cell.chars[0] : ' ';
                }
                line[column]=0;
                if (strstr(line,expected_terminal)) found=true;
            }
            Sleep(20);
        }
        ced_terminal_stop(terminal); assert(found);
        puts("OK: native Windows ConPTY shell, input/output and libvterm screen rendering");
    }

    const char *args[]={exe,"--args","with spaces","quote\"inside","trailing\\","slashes\\\"quote","$(echo unsafe)","",NULL};
    char *out=NULL; size_t length=0;
    assert(!ced_command(args,&out,&length));
    const char expected[]="with spaces\0quote\"inside\0trailing\\\0slashes\\\"quote\0$(echo unsafe)\0\0";
    assert(length==sizeof expected-1 && !memcmp(out,expected,length)); free(out);
    FILE *input=tmpfile(); assert(input);
    const char binary[]="first\0second\n";
    assert(fwrite(binary,1,sizeof binary-1,input)==sizeof binary-1); assert(!fflush(input)); rewind(input);
    const char *echo[]={exe,"--stdin",NULL};
    assert(!ced_windows_command(echo,&out,&length,fileno(input)));
    assert(length==sizeof binary-1 && !memcmp(out,binary,length)); free(out); fclose(input);
    const char *large[]={exe,"--large",NULL};
    assert(!ced_command(large,&out,&length)); assert(length==2*1024*1024 && out[0]=='x' && out[length-1]=='x'); free(out);
    const char *missing[]={"ced-nonexistent-program-948277.exe",NULL};
    assert(ced_command(missing,&out,&length)); free(out);
    char parent[CED_PATH]; assert(realpath(directory,parent));
    assert(ced_file_delete(parent,root,error));
    free(exe); free(directory);
    puts("OK: Windows UTF-8 files, atomic saves, protected mutations, JSON preferences, scanning, memory, command quoting and pipe output");
    return 0;
}
