/*
 * Unit tests for edit.c's own plumbing that a Python diff cannot easily cover: data_dir()'s
 * root-then-fallback cascade and backup()'s collision-avoiding suffix loop are host-
 * environment specific (which "root" to try is the CLI plan's job to resolve from the
 * executable's own location, not this core library's), so they are checked directly here
 * instead. apply_edits and pending are covered by the differential harness against real
 * sas4.py behaviour (see the plan's task 9 verification). test_where_it_writes below ports
 * TestWhereItWrites' two remaining assertions from tools/tests/test_sas4.py (task 14):
 * two sequential writes keep two distinct backups, and a backup that cannot be written
 * stops the edit cold -- apply_edits' single most consequential guarantee, and the one
 * thing in this whole library that had no C test at all until now.
 */
#include "edit.h"
#include "fixtures.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static int failures = 0;

static void check(const char *name, bool ok) {
    printf("  %s  %s\n", ok ? "ok  " : "FAIL", name);
    if (!ok) {
        failures++;
    }
}

/* A fresh, unique-per-process directory (see fixtures.h) -- not the fixed %TEMP%\
 * sas4_test_edit this file used to use, which left files behind across runs and would have
 * made test_two_writes_in_one_second_keep_both_backups' exact-count assertion pass once and
 * fail on every run after. */
static const char *temp_scratch_dir(void) {
    return fixtures_temp_dir();
}

/* Recursively checks whether any file under `dir` has exactly `needle_len` bytes matching
 * `needle` -- backups live one level down, in backups_dir\backup-YYYYMMDD-HHMMSS\<name>, so
 * a flat FindFirstFileA over backups_dir alone would only ever see the dated directories,
 * never the files inside them. */
static bool tree_contains_bytes(const char *dir, const uint8_t *needle, size_t needle_len) {
    char pattern[900];
    snprintf(pattern, sizeof(pattern), "%.700s\\*", dir);
    WIN32_FIND_DATAA data;
    HANDLE h = FindFirstFileA(pattern, &data);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    bool found = false;
    do {
        if (strcmp(data.cFileName, ".") == 0 || strcmp(data.cFileName, "..") == 0) {
            continue;
        }
        char child[900];
        snprintf(child, sizeof(child), "%.600s\\%.255s", dir, data.cFileName);
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (tree_contains_bytes(child, needle, needle_len)) {
                found = true;
            }
        } else {
            size_t got_len;
            uint8_t *got = fixtures_read_file(child, &got_len);
            if (got) {
                if (got_len == needle_len && memcmp(got, needle, needle_len) == 0) {
                    found = true;
                }
                free(got);
            }
        }
    } while (!found && FindNextFileA(h, &data));
    FindClose(h);
    return found;
}

static void test_writable(void) {
    printf("\nedit_writable\n");
    const char *dir = temp_scratch_dir();
    check("a temp-based directory is writable", edit_writable(dir));

    char probe[600];
    snprintf(probe, sizeof(probe), "%s\\.write-probe", dir);
    check("the probe file does not linger afterward",
          GetFileAttributesA(probe) == INVALID_FILE_ATTRIBUTES);
}

static void test_data_dir(void) {
    printf("\nedit_data_dir\n");
    char out[900];

    const char *good_root = temp_scratch_dir();
    bool ok = edit_data_dir(good_root, out, sizeof(out));
    check("a writable root is used as-is", ok && strcmp(out, good_root) == 0);

    /* A root nested under a file (not a directory) can never be created -- forces the
     * fallback path every time, deterministically, without depending on this machine's
     * actual permission layout. */
    char blocking_file[600];
    snprintf(blocking_file, sizeof(blocking_file), "%s\\blocker.txt", temp_scratch_dir());
    FILE *f = fopen(blocking_file, "w");
    if (f) {
        fclose(f);
    }
    char bad_root[700];
    snprintf(bad_root, sizeof(bad_root), "%s\\nested", blocking_file);
    bool fallback_ok = edit_data_dir(bad_root, out, sizeof(out));
    check("an unwritable root falls back to somewhere that IS writable",
          fallback_ok && edit_writable(out));
    check("the fallback is not the unwritable root itself", strcmp(out, bad_root) != 0);
    remove(blocking_file);
}

static void test_backup(void) {
    printf("\nedit_backup\n");
    const char *dir = temp_scratch_dir();
    edit_writable(dir);

    char source[700];
    snprintf(source, sizeof(source), "%s\\sample.save", dir);
    FILE *f = fopen(source, "wb");
    fwrite("invented test bytes, not a real save", 1, 37, f);
    fclose(f);

    char backups_dir[700];
    snprintf(backups_dir, sizeof(backups_dir), "%s\\backups", dir);

    char first[900], second[900];
    char err[256];
    bool ok1 = edit_backup(source, backups_dir, first, sizeof(first), err, sizeof(err));
    bool ok2 = edit_backup(source, backups_dir, second, sizeof(second), err, sizeof(err));
    check("a backup can be taken", ok1);
    check("a second backup, same second, does not collide with the first",
          ok2 && strcmp(first, second) != 0);

    FILE *check_f = fopen(first, "rb");
    char buf[64] = {0};
    size_t got = check_f ? fread(buf, 1, sizeof(buf), check_f) : 0;
    if (check_f) fclose(check_f);
    check("the backup's content matches the source", got == 37 &&
          memcmp(buf, "invented test bytes, not a real save", 37) == 0);
}

static void test_pending(void) {
    printf("\nedit_pending\n");
    static const char literal[] = "{\"a\":1,\"b\":\"x\"}";
    JsonParseResult parsed = json_parse((const uint8_t *)literal, strlen(literal));
    JsonValue *document = parsed.value;
    if (!document) {
        check("test fixture document parses", false);
        return;
    }

    JsonValue *new_a = json_new_int(1);   /* same as current -- should be dropped */
    JsonValue *new_b = json_new_string("y", 1); /* differs -- should be kept */
    EditPlanEntry plan[] = {
        {"a", new_a},
        {"b", new_b},
        {"nonexistent", new_a},
    };
    PendingList out;
    edit_pending(document, plan, 3, &out);
    check("an unchanged field is dropped from the pending list", out.count == 1);
    check("the one pending entry is the changed field",
          out.count == 1 && strcmp(out.items[0].path, "b") == 0);

    edit_pending_free(&out);
    json_free(new_a);
    json_free(new_b);
    json_free(document);
}

static void test_where_it_writes(void) {
    printf("\nTestWhereItWrites (edit_apply / backups)\n");
    const char *dir = temp_scratch_dir();

    /* test_two_writes_in_one_second_keep_both_backups */
    {
        char path[600];
        snprintf(path, sizeof(path), "%s\\two-writes.save", dir);
        char backups[700];
        snprintf(backups, sizeof(backups), "%s\\backups-two-writes", dir);
        JsonValue *doc = fixtures_document(5, 1000);
        fixtures_write_save(doc, path);
        json_free(doc);

        size_t first_len = 0;
        uint8_t *first = fixtures_read_file(path, &first_len);

        EditPlanEntry plan1[] = {{"Inventory/Profile0/Money", json_new_int(2)}};
        ApplyEditsResult r1 = edit_apply(path, backups, plan1, 1);
        check("the first write succeeds", r1.ok);
        json_free(plan1[0].value);

        size_t between_len = 0;
        uint8_t *between = fixtures_read_file(path, &between_len);

        EditPlanEntry plan2[] = {{"Inventory/Profile0/Money", json_new_int(3)}};
        ApplyEditsResult r2 = edit_apply(path, backups, plan2, 1);
        check("the second write succeeds", r2.ok);
        json_free(plan2[0].value);

        size_t backup_count = fixtures_count_files_recursive(backups);
        check("each write kept its own backup: exactly two files", backup_count == 2);

        /* Confirm both pre-write states survive somewhere under backups/, without knowing
         * either backup's exact filename (the same thing the Python test's `kept` list
         * check does). */
        check("the state before the first write survives",
              tree_contains_bytes(backups, first, first_len));
        check("the state between the two writes survives",
              tree_contains_bytes(backups, between, between_len));

        free(first);
        free(between);
    }

    /* test_a_backup_that_cannot_be_written_stops_the_edit */
    {
        char path[600];
        snprintf(path, sizeof(path), "%s\\backup-fails.save", dir);
        JsonValue *doc = fixtures_document(5, 1000);
        fixtures_write_save(doc, path);
        json_free(doc);

        size_t original_len = 0;
        uint8_t *original = fixtures_read_file(path, &original_len);

        /* A backups_dir nested under the save file itself -- a regular file can never have
         * a directory created underneath it, on any platform. */
        char impossible_backups[700];
        snprintf(impossible_backups, sizeof(impossible_backups), "%s\\impossible", path);

        EditPlanEntry plan[] = {{"Inventory/Profile0/Money", json_new_int(7)}};
        ApplyEditsResult result = edit_apply(path, impossible_backups, plan, 1);
        json_free(plan[0].value);

        check("the edit is refused", !result.ok);
        check("no backup path is reported", !result.has_backup_path);
        bool mentions_backup = false;
        for (size_t i = 0; result.message[i]; i++) {
            if (_strnicmp(result.message + i, "backup", 6) == 0) {
                mentions_backup = true;
                break;
            }
        }
        check("the message mentions a backup", mentions_backup);

        size_t after_len = 0;
        uint8_t *after = fixtures_read_file(path, &after_len);
        check("no backup means no write: the file is byte-identical to before",
              after_len == original_len && memcmp(original, after, original_len) == 0);
        free(original);
        free(after);
    }
}

int main(void) {
    test_writable();
    test_data_dir();
    test_backup();
    test_pending();
    test_where_it_writes();

    printf("\n");
    if (failures) {
        printf("FAILED: %d check(s)\n", failures);
        return 1;
    }
    printf("all checks pass\n");
    return 0;
}
