/*
 * `contribute` -- tasks 14 and 15 of the port-to-c-sas4-cli plan.
 */
#include "cmd_contribute.h"
#include "model.h"
#include "path.h"
#include "plans.h"
#include "sas4load.h"

#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONTRIBUTE_VERSION 1
#define CLAIMED_RUN 4

/* --- a growable text buffer --------------------------------------------------------------- */

typedef struct {
    char *data;
    size_t len, cap;
} Text;

static void text_reserve(Text *t, size_t extra) {
    if (t->len + extra + 1 <= t->cap) return;
    size_t want = t->cap ? t->cap * 2 : 8192;
    while (want < t->len + extra + 1) want *= 2;
    t->data = (char *)realloc(t->data, want);
    t->cap = want;
}

static void text_add(Text *t, const char *s, size_t n) {
    text_reserve(t, n);
    memcpy(t->data + t->len, s, n);
    t->len += n;
    t->data[t->len] = '\0';
}

/* One line plus the newline that "\n".join puts BETWEEN lines -- so the separator is added
 * before each line except the first, and the finished report has no trailing newline. */
static void line(Text *t, const char *fmt, ...) {
    if (t->len || t->data) text_add(t, "\n", 1);
    char stack[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(stack, sizeof stack, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof stack) {
        text_add(t, stack, (size_t)n);
        return;
    }
    char *big = (char *)malloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    text_add(t, big, (size_t)n);
    free(big);
}

/* --- the schema section ------------------------------------------------------------------- */

typedef struct {
    char *path;
    char **types;
    size_t type_count;
} SchemaPath;

typedef struct {
    SchemaPath *items;
    size_t count, cap;
} Schema;

/* Python's type(node).__name__ for the JSON types. A dict is never recorded as a leaf --
 * walk() recurses into it and records nothing for the dict itself, so an empty object
 * contributes no path at all. */
static const char *type_name_of(const JsonValue *v) {
    switch (v->type) {
        case JSON_NULL: return "NoneType";
        case JSON_BOOL: return "bool";
        case JSON_INT: return "int";
        case JSON_FLOAT: return "float";
        case JSON_STRING: return "str";
        default: return "object";
    }
}

/* seen.setdefault(path, set()).add(name) */
static void schema_add(Schema *s, const char *path, const char *name) {
    SchemaPath *entry = NULL;
    for (size_t i = 0; i < s->count; i++) {
        if (strcmp(s->items[i].path, path) == 0) {
            entry = &s->items[i];
            break;
        }
    }
    if (!entry) {
        if (s->count == s->cap) {
            s->cap = s->cap ? s->cap * 2 : 256;
            s->items = (SchemaPath *)realloc(s->items, s->cap * sizeof *s->items);
        }
        entry = &s->items[s->count++];
        entry->path = (char *)malloc(strlen(path) + 1);
        strcpy(entry->path, path);
        entry->types = NULL;
        entry->type_count = 0;
    }
    for (size_t i = 0; i < entry->type_count; i++) {
        if (strcmp(entry->types[i], name) == 0) return; /* a set, not a list */
    }
    entry->types = (char **)realloc(entry->types, (entry->type_count + 1) * sizeof *entry->types);
    entry->types[entry->type_count] = (char *)malloc(strlen(name) + 1);
    strcpy(entry->types[entry->type_count], name);
    entry->type_count++;
}

static void schema_walk(const JsonValue *node, const char *path, Schema *s) {
    if (node->type == JSON_OBJECT) {
        for (size_t i = 0; i < node->as.object.count; i++) {
            const JsonMember *m = &node->as.object.members[i];
            size_t n = strlen(path) + 1 + strlen(m->key) + 1;
            char *child = (char *)malloc(n);
            snprintf(child, n, "%s/%s", path, m->key);
            schema_walk(m->value, child, s);
            free(child);
        }
    } else if (node->type == JSON_ARRAY) {
        schema_add(s, path, "list");
        size_t n = strlen(path) + 3;
        char *child = (char *)malloc(n);
        snprintf(child, n, "%s/*", path);
        for (size_t i = 0; i < node->as.array.count; i++) {
            schema_walk(node->as.array.items[i], child, s);
        }
        free(child);
    } else {
        schema_add(s, path, type_name_of(node));
    }
}

static int cmp_str_ptr(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int cmp_schema_path(const void *a, const void *b) {
    return strcmp(((const SchemaPath *)a)->path, ((const SchemaPath *)b)->path);
}

/* ["%s  %s" % (path, "/".join(sorted(types)))] over sorted(seen). Python sorts strings by
 * code point, and UTF-8 byte order is code-point order, so strcmp is the same ordering. */
static void schema_build(const JsonValue *document, Schema *out) {
    memset(out, 0, sizeof *out);
    schema_walk(document, "", out);
    qsort(out->items, out->count, sizeof *out->items, cmp_schema_path);
    for (size_t i = 0; i < out->count; i++) {
        qsort(out->items[i].types, out->items[i].type_count,
              sizeof *out->items[i].types, cmp_str_ptr);
    }
}

static void schema_free(Schema *s) {
    for (size_t i = 0; i < s->count; i++) {
        for (size_t j = 0; j < s->items[i].type_count; j++) free(s->items[i].types[j]);
        free(s->items[i].types);
        free(s->items[i].path);
    }
    free(s->items);
    memset(s, 0, sizeof *s);
}

/* --- helpers over the document ------------------------------------------------------------ */

/* `document.get(key) or {}` -- a missing key, a null, or an empty object all become "no
 * section", and every .get() against it then yields None. */
static const JsonValue *section_or_empty(const JsonValue *document, const char *key) {
    const JsonValue *v = json_object_get(document, key);
    if (!v || v->type != JSON_OBJECT || v->as.object.count == 0) return NULL;
    return v;
}

static const JsonValue *get_or_null(const JsonValue *object, const char *key) {
    return object ? json_object_get(object, key) : NULL;
}

/* str(value) for the %s slots; None prints as "None". */
static char *shown(const JsonValue *v) {
    return model_py_str(v);
}

/* --- the report ---------------------------------------------------------------------------- */

char *contribute_report(const JsonValue *document, int slot) {
    if (!document || document->type != JSON_OBJECT) return NULL;

    const JsonValue *version = section_or_empty(document, "Version");
    const JsonValue *globals_ = section_or_empty(document, "Global");

    /* (document.get("Inventory", {}).get("Profile%d") or {}).get("Skills") or {} -- an
     * Inventory that is present but is not a dict makes Python raise AttributeError. */
    const JsonValue *inventory = json_object_get(document, "Inventory");
    if (inventory && inventory->type != JSON_OBJECT) return NULL;
    char profile_key[32];
    snprintf(profile_key, sizeof profile_key, "Profile%d", slot);
    const JsonValue *profile = get_or_null(inventory, profile_key);
    if (profile && profile->type != JSON_OBJECT) return NULL;
    if (profile && profile->as.object.count == 0) profile = NULL;
    const JsonValue *skills = get_or_null(profile, "Skills");
    if (skills && skills->type != JSON_OBJECT) return NULL;
    if (skills && skills->as.object.count == 0) skills = NULL;

    char claimed_path[64];
    snprintf(claimed_path, sizeof claimed_path,
             "Inventory/Profile%d/Strongboxes/Claimed", slot);
    PathAtResult claimed_at = path_at((JsonValue *)document, claimed_path);
    /* The except branch's fallback is [], an EMPTY LIST -- not "no value". So a path that
     * does not resolve still counts as a list, prints a raw length of 0, and goes through
     * claimed_items; only a path that resolves to something that is not a list prints
     * "not a list". Measured: getting this wrong differs on every empty character slot. */
    const JsonValue *claimed = claimed_at.ok ? claimed_at.value : NULL;
    bool claimed_is_list = !claimed_at.ok || (claimed && claimed->type == JSON_ARRAY);
    size_t claimed_len = (claimed && claimed->type == JSON_ARRAY)
                             ? claimed->as.array.count
                             : 0;

    Text t = {0};
    t.data = (char *)malloc(1);
    t.data[0] = '\0';
    t.cap = 1;
    bool first = true;
    (void)first;

    /* The first line must not be preceded by a separator; line() adds one whenever the
     * buffer already exists, so emit the head directly. */
    {
        char head[128];
        int n = snprintf(head, sizeof head, "## SAS4 save reading (report format %d)",
                         CONTRIBUTE_VERSION);
        text_add(&t, head, (size_t)n);
    }
    line(&t, "%s", "");
    line(&t, "Produced by `py sas4.py contribute`. It holds no account id, no player name and no");
    line(&t, "file path -- only numbers about a character and the shape of the file. Read it before");
    line(&t, "you post it; that is what it is printed for.");
    line(&t, "%s", "");
    line(&t, "### The file");
    line(&t, "%s", "");
    line(&t, "| | |");
    line(&t, "|---|---|");

    char *v1 = shown(get_or_null(version, "LastGame"));
    char *v2 = shown(get_or_null(version, "OriginalVersion"));
    char *v3 = shown(get_or_null(version, "Profile"));
    char *v4 = shown(get_or_null(skills, "PlayerLevel"));
    char *v5 = shown(get_or_null(globals_, "HighestRank"));
    char *v6 = shown(get_or_null(skills, "Class"));
    line(&t, "| game version | %s |", v1);
    line(&t, "| original version | %s |", v2);
    line(&t, "| profile format | %s |", v3);
    line(&t, "| character slot | %d |", slot);
    line(&t, "| character level | %s |", v4);
    line(&t, "| highest rank | %s |", v5);
    line(&t, "| class | %s |", v6);
    free(v1); free(v2); free(v3); free(v4); free(v5); free(v6);

    line(&t, "%s", "");
    line(&t, "### Masteries");
    line(&t, "%s", "");
    line(&t, "**This is the part that needs people.** Twenty-five of the twenty-seven tracks below");
    line(&t, "have no name yet. If you know what one is -- play a mission with one weapon type and");
    line(&t, "watch which row moves, or read a number off the game's own mastery screen and find it");
    line(&t, "here -- say so in the issue and it gets named in the next release.");
    line(&t, "%s", "");

    line(&t, "| track | XP | level | what it is |");
    line(&t, "|---:|---:|---:|---|");
    {
        MasteryRowList rows = {0};
        plans_mastery_rows(document, slot, &rows);
        for (size_t i = 0; i < rows.count; i++) {
            char *xp = shown(rows.items[i].xp);
            char *lvl = shown(rows.items[i].level);
            const char *name = model_mastery_name(rows.items[i].index);
            line(&t, "| %zu | %s | %s | %s |", rows.items[i].index, xp, lvl,
                 name ? name : "");
            free(xp);
            free(lvl);
        }
        plans_mastery_rows_free(&rows);
    }

    /* _item_section, twice. */
    for (int which = 0; which < 2; which++) {
        const char *key = which == 0 ? "Weapons" : "Equipment";
        line(&t, "%s", "");
        line(&t, "### %s owned", which == 0 ? "Weapons" : "Equipment");
        line(&t, "%s", "");

        char item_path[64];
        snprintf(item_path, sizeof item_path, "Inventory/Profile%d/%s", slot, key);
        PathAtResult at = path_at((JsonValue *)document, item_path);
        const JsonValue *rows = at.ok ? at.value : NULL;
        if (!rows || rows->type != JSON_ARRAY || rows->as.array.count == 0) {
            line(&t, "(none in this save)");
            continue;
        }
        line(&t, "| id | grade | augment 1 | augment 2 |");
        line(&t, "|---:|---:|---:|---:|");
        for (size_t i = 0; i < rows->as.array.count; i++) {
            const JsonValue *row = rows->as.array.items[i];
            if (row->type != JSON_OBJECT) continue;
            char *a = shown(json_object_get(row, "ID"));
            char *b = shown(json_object_get(row, "Grade"));
            char *c = shown(json_object_get(row, "Augment1ID"));
            char *d = shown(json_object_get(row, "Augment2ID"));
            line(&t, "| %s | %s | %s | %s |", a, b, c, d);
            free(a); free(b); free(c); free(d);
        }
    }

    size_t run_count = 0;
    if (claimed_is_list) {
        ItemNames empty;
        memset(&empty, 0, sizeof empty);
        ClaimedRowList runs = {0};
        plans_claimed_items(document, &empty, slot, &runs);
        run_count = runs.count;
        plans_claimed_items_free(&runs);
    }

    line(&t, "%s", "");
    line(&t, "### Strongboxes/Claimed");
    line(&t, "%s", "");
    line(&t, "| | |");
    line(&t, "|---|---|");
    if (claimed_is_list) {
        line(&t, "| raw length | %zu |", claimed_len);
    } else {
        line(&t, "| raw length | not a list |");
    }
    line(&t, "| runs parsed | %zu |", run_count);
    if (claimed_is_list) {
        line(&t, "| length accounted for | %zu of %zu |", run_count * CLAIMED_RUN, claimed_len);
    } else {
        line(&t, "| length accounted for | %zu of ? |", run_count * CLAIMED_RUN);
    }
    line(&t, "%s", "");
    line(&t, "A mismatch between the last two rows is worth reporting on its own: it means the");
    line(&t, "four-element run this tool reads is not what the game wrote into your file.");
    line(&t, "%s", "");
    line(&t, "### Every path in the file");
    line(&t, "%s", "");
    line(&t, "Names and types only -- no values. This is how fields nobody has documented get");
    line(&t, "found, and it is the one section not built from a list of named fields, which is why");
    line(&t, "it may not carry values.");
    line(&t, "%s", "");

    Schema schema;
    schema_build(document, &schema);
    line(&t, "<details><summary>%zu paths</summary>", schema.count);
    line(&t, "%s", "");
    line(&t, "```");
    for (size_t i = 0; i < schema.count; i++) {
        Text joined = {0};
        joined.data = (char *)malloc(1);
        joined.data[0] = '\0';
        joined.cap = 1;
        for (size_t j = 0; j < schema.items[i].type_count; j++) {
            if (j) text_add(&joined, "/", 1);
            text_add(&joined, schema.items[i].types[j], strlen(schema.items[i].types[j]));
        }
        line(&t, "%s  %s", schema.items[i].path, joined.data);
        free(joined.data);
    }
    schema_free(&schema);
    line(&t, "```");
    line(&t, "%s", "");
    line(&t, "</details>");
    line(&t, "%s", "");

    return t.data;
}

/* --- the guard ------------------------------------------------------------------------------ */

static bool is_lower_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

static bool is_alpha(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static const char *DESC_HEX = "a long hex string, which is the shape of an account id";
static const char *DESC_DIGITS = "a very long number, which is the shape of an account id";
static const char *DESC_PATH = "a path on your machine";
static const char *DESC_USERDATA =
    "a Steam userdata path, which contains your account number";

static void push_leak(LeakedId **out, size_t *count, size_t *cap, const char *description,
                      const char *start, size_t len) {
    if (*count == *cap) {
        *cap = *cap ? *cap * 2 : 8;
        *out = (LeakedId *)realloc(*out, *cap * sizeof **out);
    }
    (*out)[*count].description = description;
    (*out)[*count].match = (char *)malloc(len + 1);
    memcpy((*out)[*count].match, start, len);
    (*out)[*count].match[len] = '\0';
    (*count)++;
}

/* re.findall over one character-class run pattern: greedy, non-overlapping, left to right.
 * A run shorter than `minimum` yields nothing and the scan resumes after it. */
static void scan_run(const char *text, bool (*member)(char), size_t minimum,
                     const char *description, LeakedId **out, size_t *count, size_t *cap) {
    size_t i = 0;
    while (text[i]) {
        if (!member(text[i])) {
            i++;
            continue;
        }
        size_t start = i;
        while (text[i] && member(text[i])) i++;
        size_t len = i - start;
        if (len >= minimum) push_leak(out, count, cap, description, text + start, len);
    }
}

void contribute_scan(const char *text, LeakedId **out, size_t *out_count) {
    *out = NULL;
    *out_count = 0;
    size_t cap = 0;

    /* The patterns are applied in ID_PATTERNS order, each over the whole text, so the
     * findings come back in the same order the Python's nested loop produces them. */
    scan_run(text, is_lower_hex, 24, DESC_HEX, out, out_count, &cap);
    scan_run(text, is_digit, 15, DESC_DIGITS, out, out_count, &cap);

    /* [A-Za-z]:[\\/] */
    for (size_t i = 0; text[i] && text[i + 1] && text[i + 2]; i++) {
        if (is_alpha(text[i]) && text[i + 1] == ':'
            && (text[i + 2] == '\\' || text[i + 2] == '/')) {
            push_leak(out, out_count, &cap, DESC_PATH, text + i, 3);
            i += 2; /* non-overlapping, like re.findall */
        }
    }

    /* [Uu]serdata */
    for (size_t i = 0; text[i]; i++) {
        if ((text[i] == 'U' || text[i] == 'u') && strncmp(text + i + 1, "serdata", 7) == 0) {
            push_leak(out, out_count, &cap, DESC_USERDATA, text + i, 8);
            i += 7;
        }
    }
}

void contribute_scan_free(LeakedId *found, size_t count) {
    for (size_t i = 0; i < count; i++) free(found[i].match);
    free(found);
}

/* --- cmd_contribute -------------------------------------------------------------------------- */

static void makedirs(const char *path) {
    if (!path || !*path) return;
    char buf[1024];
    snprintf(buf, sizeof buf, "%.1000s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '\\' || *p == '/') {
            char saved = *p;
            *p = '\0';
            if (!(strlen(buf) == 2 && buf[1] == ':')) CreateDirectoryA(buf, NULL);
            *p = saved;
        }
    }
    CreateDirectoryA(buf, NULL);
}

int cmd_contribute(const char *file, int slot, bool print_only, const char *data_dir) {
    SaveLoad sl = sas4_load(file);
    if (!sl.ok) {
        printf("%s\n", sl.error);
        sas4_load_free(&sl);
        return 1;
    }

    char *report = contribute_report(sl.document, slot);
    if (!report) {
        /* The document's shape makes the Python raise before anything is printed. */
        sas4_load_free(&sl);
        return 1;
    }

    LeakedId *leaked = NULL;
    size_t leaked_count = 0;
    contribute_scan(report, &leaked, &leaked_count);
    if (leaked_count) {
        printf("refusing to write this report -- it contains something shaped like an id:\n");
        size_t show = leaked_count < 10 ? leaked_count : 10;
        for (size_t i = 0; i < show; i++) {
            printf("  %-56s %s\n", leaked[i].match, leaked[i].description);
        }
        printf("\nThis is a bug in the tool, not something you did. Please report it, with the\n");
        printf("lines above and without the report itself.\n");
        contribute_scan_free(leaked, leaked_count);
        free(report);
        sas4_load_free(&sl);
        return 1;
    }
    contribute_scan_free(leaked, leaked_count);

    printf("%s\n", report);
    if (print_only) {
        printf("\n(--print: nothing written)\n");
        free(report);
        sas4_load_free(&sl);
        return 0;
    }

    makedirs(data_dir);
    char out[1024];
    snprintf(out, sizeof out, "%.900s\\contribution.md", data_dir);
    /* Text mode: Python writes through a text-mode file object, so the file is CRLF. */
    FILE *f = fopen(out, "w");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", out);
        free(report);
        sas4_load_free(&sl);
        return 1;
    }
    fwrite(report, 1, strlen(report), f);
    fclose(f);

    printf("\nwritten to  %s\n", out);
    printf("\nEverything above is the whole of it. If you are happy to share it, open an issue\n");
    printf("at https://github.com/BananaSpaGetti/sas4-save-editor/issues -- there is a\n");
    printf("'Mastery track' template -- and paste it in. Nothing was sent.\n");
    free(report);
    sas4_load_free(&sl);
    return 0;
}
