/*
 * `session` -- task 12 of the port-to-c-sas4-cli plan.
 */
#include "cmd_session.h"
#include "dgdata.h"
#include "edit.h"
#include "json.h"
#include "model.h"
#include "path.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GAME_PROCESS "SAS4-Win.exe"

/* sas4.py's SECRET_KEYS and CREDENTIAL_KEYS. SECRET_KEYS is what the read path hides;
 * CREDENTIAL_KEYS is the wider set the write path warns about. They deliberately differ:
 * nkapiID is an account id worth confirming before editing, but it is not a secret, and
 * printing it is the entire point of the "account id (nkapiID)" line below. */
static const char *SECRET_KEYS[] = {"sessionID"};
static const char *CREDENTIAL_KEYS[] = {"sessionID", "nkapiID"};

static bool in_list(const char *const *list, size_t n, const char *key) {
    for (size_t i = 0; i < n; i++) {
        if (strcmp(list[i], key) == 0) return true;
    }
    return false;
}

/* Python's str.strip() for the ASCII whitespace an argument can realistically carry. */
static void strip_in_place(char *s) {
    char *start = s;
    while (*start && isspace((unsigned char)*start)) start++;
    size_t n = strlen(start);
    while (n > 0 && isspace((unsigned char)start[n - 1])) n--;
    memmove(s, start, n);
    s[n] = '\0';
}

/* No memmem on mingw; the same helper cmd_set.c uses. */
static const uint8_t *find_bytes(const uint8_t *haystack, size_t haystack_len,
                                 const uint8_t *needle, size_t needle_len) {
    if (needle_len == 0 || haystack_len < needle_len) return NULL;
    for (size_t i = 0; i + needle_len <= haystack_len; i++) {
        if (memcmp(haystack + i, needle, needle_len) == 0) return haystack + i;
    }
    return NULL;
}

static size_t count_bytes(const uint8_t *haystack, size_t haystack_len,
                          const uint8_t *needle, size_t needle_len) {
    size_t n = 0, at = 0;
    while (at + needle_len <= haystack_len) {
        const uint8_t *hit = find_bytes(haystack + at, haystack_len - at, needle, needle_len);
        if (!hit) break;
        n++;
        at = (size_t)(hit - haystack) + needle_len;
    }
    return n;
}

static bool read_all(const char *path, uint8_t **out, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) {
        fclose(f);
        return false;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)size + 1);
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        free(buf);
        return false;
    }
    buf[got] = '\0';
    *out = buf;
    *out_len = got;
    return true;
}

/* json.dumps(value, separators=(",",":"), ensure_ascii=False) as bytes. */
static bool compact(const JsonValue *v, char **out, size_t *out_len) {
    char err[192];
    return json_serialize_compact(v, out, out_len, err, sizeof err);
}

/* redact(value): the length of a string, anything else unchanged. Non-strings pass through
 * exactly as they are -- an integer sessionID would print in full, which is the Python's
 * behaviour and is reproduced rather than improved on. */
static JsonValue *redacted(const JsonValue *v) {
    if (!v || v->type != JSON_STRING) return json_clone(v);
    char text[64];
    snprintf(text, sizeof text, "<%zu chars, hidden>", v->as.string.len);
    return json_new_string(text, strlen(text));
}

int cmd_session(const char *path, const char *set_expr, bool yes, bool force,
                const char *backups_dir) {
    uint8_t *raw = NULL;
    size_t raw_len = 0;
    if (!read_all(path, &raw, &raw_len)) {
        /* Python's open() raises OSError here, uncaught -- cmd_session does not go through
         * load(), so there is no "cannot read" wrapper to reproduce. Decision 11/12: the
         * wording is not matched, the nonzero exit and the absence of stdout output are. */
        fprintf(stderr, "cannot open the session file\n");
        return 1;
    }

    char stored[9], computed[9];
    int ok = dg_verify(raw, raw_len, stored, computed);

    uint8_t *plain = NULL;
    size_t plain_len = 0;
    if (!dg_decode(raw, raw_len, &plain, &plain_len)) {
        fprintf(stderr, "not a DGDATA file\n");
        free(raw);
        return 1;
    }
    JsonParseResult parsed = json_parse(plain, plain_len);
    if (!parsed.value) {
        fprintf(stderr, "the session file's contents are not valid JSON\n");
        free(plain);
        free(raw);
        return 1;
    }
    JsonValue *document = parsed.value;

    /* --- the read path ------------------------------------------------------------------ */
    if (!set_expr || !*set_expr) {
        printf("file      %s\n", path);
        printf("checksum  %s / %s -- %s\n", stored, computed, ok == 1 ? "VALID" : "MISMATCH");

        JsonValue *printable = json_clone(document);
        for (size_t i = 0; i < printable->as.object.count; i++) {
            JsonValue *section = printable->as.object.members[i].value;
            if (section->type != JSON_OBJECT) continue;
            for (size_t j = 0; j < section->as.object.count; j++) {
                JsonMember *m = &section->as.object.members[j];
                if (!in_list(SECRET_KEYS, sizeof SECRET_KEYS / sizeof *SECRET_KEYS, m->key))
                    continue;
                JsonValue *hidden = redacted(m->value);
                json_free(m->value);
                m->value = hidden;
            }
        }
        char *text = NULL;
        size_t text_len = 0;
        char err[192];
        if (json_serialize_indent(printable, &text, &text_len, err, sizeof err)) {
            fwrite(text, 1, text_len, stdout);
            printf("\n");
            free(text);
        }
        json_free(printable);

        /* document.get("user", {}).get("nkapiID"): an ABSENT "user" yields {} and then None,
         * so there is simply no account line. A "user" that is present but is not a dict is
         * different -- .get() on a list raises AttributeError, uncaught, so the Python exits
         * nonzero right here with everything above already printed. Measured, and
         * reproduced: same stdout, same exit code, wording of the traceback not matched
         * (Decision 12). */
        const JsonValue *user = json_object_get(document, "user");
        if (user && user->type != JSON_OBJECT) {
            json_free(document);
            free(plain);
            free(raw);
            return 1;
        }
        const JsonValue *account = user ? json_object_get(user, "nkapiID") : NULL;
        if (model_py_truthy(account)) {
            char *shown = model_py_str(account);
            printf("\naccount id (nkapiID): %s\n", shown);
            printf("  must match the profile's `link` and the save folder name\n");
            free(shown);
        }
        json_free(document);
        free(plain);
        free(raw);
        return 0;
    }

    /* --- the write path ------------------------------------------------------------------ */
    const char *eq = strchr(set_expr, '=');
    char key[256];
    const char *new_value = "";
    if (eq) {
        size_t n = (size_t)(eq - set_expr);
        if (n >= sizeof key) n = sizeof key - 1;
        memcpy(key, set_expr, n);
        key[n] = '\0';
        new_value = eq + 1;
    } else {
        snprintf(key, sizeof key, "%.255s", set_expr);
    }
    strip_in_place(key);

    int rc = 1;
    if (!*new_value) {
        printf("use  session --set nkapiID=<value>\n");
        goto done;
    }

    if (in_list(CREDENTIAL_KEYS, sizeof CREDENTIAL_KEYS / sizeof *CREDENTIAL_KEYS, key)) {
        printf("WARNING: %s is a credential the server issues at login.\n", key);
        printf("Editing it only makes sense to line this file up with a profile `link` and\n");
        printf("folder name you have already changed. It cannot make the server treat you as\n");
        printf("another account.\n");
        if (!yes) {
            /* input(prompt): the prompt goes to stdout with no newline and is flushed
             * before the read, so it is visible while the terminal waits. */
            printf("continue? [y/N] ");
            fflush(stdout);
            char reply[256];
            if (!fgets(reply, sizeof reply, stdin)) {
                /* Python's input() raises EOFError here, uncaught: no "cancelled" line,
                 * exit nonzero. Decision 12 again -- the effect, not the traceback. */
                goto done;
            }
            strip_in_place(reply);
            for (char *p = reply; *p; p++) *p = (char)tolower((unsigned char)*p);
            if (strcmp(reply, "y") != 0) {
                printf("cancelled\n");
                goto done;
            }
        }
    }

    if (edit_game_running() && !force) {
        printf("%s is running -- close it first, or pass --force\n", GAME_PROCESS);
        goto done;
    }

    char saved[1024], berr[256];
    if (!edit_backup(path, backups_dir, saved, sizeof saved, berr, sizeof berr)) {
        /* backup() raises OSError, which callers must let stop the write. */
        fprintf(stderr, "%s\n", berr);
        goto done;
    }
    printf("backup   %s\n", saved);

    {
        bool found = false;
        for (size_t i = 0; i < document->as.object.count && !found; i++) {
            JsonValue *node = document->as.object.members[i].value;
            if (node->type != JSON_OBJECT) continue;
            JsonValue *current = json_object_get(node, key);
            if (!current) continue;
            found = true;

            char *old_text = NULL;
            size_t old_len = 0;
            if (!compact(current, &old_text, &old_len)) {
                fprintf(stderr, "cannot serialize the current value\n");
                goto done;
            }
            size_t anchor_len = strlen(key) + 3 + old_len; /* "key": + value */
            uint8_t *anchor = (uint8_t *)malloc(anchor_len + 1);
            int written = snprintf((char *)anchor, anchor_len + 1, "\"%s\":", key);
            memcpy(anchor + written, old_text, old_len);
            anchor_len = (size_t)written + old_len;

            if (count_bytes(plain, plain_len, anchor, anchor_len) != 1) {
                printf("cannot pin down %s uniquely\n", key);
                free(anchor);
                free(old_text);
                goto done;
            }

            CoerceResult coerced = path_coerce(new_value, current);
            if (!coerced.ok) {
                /* coerce() raises ValueError, and cmd_session -- unlike cmd_set, which
                 * catches it and formats a "holds ... ; ..." line -- lets it propagate.
                 * main() catches only SaveError, so the Python exits nonzero with NOTHING
                 * further on stdout. That rough edge in the reference is reproduced rather
                 * than smoothed over: the message goes to stderr here. */
                fprintf(stderr, "%s\n", coerced.error);
                free(anchor);
                free(old_text);
                goto done;
            }
            char *new_text = NULL;
            size_t new_len = 0;
            if (!compact(coerced.value, &new_text, &new_len)) {
                fprintf(stderr, "cannot serialize the new value\n");
                json_free(coerced.value);
                free(anchor);
                free(old_text);
                goto done;
            }
            json_free(coerced.value);

            /* plain.replace(anchor, replacement, 1): the anchor's "key": prefix is kept and
             * only its value suffix swapped. */
            const uint8_t *hit = find_bytes(plain, plain_len, anchor, anchor_len);
            size_t head = (size_t)(hit - plain);
            size_t tail = plain_len - head - anchor_len;
            size_t prefix_len = anchor_len - old_len;
            size_t built_len = head + prefix_len + new_len + tail;
            uint8_t *built = (uint8_t *)malloc(built_len + 1);
            memcpy(built, plain, head + prefix_len);
            memcpy(built + head + prefix_len, new_text, new_len);
            memcpy(built + head + prefix_len + new_len, hit + anchor_len, tail);
            built[built_len] = '\0';
            free(plain);
            plain = built;
            plain_len = built_len;

            free(new_text);
            free(anchor);
            free(old_text);
        }
        if (!found) {
            printf("no field named %s in the session\n", key);
            goto done;
        }
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
        FILE *f = fopen(path, "wb");
        if (!f) {
            fprintf(stderr, "cannot write the session file\n");
            free(out);
            goto done;
        }
        fwrite(out, 1, out_len, f);
        fclose(f);
        free(out);

        /* The checksum is read back off disk, as the Python's own final line does. */
        uint8_t *back = NULL;
        size_t back_len = 0;
        char s3[9] = {0}, c3[9] = {0};
        if (read_all(path, &back, &back_len)) {
            dg_verify(back, back_len, s3, c3);
            free(back);
        }
        printf("set %s, checksum now %s\n", key, s3);
        rc = 0;
    }

done:
    json_free(document);
    free(plain);
    free(raw);
    return rc;
}
