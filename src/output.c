#include "output.h"
#include "documents.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static char buffer[CED_OUTPUT_CAP];
static size_t used;
static uint64_t generation=1;

void ced_output_write(const char *source, const char *bytes, size_t length) {
    if (!length) return;
    char entry[8192];
    size_t size=(size_t)snprintf(entry,sizeof entry,"[%.128s] ",source);
    for (size_t i=0;i<length && size+5<sizeof entry;) {
        unsigned char c=(unsigned char)bytes[i++];
        if (c=='\r') {
            if (i==length || bytes[i]!='\n') entry[size++]='\n';
        } else if (c<128) {
            if (c=='\n' || c=='\t' || (c>=32 && c<127)) entry[size++]=(char)c;
        } else {
            size_t width=c<0xc2 ? 0 : c<0xe0 ? 2 : c<0xf0 ? 3 : c<0xf5 ? 4 : 0;
            if (width && width-1<=length-i && ced_utf8_valid(bytes+i-1,width)) {
                memcpy(entry+size,bytes+i-1,width); size+=width; i+=width-1;
            } else entry[size++]='?';
        }
    }
    if (entry[size-1]!='\n') entry[size++]='\n';
    pthread_mutex_lock(&lock);
    if (used+size>=sizeof buffer) {
        size_t drop=used+size-(sizeof buffer-1);
        while (drop<used && buffer[drop-1]!='\n') drop++;
        memmove(buffer,buffer+drop,used-drop);
        used-=drop;
    }
    memcpy(buffer+used,entry,size);
    used+=size; buffer[used]=0; generation++;
    pthread_mutex_unlock(&lock);
}

void ced_output_text(const char *source, const char *text) {
    ced_output_write(source,text,strlen(text));
}

void ced_output_copy(char out[CED_OUTPUT_CAP], uint64_t *revision) {
    pthread_mutex_lock(&lock);
    if (*revision!=generation) { memcpy(out,buffer,used+1); *revision=generation; }
    pthread_mutex_unlock(&lock);
}

void ced_output_clear(void) {
    pthread_mutex_lock(&lock);
    used=0; *buffer=0; generation++;
    pthread_mutex_unlock(&lock);
}
