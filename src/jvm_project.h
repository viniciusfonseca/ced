#ifndef CED_JVM_PROJECT_H
#define CED_JVM_PROJECT_H
#include "preferences.h"

typedef enum { CED_JVM_SOURCE, CED_JVM_MAVEN, CED_JVM_GRADLE } CedJvmBuild;

/* Paths are canonical and must stay within the trusted workspace. */
bool ced_jvm_contains(const char *root, const char *path);
bool ced_jvm_maven_settings(const char *workspace, const char *setting, char *path, char *error);
bool ced_jvm_project(const char *workspace, const char *document, const CedJvmSettings *settings,
    char *root, CedJvmBuild *build, char *error);
#endif
