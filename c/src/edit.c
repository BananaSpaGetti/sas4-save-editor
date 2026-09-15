/*
 * The shared edit machinery -- task 9 of port-to-c-sas4-core.
 */
#include "edit.h"
#include "anchor.h"
#include "dgdata.h"
#include "path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <windows.h>
#include <tlhelp32.h>

/* os.makedirs(path, exist_ok=True) -- create every missing path component. Accepts either
 * slash direction, matching this port's own convention of building paths with '\\' while
 * still tolerating '/' in anything a caller hands in. */
static bool make_dirs(const char *path) {
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", path);
    size_t len = strlen(buf);
    for (size_t i = 1; i < len; i++) {
        if (buf[i] == '\\' || buf[i] == '/') {
            char saved = buf[i];
            buf[i] = '\0';
            /* Skip a bare drive letter ("C:") -- CreateDirectory on that alone fails and
             * is not a real path component to create anyway. */
            size_t here_len = strlen(buf);
            if (!(here_len == 2 && buf[1] == ':')) {
                CreateDirectoryA(buf, NULL);
            }
            buf[i] = saved;
        }
    }
    if (CreateDirectoryA(buf, NULL)) {
        return true;
    }
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

bool edit_writable(const char *directory) {
    if (!make_dirs(directory)) {
        return false;
    }
    char probe[1024];
    snprintf(probe, sizeof(probe), "%s\\.write-probe", directory);
    FILE *f = fopen(probe, "w");
    if (!f) {
        return false;
    }
    fclose(f);
    remove(probe);
    return true;
}

bool edit_data_dir(const char *root, char *out, size_t out_cap) {
    if (edit_writable(root)) {
        snprintf(out, out_cap, "%s", root);
        return true;
    }
    const char *localappdata = getenv("LOCALAPPDATA");
    const char *base = localappdata;
    if (!base || base[0] == '\0') {
        base = getenv("USERPROFILE");
    }
    if (base && base[0] != '\0') {
        char fallback[900];
        snprintf(fallback, sizeof(fallback), "%s\\SAS4Trainer", base);
        if (edit_writable(fallback)) {
            snprintf(out, out_cap, "%s", fallback);
            return true;
        }
    }
    char temp[MAX_PATH];
    DWORD n = GetTempPathA(sizeof(temp), temp);
    if (n == 0 || n > sizeof(temp)) {
        return false;
    }
    if (n > 0 && (temp[n - 1] == '\\' || temp[n - 1] == '/')) {
        temp[n - 1] = '\0';
    }
    snprintf(out, out_cap, "%s", temp);
    return true;
}

bool edit_game_running(void) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }
    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"SAS4-Win.exe") == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

bool edit_backup(const char *path, const char *backups_dir, char *out, size_t out_cap,
                  char *err, size_t err_cap) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", tm_info);

    const char *slash1 = strrchr(path, '\\');
    const char *slash2 = strrchr(path, '/');
    const char *base_name = slash1 > slash2 ? slash1 : slash2;
    base_name = base_name ? base_name + 1 : path;

    char directory[700];
    char target[900];
    int attempt = 1;
    snprintf(directory, sizeof(directory), "%s\\backup-%s", backups_dir, stamp);
    snprintf(target, sizeof(target), "%s\\%s", directory, base_name);
    while (GetFileAttributesA(target) != INVALID_FILE_ATTRIBUTES) {
        attempt++;
        snprintf(directory, sizeof(directory), "%s\\backup-%s-%02d", backups_dir, stamp,
                 attempt);
        snprintf(target, sizeof(target), "%s\\%s", directory, base_name);
    }
    if (!make_dirs(directory)) {
        snprintf(err, err_cap, "could not create backup directory");
        return false;
    }
    if (!CopyFileA(path, target, FALSE)) {
        snprintf(err, err_cap, "could not copy file (error %lu)", (unsigned long)GetLastError());
        return false;
    }
    snprintf(out, out_cap, "%s", target);
    return true;
}

void edit_pending(JsonValue *document, const EditPlanEntry *plan, size_t plan_count,
                   PendingList *out) {
    out->items = NULL;
    out->count = 0;
    out->cap = 0;
    for (size_t i = 0; i < plan_count; i++) {
        PathAtResult r = path_at(document, plan[i].path);
        if (!r.ok) {
            continue;
        }
        if (json_equal(r.value, plan[i].value)) {
            continue;
        }
        if (out->count == out->cap) {
            out->cap = out->cap ? out->cap * 2 : 8;
            out->items = (PendingEntry *)realloc(out->items, out->cap * sizeof(PendingEntry));
        }
        out->items[out->count].path = strdup(plan[i].path);
        out->items[out->count].old_value = json_clone(r.value);
        out->items[out->count].new_value = plan[i].value;
        out->count++;
    }
}

void edit_pending_free(PendingList *list) {
    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i].path);
        json_free(list->items[i].old_value);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

static uint8_t *read_whole_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)size > 0 ? (size_t)size : 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        free(buf);
        return NULL;
    }
    *out_len = (size_t)size;
    return buf;
}

static bool write_whole_file(const char *path, const uint8_t *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    size_t wrote = fwrite(data, 1, len, f);
    fclose(f);
    return wrote == len;
}

/* The first (leftmost) occurrence of needle in haystack, replaced by replacement --
 * matches Python's bytes.replace(old, new, 1). anchor_for already confirmed exactly one
 * occurrence exists, so "not found" here would mean the plaintext changed out from under
 * the anchor between computing it and using it -- not expected, but handled rather than
 * assumed. Allocates *out via malloc (caller frees). */
static bool replace_first(const uint8_t *haystack, size_t haystack_len,
                           const uint8_t *needle, size_t needle_len,
                           const uint8_t *replacement, size_t replacement_len,
                           uint8_t **out, size_t *out_len) {
    size_t pos = haystack_len + 1; /* sentinel: not found */
    for (size_t i = 0; i + needle_len <= haystack_len; i++) {
        if (memcmp(haystack + i, needle, needle_len) == 0) {
            pos = i;
            break;
        }
    }
    if (pos > haystack_len) {
        return false;
    }
    size_t new_len = haystack_len - needle_len + replacement_len;
    uint8_t *buf = (uint8_t *)malloc(new_len > 0 ? new_len : 1);
    memcpy(buf, haystack, pos);
    memcpy(buf + pos, replacement, replacement_len);
    memcpy(buf + pos + replacement_len, haystack + pos + needle_len,
           haystack_len - pos - needle_len);
    *out = buf;
    *out_len = new_len;
    return true;
}

ApplyEditsResult edit_apply(const char *file_path, const char *backups_dir,
                             const EditPlanEntry *plan, size_t plan_count) {
    ApplyEditsResult result;
    result.ok = false;
    result.has_backup_path = false;
    result.backup_path[0] = '\0';
    result.message[0] = '\0';

    size_t raw_len;
    uint8_t *raw = read_whole_file(file_path, &raw_len);
    if (!raw) {
        snprintf(result.message, sizeof(result.message), "could not read '%s'", file_path);
        return result;
    }

    char backup_err[256];
    if (!edit_backup(file_path, backups_dir, result.backup_path, sizeof(result.backup_path),
                      backup_err, sizeof(backup_err))) {
        free(raw);
        snprintf(result.message, sizeof(result.message),
                 "could not write a backup (%s) -- nothing written.\nBackups go to %s",
                 backup_err, backups_dir);
        return result;
    }
    result.has_backup_path = true;

    uint8_t *plain;
    size_t plain_len;
    if (!dg_decode(raw, raw_len, &plain, &plain_len)) {
        free(raw);
        snprintf(result.message, sizeof(result.message),
                 "not a DGDATA file -- nothing written");
        return result;
    }
    free(raw);

    for (size_t i = 0; i < plan_count; i++) {
        JsonParseResult parsed = json_parse(plain, plain_len);
        if (!parsed.value) {
            free(plain);
            snprintf(result.message, sizeof(result.message),
                     "aborted at %s: plaintext no longer parses -- nothing written",
                     plan[i].path);
            return result;
        }
        AnchorResult anchor = anchor_for(parsed.value, plain, plain_len, plan[i].path);
        if (!anchor.ok) {
            json_free(parsed.value);
            free(plain);
            snprintf(result.message, sizeof(result.message),
                     "aborted at %s: %s -- nothing written", plan[i].path, anchor.error);
            return result;
        }
        json_free(parsed.value);

        char *new_compact;
        size_t new_compact_len;
        char err[160];
        if (!json_serialize_compact(plan[i].value, &new_compact, &new_compact_len, err,
                                     sizeof(err))) {
            anchor_free(&anchor);
            free(plain);
            snprintf(result.message, sizeof(result.message),
                     "aborted at %s: %s -- nothing written", plan[i].path, err);
            return result;
        }

        size_t prefix_len = anchor.anchor_len - anchor.value_len;
        size_t replacement_len = prefix_len + new_compact_len;
        uint8_t *replacement = (uint8_t *)malloc(replacement_len > 0 ? replacement_len : 1);
        memcpy(replacement, anchor.anchor, prefix_len);
        memcpy(replacement + prefix_len, new_compact, new_compact_len);
        free(new_compact);

        uint8_t *new_plain;
        size_t new_plain_len;
        bool replaced = replace_first(plain, plain_len, anchor.anchor, anchor.anchor_len,
                                       replacement, replacement_len, &new_plain,
                                       &new_plain_len);
        free(replacement);
        anchor_free(&anchor);
        if (!replaced) {
            free(plain);
            snprintf(result.message, sizeof(result.message),
                     "aborted at %s: anchor vanished before it could be replaced -- "
                     "nothing written", plan[i].path);
            return result;
        }
        free(plain);
        plain = new_plain;
        plain_len = new_plain_len;
    }

    uint8_t *built;
    size_t built_len;
    dg_encode(plain, plain_len, &built, &built_len);
    free(plain);

    char stored[9], computed[9];
    int verdict = dg_verify(built, built_len, stored, computed);
    if (verdict != 1) {
        free(built);
        snprintf(result.message, sizeof(result.message),
                 "the rebuilt file does not verify -- nothing written");
        return result;
    }

    if (!write_whole_file(file_path, built, built_len)) {
        free(built);
        snprintf(result.message, sizeof(result.message), "could not write '%s'", file_path);
        return result;
    }
    free(built);

    size_t reread_len;
    uint8_t *reread = read_whole_file(file_path, &reread_len);
    bool final_ok = false;
    char final_stored[9] = "";
    if (reread) {
        char final_computed[9];
        int final_verdict = dg_verify(reread, reread_len, final_stored, final_computed);
        final_ok = final_verdict == 1;
        free(reread);
    }

    result.ok = final_ok;
    snprintf(result.message, sizeof(result.message), "wrote %zu field(s), checksum %s (%s)",
             plan_count, final_stored, final_ok ? "VALID" : "MISMATCH");
    return result;
}
