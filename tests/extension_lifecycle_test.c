#include "extension_temp.h"
#include "extensions/manager.h"
#include "output.h"
#include "file_operations.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static CedCommandContext context(void *user) { (void)user; return (CedCommandContext){.workspace_trusted=true}; }
static bool document(void *user,size_t index,CedExtensionDocument *out) { (void)user;(void)index;(void)out;return false; }
static void message(void *user,const char *text) { (void)user;(void)text; }
static void tick(CedExtensions *m) { ced_extensions_tick(m); struct timespec delay={.tv_nsec=5000000}; nanosleep(&delay,NULL); }
static void settle(CedExtensions *m) { double deadline=ced_ext_now()+6; do { tick(m); assert(ced_ext_now()<deadline); } while(m->job.running || m->reload); }
static void copy(const char *from,const char *to) {
    FILE *in=fopen(from,"rb"),*out=fopen(to,"wb");assert(in && out); char bytes[16384];size_t n;
    while((n=fread(bytes,1,sizeof bytes,in))) assert(fwrite(bytes,1,n,out)==n);
    assert(!ferror(in));fclose(in);assert(!fclose(out));
#ifndef _WIN32
    assert(!chmod(to,0755));
#endif
}
int main(int argc,char **argv) {
    assert(argc==2);
    char root[CED_PATH],parent[CED_PATH],profile[CED_PATH],package[CED_PATH],path[CED_PATH],error[CED_ERROR]={0};
    extension_temp("ced-extension-lifecycle-XXXXXX", root, parent);
#ifdef _WIN32
    const char *entrypoint = "fixture.exe";
    assert(SetEnvironmentVariableW(L"CED_EXTENSION_TEST_PRIVATE", L"must not reach extensions"));
#else
    const char *entrypoint = "fixture";
#endif
    assert(ced_join(profile,sizeof profile,root,"profile")); assert(ced_join(package,sizeof package,root,"source")); assert(!mkdir(package,0700));
    assert(ced_join(path,sizeof path,package,entrypoint)); copy(argv[1],path);
    const char *modes[]={"crash","malformed","incompatible","hang","flood"};
    CedCommands commands={0};
    for(size_t n=0;n<sizeof modes/sizeof *modes;n++) {
        assert(ced_join(path,sizeof path,package,"ced-extension.json")); FILE *f=fopen(path,"w"); assert(f);
        fprintf(f,"{\"manifestVersion\":1,\"id\":\"fixture.%s\",\"name\":\"Fixture\",\"version\":\"1.0.0\",\"ced\":{\"minVersion\":\"0.2.0\",\"apiMajor\":1},\"kind\":\"executable\",\"entrypoints\":{\"%s\":\"%s\"},\"activationEvents\":[\"onCommand:fixture.%s.run\"],\"capabilities\":[],\"untrustedWorkspaces\":false,\"contributes\":{\"commands\":[{\"id\":\"fixture.%s.run\",\"title\":\"Run\"}]}}",modes[n],ced_extension_platform(),entrypoint,modes[n],modes[n]); assert(!fclose(f));
        CedExtensionRecord approved={0}; assert(ced_extension_inspect(package,&approved,error));
        assert(ced_extension_install(profile,&approved,error)); assert(ced_extension_set_trusted(profile,root,true,error));
        CedExtensions *m=ced_extensions_create(&commands,(CedExtensionHost){NULL,context,document,NULL,message,NULL},profile,root); assert(m); settle(m);
        char command[256]; snprintf(command,sizeof command,"fixture.%s.run",modes[n]); CedCommandContext ctx=context(NULL);
        assert(ced_commands_execute(&commands,command,&ctx,error));
        double start=ced_ext_now(),deadline=start+6;
        CedExtensionInstance *instance=NULL;
        for(size_t i=0;i<m->count;i++) if(!strcmp(m->instances[i].record.manifest.id,approved.manifest.id)) instance=&m->instances[i]; assert(instance);
        while(*instance->command || (strcmp(modes[n],"flood") && instance->process)) { tick(m); assert(ced_ext_now()<deadline); }
        if(!strcmp(modes[n],"flood")) {
            assert(instance->state==EXT_ACTIVE && instance->process);
            char output[CED_OUTPUT_CAP];
            uint64_t revision=0;
            ced_output_copy(output,&revision);
            assert(strstr(output,"[fixture.flood] xxxxxxxx"));
            char log[CED_PATH],relative[256];snprintf(relative,sizeof relative,"logs/extensions/%s/%s.log",instance->session,approved.manifest.id);
            assert(ced_join(log,sizeof log,profile,relative)); struct stat st;assert(!stat(log,&st));assert(st.st_size<=3*1024*1024);
        } else { assert(instance->state==EXT_FAILED && !instance->process && *instance->error); }
        if(!strcmp(modes[n],"hang")) {
            assert(ced_ext_now()-start>=3 && ced_ext_now()-start<5.5);
#ifdef _WIN32
            char log[CED_PATH], relative[256], output[4096];
            snprintf(relative, sizeof relative, "logs/extensions/%s/%s.log", instance->session, approved.manifest.id);
            assert(ced_join(log, sizeof log, profile, relative));
            FILE *stream = fopen(log,"rb"); assert(stream);
            size_t size = fread(output, 1, sizeof output - 1, stream);
            fclose(stream);
            output[size] = 0;
            const char *line = strstr(output, "descendant pid=");
            unsigned long pid = 0;
            if (!line) fprintf(stderr,"Missing descendant marker from %s:\n%s\n", instance->error, output);
            assert(line && sscanf(line, "descendant pid=%lu", &pid)==1);
            HANDLE descendant = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
            if (descendant) {
                assert(WaitForSingleObject(descendant, 1000)==WAIT_OBJECT_0);
                CloseHandle(descendant);
            } else assert(GetLastError()==ERROR_INVALID_PARAMETER);
#endif
        }
        ced_extensions_destroy(m); assert(commands.count==0 && ced_rpc_memory_used()==0);
        ced_extension_record_free(&approved);
    }
    ced_commands_free(&commands); assert(ced_file_delete(parent,root,error));
    puts("OK: native crash, malformed framing, incompatible handshake, initialization timeout, unresponsive process cleanup, stderr flood and bounded cleanup"); return 0;
}
