/*
 * `set` -- task 6 of the port-to-c-sas4-cli plan.
 */
#include "cmd_set.h"
#include "anchor.h"
#include "dgdata.h"
#include "edit.h"
#include "json.h"
#include "path.h"
#include "sas4load.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GAME_PROCESS "SAS4-Win.exe"

/* type(value).__name__ for the shapes a JSON document can hold. */
static const char *py_type_name(const JsonValue *v) {
    if (!v || v->type == JSON_NULL) return "NoneType";
    switch (v->type) {
        case JSON_BOOL: return "bool";
        case JSON_INT: return "int";
        case JSON_FLOAT: return "float";
        case JSON_STRING: return "str";
        case JSON_ARRAY: return "list";
        case JSON_OBJECT: return "dict";
        default: return "object";
    }
}

/* json.dumps(v) -- Python's default, ensure_ascii=True. Malloc'd; caller frees. */
static char *dumps(const JsonValue *v) {
    char *out = NULL;
    size_t out_len = 0;
    char err[128];
    if (!json_serialize_default(v, &out, &out_len, err, sizeof err)) {
        out = malloc(2);
        out[0] = '?';
        out[1] = '\0';
    }
    return out;
}

/* memmem, which mingw does not provide. */
static const uint8_t *find_bytes(const uint8_t *haystack, size_t haystack_len,
                                  const uint8_t *needle, size_t needle_len) {
    if (needle_len == 0 || haystack_len < needle_len) return NULL;
    for (size_t i = 0; i + needle_len <= haystack_len; i++) {
        if (memcmp(haystack + i, needle, needle_len) == 0) return haystack + i;
    }
    return NULL;
}

int cmd_set(const char *file, const char *path, const char *value, bool dry_run, bool force,
            const char *backups_dir) {
    if (edit_game_running() && !force) {
        printf("%s is running. It rewrites the save on its own schedule and would overwrite\n",
               GAME_PROCESS);
        printf("this edit. Close the game first, or pass --force if you know better.\n");
        return 1;
    }

    SaveLoad sl = sas4_load(file);
    if (!sl.ok) {
        printf("%s\n", sl.error);
        sas4_load_free(&sl);
        return 1;
    }

    char stored[9], computed[9];
    int verify_rc = dg_verify(sl.raw, sl.raw_len, stored, computed);
    if (verify_rc != 1 && !force) {
        printf("this file does not verify (%s vs %s) -- refusing to edit it\n", stored,
               computed);
        sas4_load_free(&sl);
        return 1;
    }

    PathAtResult at = path_at(sl.document, path);
    if (!at.ok) {
        printf("no such path: %s\n", path);
        printf("find one with:  py sas4.py list --grep <part of the name>\n");
        sas4_load_free(&sl);
        return 1;
    }
    const JsonValue *current = at.value;

    CoerceResult coerced = path_coerce(value, current);
    if (!coerced.ok) {
        char *current_dump = dumps(current);
        printf("%s holds %s (%s); %s\n", path, current_dump, py_type_name(current),
               coerced.error);
        free(current_dump);
        sas4_load_free(&sl);
        return 1;
    }

    if (json_equal(coerced.value, current)) {
        char *current_dump = dumps(current);
        printf("%s is already %s\n", path, current_dump);
        free(current_dump);
        json_free(coerced.value);
        sas4_load_free(&sl);
        return 0;
    }

    AnchorResult anchor = anchor_for(sl.document, sl.plain, sl.plain_len, path);
    if (!anchor.ok) {
        printf("%s\n", anchor.error);
        json_free(coerced.value);
        sas4_load_free(&sl);
        return 1;
    }

    /* replacement = anchor[:-old_length] + json.dumps(new, separators=(",",":"),
     * ensure_ascii=False) -- the compact form here, unlike the printed messages. */
    char *new_compact = NULL;
    size_t new_compact_len = 0;
    char err[128];
    if (!json_serialize_compact(coerced.value, &new_compact, &new_compact_len, err,
                                 sizeof err)) {
        printf("%s\n", err);
        anchor_free(&anchor);
        json_free(coerced.value);
        sas4_load_free(&sl);
        return 1;
    }
    size_t head_len = anchor.anchor_len - anchor.value_len;
    size_t replacement_len = head_len + new_compact_len;
    uint8_t *replacement = malloc(replacement_len);
    memcpy(replacement, anchor.anchor, head_len);
    memcpy(replacement + head_len, new_compact, new_compact_len);
    free(new_compact);

    char *current_dump = dumps(current);
    char *new_dump = dumps(coerced.value);
    printf("%s\n", path);
    printf("  %s  ->  %s\n", current_dump, new_dump);
    free(current_dump);
    free(new_dump);

    if (dry_run) {
        printf("  (dry run, nothing written)\n");
        free(replacement);
        anchor_free(&anchor);
        json_free(coerced.value);
        sas4_load_free(&sl);
        return 0;
    }

    char saved[1024];
    char backup_err[256];
    if (!edit_backup(file, backups_dir, saved, sizeof saved, backup_err, sizeof backup_err)) {
        /* backup() raises OSError in the Python; main() does not catch it, so this is the
         * same uncaught-exception family as Decision 11 -- reported here rather than
         * reproduced as a traceback, with nothing written either way. */
        fprintf(stderr, "%s\n", backup_err);
        free(replacement);
        anchor_free(&anchor);
        json_free(coerced.value);
        sas4_load_free(&sl);
        return 1;
    }
    printf("  backup   %s\n", saved);

    /* plain.replace(anchor, replacement, 1) */
    const uint8_t *hit = find_bytes(sl.plain, sl.plain_len, anchor.anchor, anchor.anchor_len);
    if (!hit) {
        /* anchor_for guarantees exactly one occurrence, so this cannot happen; refuse
         * rather than write something unexamined if it ever does. */
        printf("  built file does not verify -- nothing written\n");
        free(replacement);
        anchor_free(&anchor);
        json_free(coerced.value);
        sas4_load_free(&sl);
        return 1;
    }
    size_t prefix = (size_t)(hit - sl.plain);
    size_t suffix_at = prefix + anchor.anchor_len;
    size_t patched_len = sl.plain_len - anchor.anchor_len + replacement_len;
    uint8_t *patched = malloc(patched_len);
    memcpy(patched, sl.plain, prefix);
    memcpy(patched + prefix, replacement, replacement_len);
    memcpy(patched + prefix + replacement_len, sl.plain + suffix_at, sl.plain_len - suffix_at);
    free(replacement);

    uint8_t *built = NULL;
    size_t built_len = 0;
    dg_encode(patched, patched_len, &built, &built_len);
    free(patched);

    char bs[9], bc[9];
    if (dg_verify(built, built_len, bs, bc) != 1) {
        printf("  built file does not verify -- nothing written\n");
        free(built);
        anchor_free(&anchor);
        json_free(coerced.value);
        sas4_load_free(&sl);
        return 1;
    }

    FILE *f = fopen(file, "wb");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", file);
        free(built);
        anchor_free(&anchor);
        json_free(coerced.value);
        sas4_load_free(&sl);
        return 1;
    }
    fwrite(built, 1, built_len, f);
    fclose(f);
    free(built);
    anchor_free(&anchor);
    json_free(coerced.value);
    sas4_load_free(&sl);

    SaveLoad check = sas4_load(file);
    if (!check.ok) {
        printf("%s\n", check.error);
        sas4_load_free(&check);
        return 1;
    }
    char cs[9], cc[9];
    int check_rc = dg_verify(check.raw, check.raw_len, cs, cc);
    printf("  checksum %s (%s)\n", cs, check_rc == 1 ? "VALID" : "MISMATCH");
    PathAtResult back = path_at(check.document, path);
    char *back_dump = dumps(back.ok ? back.value : NULL);
    printf("  reads back as %s\n", back_dump);
    free(back_dump);
    sas4_load_free(&check);
    return check_rc == 1 ? 0 : 1;
}
