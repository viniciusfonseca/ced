#include "jvm_project.h"
#include "file_operations.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

bool ced_jvm_contains(const char *root, const char *path) { return ced_path_within(path, root); }

bool ced_jvm_absolute(const char *path) {
#ifdef _WIN32
    return (isalpha((unsigned char)path[0]) && path[1] == ':' && (path[2] == '/' || path[2] == '\\')) ||
        ((path[0] == '/' && path[1] == '/') || (path[0] == '\\' && path[1] == '\\'));
#else
    return path[0] == '/';
#endif
}

static bool regular(const char *root, const char *name) {
    char path[CED_PATH], canonical[CED_PATH]; struct stat st;
    return ced_join(path,sizeof path,root,name) && realpath(path,canonical) &&
        ced_jvm_contains(root,canonical) && !stat(canonical,&st) && S_ISREG(st.st_mode);
}

static void parent(char *path) {
    char directory[CED_PATH];
    if (ced_parent_path(path, directory, sizeof directory)) strcpy(path, directory);
}

static bool gradle_root(const char *root) {
    return regular(root,"settings.gradle") || regular(root,"settings.gradle.kts");
}

static bool gradle_build(const char *root) {
    return regular(root,"build.gradle") || regular(root,"build.gradle.kts");
}

static bool configured(const char *workspace, const char *setting, char *path) {
    char joined[CED_PATH];
    bool absolute = ced_jvm_absolute(setting);
    if (!absolute && !ced_join(joined,sizeof joined,workspace,setting)) return false;
    return realpath(absolute ? setting : joined,path) && ced_jvm_contains(workspace,path);
}

bool ced_jvm_maven_settings(const char *workspace, const char *setting, char *path, char *error) {
    *path=0;
    if (!*setting) return true;
    const char *home=getenv("HOME");
    char expanded[CED_PATH];
    bool resolved;
    if (ced_jvm_absolute(setting)) resolved=realpath(setting,path)!=NULL;
    else if (!strncmp(setting,"~/",2))
        resolved=home && ced_jvm_absolute(home) && ced_join(expanded,sizeof expanded,home,setting+2) && realpath(expanded,path);
    else resolved=configured(workspace,setting,path);
    struct stat st;
    FILE *file = resolved && !stat(path,&st) && S_ISREG(st.st_mode) ? fopen(path,"rb") : NULL;
    if (file) { fclose(file); return true; }
    *path=0;
    snprintf(error,CED_ERROR,"maven.settingsPath must name a readable Maven settings file.");
    return false;
}

bool ced_jvm_project(const char *workspace, const char *document, const CedJvmSettings *settings,
    char *root, CedJvmBuild *build, char *error) {
    *root=*error=0; *build=CED_JVM_SOURCE;
    char directory[CED_PATH]; struct stat st;
    if (!realpath(document,directory) || !ced_jvm_contains(workspace,directory)) {
        snprintf(error,CED_ERROR,"The source file is outside the trusted workspace."); return false;
    }
    parent(directory);
    if (*settings->pom && *settings->gradle) {
        snprintf(error,CED_ERROR,"Set either maven.pomPath or gradle.projectPath for this workspace."); return false;
    }
    if (*settings->pom) {
        if (!configured(workspace,settings->pom,root) || strcmp(ced_basename(root),"pom.xml") ||
            stat(root,&st) || !S_ISREG(st.st_mode)) {
            snprintf(error,CED_ERROR,"maven.pomPath must name a pom.xml inside the trusted workspace."); return false;
        }
        parent(root); *build=CED_JVM_MAVEN;
    } else if (*settings->gradle) {
        if (!configured(workspace,settings->gradle,root) || (!gradle_root(root) && !gradle_build(root))) {
            snprintf(error,CED_ERROR,"gradle.projectPath must name a Gradle project inside the trusted workspace."); return false;
        }
        *build=CED_JVM_GRADLE;
    } else {
        snprintf(root,CED_PATH,"%s",workspace);
        char path[CED_PATH];
        strcpy(path,directory);
        for (;;) {
            if (gradle_root(path)) { strcpy(root,path); *build=CED_JVM_GRADLE; break; }
            if (regular(path,"pom.xml") && *build!=CED_JVM_GRADLE) { strcpy(root,path); *build=CED_JVM_MAVEN; }
            else if (*build==CED_JVM_SOURCE && gradle_build(path)) { strcpy(root,path); *build=CED_JVM_GRADLE; }
            if (strlen(path) <= strlen(workspace)) break;
            parent(path);
            if (!ced_jvm_contains(workspace,path)) break;
        }
    }
    if (ced_jvm_contains(root,directory)) return true;
    snprintf(error,CED_ERROR,"The source file is outside the configured JVM project.");
    return false;
}
