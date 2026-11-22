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

static void assert_ignored(CedWorkspace *ws, const char *path, bool ignored) {
    for (int i = 0; i < ws->count; i++) if (!strcmp(ws->entries[i].path, path)) {
        assert(ws->entries[i].ignored == ignored); return;
    }
    fprintf(stderr, "Missing tree entry: %s\n", path); assert(false);
}

static void test_workspace_ignored(void) {
    char repo[CED_PATH], folder[CED_PATH]; path_for(repo, "ignore-tree"); assert(!mkdir(repo, 0700));
    const char *init[] = {"git", "-C", repo, "init", "-q", NULL}; command(init);
    write_text("ignore-tree/.gitignore", "*.tmp\n!keep.tmp\ncache/\n");
    write_text("ignore-tree/hidden.tmp", "search-token"); write_text("ignore-tree/keep.tmp", "search-token");
    write_text("ignore-tree/tracked.tmp", "search-token"); write_text("ignore-tree/odd\nname.tmp", "ignored");
    const char *add[] = {"git", "-C", repo, "add", "-f", "--", "tracked.tmp", NULL}; command(add);
    path_for(folder, "ignore-tree/cache"); assert(!mkdir(folder, 0700));
    write_text("ignore-tree/cache/file.c", "search-token");
    path_for(folder, "ignore-tree/nested"); assert(!mkdir(folder, 0700));
    write_text("ignore-tree/nested/.gitignore", "*.log\n!keep.log\n");
    write_text("ignore-tree/nested/hidden.log", "ignored"); write_text("ignore-tree/nested/keep.log", "visible");
    write_text("ignore-tree/.git/info/exclude", "local.txt\n"); write_text("ignore-tree/local.txt", "ignored");
    path_for(folder, "ignore-tree/vendor"); assert(!mkdir(folder, 0700));
    path_for(folder, "ignore-tree/vendor/module"); assert(!mkdir(folder, 0700));
    write_text("ignore-tree/vendor/module/inside.tmp", "outside the parent Git index");
    const char *gitlink[] = {"git", "-C", repo, "update-index", "--add", "--cacheinfo",
                            "160000,1111111111111111111111111111111111111111,vendor/module", NULL};
    command(gitlink);
    write_text("ignore-tree/:literal.tmp", "ignored");
    write_text("ignore-tree/zz-late.tmp", "ignored");
    CedWorkspace ws = {0}; assert(ced_workspace_open(&ws, repo, error));
    assert_ignored(&ws, "hidden.tmp", true); assert_ignored(&ws, "odd\nname.tmp", true);
    assert_ignored(&ws, "keep.tmp", false); assert_ignored(&ws, "tracked.tmp", false);
    assert_ignored(&ws, ":literal.tmp", true); assert_ignored(&ws, "zz-late.tmp", true);
    assert_ignored(&ws, "vendor/module", false); assert_ignored(&ws, "vendor/module/inside.tmp", false);
    assert_ignored(&ws, "cache", true); assert_ignored(&ws, "cache/file.c", true);
    assert_ignored(&ws, "nested", false); assert_ignored(&ws, "nested/hidden.log", true);
    assert_ignored(&ws, "nested/keep.log", false); assert_ignored(&ws, "local.txt", true);
    static CedSearch search;
    pthread_mutex_init(&search.mutex, NULL);
    assert(ced_search_start(&search, &ws, "search-token", true));
    ced_search_finish(&search);
    assert(search.count == 4);
    assert(ced_search_start(&search, &ws, "search-token", false));
    ced_search_finish(&search);
    assert(search.count == 2);
    assert(!strcmp(search.hits[0].path, "keep.tmp"));
    assert(!strcmp(search.hits[1].path, "tracked.tmp"));
    pthread_mutex_destroy(&search.mutex);
    path_for(folder, "ignore-tree/nested");
    assert(ced_workspace_open(&ws, folder, error));
    assert_ignored(&ws, "hidden.log", true); assert_ignored(&ws, "keep.log", false);
    write_text("ignore-tree/nested/.gitignore", "");
    assert(ced_workspace_open(&ws, folder, error)); assert_ignored(&ws, "hidden.log", false);
    ced_workspace_free(&ws);
    assert(ced_file_delete(root, repo, error));
    puts("OK: tree ignore flags follow Git rules, negation, tracked files, nested workspaces, excluded directories, unusual names and refresh");
}

static void test_workspace_search(void) {
    char path[CED_PATH]; path_for(path, "nested"); assert(!mkdir(path, 0700));
    write_text("nested/find.txt", "first\nneedle here\nneedle needle\n");
    write_text("nested/unicode.txt", "\té alvo\n");
    path_for(path, "node_modules"); assert(!mkdir(path, 0700));
    write_text("node_modules/dependency.txt", "dependency contents");
    path_for(path, "dist"); assert(!mkdir(path, 0700));
    write_text("dist/package.txt", "package contents");
    write_text(".DS_Store", "");
    path_for(path, "nested/.git"); assert(!mkdir(path, 0700));
    write_text("nested/.git/config", "needle");
    path_for(path, "loop"); assert(!symlink(root, path));
    CedWorkspace ws = {0};
    assert(ced_workspace_open(&ws, root, error));
    bool nested = false;
    for (int i = 0; i < ws.count; i++) {
        assert(strncmp(ws.entries[i].path, "nested/.git", strlen("nested/.git")));
        assert(strcmp(ws.entries[i].path, "loop"));
        if (!strcmp(ws.entries[i].path, "nested")) { nested = true; ws.entries[i].expanded = true; }
    }
    assert(nested);
    assert_ignored(&ws, "dist/package.txt", false);
    assert_ignored(&ws, "node_modules/dependency.txt", false);
    assert_ignored(&ws, ".DS_Store", false);
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
    assert(ced_search_start(&search, &ws, "needle", false));
    ced_search_finish(&search);
    assert(search.count == 3);
    assert(search.hits[0].line == 2 && search.hits[0].column == 1);
    assert(search.hits[2].line == 3 && search.hits[2].column == 8);
    assert(ced_search_start(&search, &ws, "Needle", false));
    ced_search_finish(&search); assert(search.count == 0);
    assert(ced_search_start(&search, &ws, "alvo", false));
    ced_search_finish(&search); assert(search.count == 1 && search.hits[0].column == 7);
    assert(ced_search_start(&search, &ws, "needle", false));
    ced_search_cancel(&search);
    assert(!search.running);
    assert(ced_search_start(&search, &ws, "", false)); assert(!search.count);
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

static void test_git_discard(void) {
    char repository[CED_PATH], file[CED_PATH];
    path_for(repository,"discard"); assert(!mkdir(repository,0700));
    const char *init[]={"git","-C",repository,"init","-b","main",NULL}; command(init);
    const char *name[]={"git","-C",repository,"config","user.name","CED Test",NULL}; command(name);
    const char *email[]={"git","-C",repository,"config","user.email","test@example.invalid",NULL}; command(email);
    write_text("discard/a [literal] ' ü.txt","original\n");
    write_text("discard/other.txt","other original\n");
    write_text("discard/staged-delete.txt","keep deletion staged\n");
    CedGit git={0}; assert(ced_git_refresh(&git,repository,error));
    assert(ced_git_stage_all(&git,false,error)); assert(ced_git_commit(&git,"initial",error));
    write_text("discard/a [literal] ' ü.txt","staged\n");
    assert(ced_git_stage(&git,"a [literal] ' ü.txt",false,error));
    write_text("discard/a [literal] ' ü.txt","unstaged\n");
    write_text("discard/other.txt","keep this change\n");
    assert(ced_git_discard(&git,"a [literal] ' ü.txt",error));
    assert(ced_git_refresh(&git,repository,error));
    CedChange *change=find_change(&git,"a [literal] ' ü.txt"); assert(change && change->index=='M' && change->worktree==' ');
    assert(find_change(&git,"other.txt"));
    char *before=NULL,*after=NULL;
    assert(ced_git_versions(&git,change,true,&before,&after,error));
    assert(!strcmp(before,"original\n") && !strcmp(after,"staged\n")); free(before); free(after);
    CedDocument restored={0}; path_for(file,"discard/a [literal] ' ü.txt");
    assert(ced_document_open(&restored,file,error)); assert(!strcmp(restored.text,"staged\n")); ced_document_free(&restored);
    assert(!unlink(file)); assert(ced_git_discard(&git,"a [literal] ' ü.txt",error));
    assert(ced_document_open(&restored,file,error)); assert(!strcmp(restored.text,"staged\n")); ced_document_free(&restored);
    write_text("discard/new [file].txt","untracked\n");
    assert(ced_git_discard(&git,"new [file].txt",error)); path_for(file,"discard/new [file].txt"); assert(access(file,F_OK));
    write_text("discard/added.txt","index version\n"); assert(ced_git_stage(&git,"added.txt",false,error));
    write_text("discard/added.txt","working version\n"); assert(ced_git_discard(&git,"added.txt",error));
    path_for(file,"discard/added.txt"); assert(ced_document_open(&restored,file,error));
    assert(!strcmp(restored.text,"index version\n")); ced_document_free(&restored);
    assert(!ced_git_discard(&git,"added.txt",error)); /* staged-only changes are never discarded */
    write_text("discard/.gitignore","ignored.tmp\n"); write_text("discard/ignored.tmp","keep ignored\n");
    assert(!ced_git_discard(&git,"ignored.tmp",error)); path_for(file,"discard/ignored.tmp"); assert(!access(file,F_OK));
    assert(!ced_git_discard(&git,"../other.txt",error)); assert(!ced_git_discard(&git,".",error));
    assert(!ced_git_discard(&git,".git/config",error)); assert(!ced_git_discard(&git,repository,error));
    path_for(file,"discard/link"); assert(!symlink("other.txt",file));
    assert(ced_git_discard(&git,"link",error)); assert(access(file,F_OK));
    path_for(file,"discard/other.txt"); assert(ced_document_open(&restored,file,error));
    assert(!strcmp(restored.text,"keep this change\n")); ced_document_free(&restored);
    const char *cached_delete[]={"git","-C",repository,"rm","--cached","--","staged-delete.txt",NULL}; command(cached_delete);
    assert(ced_git_discard(&git,"staged-delete.txt",error));
    assert(ced_git_refresh(&git,repository,error));
    change=find_change(&git,"staged-delete.txt"); assert(change && change->index=='D' && change->worktree==' ');
    assert(ced_git_stage_all(&git,false,error)); assert(ced_git_commit(&git,"checkpoint",error));
    const char *rename[]={"git","-C",repository,"mv","--","a [literal] ' ü.txt","renamed.txt",NULL}; command(rename);
    write_text("discard/renamed.txt","unstaged after rename\n");
    assert(ced_git_discard(&git,"renamed.txt",error)); assert(ced_git_refresh(&git,repository,error));
    change=find_change(&git,"renamed.txt"); assert(change && change->index=='R' && change->worktree==' ');
    assert(!strcmp(change->original,"a [literal] ' ü.txt"));
    assert(ced_git_commit(&git,"rename",error));
    const char *branch[]={"git","-C",repository,"checkout","-b","side",NULL}; command(branch);
    write_text("discard/renamed.txt","side version\n"); assert(ced_git_stage_all(&git,false,error)); assert(ced_git_commit(&git,"side",error));
    const char *checkout[]={"git","-C",repository,"checkout","main",NULL}; command(checkout);
    write_text("discard/renamed.txt","main version\n"); assert(ced_git_stage_all(&git,false,error)); assert(ced_git_commit(&git,"main",error));
    const char *merge[]={"git","-C",repository,"merge","--no-edit","side",NULL}; char *output=NULL;
    assert(ced_command(merge,&output,NULL)!=0); free(output);
    assert(!ced_git_discard(&git,"renamed.txt",error) && strstr(error,"merge conflict"));
    path_for(file,"discard/renamed.txt"); assert(ced_document_open(&restored,file,error));
    assert(strstr(restored.text,"<<<<<<<")); ced_document_free(&restored);
    ced_git_free(&git);
    puts("OK: discard restores the index, preserves staged/other/ignored files, restores deletions, deletes untracked literal paths and never follows symlink targets");
}

static void test_git_bulk_and_sync(void) {
    char repository[CED_PATH], remote[CED_PATH], peer_path[CED_PATH], path[CED_PATH];
    path_for(repository, "bulk"); assert(!mkdir(repository, 0700));
    path_for(remote, "remote.git"); path_for(peer_path, "peer");
    const char *init[] = {"git", "-C", repository, "init", "-b", "main", NULL}; command(init);
    const char *name[] = {"git", "-C", repository, "config", "user.name", "CED Test", NULL}; command(name);
    const char *email[] = {"git", "-C", repository, "config", "user.email", "test@example.invalid", NULL}; command(email);
    write_text("bulk/.gitignore", "ignored.tmp\n");
    write_text("bulk/ignored.tmp", "ignored\n");
    write_text("bulk/a [literal].txt", "initial\n");
    write_text("bulk/b.txt", "delete later\n");
    CedGit git = {0}, peer = {0};
    assert(ced_git_refresh(&git, repository, error));
    assert(ced_git_stage_all(&git, false, error));
    assert(ced_git_refresh(&git, repository, error));
    assert(git.count == 3 && !find_change(&git, "ignored.tmp"));
    for (int i = 0; i < git.count; i++) assert(git.changes[i].index == 'A');
    write_text("bulk/a [literal].txt", "edited after staging\n");
    assert(ced_git_stage_all(&git, true, error)); /* no HEAD, including a modified staged file */
    assert(ced_git_refresh(&git, repository, error));
    for (int i = 0; i < git.count; i++) assert(git.changes[i].index == '?');
    CedDocument document = {0}; path_for(path, "bulk/a [literal].txt");
    assert(ced_document_open(&document, path, error));
    assert(!strcmp(document.text, "edited after staging\n")); ced_document_free(&document);
    assert(ced_git_stage_all(&git, false, error));
    assert(ced_git_commit(&git, "initial bulk files", error));
    write_text("bulk/a [literal].txt", "modified\n");
    path_for(path, "bulk/b.txt"); assert(!unlink(path));
    write_text("bulk/new.txt", "new\n");
    assert(ced_git_stage_all(&git, false, error));
    assert(ced_git_refresh(&git, repository, error));
    assert(find_change(&git, "a [literal].txt")->index == 'M');
    assert(find_change(&git, "b.txt")->index == 'D');
    assert(find_change(&git, "new.txt")->index == 'A');
    assert(ced_git_stage_all(&git, true, error));
    assert(ced_git_refresh(&git, repository, error));
    for (int i = 0; i < git.count; i++) assert(git.changes[i].index == ' ' || git.changes[i].index == '?');
    assert(ced_git_stage_all(&git, false, error));
    assert(ced_git_commit(&git, "bulk changes", error));
    assert(!ced_git_sync(&git, false, error) && error[0]); /* no remote */
    const char *bare[] = {"git", "init", "--bare", "-b", "main", remote, NULL}; command(bare);
    const char *add_remote[] = {"git", "-C", repository, "remote", "add", "origin", remote, NULL}; command(add_remote);
    const char *tracking_remote[] = {"git", "-C", repository, "config", "branch.main.remote", "origin", NULL}; command(tracking_remote);
    const char *tracking_merge[] = {"git", "-C", repository, "config", "branch.main.merge", "refs/heads/main", NULL}; command(tracking_merge);
    assert(ced_git_sync(&git, false, error));
    const char *clone[] = {"git", "clone", "--", remote, peer_path, NULL}; command(clone);
    const char *peer_name[] = {"git", "-C", peer_path, "config", "user.name", "CED Peer", NULL}; command(peer_name);
    const char *peer_email[] = {"git", "-C", peer_path, "config", "user.email", "peer@example.invalid", NULL}; command(peer_email);
    assert(ced_git_refresh(&peer, peer_path, error));
    write_text("peer/remote.txt", "from remote\n");
    assert(ced_git_stage_all(&peer, false, error));
    assert(ced_git_commit(&peer, "remote change", error));
    assert(ced_git_sync(&peer, false, error));
    assert(ced_git_sync(&git, true, error));
    path_for(path, "bulk/remote.txt");
    assert(ced_document_open(&document, path, error));
    assert(!strcmp(document.text, "from remote\n")); ced_document_free(&document);
    write_text("bulk/local.txt", "local history\n");
    assert(ced_git_stage_all(&git, false, error));
    assert(ced_git_commit(&git, "local commit", error));
    write_text("peer/another.txt", "remote history\n");
    assert(ced_git_stage_all(&peer, false, error));
    assert(ced_git_commit(&peer, "another remote commit", error));
    assert(ced_git_sync(&peer, false, error));
    assert(!ced_git_sync(&git, true, error) && error[0]); /* ff-only rejects divergence */
    assert(!ced_git_sync(&git, false, error) && error[0]); /* push never forces */
    path_for(path, "bulk/local.txt"); assert(!access(path, F_OK));
    path_for(path, "bulk/another.txt"); assert(access(path, F_OK));
    ced_git_free(&git); ced_git_free(&peer);
    puts("OK: stage/unstage all, unborn index, ignored/literal paths, local-remote push/pull and divergence rejection");
}

static void test_preferences(void) {
    const char *home=getenv("HOME"); char *original_home=home ? strdup(home) : NULL;
    assert(!setenv("HOME",root,1));
    char directory[CED_PATH],path[CED_PATH]; path_for(directory,".ced"); assert(!mkdir(directory,0700));
    assert(ced_font_size_load()==CED_FONT_SIZE_DEFAULT && !ced_terminal_right_load());
    write_text(".ced/settings.conf","font_size=21\nterminal_position=right\n");
    assert(ced_font_size_load()==21 && ced_terminal_right_load());
    assert(ced_preferences_ensure(path,error));
    CedPreferences settings; assert(ced_preferences_load(&settings,error));
    assert(settings.font_size==21 && settings.terminal_right && !strcmp(settings.font_family,"Geist Mono"));
    write_text(".ced/settings.conf","font_size=30\nterminal_position=bottom\n");
    assert(ced_font_size_load()==21 && ced_terminal_right_load()); /* JSON takes precedence. */
    write_text(".ced/settings.json","{\"editor.fontSize\":19,\"editor.fontFamily\":\"Menlo\",\"editor.fontPath\":\"\",\"terminal.position\":\"right\",\"future\":{\"enabled\":true}}");
    assert(ced_font_size_save(22)); assert(ced_terminal_right_save(false));
    assert(ced_preferences_load(&settings,error));
    assert(settings.font_size==22 && !settings.terminal_right && !strcmp(settings.font_family,"Menlo"));
    assert(!settings.terminal_visible && !*settings.terminal_tab);
    assert(ced_terminal_state_save(true, "viniciusfonseca.agents.panel"));
    assert(ced_preferences_load(&settings,error));
    assert(settings.terminal_visible && !strcmp(settings.terminal_tab, "viniciusfonseca.agents.panel"));
    assert(ced_terminal_state_save(false, settings.terminal_tab));
    assert(ced_terminal_right_save(true));
    assert(ced_preferences_load(&settings,error));
    assert(settings.terminal_right && !settings.terminal_visible && !strcmp(settings.terminal_tab, "viniciusfonseca.agents.panel"));
    assert(ced_terminal_state_save(true, ""));
    assert(ced_preferences_load(&settings,error));
    assert(settings.terminal_right && settings.terminal_visible && !*settings.terminal_tab);
    CedRecentFolders recent;
    assert(ced_recent_folders_load(&recent, error) && recent.count == 0);
    for (int i = 0; i <= CED_RECENT_FOLDERS_LIMIT; i++) {
        char name[32], folder[CED_PATH];
        snprintf(name, sizeof name, "project %d", i);
        path_for(folder, name);
        assert(ced_recent_folder_record(folder));
    }
    char first[CED_PATH], newest[CED_PATH], name[32];
    path_for(first, "project 1");
    snprintf(name, sizeof name, "project %d", CED_RECENT_FOLDERS_LIMIT);
    path_for(newest, name);
    assert(ced_recent_folders_load(&recent, error) && recent.count == CED_RECENT_FOLDERS_LIMIT);
    assert(!strcmp(recent.paths[0], newest) && !strcmp(recent.paths[recent.count - 1], first));
    assert(ced_recent_folder_record(first) && ced_font_size_save(22));
    assert(ced_recent_folders_load(&recent, error) && recent.count == CED_RECENT_FOLDERS_LIMIT);
    assert(!strcmp(recent.paths[0], first) && !strcmp(recent.paths[1], newest));
    CedDocument doc={0}; assert(ced_document_open(&doc,path,error));
    assert(strstr(doc.text,"future") && strstr(doc.text,"enabled") && strstr(doc.text,"Menlo")); ced_document_free(&doc);
    assert(ced_font_size_save(CED_FONT_SIZE_MIN) && ced_font_size_load()==CED_FONT_SIZE_MIN);
    assert(ced_font_size_save(CED_FONT_SIZE_MAX) && ced_font_size_load()==CED_FONT_SIZE_MAX);
    assert(!ced_font_size_save(CED_FONT_SIZE_MIN-1) && !ced_font_size_save(CED_FONT_SIZE_MAX+1));
    const char *invalid[]={"", "[]", "{", "{} garbage", "{\"editor.fontSize\":7}",
        "{\"editor.fontSize\":33}", "{\"editor.fontSize\":14.5}", "{\"editor.fontSize\":\"14\"}",
        "{\"editor.fontSize\":014}", "{\"editor.fontSize\":1e999}",
        "{\"terminal.position\":\"left\"}", "{\"terminal.visible\":\"true\"}", "{\"terminal.tab\":false}",
        "{\"terminal.tab\":\"bad\\nname\"}", "{\"editor.fontFamily\":42}",
        "{\"editor.fontPath\":\"relative.ttf\"}", "{\"editor.fontFamily\":\"Menlo\\u0000hidden\"}",
        "{\"editor.fontSize\":14,\"editor.fontSize\":15}"};
    for (size_t n=0;n<sizeof invalid/sizeof *invalid;n++) {
        CedPreferences previous=settings;
        assert(!ced_preferences_validate(invalid[n],strlen(invalid[n]),&settings,error));
        assert(settings.font_size==previous.font_size && !strcmp(settings.font_family,previous.font_family));
        write_text(".ced/settings.json",invalid[n]);
        assert(!ced_font_size_save(18) && !ced_terminal_right_save(true));
        assert(!ced_terminal_state_save(false, ""));
        assert(!ced_recent_folder_record(first));
        assert(ced_preferences_ensure(path,error)); /* Invalid JSON can still be opened for repair. */
        assert(ced_document_open(&doc,path,error)); assert(!strcmp(doc.text,invalid[n])); ced_document_free(&doc);
    }
    char *large=malloc(CED_PREFERENCES_LIMIT+2); assert(large);
    memset(large,' ',CED_PREFERENCES_LIMIT+1); large[CED_PREFERENCES_LIMIT+1]=0;
    write_text(".ced/settings.json",large); assert(!ced_font_size_save(18)); free(large);
    assert(!unlink(path)); assert(!mkdir(path,0700));
    assert(!ced_font_size_save(18) && !ced_terminal_right_save(true) && !ced_preferences_ensure(path,error));
    assert(!rmdir(path)); assert(ced_font_size_save(18)); assert(ced_font_size_load()==18);
    assert(!unsetenv("HOME"));
    assert(ced_font_size_load()==CED_FONT_SIZE_DEFAULT && !ced_terminal_right_load());
    assert(!ced_font_size_save(18) && !ced_terminal_right_save(true));
    if (original_home) { assert(!setenv("HOME",original_home,1)); free(original_home); }
    puts("OK: JSON preferences, legacy migration, preserved fields, validation, limits and atomic-write failures");
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
    test_workspace_ignored();
    test_workspace_search();
    test_git();
    test_git_discard();
    test_git_bulk_and_sync();
    const char *remove[] = {"rm", "-rf", "--", root, NULL}; command(remove);
    puts("Todos os testes passaram.");
    return 0;
}
