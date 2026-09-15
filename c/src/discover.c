#include "discover.h"
#include "edit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

static bool is_directory(const char *path) {
    DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static bool is_file(const char *path) {
    DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static bool registry_string(HKEY root, const char *key, const char *name, char *out,
                             size_t out_cap) {
    HKEY handle;
    if (RegOpenKeyExA(root, key, 0, KEY_QUERY_VALUE, &handle) != ERROR_SUCCESS) return false;
    DWORD type = 0;
    DWORD size = (DWORD)out_cap;
    LSTATUS status = RegQueryValueExA(handle, name, NULL, &type, (LPBYTE)out, &size);
    RegCloseKey(handle);
    if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return false;
    if (size == 0 || size >= out_cap) return false;
    out[size] = '\0'; /* RegQueryValueExA does not guarantee NUL termination */
    return out[0] != '\0';
}

bool discover_find_steam(char *out, size_t out_cap) {
    char buf[DISCOVER_PATH_CAP];
    if (registry_string(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath", buf,
                         sizeof buf) &&
        is_directory(buf)) {
        snprintf(out, out_cap, "%s", buf);
        return true;
    }
    if (registry_string(HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Valve\\Steam",
                         "InstallPath", buf, sizeof buf) &&
        is_directory(buf)) {
        snprintf(out, out_cap, "%s", buf);
        return true;
    }
    static const char *const GUESSES[] = {"C:\\Program Files (x86)\\Steam",
                                           "C:\\Program Files\\Steam"};
    for (size_t i = 0; i < 2; i++) {
        if (is_directory(GUESSES[i])) {
            snprintf(out, out_cap, "%s", GUESSES[i]);
            return true;
        }
    }
    return false;
}

/* Subdirectory names of `path`, in FindFirstFileA/FindNextFileA order (matching os.listdir's
 * own underlying enumeration order on Windows -- see discover.h). "." and ".." are skipped. */
typedef struct {
    char (*items)[MAX_PATH];
    size_t count;
    size_t cap;
} NameList;

static void namelist_push(NameList *l, const char *name) {
    if (l->count == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->items = realloc(l->items, l->cap * sizeof *l->items);
    }
    snprintf(l->items[l->count], MAX_PATH, "%s", name);
    l->count++;
}

static void subdirs(const char *path, NameList *out) {
    char pattern[DISCOVER_PATH_CAP + 64];
    snprintf(pattern, sizeof pattern, "%s\\*", path);
    WIN32_FIND_DATAA data;
    HANDLE h = FindFirstFileA(pattern, &data);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (strcmp(data.cFileName, ".") == 0 || strcmp(data.cFileName, "..") == 0) continue;
        namelist_push(out, data.cFileName);
    } while (FindNextFileA(h, &data));
    FindClose(h);
}

static void pathlist_push(DiscoverPathList *l, const char *path) {
    if (l->count == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->items = realloc(l->items, l->cap * sizeof *l->items);
    }
    snprintf(l->items[l->count], DISCOVER_PATH_CAP, "%s", path);
    l->count++;
}

static FILETIME mtime_of(const char *path) {
    FILETIME ft = {0, 0};
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return ft;
    GetFileTime(h, NULL, NULL, &ft);
    CloseHandle(h);
    return ft;
}

static int filetime_cmp(const void *pa, const void *pb) {
    const FILETIME *a = pa, *b = pb;
    /* Newest first -- CompareFileTime returns -1/0/1 for a<b/a==b/a>b. */
    return -CompareFileTime(a, b);
}

/* Split out from discover_find_profiles() so a test can point it at a synthetic
 * userdata-shaped tree instead of a real Steam install -- discover_find_profiles() itself
 * calls discover_find_steam() internally and so cannot be redirected at test data. */
void discover_find_profiles_under(const char *steam, DiscoverPathList *out) {
    memset(out, 0, sizeof *out);
    char userdata[DISCOVER_PATH_CAP + 64];
    /* Precision caps (%.Ns) bound each substitution explicitly so gcc's -Wformat-truncation
     * can prove the result always fits, regardless of a source buffer's own declared
     * capacity -- real Steam/profile paths never come close to these caps. */
    snprintf(userdata, sizeof userdata, "%.899s\\userdata", steam);

    NameList users = {0};
    subdirs(userdata, &users);

    /* Collect candidates in enumeration order first, then stable-sort by mtime descending
     * -- Python's sort() is a stable Timsort, so ties keep this same original order. qsort
     * is not guaranteed stable, so this uses an explicit insertion sort below instead: its
     * `> 0` (not `>= 0`) comparison never swaps two equal-mtime entries past each other,
     * which is what makes it stable without needing a separate index tiebreaker. */
    typedef struct {
        char path[DISCOVER_PATH_CAP];
        FILETIME mtime;
    } Candidate;
    Candidate *cands = NULL;
    size_t n = 0, cap = 0;

    for (size_t ui = 0; ui < users.count; ui++) {
        char docs[DISCOVER_PATH_CAP + 64];
        snprintf(docs, sizeof docs, "%.500s\\%.259s\\678800\\local\\Data\\Docs", userdata,
                 users.items[ui]);
        NameList accounts = {0};
        subdirs(docs, &accounts);
        for (size_t ai = 0; ai < accounts.count; ai++) {
            char candidate[DISCOVER_PATH_CAP + 64];
            snprintf(candidate, sizeof candidate, "%.500s\\%.259s\\Profile.save", docs,
                     accounts.items[ai]);
            if (!is_file(candidate)) continue;
            if (n == cap) {
                cap = cap ? cap * 2 : 8;
                cands = realloc(cands, cap * sizeof *cands);
            }
            snprintf(cands[n].path, sizeof cands[n].path, "%.899s", candidate);
            cands[n].mtime = mtime_of(candidate);
            n++;
        }
        free(accounts.items);
    }
    free(users.items);

    /* Stable insertion sort, newest mtime first. */
    for (size_t i = 1; i < n; i++) {
        Candidate key = cands[i];
        size_t j = i;
        while (j > 0 && filetime_cmp(&cands[j - 1].mtime, &key.mtime) > 0) {
            cands[j] = cands[j - 1];
            j--;
        }
        cands[j] = key;
    }

    for (size_t i = 0; i < n; i++) pathlist_push(out, cands[i].path);
    free(cands);
}

void discover_find_profiles(DiscoverPathList *out) {
    char steam[DISCOVER_PATH_CAP];
    if (!discover_find_steam(steam, sizeof steam)) {
        memset(out, 0, sizeof *out);
        return;
    }
    discover_find_profiles_under(steam, out);
}

void discover_find_profiles_free(DiscoverPathList *list) {
    free(list->items);
    list->items = NULL;
    list->count = list->cap = 0;
}

void discover_session_for(const char *profile_path, char *out, size_t out_cap) {
    /* .../Docs/<account>/Profile.save -> .../Docs -- strip the file name, then the account
     * directory, matching dirname(dirname(profile_path)). */
    char buf[DISCOVER_PATH_CAP];
    snprintf(buf, sizeof buf, "%s", profile_path);
    for (int pass = 0; pass < 2; pass++) {
        char *slash1 = strrchr(buf, '\\');
        char *slash2 = strrchr(buf, '/');
        char *slash = slash1 > slash2 ? slash1 : slash2;
        if (slash) *slash = '\0';
    }
    snprintf(out, out_cap, "%s\\com.ninjakiwi.link\\Live\\current.session", buf);
}

void discover_root(char *out, size_t out_cap) {
    char exe_path[DISCOVER_PATH_CAP];
    DWORD n = GetModuleFileNameA(NULL, exe_path, sizeof exe_path);
    if (n == 0 || n >= sizeof exe_path) {
        snprintf(out, out_cap, ".");
        return;
    }
    /* dirname twice: exe's own directory, then its parent. */
    for (int pass = 0; pass < 2; pass++) {
        char *slash1 = strrchr(exe_path, '\\');
        char *slash2 = strrchr(exe_path, '/');
        char *slash = slash1 > slash2 ? slash1 : slash2;
        if (slash) *slash = '\0';
    }
    snprintf(out, out_cap, "%s", exe_path);
}

void discover_context_build(DiscoverContext *ctx) {
    memset(ctx, 0, sizeof *ctx);
    discover_root(ctx->root, sizeof ctx->root);
    edit_data_dir(ctx->root, ctx->data, sizeof ctx->data);
    ctx->steam_found = discover_find_steam(ctx->steam, sizeof ctx->steam);
    discover_find_profiles(&ctx->profiles);
    if (ctx->profiles.count > 0) {
        ctx->live_found = true;
        snprintf(ctx->live, sizeof ctx->live, "%s", ctx->profiles.items[0]);
        discover_session_for(ctx->live, ctx->session, sizeof ctx->session);
    }
}

void discover_context_free(DiscoverContext *ctx) { discover_find_profiles_free(&ctx->profiles); }

int discover_cmd_where(const DiscoverContext *ctx) {
    printf("Steam: %s\n", ctx->steam_found ? ctx->steam : "not found");
    bool data_is_root = strcmp(ctx->data, ctx->root) == 0;
    printf("writes to: %s%s\n", ctx->data,
           data_is_root ? "" : "   (beside the tools is not writable)");
    if (ctx->profiles.count == 0) {
        printf("no SAS4 profile found. Pass --file <path> to any command to point at one.\n");
        return 1;
    }
    printf("profiles (newest first):\n");
    for (size_t i = 0; i < ctx->profiles.count; i++) {
        bool is_live = strcmp(ctx->profiles.items[i], ctx->live) == 0;
        printf("  %s%s\n", ctx->profiles.items[i], is_live ? "  <- default" : "");
    }
    printf("session: %s\n", ctx->live_found ? ctx->session : "n/a");
    return 0;
}
