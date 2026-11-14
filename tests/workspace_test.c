#include "workspace.h"
#include "preferences.h"
#include "file_operations.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char root[CED_PATH], error[CED_ERROR];

static void path_for(char *out, const char *name) { assert(ced_join(out, CED_PATH, root, name)); }

static void write_file(const char *name, const void *data, size_t size) {
    char path[CED_PATH]; path_for(path, name);
    FILE *f = fopen(path, "wb"); assert(f);
    assert(fwrite(data, 1, size, f) == size);
    assert(!fclose(f));
}

static void write_text(const char *name, const char *text) { write_file(name, text, strlen(text)); }

static void command(const char *const argv[]) {
    char *out = NULL;
    int result = ced_command(argv, &out, NULL);
    if (result) fprintf(stderr, "Command failed: %s\n", out ? out : "(no output)");
    free(out);
    assert(result == 0);
}

static void test_command_buffers(void) {
    char *out = NULL;
    size_t length = 99;
    const char *empty[] = {"/usr/bin/true", NULL};
    assert(!ced_command(empty, &out, &length));
    if (length) fprintf(stderr, "Unexpected command output: %s\n", out);
    assert(out && !*out && !length); free(out);
    const char *binary[] = {"/bin/sh", "-c", "printf 'a\\000b'", NULL};
    assert(!ced_command(binary, &out, &length));
    assert(length == 3 && !memcmp(out, "a\0b", 3) && !out[length]); free(out);
    const char *large[] = {"/bin/sh", "-c", "dd if=/dev/zero bs=1048576 count=8 2>/dev/null", NULL};
    assert(!ced_command(large, &out, &length));
    assert(length == 8 * 1024 * 1024 && !out[length]); free(out);
    const char *overflow[] = {"/bin/sh", "-c", "dd if=/dev/zero bs=1048576 count=9 2>/dev/null", NULL};
    assert(ced_command(overflow, &out, &length) == -1);
    assert(out && length <= 8 * 1024 * 1024 && !out[length]); free(out);
    puts("OK: empty/binary command output, growing buffers and output limit");
}

static void test_documents(void) {
    char path[CED_PATH]; path_for(path, "source.c");
    write_text("source.c", "int main(void) {\r\n    return 0;\r\n}\r\n");
    CedDocument doc = {0};
    assert(ced_document_open(&doc, path, error));
    assert(doc.crlf && !strchr(doc.text, '\r'));
    assert(!ced_document_dirty(&doc));
    strcpy(doc.text, "int main(void) {\n    return 42;\n}\n");
    assert(ced_document_dirty(&doc));
    assert(ced_document_save(&doc, path, false, error));
    assert(!ced_document_dirty(&doc));
    FILE *f = fopen(path, "rb"); assert(f);
    char bytes[128] = {0};
    assert(fread(bytes, 1, sizeof bytes - 1, f) > 0); fclose(f);
    assert(strstr(bytes, "42;\r\n"));
    write_text("source.c", "external edit\n");
    strcpy(doc.text, "my edit\n");
    assert(!ced_document_save(&doc, path, false, error));
    assert(strstr(error, "changed on disk"));
    char copy[CED_PATH]; path_for(copy, "copy.c");
    assert(ced_document_save(&doc, copy, false, error));
    ced_document_free(&doc);

    write_file("binary.bin", "abc\0def", 7);
    path_for(path, "binary.bin");
    assert(!ced_document_open(&doc, path, error));
    path_for(path, "large.txt");
    f = fopen(path, "wb"); assert(f);
    for (int i = 0; i < CED_TEXT_CAP; i++) fputc('x', f);
    fclose(f);
    assert(!ced_document_open(&doc, path, error));
    path_for(path, "limit.txt");
    f = fopen(path, "wb"); assert(f);
    for (int i = 0; i < CED_TEXT_CAP - 1; i++) fputc('x', f);
    fclose(f);
    assert(ced_document_open(&doc, path, error));
    assert(doc.length == CED_TEXT_CAP - 1 && !doc.text[doc.length]);
    ced_document_free(&doc);
    path_for(path, "executable.sh");
    write_text("executable.sh", "echo original\n");
    assert(!chmod(path, 0755));
    assert(ced_document_open(&doc, path, error));
    strcpy(doc.text, "echo changed\n");
    assert(ced_document_save(&doc, path, false, error));
    struct stat st; assert(!stat(path, &st)); assert((st.st_mode & 0777) == 0755);
    ced_document_free(&doc);
    assert(!strcmp(ced_language("a.ts"), "ts"));
    assert(!strcmp(ced_language("no_extension"), "text"));
    puts("OK: open/save, CRLF, conflict detection, binary/size limits, permissions");
}

static void test_workspace_search(void) {
    char path[CED_PATH]; path_for(path, "nested"); assert(!mkdir(path, 0700));
    write_text("nested/find.txt", "first\nneedle here\nneedle needle\n");
    write_text("nested/unicode.txt", "\té alvo\n");
    path_for(path, "node_modules"); assert(!mkdir(path, 0700));
    write_text("node_modules/ignored.txt", "needle");
    path_for(path, "loop"); assert(!symlink(root, path));
    CedWorkspace ws = {0};
    assert(ced_workspace_open(&ws, root, error));
    bool nested = false;
    for (int i = 0; i < ws.count; i++) {
        assert(!strstr(ws.entries[i].path, "node_modules"));
        assert(strcmp(ws.entries[i].path, "loop"));
        if (!strcmp(ws.entries[i].path, "nested")) { nested = true; ws.entries[i].expanded = true; }
    }
    assert(nested);
    for (int i = 0; i < 150; i++) {
        char name[64]; snprintf(name, sizeof name, "nested/item-%03d.txt", i);
        write_text(name, "");
    }
    assert(ced_workspace_open(&ws, root, error));
    assert(ws.count > 150);
    for (int i = 0; i < ws.count; i++)
        if (!strcmp(ws.entries[i].path, "nested")) assert(ws.entries[i].expanded);
    static CedSearch search;
    pthread_mutex_init(&search.mutex, NULL);
    assert(ced_search_start(&search, &ws, "needle"));
    ced_search_finish(&search);
    assert(search.count == 3);
    assert(search.hits[0].line == 2 && search.hits[0].column == 1);
    assert(search.hits[2].line == 3 && search.hits[2].column == 8);
    assert(ced_search_start(&search, &ws, "Needle"));
    ced_search_finish(&search); assert(search.count == 0);
    assert(ced_search_start(&search, &ws, "alvo"));
    ced_search_finish(&search); assert(search.count == 1 && search.hits[0].column == 7);
    assert(ced_search_start(&search, &ws, "needle"));
    ced_search_cancel(&search);
    assert(!search.running);
    assert(ced_search_start(&search, &ws, "")); assert(!search.count);
    pthread_mutex_destroy(&search.mutex);
    ced_workspace_free(&ws);
    puts("OK: recursive tree, exclusions, symlinks, refresh, async search and locations");
}

static CedChange *find_change(CedGit *git, const char *path) {
    for (int i = 0; i < git->count; i++) if (!strcmp(git->changes[i].path, path)) return &git->changes[i];
    return NULL;
}

static void test_git(void) {
    const char *init[] = {"git", "-C", root, "init", "-b", "main", NULL}; command(init);
    const char *name[] = {"git", "-C", root, "config", "user.name", "CED Test", NULL}; command(name);
    const char *email[] = {"git", "-C", root, "config", "user.email", "test@example.invalid", NULL}; command(email);
    CedGit git = {0};
    assert(ced_git_refresh(&git, root, error));
    assert(git.available && !strcmp(git.branch, "main"));
    write_text("a [literal] ' ü.txt", "before\n");
    assert(ced_git_stage(&git, "a [literal] ' ü.txt", false, error));
    assert(ced_git_stage(&git, "a [literal] ' ü.txt", true, error));
    assert(ced_git_stage(&git, "a [literal] ' ü.txt", false, error));
    assert(ced_git_refresh(&git, root, error));
    CedChange *change = find_change(&git, "a [literal] ' ü.txt"); assert(change && change->index == 'A');
    char *before, *after;
    assert(ced_git_versions(&git, change, true, &before, &after, error));
    assert(!*before && !strcmp(after, "before\n")); free(before); free(after);
    assert(ced_git_commit(&git, "initial ' quoted $(literal)", error));
    write_text("a [literal] ' ü.txt", "after\n");
    assert(ced_git_refresh(&git, root, error));
    change = find_change(&git, "a [literal] ' ü.txt"); assert(change && change->worktree == 'M');
    assert(ced_git_versions(&git, change, false, &before, &after, error));
    assert(!strcmp(before, "before\n") && !strcmp(after, "after\n")); free(before); free(after);
    assert(ced_git_stage(&git, "a [literal] ' ü.txt", false, error));
    write_text("a [literal] ' ü.txt", "third\n");
    assert(ced_git_refresh(&git, root, error));
    change = find_change(&git, "a [literal] ' ü.txt"); assert(change && change->index == 'M' && change->worktree == 'M');
    assert(ced_git_versions(&git, change, true, &before, &after, error));
    assert(!strcmp(before, "before\n") && !strcmp(after, "after\n")); free(before); free(after);
    assert(ced_git_versions(&git, change, false, &before, &after, error));
    assert(!strcmp(before, "after\n") && !strcmp(after, "third\n")); free(before); free(after);
    assert(ced_git_stage(&git, "a [literal] ' ü.txt", true, error));
    assert(ced_git_stage(&git, "a [literal] ' ü.txt", false, error));
    assert(ced_git_commit(&git, "change", error));
    const char *move[] = {"git", "-C", root, "mv", "--", "a [literal] ' ü.txt", "renamed.txt", NULL}; command(move);
    assert(ced_git_refresh(&git, root, error));
    change = find_change(&git, "renamed.txt"); assert(change && change->index == 'R' && change->original[0]);
    assert(ced_git_versions(&git, change, true, &before, &after, error));
    assert(!strcmp(before, "third\n") && !strcmp(after, "third\n")); free(before); free(after);
    assert(ced_git_stage(&git, "renamed.txt", true, error));
    assert(ced_git_refresh(&git, root, error));
    for (int i = 0; i < git.count; i++) assert(git.changes[i].index == ' ' || git.changes[i].index == '?');
    change = find_change(&git, "a [literal] ' ü.txt"); assert(change && change->worktree == 'D');
    assert(ced_git_versions(&git, change, false, &before, &after, error));
    assert(!strcmp(before, "third\n") && !*after); free(before); free(after);
    char nested[CED_PATH]; path_for(nested, "nested");
    assert(ced_git_refresh(&git, nested, error)); assert(!strcmp(git.root, root));
    const char *add_all[] = {"git", "-C", root, "add", "--all", NULL}; command(add_all);
    assert(ced_git_commit(&git, "clean tree", error));
    assert(ced_git_refresh(&git, root, error));
    assert(!git.count && !git.changes);
    ced_git_free(&git);
    puts("OK: Git status, literal filenames, unborn unstage, stage, commit, staged/unstaged diff, rename, delete, subdirectory");
}

static void test_preferences(void) {
    const char *home = getenv("HOME");
    char *original_home = home ? strdup(home) : NULL;
    assert(!setenv("HOME", root, 1));
    assert(ced_font_size_load() == CED_FONT_SIZE_DEFAULT);
    assert(ced_font_size_save(21));
    assert(ced_font_size_load() == 21);
    assert(ced_font_size_save(CED_FONT_SIZE_MIN));
    assert(ced_font_size_load() == CED_FONT_SIZE_MIN);
    assert(ced_font_size_save(CED_FONT_SIZE_MAX));
    assert(ced_font_size_load() == CED_FONT_SIZE_MAX);
    assert(!ced_font_size_save(CED_FONT_SIZE_MIN - 1));
    assert(!ced_font_size_save(CED_FONT_SIZE_MAX + 1));
    assert(ced_font_size_load() == CED_FONT_SIZE_MAX);
    const char *invalid[] = {"", "font_size=oops\n", "font_size=7\n", "font_size=33\n",
                             "font_size=21oops\n", "font_size=999999999999999999999999\n"};
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        write_text(".ced/settings.conf", invalid[i]);
        assert(ced_font_size_load() == CED_FONT_SIZE_DEFAULT);
    }
    char path[CED_PATH];
    path_for(path, ".ced/settings.conf");
    assert(!unlink(path));
    assert(!mkdir(path, 0700));
    assert(!ced_font_size_save(18));
    assert(!rmdir(path));
    assert(ced_font_size_save(18));
    assert(ced_font_size_load() == 18);
    assert(!unsetenv("HOME"));
    assert(ced_font_size_load() == CED_FONT_SIZE_DEFAULT);
    assert(!ced_font_size_save(18));
    if (original_home) { assert(!setenv("HOME", original_home, 1)); free(original_home); }
    puts("OK: persistent zoom, defaults, bounds, invalid settings and write failures");
}

static void test_file_operations(void) {
    char directory[CED_PATH], file[CED_PATH], renamed[CED_PATH], folder[CED_PATH], next[CED_PATH];
    path_for(directory, "actions"); assert(!mkdir(directory, 0700));
    assert(ced_file_create(root, directory, "first ü.txt", file, error));
    write_text("actions/first ü.txt", "keep this\n");
    assert(!ced_file_create(root, directory, "first ü.txt", next, error));
    assert(ced_file_create(root, directory, "existing.txt", next, error));
    assert(!ced_directory_create(root, directory, "existing.txt", next, error));
    assert(!ced_file_rename(root, file, "existing.txt", renamed, error));
    CedDocument doc = {0}; assert(ced_document_open(&doc, file, error));
    assert(!strcmp(doc.text, "keep this\n")); ced_document_free(&doc);
    assert(ced_file_rename(root, file, "renamed ' ü.md", renamed, error));
    assert(access(file, F_OK) != 0 && !access(renamed, F_OK));
    assert(ced_file_rename(root, renamed, "renamed ' ü.md", next, error));
    assert(!strcmp(renamed, next));
    const char *invalid[] = {"", ".", "..", "../escape", "a/b", "line\nbreak"};
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        assert(!ced_file_create(root, directory, invalid[i], next, error));
        assert(!ced_directory_create(root, directory, invalid[i], next, error));
        assert(!ced_file_rename(root, renamed, invalid[i], next, error));
    }
    assert(!ced_file_delete(root, root, error));
    assert(!ced_file_rename(root, root, "moved-root", next, error));
    char outside_template[] = "/tmp/ced-outside-XXXXXX", outside[CED_PATH], sentinel[CED_PATH], link[CED_PATH];
    assert(mkdtemp(outside_template)); assert(realpath(outside_template, outside));
    assert(ced_join(sentinel, sizeof sentinel, outside, "keep.txt"));
    FILE *f = fopen(sentinel, "w"); assert(f); fputs("outside", f); fclose(f);
    assert(!ced_file_create(root, outside, "forbidden.txt", next, error));
    assert(!ced_directory_create(root, outside, "forbidden", next, error));
    assert(!ced_file_delete(root, sentinel, error));
    assert(ced_join(link, sizeof link, directory, "outside-link")); assert(!symlink(outside, link));
    assert(!ced_file_create(root, link, "forbidden.txt", next, error));
    assert(!ced_directory_create(root, link, "forbidden", next, error));
    char traversal[CED_PATH]; assert(ced_join(traversal, sizeof traversal, directory, "../actions"));
    assert(!ced_file_create(root, traversal, "forbidden.txt", next, error));
    assert(ced_directory_create(root, directory, "group", folder, error));
    struct stat folder_stat;
    assert(!stat(folder, &folder_stat) && S_ISDIR(folder_stat.st_mode));
    assert(!ced_directory_create(root, directory, "group", next, error));
    assert(!ced_file_create(root, directory, "group", next, error));
    assert(ced_file_create(root, folder, "child.c", file, error));
    assert(ced_file_rename(root, folder, "renamed-group", next, error));
    assert(access(folder, F_OK) != 0);
    assert(ced_join(file, sizeof file, next, "child.c")); assert(!access(file, F_OK));
    assert(ced_file_delete(root, directory, error));
    assert(access(directory, F_OK) != 0 && !access(sentinel, F_OK));
    assert(!unlink(sentinel)); assert(!rmdir(outside));
    assert(ced_path_within("/project/src/a.c", "/project/src"));
    assert(!ced_path_within("/project/src-other/a.c", "/project/src"));
    assert(ced_parent_path("/a.c", next, sizeof next) && !strcmp(next, "/"));
    assert(ced_markdown_file("README.md") && ced_markdown_file("README.MD") && ced_markdown_file("notes.markdown"));
    assert(!ced_markdown_file("notes.md.c"));
    puts("OK: file/folder creation, exclusive rename, directory rename/delete, collisions, invalid names, root protection, symlink containment and Markdown detection");
}

int main(void) {
    char temp[] = "/tmp/ced-test-XXXXXX";
    assert(mkdtemp(temp));
    assert(realpath(temp, root));
    test_command_buffers();
    test_preferences();
    test_file_operations();
    test_documents();
    test_workspace_search();
    test_git();
    const char *remove[] = {"rm", "-rf", "--", root, NULL}; command(remove);
    puts("Todos os testes passaram.");
    return 0;
}
