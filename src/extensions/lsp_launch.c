#include "lsp_launch.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct CedExtensionLspLaunch {
    pthread_mutex_t mutex;
    bool done, abandoned;
    CedLspLanguage language;
    CedJvmBuild build;
    char profile[CED_PATH], package[CED_PATH], root[CED_PATH], gradle_java[CED_PATH], maven_settings[CED_PATH], server_executable[CED_PATH];
    char id[129], version[64], hash[65], error[CED_ERROR];
    CedLsp *peer;
    int lease;
};

static void dispose(CedExtensionLspLaunch *launch) {
    ced_lsp_stop(launch->peer);
    if (launch->lease>=0) close(launch->lease);
    pthread_mutex_destroy(&launch->mutex);
    free(launch);
}

static void *worker(void *data) {
    CedExtensionLspLaunch *job=data;
    job->lease=ced_extension_lease(job->profile,job->id,job->version,false,job->error);
    CedExtensionRecord verified={0};
    char executable[CED_PATH], label[64];
    bool java=job->language==CED_LSP_JAVA;
    unsigned language=java ? CED_EXT_LSP_JAVA : CED_EXT_LSP_KOTLIN;
    bool valid=job->lease>=0 && ced_extension_inspect(job->package,&verified,job->error) &&
        !strcmp(job->hash,verified.hash) && (verified.manifest.language_servers & language) &&
        ced_join(executable,sizeof executable,job->package,verified.manifest.entrypoint);
    ced_extension_record_free(&verified);
    if (!valid && !*job->error) snprintf(job->error,sizeof job->error,"Installed language provider changed or is unavailable.");
    snprintf(label,sizeof label,"%s LSP · %s",java ? "Java" : "Kotlin",
        job->build==CED_JVM_MAVEN ? "Maven" : job->build==CED_JVM_GRADLE ? "Gradle" : "Source");
    cJSON *settings=NULL;
    if (java) {
        settings=cJSON_CreateObject();
        cJSON *java_config=cJSON_AddObjectToObject(settings,"java");
        cJSON *imports=cJSON_AddObjectToObject(java_config,"import");
        cJSON *maven=cJSON_AddObjectToObject(imports,"maven");
        cJSON *gradle=cJSON_AddObjectToObject(imports,"gradle");
        cJSON_AddBoolToObject(maven,"enabled",job->build==CED_JVM_MAVEN);
        cJSON_AddBoolToObject(gradle,"enabled",job->build==CED_JVM_GRADLE);
        if (*job->maven_settings) {
            cJSON *configuration=cJSON_AddObjectToObject(java_config,"configuration");
            cJSON *maven_config=cJSON_AddObjectToObject(configuration,"maven");
            cJSON_AddStringToObject(maven_config,"userSettings",job->maven_settings);
        }
        if (job->build==CED_JVM_GRADLE && *job->gradle_java) {
            cJSON *jdk=cJSON_AddObjectToObject(gradle,"java");
            cJSON_AddStringToObject(jdk,"home",job->gradle_java);
        }
    }
    char *json=settings ? cJSON_PrintUnformatted(settings) : NULL;
    cJSON_Delete(settings);
    const char *gradle_jdk=!java && job->build==CED_JVM_GRADLE ? job->gradle_java : "";
    const char *argv[]={executable,"--language-server",java ? "java" : "kotlin",job->root,
        *gradle_jdk || *job->server_executable ? gradle_jdk : NULL,
        *job->server_executable ? job->server_executable : NULL,NULL};
    CedLspServer server={job->root,label,job->language,argv,json,java ? NULL : job->maven_settings};
    pthread_mutex_lock(&job->mutex);
    if (valid && !job->abandoned) job->peer=ced_lsp_start_server(&server,job->error,sizeof job->error);
    cJSON_free(json);
    job->done=true;
    bool dispose_now=job->abandoned;
    pthread_mutex_unlock(&job->mutex);
    if (dispose_now) dispose(job);
    return NULL;
}

CedExtensionLspLaunch *ced_extension_lsp_launch(const char *profile, const CedExtensionRecord *record,
    CedLspLanguage language, const char *root, CedJvmBuild build, const char *gradle_java,
    const char *maven_settings, const char *server_executable, char *error) {
    CedExtensionLspLaunch *job=calloc(1,sizeof *job);
    if (job && !pthread_mutex_init(&job->mutex,NULL)) {
        job->language=language; job->build=build; job->lease=-1;
        strcpy(job->profile,profile); strcpy(job->package,record->package); strcpy(job->root,root);
        strcpy(job->gradle_java,gradle_java); strcpy(job->maven_settings,maven_settings);
        strcpy(job->server_executable,server_executable);
        strcpy(job->id,record->manifest.id); strcpy(job->version,record->manifest.version); strcpy(job->hash,record->hash);
        pthread_t thread;
        if (!pthread_create(&thread,NULL,worker,job)) { pthread_detach(thread); return job; }
        pthread_mutex_destroy(&job->mutex);
    }
    free(job); snprintf(error,CED_ERROR,"Could not start language server worker.");
    return NULL;
}

bool ced_extension_lsp_take(CedExtensionLspLaunch *job, CedLsp **peer, int *lease, char *error) {
    if (!job || pthread_mutex_trylock(&job->mutex)) return false;
    bool done=job->done;
    if (done) {
        *peer=job->peer; job->peer=NULL;
        if (*peer) { *lease=job->lease; job->lease=-1; }
        snprintf(error,CED_ERROR,"%s",job->error);
    }
    pthread_mutex_unlock(&job->mutex);
    if (done) dispose(job);
    return done;
}

void ced_extension_lsp_cancel(CedExtensionLspLaunch *job) {
    if (!job) return;
    pthread_mutex_lock(&job->mutex);
    bool done=job->done;
    job->abandoned=true;
    pthread_mutex_unlock(&job->mutex);
    if (done) dispose(job);
}
