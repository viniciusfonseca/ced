#include "documents.h"
#include "editor_bridge.h"
#include "cui_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cui_ctx *ui;
static CedDocument document;
static void *fail_allocation(size_t size, void *user) { (void)size; (void)user; return NULL; }
static void draw(void) {
    cui_push_id_int(ui,1);
    ced_editor_render(ui,"source",&document,&(cui_code_editor_opts){.height=300,.autofocus=true});
    cui_pop_id(ui);
}
static void frame(void) { cui_begin_frame(ui,.02f); draw(); cui_end_frame(ui); }
static bool commit(CedTextEdit *edits, size_t count, uint64_t version) {
    char error[CED_ERROR]; CedEditTransaction transaction;
    if (!ced_document_prepare_edit(&document,version,edits,count,0,0,false,0,0,&transaction,error)) return false;
    cui_begin_frame(ui,.02f); cui_push_id_int(ui,1);
    bool ok=ced_editor_commit(ui,"source",&document,version,&transaction,error);
    cui_pop_id(ui); draw(); cui_end_frame(ui); ced_edit_transaction_free(&transaction); return ok;
}
int main(void) {
    ui=cui_create(NULL); assert(ui); cui_set_viewport(ui,800,500,1); cui_input_focus(ui,true);
    document.text=calloc(CED_TEXT_CAP,1); assert(document.text); strcpy(document.text,"hello é🙂 world\n"); ced_document_identify(&document,1);
    frame(); frame();
    CedTextEdit edits[]={{13,18,"WORLD"},{0,5,"HELLO"}};
    assert(commit(edits,2,1)); assert(document.version==2 && !strcmp(document.text,"HELLO é🙂 WORLD\n"));
    cui_input_key(ui,CUI_KEY_Z,true,CUI_MOD_PRIMARY); frame(); cui_input_key(ui,CUI_KEY_Z,false,0); frame();
    assert(document.version==3 && !strcmp(document.text,"hello é🙂 world\n"));
    cui_input_key(ui,CUI_KEY_Z,true,CUI_MOD_PRIMARY|CUI_MOD_SHIFT); frame(); cui_input_key(ui,CUI_KEY_Z,false,0); frame();
    assert(document.version==4 && !strcmp(document.text,"HELLO é🙂 WORLD\n"));
    cui_begin_frame(ui,.02f); cui_push_id_int(ui,1); cui_code_editor_select(ui,"source",0,5); cui_pop_id(ui); draw(); cui_end_frame(ui);
    cui_input_text(ui,"HELLO"); frame(); assert(document.version==4);
    assert(!commit(edits,2,1));
    CedTextEdit noop={0,5,"HELLO"}; assert(commit(&noop,1,4)); assert(document.version==4);
    CedEditTransaction transaction; char error[CED_ERROR];
    CedTextEdit overlap[]={{0,5,"a"},{3,7,"b"}};
    assert(!ced_document_prepare_edit(&document,4,overlap,2,0,0,false,0,0,&transaction,error));
    CedTextEdit split={7,8,"X"}; assert(!ced_document_prepare_edit(&document,4,&split,1,0,0,false,0,0,&transaction,error));
    CedTextEdit invalid={0,0,"\xc0\xaf"}; assert(!ced_document_prepare_edit(&document,4,&invalid,1,0,0,false,0,0,&transaction,error));
    invalid.text="\r\n"; assert(!ced_document_prepare_edit(&document,4,&invalid,1,0,0,false,0,0,&transaction,error));
    CedTextEdit boundary[]={{0,5,"x"},{5,5,"y"}};
    assert(!ced_document_prepare_edit(&document,4,boundary,2,0,0,false,0,0,&transaction,error));
    CedTextEdit adjacent[]={{0,5,"x"},{5,6,"y"}};
    assert(ced_document_prepare_edit(&document,4,adjacent,2,5,5,false,0,0,&transaction,error));
    assert(transaction.anchor==2 && transaction.cursor==2); ced_edit_transaction_free(&transaction);
    assert(!ced_utf8_valid("\xed\xa0\x80",3)); assert(!ced_utf8_valid("\xf4\x90\x80\x80",4));
    /* Editing an initialized inactive tab must not steal focus. */
    cui_begin_frame(ui,.02f); cui_id focus=ui->focus; cui_push_id_int(ui,1);
    CedTextEdit insertion={0,0,"!"};
    assert(ced_document_prepare_edit(&document,4,&insertion,1,0,0,false,0,0,&transaction,error));
    cui_allocator allocator=ui->alloc; ui->alloc.alloc=fail_allocation;
    assert(!ced_editor_commit(ui,"source",&document,4,&transaction,error));
    assert(document.version==4 && document.text[0]=='H'); ui->alloc=allocator;
    assert(ced_editor_commit(ui,"source",&document,4,&transaction,error)); assert(ui->focus==focus);
    ced_edit_transaction_free(&transaction); cui_pop_id(ui); cui_text(ui,"Other tab",.size=13); cui_end_frame(ui);
    /* Keep inactive history alive across state GC, then undo it. */
    for (int n=0;n<700;n++) { cui_begin_frame(ui,.02f); cui_push_id_int(ui,1); ced_editor_touch(ui,"source"); cui_pop_id(ui); cui_end_frame(ui); }
    frame(); cui_input_key(ui,CUI_KEY_Z,true,CUI_MOD_PRIMARY); frame(); cui_input_key(ui,CUI_KEY_Z,false,0); frame();
    assert(!strcmp(document.text,"HELLO é🙂 WORLD\n"));
    cui_destroy(ui); ced_document_free(&document);
    puts("OK: atomic unordered edits, one-step undo/redo, stale revisions, no-ops, UTF-8 ranges, inactive editor ownership and history"); return 0;
}
