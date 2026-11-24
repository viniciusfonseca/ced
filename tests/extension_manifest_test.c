#include "extension_temp.h"
#include "extensions/catalog.h"
#include "extensions/sha256.h"
#include "file_operations.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void write_file(const char *path, const char *text) { FILE *f=fopen(path,"w"); assert(f); assert(fputs(text,f)>=0); assert(!fclose(f)); }
int main(int argc, char **argv) {
    assert(argc==2); char error[CED_ERROR]={0};
    CedSha256 sha; char digest[65]; ced_sha256_init(&sha); ced_sha256_update(&sha,"a",1); ced_sha256_update(&sha,"bc",2); ced_sha256_finish(&sha,digest);
    assert(!strcmp(digest,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    assert(ced_extension_id_valid("example.text-tools")); assert(!ced_extension_id_valid("ced.foo")); assert(!ced_extension_id_valid("Foo.bar"));
    assert(ced_extension_version_valid("1.2.3")); assert(!ced_extension_version_valid("01.2.3")); assert(!ced_extension_version_valid("1.2.3-beta"));
    assert(!ced_extension_relative_path("../bin/x")); assert(!ced_extension_relative_path("bin//x")); assert(!ced_extension_relative_path("/bin/x"));
    const char *bad[]={"{\"x\":1,\"x\":2}","{\"x\":\"\\u0000\"}","{\"x\":\"\\ud800\"}","{} garbage","\xef\xbb\xbf{}","[[]]","{\"x\":01}","{\"x\":1.}","{\"x\":1e}","{\"x\":1e+}","{\"x\":true}\x01",NULL};
    for (int n=0;bad[n];n++) { cJSON *value=ced_ext_json_parse(bad[n],strlen(bad[n]),1,error); assert(!value); }
    const char *declarative="{\"manifestVersion\":1,\"id\":\"test.config\",\"name\":\"Config\",\"version\":\"1.0.0\",\"ced\":{\"minVersion\":\"0.2.0\",\"apiMajor\":1},\"kind\":\"declarative\",\"contributes\":{\"configuration\":[{\"key\":\"test.config.flag\",\"type\":\"boolean\",\"default\":true,\"scope\":\"user\"}]}}";
    CedExtensionManifest manifest;
    assert(ced_extension_manifest_parse(declarative,strlen(declarative),&manifest,error)); ced_extension_manifest_free(&manifest);
    cJSON *json=cJSON_Parse(declarative); cJSON_AddNumberToObject(json,"unknown",1); char *text=cJSON_PrintUnformatted(json);
    assert(!ced_extension_manifest_parse(text,strlen(text),&manifest,error)); cJSON_free(text); cJSON_Delete(json);
    CedExtensionRecord approved={0}; assert(ced_extension_inspect(argv[1],&approved,error));
    char root[CED_PATH], parent[CED_PATH]; extension_temp("ced-extension-catalog-XXXXXX", root, parent);
    char profile[CED_PATH], workspace[CED_PATH], path[CED_PATH];
    assert(ced_join(profile,sizeof profile,root,"profile")); assert(ced_join(workspace,sizeof workspace,root,"workspace")); assert(!mkdir(workspace,0700));
    assert(ced_extension_install(profile,&approved,error));
    assert(ced_extension_install(profile,&approved,error)); /* identical reinstall */
    CedExtensionCatalog catalog={0}; assert(ced_extension_catalog_load(profile,workspace,&catalog,error));
    assert(catalog.count==1 && catalog.entries[0].valid && catalog.entries[0].enabled && !catalog.workspace_trusted);
    assert(catalog.entries[0].granted==(CED_EXT_READ|CED_EXT_EDIT));
    char installed[CED_PATH]; snprintf(installed,sizeof installed,"%s",catalog.entries[0].package);
    ced_extension_catalog_free(&catalog);
    /* Author a new version in a separate temporary copy. */
    char author_profile[CED_PATH], author_package[CED_PATH];
    assert(ced_join(author_profile,sizeof author_profile,root,"author")); assert(ced_extension_install(author_profile,&approved,error));
    assert(ced_extension_catalog_load(author_profile,workspace,&catalog,error));
    snprintf(author_package,sizeof author_package,"%s",catalog.entries[0].package); ced_extension_catalog_free(&catalog);
    assert(ced_join(path,sizeof path,author_package,"ced-extension.json"));
    FILE *manifest_file=fopen(path,"r"); assert(manifest_file); char manifest_bytes[8192];
    size_t manifest_length=fread(manifest_bytes,1,sizeof manifest_bytes-1,manifest_file); fclose(manifest_file); manifest_bytes[manifest_length]=0;
    json=cJSON_Parse(manifest_bytes); assert(json); assert(cJSON_ReplaceItemInObjectCaseSensitive(json,"version",cJSON_CreateString("0.1.1")));
    cJSON *contributes = cJSON_GetObjectItemCaseSensitive(json, "contributes");
    cJSON *tabs = cJSON_AddArrayToObject(contributes, "tabs");
    const char *command = approved.manifest.commands[0].id;
    char tab_id[257];
    snprintf(tab_id, sizeof tab_id, "%s.panel", approved.manifest.id);
    cJSON *tab = cJSON_CreateObject();
    assert(tabs && tab && cJSON_AddItemToArray(tabs, tab));
    assert(cJSON_AddStringToObject(tab, "id", tab_id));
    assert(cJSON_AddStringToObject(tab, "title", "Tools"));
    assert(cJSON_AddStringToObject(tab, "panel", "sidebar"));
    assert(cJSON_AddStringToObject(tab, "command", command));
    text = cJSON_PrintUnformatted(json); assert(text);
    assert(ced_extension_manifest_parse(text, strlen(text), &manifest, error));
    assert(manifest.tab_count == 1 && !manifest.tabs[0].terminal);
    ced_extension_manifest_free(&manifest); cJSON_free(text);
    assert(cJSON_ReplaceItemInObjectCaseSensitive(tab, "panel", cJSON_CreateString("invalid")));
    text = cJSON_PrintUnformatted(json); assert(text);
    assert(!ced_extension_manifest_parse(text, strlen(text), &manifest, error));
    cJSON_free(text);
    assert(cJSON_ReplaceItemInObjectCaseSensitive(tab, "panel", cJSON_CreateString("terminal")));
    text=cJSON_PrintUnformatted(json);
#ifndef _WIN32
    assert(!chmod(path,0600));
#endif
    write_file(path,text); cJSON_free(text); cJSON_Delete(json);
    CedExtensionRecord update={0}; assert(ced_extension_inspect(author_package,&update,error));
    assert(update.manifest.tab_count == 1 && update.manifest.tabs[0].terminal);
    assert(ced_extension_install(profile,&update,error)); assert(!access(installed,F_OK));
    assert(ced_join(path,sizeof path,author_package,"extra.txt")); write_file(path,"changed after review");
    assert(!ced_extension_install(profile,&update,error)); /* reviewed hash changed */
    ced_extension_record_free(&update); assert(ced_extension_inspect(author_package,&update,error));
    assert(!ced_extension_install(profile,&update,error)); /* same version, new hash */
    ced_extension_record_free(&update);
    assert(ced_extension_set_trusted(profile,workspace,true,error));
    assert(ced_extension_set_enabled(profile,approved.manifest.id,false,error));
    assert(ced_extension_catalog_load(profile,workspace,&catalog,error)); assert(catalog.workspace_trusted && !catalog.entries[0].enabled); ced_extension_catalog_free(&catalog);
    /* Links are rejected even when the linked file is not an entrypoint. */
    assert(ced_join(path,sizeof path,installed,"linked"));
#ifdef _WIN32
    wchar_t *link = ced_windows_wide(path), *target = ced_windows_wide(workspace);
    assert(link && target);
    for (wchar_t *p = link; *p; p++) if (*p == L'/') *p = L'\\';
    for (wchar_t *p = target; *p; p++) if (*p == L'/') *p = L'\\';
    /* Creating symlinks on Windows requires Developer Mode or elevated rights. */
    if (CreateSymbolicLinkW(link, target, SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
        DWORD attributes = GetFileAttributesW(link);
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            CedExtensionRecord linked;
            assert(!ced_extension_inspect(installed,&linked,error));
            assert(RemoveDirectoryW(link));
        } else {
            assert(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version"));
            if (attributes != INVALID_FILE_ATTRIBUTES) assert(RemoveDirectoryW(link));
            puts("SKIP: this Wine filesystem did not expose a Windows reparse point");
        }
    }
    free(target); free(link);
#else
    assert(!symlink("/etc/passwd",path));
    CedExtensionRecord linked; assert(!ced_extension_inspect(installed,&linked,error)); assert(!unlink(path));
#endif
    int lease=ced_extension_lease(profile,approved.manifest.id,approved.manifest.version,false,error); assert(lease>=0);
    assert(!ced_extension_uninstall(profile,approved.manifest.id,error)); close(lease);
    assert(ced_extension_uninstall(profile,approved.manifest.id,error));
    assert(access(installed,F_OK)); /* uninstall removes retained versions too */
    assert(ced_extension_catalog_load(profile,workspace,&catalog,error)); assert(catalog.count==0); ced_extension_catalog_free(&catalog);
    assert(ced_join(path,sizeof path,profile,"extensions.json")); write_file(path,"broken profile");
    assert(!ced_extension_catalog_load(profile,workspace,&catalog,error)); ced_extension_catalog_free(&catalog);
    ced_extension_record_free(&approved);
    assert(ced_file_delete(parent,root,error));
    puts("OK: strict manifests/JSON, SHA-256, local install, permissions, trust persistence, leases, uninstall and corrupt profiles");
    return 0;
}
