#ifndef CED_EXTENSION_MANIFEST_H
#define CED_EXTENSION_MANIFEST_H

#include "commands.h"
#include "json.h"

#define CED_EXTENSION_LIMIT 8
#define CED_EXTENSION_COMMAND_LIMIT 100
#define CED_EXTENSION_TAB_LIMIT 4
#define CED_EXTENSION_MANIFEST_LIMIT (256 * 1024)
#define CED_EXTENSION_PACKAGE_LIMIT (64 * 1024 * 1024)
#define CED_EXTENSION_FILE_LIMIT 2000
#define CED_EXTENSION_API "1.0"
#define CED_EXTENSION_APP_VERSION "0.2.0"
enum { CED_EXT_READ = 1, CED_EXT_EDIT = 2 };
enum { CED_EXT_LSP_JAVA = 1, CED_EXT_LSP_KOTLIN = 2 };

typedef struct {
    char id[257], title[513], category[513];
    CedCommandCondition when;
} CedExtensionCommand;
typedef struct {
    char id[257], title[81];
    size_t command;
    bool terminal;
} CedExtensionTab;
typedef struct {
    char id[129], name[513], version[64], description[513], entrypoint[CED_PATH];
    unsigned capabilities, language_servers;
    bool executable;
    CedExtensionCommand *commands;
    size_t command_count;
    CedExtensionTab tabs[CED_EXTENSION_TAB_LIMIT];
    size_t tab_count;
    cJSON *configuration, *requirements;
} CedExtensionManifest;

const char *ced_extension_platform(void);
bool ced_extension_id_valid(const char *id);
bool ced_extension_version_valid(const char *version);
bool ced_extension_relative_path(const char *path);
bool ced_extension_manifest_parse(const char *bytes, size_t length, CedExtensionManifest *manifest, char *error);
bool ced_extension_manifest_load(const char *package, CedExtensionManifest *manifest, char *error);
void ced_extension_manifest_free(CedExtensionManifest *manifest);

#endif
