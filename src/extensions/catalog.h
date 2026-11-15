#ifndef CED_EXTENSION_CATALOG_H
#define CED_EXTENSION_CATALOG_H

#include "manifest.h"

typedef struct {
    CedExtensionManifest manifest;
    char source[CED_PATH], package[CED_PATH], hash[65], error[CED_ERROR];
    bool enabled, valid;
    unsigned granted;
} CedExtensionRecord;
typedef struct {
    CedExtensionRecord entries[CED_EXTENSION_LIMIT];
    size_t count;
    bool workspace_trusted;
    char error[CED_ERROR];
} CedExtensionCatalog;

/* All catalog operations perform filesystem I/O and belong on a worker. */
bool ced_extension_inspect(const char *source, CedExtensionRecord *record, char *error);
bool ced_extension_catalog_load(const char *profile, const char *workspace, CedExtensionCatalog *catalog, char *error);
bool ced_extension_install(const char *profile, const CedExtensionRecord *approved, char *error);
bool ced_extension_set_enabled(const char *profile, const char *id, bool enabled, char *error);
bool ced_extension_set_trusted(const char *profile, const char *workspace, bool trusted, char *error);
bool ced_extension_uninstall(const char *profile, const char *id, char *error);
/* Shared lease prevents another window from deleting an executing package. */
int ced_extension_lease(const char *profile, const char *id, const char *version, bool exclusive, char *error);
void ced_extension_record_free(CedExtensionRecord *record);
void ced_extension_catalog_free(CedExtensionCatalog *catalog);

#endif
