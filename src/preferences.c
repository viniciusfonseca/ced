#include "preferences.h"
#include "workspace.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool settings_paths(char *directory, char *path) {
    const char *home = getenv("HOME");
    return home && *home && ced_join(directory, CED_PATH, home, ".ced") &&
           ced_join(path, CED_PATH, directory, "settings.conf");
}

int ced_font_size_load(void) {
    char directory[CED_PATH], path[CED_PATH], line[64];
    if (!settings_paths(directory, path)) return CED_FONT_SIZE_DEFAULT;
    FILE *f = fopen(path, "r");
    if (!f) return CED_FONT_SIZE_DEFAULT;
    bool read = fgets(line, sizeof line, f) != NULL;
    fclose(f);
    if (!read || strncmp(line, "font_size=", 10)) return CED_FONT_SIZE_DEFAULT;
    char *end;
    errno = 0;
    long size = strtol(line + 10, &end, 10);
    if (errno || end == line + 10) return CED_FONT_SIZE_DEFAULT;
    while (isspace((unsigned char)*end)) end++;
    if (*end || size < CED_FONT_SIZE_MIN || size > CED_FONT_SIZE_MAX) return CED_FONT_SIZE_DEFAULT;
    return (int)size;
}

bool ced_font_size_save(int size) {
    if (size < CED_FONT_SIZE_MIN || size > CED_FONT_SIZE_MAX) return false;
    char directory[CED_PATH], path[CED_PATH], temp[CED_PATH];
    if (!settings_paths(directory, path)) return false;
    if (mkdir(directory, 0700) && errno != EEXIST) return false;
    int n = snprintf(temp, sizeof temp, "%s.XXXXXX", path);
    if (n < 0 || (size_t)n >= sizeof temp) return false;
    int fd = mkstemp(temp);
    if (fd < 0) return false;
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(temp); return false; }
    bool ok = fprintf(f, "font_size=%d\n", size) > 0;
    if (fflush(f) || fsync(fd)) ok = false;
    if (fclose(f)) ok = false;
    if (ok && rename(temp, path)) ok = false;
    if (!ok) unlink(temp);
    return ok;
}
