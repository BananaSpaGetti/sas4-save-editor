/*
 * `graft` -- task 13 of the port-to-c-sas4-cli plan.
 */
#include "cmd_graft.h"
#include "anchor.h"
#include "dgdata.h"
#include "edit.h"
#include "json.h"
#include "model.h"
#include "path.h"
#include "sas4load.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GAME_PROCESS "SAS4-Win.exe"
#define PREVIEW_WIDTH 60 /* json.dumps(...)[:60] */

/* sas4.py's IDENTITY_FIELDS: `link` is the account, `analytics` wraps a 32-hex id of its
 * own. Both must survive a graft untouched -- that promise is the whole reason the command
 * exists rather than "copy the file". */
static const char *IDENTITY_FIELDS[] = {"link", "analytics"};

static bool is_identity(const char *key) {
    for (size_t i = 0; i < sizeof IDENTITY_FIELDS / sizeof *IDENTITY_FIELDS; i++) {
        if (strcmp(IDENTITY_FIELDS[i], key) == 0) return true;
    }
    return false;
}

/* The first identity-field key found anywhere inside `value`, or NULL. Keys are checked
 * before their own subtree is descended into, and members are visited in order, so the
 * "first" found matches Python's traversal exactly -- it is printed, so it must. */
static const char *contains_identity(const JsonValue *value) {
    if (!value) return NULL;
    if (value->type == JSON_OBJECT) {
        for (size_t i = 0; i < value->as.object.count; i++) {
            const JsonMember *m = &value->as.object.members[i];
            if (is_identity(m->key)) return m->key;
            const char *found = contains_identity(m->value);
            if (found) return found;
        }
    } else if (value->type == JSON_ARRAY) {
        for (size_t i = 0; i < value->as.array.count; i++) {
            const char *found = contains_identity(value->as.array.items[i]);
            if (found) return found;
        }
    }
    return NULL;
}

static void strip_in_place(char *s) {
    char *start = s;
    while (*start && isspace((unsigned char)*start)) start++;
    size_t n = strlen(start);
    while (n > 0 && isspace((unsigned char)start[n - 1])) n--;
    memmove(s, start, n);
    s[n] = '\0';
}

/* The part of `path` after its last "/" -- field.split("/")[-1]. */
static const char *leaf_of(const char *path) {
    const char *cut = strrchr(path, '/');
    return cut ? cut + 1 : path;
}

/* json.dumps(value)[:60] -- bare, so ensure_ascii=True AND the default ", "/": " separators
 * rather than the compact ones the write path uses. */
static void dumps_clipped(const JsonValue *v, char *out, size_t out_cap) {
    char *text = NULL;
    size_t len = 0;
    char err[128];
    if (!json_serialize_default(v, &text, &len, err, sizeof err)) {
        snprintf(out, out_cap, "?");
        return;
    }
    size_t take = len < (size_t)PREVIEW_WIDTH ? len : (size_t)PREVIEW_WIDTH;
    if (take >= out_cap) take = out_cap - 1;
    memcpy(out, text, take);
    out[take] = '\0';
    free(text);
}

static const uint8_t *find_bytes(const uint8_t *haystack, size_t haystack_len,
                                 const uint8_t *needle, size_t needle_len) {
    if (needle_len == 0 || haystack_len < needle_len) return NULL;
    for (size_t i = 0; i + needle_len <= haystack_len; i++) {
        if (memcmp(haystack + i, needle, needle_len) == 0) return haystack + i;
    }
    return NULL;
}

typedef struct {
    char *field;
    JsonValue *old_value; /* owned: a clone, or the string "<absent>" */
    JsonValue *new_value; /* owned: a clone from the source document */
} GraftEntry;

int cmd_graft(const char *file, const char *source, const char *fields, bool apply,
              bool force, const char *backups_dir) {
    SaveLoad src = sas4_load(source);
    if (!src.ok) {
        printf("%s\n", src.error);
        sas4_load_free(&src);
        return 1;
    }
    SaveLoad dst = sas4_load(file);
    if (!dst.ok) {
        printf("%s\n", dst.error);
        sas4_load_free(&dst);
        sas4_load_free(&src);
        return 1;
    }

    int rc = 1;
    GraftEntry *plan = NULL;
    size_t plan_count = 0, plan_cap = 0;
    uint8_t *plain = NULL;
    size_t plain_len = 0;

    if (!fields || !*fields) {
        printf("name at least one field, e.g.\n");
        printf("  graft other.save --fields Inventory/Profile0/Money,"
               "Inventory/Profile0/Weapons\n");
        goto done;
    }

    {
        char *list = (char *)malloc(strlen(fields) + 1);
        strcpy(list, fields);
        char *save_ptr = list;
        while (save_ptr) {
            char *comma = strchr(save_ptr, ',');
            if (comma) *comma = '\0';
            char field[512];
            snprintf(field, sizeof field, "%.511s", save_ptr);
            save_ptr = comma ? comma + 1 : NULL;
            strip_in_place(field);

            PathAtResult from = path_at(src.document, field);
            if (!from.ok) {
                printf("skip %s -- not in the source\n", field);
                continue;
            }
            if (is_identity(leaf_of(field))) {
                printf("skip %s -- that is an identity field, keeping yours\n", field);
                continue;
            }
            const char *buried = contains_identity(from.value);
            if (buried) {
                JsonValue *as_string = json_new_string(buried, strlen(buried));
                char *shown = model_py_repr(as_string);
                printf("skip %s -- it contains the identity field %s, keeping yours\n",
                       field, shown);
                free(shown);
                json_free(as_string);
                continue;
            }

            PathAtResult into = path_at(dst.document, field);
            if (plan_count == plan_cap) {
                plan_cap = plan_cap ? plan_cap * 2 : 8;
                plan = (GraftEntry *)realloc(plan, plan_cap * sizeof *plan);
            }
            plan[plan_count].field = (char *)malloc(strlen(field) + 1);
            strcpy(plan[plan_count].field, field);
            /* Python's fallback is the STRING "<absent>", so it renders with quotes. */
            plan[plan_count].old_value =
                into.ok ? json_clone(into.value)
                        : json_new_string("<absent>", strlen("<absent>"));
            plan[plan_count].new_value = json_clone(from.value);
            plan_count++;
        }
        free(list);
    }

    if (plan_count == 0) {
        printf("nothing to graft\n");
        goto done;
    }

    printf("from %s\ninto %s\n\n", source, file);
    for (size_t i = 0; i < plan_count; i++) {
        char before[PREVIEW_WIDTH + 1], after[PREVIEW_WIDTH + 1];
        dumps_clipped(plan[i].old_value, before, sizeof before);
        dumps_clipped(plan[i].new_value, after, sizeof after);
        printf("  %s\n", plan[i].field);
        printf("      %s  ->  %s\n", before, after);
    }

    if (!apply) {
        printf("\n(preview -- pass --apply to write, after closing the game)\n");
        rc = 0;
        goto done;
    }
    if (edit_game_running() && !force) {
        printf("\n%s is running -- close it first\n", GAME_PROCESS);
        goto done;
    }

    {
        char saved[1024], berr[256];
        if (!edit_backup(file, backups_dir, saved, sizeof saved, berr, sizeof berr)) {
            fprintf(stderr, "%s\n", berr);
            goto done;
        }
        printf("\nbackup   %s\n", saved);
    }

    plain = (uint8_t *)malloc(dst.plain_len + 1);
    memcpy(plain, dst.plain, dst.plain_len);
    plain[dst.plain_len] = '\0';
    plain_len = dst.plain_len;

    for (size_t i = 0; i < plan_count; i++) {
        /* The document is re-parsed from the CURRENT plaintext each time round, because the
         * previous splice moved every byte after it -- anchor_for's uniqueness check is
         * against the whole file. */
        JsonParseResult round = json_parse(plain, plain_len);
        if (!round.value) {
            fprintf(stderr, "the save's plaintext stopped being valid JSON mid-graft\n");
            goto done;
        }
        AnchorResult a = anchor_for(round.value, plain, plain_len, plan[i].field);
        if (!a.ok) {
            printf("skip %s -- %s\n", plan[i].field, a.error);
            anchor_free(&a);
            json_free(round.value);
            continue;
        }

        char *new_text = NULL;
        size_t new_len = 0;
        char serr[192];
        if (!json_serialize_compact(plan[i].new_value, &new_text, &new_len, serr,
                                     sizeof serr)) {
            fprintf(stderr, "cannot serialize the grafted value: %s\n", serr);
            anchor_free(&a);
            json_free(round.value);
            goto done;
        }

        const uint8_t *hit = find_bytes(plain, plain_len, a.anchor, a.anchor_len);
        size_t head = (size_t)(hit - plain);
        size_t prefix_len = a.anchor_len - a.value_len;
        size_t tail = plain_len - head - a.anchor_len;
        size_t built_len = head + prefix_len + new_len + tail;
        uint8_t *built = (uint8_t *)malloc(built_len + 1);
        memcpy(built, plain, head + prefix_len);
        memcpy(built + head + prefix_len, new_text, new_len);
        memcpy(built + head + prefix_len + new_len, hit + a.anchor_len, tail);
        built[built_len] = '\0';
        free(plain);
        plain = built;
        plain_len = built_len;

        free(new_text);
        anchor_free(&a);
        json_free(round.value);
    }

    {
        uint8_t *out = NULL;
        size_t out_len = 0;
        dg_encode(plain, plain_len, &out, &out_len);
        char s2[9], c2[9];
        if (dg_verify(out, out_len, s2, c2) != 1) {
            printf("rebuilt file does not verify -- nothing written\n");
            free(out);
            goto done;
        }
        FILE *f = fopen(file, "wb");
        if (!f) {
            fprintf(stderr, "cannot write the save\n");
            free(out);
            goto done;
        }
        fwrite(out, 1, out_len, f);
        fclose(f);
        free(out);

        char s3[9] = {0}, c3[9] = {0};
        FILE *back = fopen(file, "rb");
        if (back) {
            fseek(back, 0, SEEK_END);
            long n = ftell(back);
            fseek(back, 0, SEEK_SET);
            uint8_t *blob = (uint8_t *)malloc((size_t)n);
            if (fread(blob, 1, (size_t)n, back) == (size_t)n) {
                dg_verify(blob, (size_t)n, s3, c3);
            }
            free(blob);
            fclose(back);
        }
        /* len(plan), not the number actually spliced: a field skipped by anchor_for above
         * is still counted here, exactly as the Python counts it. */
        printf("grafted %zu field(s), checksum now %s\n", plan_count, s3);
        rc = 0;
    }

done:
    for (size_t i = 0; i < plan_count; i++) {
        free(plan[i].field);
        json_free(plan[i].old_value);
        json_free(plan[i].new_value);
    }
    free(plan);
    free(plain);
    sas4_load_free(&dst);
    sas4_load_free(&src);
    return rc;
}
