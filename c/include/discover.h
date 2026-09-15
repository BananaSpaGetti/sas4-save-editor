/*
 * Profile discovery -- port of sas4.py's find_steam(), find_profiles(), session_for(),
 * ROOT/HERE/DATA and the LIVE/SESSION module-level defaults, plus cmd_where.
 *
 * ROOT: sas4.py computes ROOT as the parent of the directory its own file (HERE) lives in
 * -- one level up from tools/. The deployed zip's design premise (see the plan's Context)
 * is that the code lives exactly one level below the folder holding the thing someone
 * double-clicks. discover_root() reproduces this the same way: one level up from the
 * directory holding the running executable, via GetModuleFileNameA rather than argv[0]
 * (argv[0] can be a relative path or omit the extension; the module's own path cannot).
 * This is only correct as long as whatever launches sas4.exe keeps it exactly one level
 * below the deployed root, the same invariant tools/ already keeps for the Python -- task
 * 17 (the .bat launcher) must preserve it or this needs revisiting.
 */
#ifndef SAS4_DISCOVER_H
#define SAS4_DISCOVER_H

#include <stdbool.h>
#include <stddef.h>

#define DISCOVER_PATH_CAP 900

/* The Steam install directory: HKCU\Software\Valve\Steam!SteamPath, then
 * HKLM\SOFTWARE\WOW6432Node\Valve\Steam!InstallPath, then two hardcoded fallback guesses,
 * each accepted only if it names a directory that exists. False (out untouched) if none
 * of those found one. */
bool discover_find_steam(char *out, size_t out_cap);

typedef struct {
    char (*items)[DISCOVER_PATH_CAP];
    size_t count;
    size_t cap;
} DiscoverPathList;

/* Every userdata/<id>/678800/local/Data/Docs/<accountId>/Profile.save under the Steam
 * install, newest write-time first (matches Python's os.path.getmtime sort; Python's sort
 * is stable, so two files with the same mtime keep find_profiles' own enumeration order,
 * which walks user ids then account ids in os.listdir's platform order -- FindFirstFileA
 * enumerates in the same underlying directory order os.listdir does on Windows, so this
 * matches without extra sorting). Empty (not an error) if Steam was not found or no
 * profile exists. */
void discover_find_profiles(DiscoverPathList *out);

/* Same walk as discover_find_profiles(), but rooted at a caller-supplied Steam-install-shaped
 * directory instead of a real discovered one -- lets a test build a synthetic
 * userdata/<id>/678800/local/Data/Docs/<account>/Profile.save tree with invented ids and
 * controlled mtimes rather than depending on a real machine's Steam install. */
void discover_find_profiles_under(const char *steam_root, DiscoverPathList *out);

void discover_find_profiles_free(DiscoverPathList *list);

/* The current.session path that sits beside a given profile's Docs directory -- pure
 * string manipulation, no filesystem access, matching session_for(). */
void discover_session_for(const char *profile_path, char *out, size_t out_cap);

/* One level up from the running executable's own directory (see the file comment). */
void discover_root(char *out, size_t out_cap);

/* Everything sas4.py computes once at import time and reuses (ROOT, DATA via
 * edit_data_dir, the newest profile as LIVE, and SESSION for it) -- computed together
 * because SESSION depends on LIVE and DATA depends on ROOT. */
typedef struct {
    char root[DISCOVER_PATH_CAP];
    char data[DISCOVER_PATH_CAP];
    char steam[DISCOVER_PATH_CAP];   /* empty if not found */
    bool steam_found;
    DiscoverPathList profiles;
    char live[DISCOVER_PATH_CAP];    /* empty if no profile found */
    bool live_found;
    char session[DISCOVER_PATH_CAP]; /* empty if no live profile */
} DiscoverContext;

void discover_context_build(DiscoverContext *ctx);
void discover_context_free(DiscoverContext *ctx);

/* cmd_where's exact stdout, matching the Python. Returns the process exit code (0 if a
 * profile was found, 1 if none was). */
int discover_cmd_where(const DiscoverContext *ctx);

#endif
