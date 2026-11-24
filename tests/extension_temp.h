#ifndef CED_TEST_EXTENSION_TEMP_H
#define CED_TEST_EXTENSION_TEMP_H

#include "extensions/files.h"
#include <assert.h>
#include <stdlib.h>

static void extension_temp(const char *name, char root[CED_PATH], char parent[CED_PATH]) {
#ifdef _WIN32
    ced_windows_init();
    wchar_t windows_temp[MAX_PATH];
    DWORD length = GetTempPathW(MAX_PATH, windows_temp);
    assert(length && length < MAX_PATH);
    char *temp = ced_windows_utf8(windows_temp);
    assert(temp && realpath(temp, parent));
    free(temp);
#else
    assert(realpath("/tmp", parent));
#endif
    char path[CED_PATH];
    assert(ced_join(path, sizeof path, parent, name) && ced_ext_mkdtemp(path) && realpath(path, root));
}

#endif
