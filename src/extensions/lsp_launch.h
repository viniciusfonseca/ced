#ifndef CED_EXTENSION_LSP_LAUNCH_H
#define CED_EXTENSION_LSP_LAUNCH_H
#include "catalog.h"
#include "jvm_project.h"
#include "lsp.h"

typedef struct CedExtensionLspLaunch CedExtensionLspLaunch;
CedExtensionLspLaunch *ced_extension_lsp_launch(const char *profile, const CedExtensionRecord *record,
    CedLspLanguage language, const char *root, CedJvmBuild build, const char *gradle_java, const char *maven_settings, char *error);
bool ced_extension_lsp_take(CedExtensionLspLaunch *launch, CedLsp **peer, int *lease, char *error);
void ced_extension_lsp_cancel(CedExtensionLspLaunch *launch);
#endif
