#include "extension_temp.h"
#include "extensions/manager.h"
#include "editor_bridge.h"
#include "file_operations.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct { cui_ctx *ui; CedDocument doc; CedCommandContext context; unsigned commits; char message[CED_ERROR]; } Host;
static CedCommandContext context(void *user) { return ((Host*)user)->context; }
static bool document(void *user,size_t index,CedExtensionDocument *out) {
    Host *host=user; if (index) return false;
    *out=(CedExtensionDocument){&host->doc,"text",host->context.anchor,host->context.cursor}; return true;
}
static bool commit(void *user,CedDocument *doc,uint64_t version,const CedEditTransaction *transaction,char *error) {
    Host *host=user; cui_push_id_int(host->ui,1);
    bool ok=ced_editor_commit(host->ui,"source",doc,version,transaction,error); cui_pop_id(host->ui);
    if (ok && transaction->changed) { host->commits++; host->context.version=doc->version; }
    return ok;
}
static void message(void *user,const char *text) { snprintf(((Host*)user)->message,CED_ERROR,"%s",text); }
static void tick(Host *host,CedExtensions *m) {
    cui_begin_frame(host->ui,.01f); ced_extensions_tick(m); cui_push_id_int(host->ui,1);
    cui_code_editor(host->ui,"source",host->doc.text,CED_TEXT_CAP,.height=300,.autofocus=true);
    cui_pop_id(host->ui); cui_end_frame(host->ui);
    struct timespec delay={.tv_nsec=5000000}; nanosleep(&delay,NULL);
}
static void settle(Host *host,CedExtensions *m) {
    double deadline=ced_ext_now()+6;
    do { tick(host,m); assert(ced_ext_now()<deadline); } while(m->job.running || m->reload);
}
int main(int argc,char **argv) {
    assert(argc==2); char root[CED_PATH], parent[CED_PATH];
    extension_temp("ced extension integration-XXXXXX", root, parent);
    char profile[CED_PATH],path[CED_PATH],error[CED_ERROR]={0}; assert(ced_join(profile,sizeof profile,root,"profile"));
    assert(ced_join(path,sizeof path,root,"notes.txt")); FILE *f=fopen(path,"w"); assert(f); fputs("hello é🙂 world\n",f); fclose(f);
    Host host={0}; assert(ced_document_open(&host.doc,path,error)); ced_document_identify(&host.doc,1);
    host.context=(CedCommandContext){1,1,1,0,5,true,true,true,"text"};
    host.ui=cui_create(NULL); assert(host.ui); cui_set_viewport(host.ui,800,500,1); cui_input_focus(host.ui,true);
    CedCommands commands={0}; CedExtensions *m=ced_extensions_create(&commands,(CedExtensionHost){&host,context,document,commit,message,NULL},profile,root); assert(m);
    settle(&host,m); assert(!m->count && !m->trusted);
    assert(ced_extensions_job(m,EXT_INSPECT,argv[1])); settle(&host,m); assert(m->review_open);
    assert(ced_extensions_job(m,EXT_INSTALL,NULL)); settle(&host,m); assert(m->count==1 && commands.count==1);
    /* API trust is independent from a condition supplied by a caller. */
    assert(!ced_commands_execute(&commands,"example.text-tools.uppercase",&host.context,error)); assert(!m->instances[0].process);
    assert(ced_extensions_job(m,EXT_TRUST,NULL)); settle(&host,m); assert(m->trusted);
    assert(ced_commands_execute(&commands,"example.text-tools.uppercase",&host.context,error));
    double deadline=ced_ext_now()+6;
    while (*m->instances[0].command) { tick(&host,m); if (*m->instances[0].error) fprintf(stderr,"%s\n",m->instances[0].error); assert(ced_ext_now()<deadline); }
    assert(host.commits==1 && host.doc.version==2 && !strcmp(host.doc.text,"HELLO é🙂 world\n"));
    assert(ced_document_dirty(&host.doc)); f=fopen(path,"r"); assert(f); char original[100]; assert(fgets(original,sizeof original,f)); fclose(f); assert(!strcmp(original,"hello é🙂 world\n"));
    /* Selection moves while work is pending: no extension edit may commit. */
    host.context.anchor=13; host.context.cursor=18; host.context.selection_version++;
    assert(ced_commands_execute(&commands,"example.text-tools.uppercase",&host.context,error)); host.context.selection_version++;
    deadline=ced_ext_now()+6;
    while (*m->instances[0].command) { tick(&host,m); assert(ced_ext_now()<deadline); }
    assert(host.commits==1 && host.doc.version==2);
    char outside[CED_PATH]; assert(ced_join(outside, sizeof outside, parent, "ced-extension-outside-XXXXXX"));
    int outside_fd=mkstemp(outside); assert(outside_fd>=0); close(outside_fd);
    assert(realpath(outside,host.doc.path)); host.context.selection_version++;
    assert(!ced_extensions_authorized(&m->instances[0],&host.doc));
    assert(ced_commands_execute(&commands,"example.text-tools.uppercase",&host.context,error));
    deadline=ced_ext_now()+6;
    while (*m->instances[0].command) { tick(&host,m); assert(ced_ext_now()<deadline); }
    assert(host.commits==1); /* open outside documents do not imply a grant */
    assert(ced_extensions_grant_document(&m->instances[0],host.doc.id,true));
    assert(ced_extensions_authorized(&m->instances[0],&host.doc));
    assert(ced_commands_execute(&commands,"example.text-tools.uppercase",&host.context,error));
    deadline=ced_ext_now()+6;
    while (*m->instances[0].command) { tick(&host,m); assert(ced_ext_now()<deadline); }
    assert(host.commits==2 && host.doc.version==3 && !strcmp(host.doc.text,"HELLO é🙂 WORLD\n"));
    *host.doc.path=0; assert(ced_extensions_authorized(&m->instances[0],&host.doc));
    char old_session[96]; snprintf(old_session,sizeof old_session,"%s",m->instances[0].session);
    assert(ced_commands_execute(&commands,"example.text-tools.uppercase",&host.context,error));
    assert(ced_extensions_grant_document(&m->instances[0],host.doc.id,false));
    assert(!ced_extensions_authorized(&m->instances[0],&host.doc) && m->instances[0].state==EXT_STOPPING && !*m->instances[0].command);
    deadline=ced_ext_now()+6;
    while (m->instances[0].process) { tick(&host,m); assert(ced_ext_now()<deadline); }
    assert(host.commits==2 && host.doc.version==3);
    assert(realpath(path,host.doc.path));
    host.context.anchor=0; host.context.cursor=18; host.context.selection_version++;
    assert(ced_commands_execute(&commands,"example.text-tools.uppercase",&host.context,error));
    assert(strcmp(old_session,m->instances[0].session)); /* no reuse/replay of the revoked session */
    deadline=ced_ext_now()+6;
    while (*m->instances[0].command) { tick(&host,m); assert(ced_ext_now()<deadline); }
    assert(host.doc.version==3 && host.commits==2); /* Unicode-preserving no-op */
    /* Real Save As outside the root cancels a pending command immediately. */
    unsigned previous_access=ced_extensions_document_access(m,&host.doc); assert(previous_access==1);
    assert(ced_commands_execute(&commands,"example.text-tools.uppercase",&host.context,error));
    assert(ced_document_save(&host.doc,outside,true,error));
    ced_extensions_document_changed(m,&host.doc,previous_access);
    assert(m->instances[0].state==EXT_STOPPING && !*m->instances[0].command && !ced_extensions_authorized(&m->instances[0],&host.doc));
    deadline=ced_ext_now()+6;
    while (m->instances[0].process) { tick(&host,m); assert(ced_ext_now()<deadline); }
    assert(host.commits==2 && host.doc.version==3);
    assert(ced_document_save(&host.doc,path,true,error)); assert(!unlink(outside));
    /* A workspace generation change invalidates work before the next broker tick. */
    assert(ced_commands_execute(&commands,"example.text-tools.uppercase",&host.context,error)); ced_extensions_workspace(m,root); settle(&host,m);
    assert(host.commits==2 && !m->instances[0].process);
    /* Trust and installed metadata survive a fresh manager. */
    ced_extensions_destroy(m); assert(commands.count==0);
    m=ced_extensions_create(&commands,(CedExtensionHost){&host,context,document,commit,message,NULL},profile,root); settle(&host,m); assert(m->trusted && m->count==1);
    ced_extensions_destroy(m);
    CedExtensions *closing=ced_extensions_create(&commands,(CedExtensionHost){&host,context,document,commit,message,NULL},profile,root);
    assert(ced_extensions_job(closing,EXT_INSPECT,argv[1])); double closing_started=ced_ext_now();
    ced_extensions_destroy(closing); assert(ced_ext_now()-closing_started<1.5);
    /* Let an abandoned inspection release its own results; it may not touch UI. */
    struct timespec cleanup_delay={.tv_nsec=100000000}; nanosleep(&cleanup_delay,NULL);
    ced_commands_free(&commands); cui_destroy(host.ui); ced_document_free(&host.doc);
    assert(ced_rpc_memory_used()==0 && ced_file_delete(parent,root,error));
    puts("OK: reviewed local install, trust, lazy native process handshake, command, unsaved edit, stale selection, workspace invalidation, restart and cleanup"); return 0;
}
