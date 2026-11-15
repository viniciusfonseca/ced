#include "rpc_transport.h"
#include "extensions/json.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    const char *json="{\"jsonrpc\":\"2.0\",\"id\":\"ext:1\",\"method\":\"documents/list\",\"params\":{}}";
    size_t length; char *frame=ced_rpc_frame(json,strlen(json),&length); assert(frame);
    CedRpcReader reader={0}; char *body; size_t size;
    for (size_t n=0;n<length;n++) {
        assert(ced_rpc_feed(&reader,frame+n,1));
        int result=ced_rpc_next(&reader,&body,&size);
        if (n+1<length) assert(result==0);
        else { assert(result==1 && size==strlen(json) && !strcmp(body,json)); free(body); ced_rpc_release(size+1); }
    }
    assert(ced_rpc_feed(&reader,frame,length)); assert(ced_rpc_feed(&reader,frame,length));
    for (int n=0;n<2;n++) { assert(ced_rpc_next(&reader,&body,&size)==1); free(body); ced_rpc_release(size+1); }
    assert(ced_rpc_next(&reader,&body,&size)==0); ced_rpc_reader_free(&reader); ced_rpc_frame_free(frame,length);
    const char *bad[]={"Content-Length: 0\r\n\r\n","Content-Length: -1\r\n\r\n","Content-Length: 8388609\r\n\r\n","Content-Length: 99999999999999999999999\r\n\r\n","Content-Length: 2\r\nContent-Length: 2\r\n\r\n{}",NULL};
    for (int n=0;bad[n];n++) { assert(ced_rpc_feed(&reader,bad[n],strlen(bad[n]))); assert(ced_rpc_next(&reader,&body,&size)==-1); ced_rpc_reader_free(&reader); }
    char header[1025]; memset(header,'x',sizeof header); assert(ced_rpc_feed(&reader,header,sizeof header)); assert(ced_rpc_next(&reader,&body,&size)==-1); ced_rpc_reader_free(&reader);
    assert(!ced_rpc_reserve(65*1024*1024)); assert(ced_rpc_memory_used()==0);
    const char *nonfinite[]={"{\"value\":1e999}","{\"value\":-1e999}","{\"nested\":[0,{\"value\":1e999}]}",NULL};
    char error[CED_ERROR];
    for (int n=0;nonfinite[n];n++) assert(!ced_ext_json_parse(nonfinite[n],strlen(nonfinite[n]),64,error));
    const char *finite="{\"value\":1e308,\"nested\":[-1.5e-3]}";
    cJSON *parsed=ced_ext_json_parse(finite,strlen(finite),64,error); assert(parsed); cJSON_Delete(parsed);
    assert(ced_ext_rpc_id("ext:1","ext:",NULL));
    assert(!ced_ext_rpc_id("ext:01","ext:",NULL) && !ced_ext_rpc_id("ced:1","ext:",NULL));
    assert(!ced_ext_rpc_id("ext:18446744073709551616","ext:",NULL));
    puts("OK: fragmented/coalesced frames, malformed headers, overflow, quotas and allocation accounting"); return 0;
}
