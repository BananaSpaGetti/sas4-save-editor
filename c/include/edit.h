/*
 * The shared edit machinery -- port of sas4.py's backup, pending, data_dir/_writable,
 * game_running, and apply_edits, the single choke point every write in the tool goes
 * through. Task 9 of port-to-c-sas4-core.
 */
#ifndef SAS4_EDIT_H
#define SAS4_EDIT_H

#include "json.h"

#include <stdbool.h>
#include <stddef.h>

/* True if a file can actually be created in `directory` (creates it if missing, writes and
 * removes a probe file). Matches _writable()'s own creates-then-deletes-a-probe check. */
bool edit_writable(const char *directory);

/* Where backups, dumps and caches go: `root` (the tool's own folder, "beside sas4.bat" --
 * the CLI plan's job to resolve from argv[0]/the executable's path, not this one's) if it
 * is writable, else a per-user fallback (%LOCALAPPDATA%\SAS4Trainer on Windows,
 * $XDG_DATA_HOME/sas4trainer or ~/.local/share/sas4trainer elsewhere), else the system
 * temp directory as a last resort. Writes the chosen directory into `out` (a buffer of at
 * least `out_cap` bytes) and returns true, or returns false if even the temp directory
 * probe fails (matches the Python: it never actually fails, temp is the backstop, but this
 * port still reports the possibility rather than silently returning an empty path). */
bool edit_data_dir(const char *root, char *out, size_t out_cap);

/* Whether SAS4-Win.exe is running (Windows only -- CreateToolhelp32Snapshot, the same
 * approach main_ptrscan.c's port already uses for a running-process check, rather than
 * shelling out to tasklist the way the Python does; the observable behaviour -- true only
 * while the game holds the process table entry -- is what matters, not the mechanism). */
bool edit_game_running(void);

/* A timestamped copy of the file at `path`, under `backups_dir`, taken before anything
 * writes over it -- backup-YYYYMMDD-HHMMSS, with a numeric suffix loop so two backups in
 * the same second do not collide (matches backup()'s own zero-padded "-NN" suffix).
 * Returns true and writes the copy's path into `out` (at least `out_cap` bytes) on
 * success; false (with `err` filled) if the copy could not be made -- the caller must let
 * that abort the write, matching apply_edits' own "nothing written" contract. */
bool edit_backup(const char *path, const char *backups_dir, char *out, size_t out_cap,
                  char *err, size_t err_cap);

/* One (path, new-value) entry of an edit plan. Does not own `value`. */
typedef struct {
    const char *path;
    JsonValue *value;
} EditPlanEntry;

typedef struct {
    char *path;         /* malloc'd */
    JsonValue *old_value; /* malloc'd copy -- the plan-comparison snapshot, since the
                            * document this came from may be reparsed or freed by the time
                            * a caller inspects this */
    JsonValue *new_value; /* not owned; points at the caller's plan entry's value */
} PendingEntry;

typedef struct {
    PendingEntry *items;
    size_t count;
    size_t cap;
} PendingList;

/* The subset of `plan` that would actually change something in `document` -- entries whose
 * current value already equals the proposed one are dropped, matching pending()'s own
 * `current != value` filter (a JSON-equality comparison, not identity). A path this
 * document does not have is silently skipped too, matching pending()'s catch of KeyError/
 * IndexError/TypeError from at_path. */
void edit_pending(JsonValue *document, const EditPlanEntry *plan, size_t plan_count,
                   PendingList *out);
void edit_pending_free(PendingList *list);

typedef struct {
    bool ok;
    char backup_path[1024]; /* filled whenever a backup was actually taken, even on later
                              * failure -- matches apply_edits returning (False, saved, ...)
                              * on every failure past the backup step */
    bool has_backup_path;
    char message[512];
} ApplyEditsResult;

/* Writes `plan` into the save at `file_path`, at byte level, over one backup -- the full
 * sequence from the plan's Context: read the raw file; back up first, aborting with
 * nothing written if the backup itself fails; decode; for each edit re-parse the current
 * plaintext (each replacement moves where the next anchor sits), compute the anchor via
 * anchor_for, splice the value's bytes in; abort with nothing written if any anchor fails
 * to resolve; encode; verify the checksum; write; re-read from disk and verify again.
 * `backups_dir` is where edit_backup puts its copy. */
ApplyEditsResult edit_apply(const char *file_path, const char *backups_dir,
                             const EditPlanEntry *plan, size_t plan_count);

#endif
