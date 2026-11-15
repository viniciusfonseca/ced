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

typedef struct { int font_size; bool terminal_right; } Preferences;

static Preferences preferences_load(void) {
    Preferences settings = {.font_size = CED_FONT_SIZE_DEFAULT};
    char directory[CED_PATH], path[CED_PATH], line[128];
    if (!settings_paths(directory, path)) return settings;
    FILE *f = fopen(path, "r");
    if (!f) return settings;
    while (fgets(line, sizeof line, f)) {
        if (!strchr(line, '\n') && !feof(f)) {
            int ch;
            while ((ch = fgetc(f)) != '\n' && ch != EOF) {}
            continue;
        }
        size_t length = strlen(line);
        while (length && isspace((unsigned char)line[length - 1])) line[--length] = 0;
        if (!strncmp(line, "font_size=", 10)) {
            char *end;
            errno = 0;
            long size = strtol(line + 10, &end, 10);
            if (!errno && end != line + 10 && !*end && size >= CED_FONT_SIZE_MIN && size <= CED_FONT_SIZE_MAX)
                settings.font_size = (int)size;
        } else if (!strcmp(line, "terminal_position=right")) settings.terminal_right = true;
        else if (!strcmp(line, "terminal_position=bottom")) settings.terminal_right = false;
    }
    fclose(f);
    return settings;
}

static bool preferences_save(Preferences settings) {
    char directory[CED_PATH], path[CED_PATH], temp[CED_PATH];
    if (!settings_paths(directory, path)) return false;
    if (mkdir(directory, 0700) && errno != EEXIST) return false;
    int n = snprintf(temp, sizeof temp, "%s.XXXXXX", path);
    if (n < 0 || (size_t)n >= sizeof temp) return false;
    int fd = mkstemp(temp);
    if (fd < 0) return false;
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(temp); return false; }
    bool ok = fprintf(f, "font_size=%d\nterminal_position=%s\n", settings.font_size,
                      settings.terminal_right ? "right" : "bottom") > 0;
    if (fflush(f) || fsync(fd)) ok = false;
    if (fclose(f)) ok = false;
    if (ok && rename(temp, path)) ok = false;
    if (!ok) unlink(temp);
    return ok;
}

int ced_font_size_load(void) { return preferences_load().font_size; }

bool ced_font_size_save(int size) {
    if (size < CED_FONT_SIZE_MIN || size > CED_FONT_SIZE_MAX) return false;
    Preferences settings = preferences_load();
    settings.font_size = size;
    return preferences_save(settings);
}

bool ced_terminal_right_load(void) { return preferences_load().terminal_right; }

bool ced_terminal_right_save(bool right) {
    Preferences settings = preferences_load();
    settings.terminal_right = right;
    return preferences_save(settings);
}
