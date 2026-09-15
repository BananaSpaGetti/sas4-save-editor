/*
 * Tests discover.c's one piece of real logic -- the newest-mtime-first, stable sort in
 * discover_find_profiles_under() -- against a synthetic userdata/<id>/678800/local/Data/
 * Docs/<account>/Profile.save tree with INVENTED ids, built under fixtures_temp_dir() with
 * controlled mtimes via SetFileTime. Never touches a real Steam install or a real save.
 *
 * discover_find_steam(), the real registry read, and cmd_where's exact stdout against the
 * real Python oracle are covered by SAS4Trainer/c/tests/verify_task2-style scripting
 * outside this suite (a real machine's Steam install is not something a unit test should
 * depend on existing, or invent). This file also exercises session_for()'s pure string
 * manipulation and cmd_where's formatting branches (the "<- default" marker, the "no
 * profile found" path, and the "beside the tools is not writable" note) directly against
 * synthetic DiscoverContext values, since none of those need real discovery either.
 */
#include "discover.h"
#include "fixtures.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

static int failures = 0;

static void check(const char *name, int ok) {
    printf("  %s  %s\n", ok ? "ok  " : "FAIL", name);
    if (!ok) failures++;
}

static void make_dirs(const char *path) {
    char buf[900];
    snprintf(buf, sizeof buf, "%s", path);
    size_t len = strlen(buf);
    for (size_t i = 1; i < len; i++) {
        if (buf[i] == '\\' || buf[i] == '/') {
            char saved = buf[i];
            buf[i] = '\0';
            CreateDirectoryA(buf, NULL);
            buf[i] = saved;
        }
    }
    CreateDirectoryA(buf, NULL);
}

/* Writes an empty Profile.save at `path` (content is irrelevant to discovery -- only
 * existence and mtime matter) and stamps its write time. */
static void make_profile(const char *path, FILETIME ft) {
    char dir[900];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash1 = strrchr(dir, '\\');
    char *slash2 = strrchr(dir, '/');
    char *slash = slash1 > slash2 ? slash1 : slash2;
    if (slash) *slash = '\0';
    make_dirs(dir);

    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                            NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(h, "invented", 8, &written, NULL);
        SetFileTime(h, NULL, NULL, &ft);
        CloseHandle(h);
    }
}

static FILETIME filetime_from_unix(long long seconds_from_now) {
    /* An arbitrary but ordered base, offset by `seconds_from_now` -- the test only needs
     * relative order, never a real wall-clock value. */
    ULARGE_INTEGER u;
    u.QuadPart = (ULONGLONG)(116444736000000000LL + (seconds_from_now * 10000000LL));
    FILETIME ft;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    return ft;
}

static void test_newest_first_and_tie_stability(void) {
    const char *root = fixtures_temp_dir();
    char steam[900];
    snprintf(steam, sizeof steam, "%s\\steam", root);

    /* Two Steam "users" (invented ids), each with one or more account folders. userA/acctOld
     * is oldest, userA/acctNew is newest, userB/acctTieA and userB/acctTieB share the exact
     * same mtime -- userB's own account-folder enumeration order (acctTieA before acctTieB,
     * since FindFirstFileA/FindNextFileA on a freshly created tree returns entries in
     * creation order on this filesystem) is what the stable sort must preserve. */
    char p_old[900], p_new[900], p_tie_a[900], p_tie_b[900];
    snprintf(p_old, sizeof p_old,
             "%s\\userdata\\userA\\678800\\local\\Data\\Docs\\acctOld\\Profile.save", steam);
    snprintf(p_new, sizeof p_new,
             "%s\\userdata\\userA\\678800\\local\\Data\\Docs\\acctNew\\Profile.save", steam);
    snprintf(p_tie_a, sizeof p_tie_a,
             "%s\\userdata\\userB\\678800\\local\\Data\\Docs\\acctTieA\\Profile.save", steam);
    snprintf(p_tie_b, sizeof p_tie_b,
             "%s\\userdata\\userB\\678800\\local\\Data\\Docs\\acctTieB\\Profile.save", steam);

    make_profile(p_old, filetime_from_unix(1000));
    make_profile(p_new, filetime_from_unix(3000));
    make_profile(p_tie_a, filetime_from_unix(2000));
    make_profile(p_tie_b, filetime_from_unix(2000));

    DiscoverPathList out;
    discover_find_profiles_under(steam, &out);

    check("finds all four synthetic profiles", out.count == 4);
    if (out.count == 4) {
        check("newest mtime sorts first", strcmp(out.items[0], p_new) == 0);
        check("oldest mtime sorts last", strcmp(out.items[3], p_old) == 0);
        /* Both tied entries land in the two middle slots, in their original enumeration
         * order (acctTieA before acctTieB), never swapped past each other or past p_new. */
        check("tied mtimes keep original enumeration order (a before b)",
              strcmp(out.items[1], p_tie_a) == 0 && strcmp(out.items[2], p_tie_b) == 0);
    }
    discover_find_profiles_free(&out);
}

static void test_no_profiles_found(void) {
    const char *root = fixtures_temp_dir();
    char empty_steam[900];
    snprintf(empty_steam, sizeof empty_steam, "%s\\empty-steam", root);
    make_dirs(empty_steam);

    DiscoverPathList out;
    discover_find_profiles_under(empty_steam, &out);
    check("an empty (but existing) Steam-shaped tree yields zero profiles", out.count == 0);
    discover_find_profiles_free(&out);

    DiscoverPathList out2;
    discover_find_profiles_under("Z:\\this\\does\\not\\exist\\at\\all", &out2);
    check("a Steam root that does not exist at all also yields zero profiles",
          out2.count == 0);
    discover_find_profiles_free(&out2);
}

static void test_session_for(void) {
    char out[900];
    discover_session_for(
        "C:\\steam\\userdata\\1\\678800\\local\\Data\\Docs\\invented-account\\Profile.save",
        out, sizeof out);
    check("session_for strips the account dir and Profile.save, adds the session path",
          strcmp(out,
                 "C:\\steam\\userdata\\1\\678800\\local\\Data\\Docs\\com.ninjakiwi.link\\"
                 "Live\\current.session") == 0);
}

static void test_cmd_where_formatting(void) {
    /* Every value here is invented -- none of this is a real path or account id. */
    DiscoverContext ctx;
    memset(&ctx, 0, sizeof ctx);
    snprintf(ctx.root, sizeof ctx.root, "C:\\invented\\SAS4Trainer");
    snprintf(ctx.data, sizeof ctx.data, "C:\\invented\\SAS4Trainer");
    snprintf(ctx.steam, sizeof ctx.steam, "C:\\invented\\Steam");
    ctx.steam_found = true;
    ctx.profiles.items = NULL;
    ctx.profiles.count = 0;

    /* No profiles at all -- exit code 1, "no profile found" line. Lets cmd_where print
     * normally (all values here are invented, so there is nothing to hide); the check is
     * on the return code, not the text. */
    int rc = discover_cmd_where(&ctx);
    check("no profiles -> exit code 1", rc == 1);

    /* DATA != ROOT (the "beside the tools is not writable" note). */
    DiscoverPathList list = {0};
    char slot[DISCOVER_PATH_CAP];
    snprintf(slot, sizeof slot, "C:\\invented\\p1\\Profile.save");
    /* pathlist_push is file-static in discover.c; build the list by hand here instead. */
    list.items = malloc(sizeof(*list.items));
    snprintf(list.items[0], DISCOVER_PATH_CAP, "%s", slot);
    list.count = 1;
    list.cap = 1;
    ctx.profiles = list;
    snprintf(ctx.live, sizeof ctx.live, "%s", slot);
    ctx.live_found = true;
    snprintf(ctx.data, sizeof ctx.data, "C:\\invented\\fallback-data-dir");

    rc = discover_cmd_where(&ctx);
    check("one profile found -> exit code 0", rc == 0);

    free(list.items);
}

int main(void) {
    printf("newest-first and tie stability (synthetic tree)\n");
    test_newest_first_and_tie_stability();

    printf("\nno profiles found\n");
    test_no_profiles_found();

    printf("\nsession_for\n");
    test_session_for();

    printf("\ncmd_where formatting branches\n");
    test_cmd_where_formatting();

    printf("\n%s\n", failures == 0 ? "all checks pass" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
