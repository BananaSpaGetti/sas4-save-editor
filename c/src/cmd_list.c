/*
 * `list` and `kinds` -- port of sas4.py's cmd_list and cmd_kinds. Task 4 of the
 * port-to-c-sas4-cli plan.
 */
#include "cmd_list.h"
#include "json.h"
#include "path.h"
#include "sas4load.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Python's str.lower() is Unicode-aware; this is ASCII-only. Every path in a real save is
 * ASCII (measured across every save in the repo -- see json.h), and a path is built from the
 * document's own keys, so the two agree wherever this tool is actually used. */
static void ascii_lower(const char *src, char *dst, size_t cap) {
    size_t i = 0;
    for (; src[i] && i + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        dst[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
    }
    dst[i] = '\0';
}

int cmd_list(const char *file, const char *grep, const char *type, const char *under) {
    SaveLoad sl = sas4_load(file);
    if (!sl.ok) {
        printf("%s\n", sl.error);
        sas4_load_free(&sl);
        return 1;
    }

    char grep_lower[512] = {0};
    if (grep) ascii_lower(grep, grep_lower, sizeof grep_lower);

    /* "/" + args.under.strip("/").lower() -- the prefix a path must start with. */
    char under_prefix[512] = {0};
    if (under) {
        const char *start = under;
        while (*start == '/') start++;
        size_t n = strlen(start);
        while (n > 0 && start[n - 1] == '/') n--;
        char trimmed[512];
        if (n >= sizeof trimmed) n = sizeof trimmed - 1;
        memcpy(trimmed, start, n);
        trimmed[n] = '\0';
        char lowered[512];
        ascii_lower(trimmed, lowered, sizeof lowered);
        snprintf(under_prefix, sizeof under_prefix, "/%.500s", lowered);
    }

    ScalarList scalars = {0};
    path_scalars(sl.document, "", &scalars);

    size_t shown = 0;
    for (size_t i = 0; i < scalars.count; i++) {
        const char *path = scalars.items[i].path;
        const JsonValue *value = scalars.items[i].value;
        const char *kind = path_kind_of(value);

        char path_lower[1024];
        if (grep || under) ascii_lower(path, path_lower, sizeof path_lower);

        if (grep && !strstr(path_lower, grep_lower)) continue;
        if (type && strcmp(kind, type) != 0) continue;
        if (under && strncmp(path_lower, under_prefix, strlen(under_prefix)) != 0) continue;

        char *dumped = NULL;
        size_t dumped_len = 0;
        char err[128];
        char snippet[64];
        if (json_serialize_default(value, &dumped, &dumped_len, err, sizeof err)) {
            size_t take = dumped_len < 50 ? dumped_len : 50;
            memcpy(snippet, dumped, take);
            snippet[take] = '\0';
            free(dumped);
        } else {
            /* Only a float can be refused (Decision 6); no real save holds one. */
            snprintf(snippet, sizeof snippet, "<unserializable>");
        }
        printf("  %-6s %-58s %s\n", kind, path, snippet);
        shown++;
    }
    printf("\n  %zu values\n", shown);

    path_scalars_free(&scalars);
    sas4_load_free(&sl);
    return 0;
}

/* --- kinds ---------------------------------------------------------------------------------- */

/* An insertion-ordered counter, so ties come out in first-seen order the way
 * collections.Counter.most_common() does (Python's sort is stable and a Counter iterates in
 * insertion order). */
typedef struct {
    char *name;
    size_t count;
} Tally;

typedef struct {
    Tally *items;
    size_t count;
    size_t cap;
} TallyList;

static void tally_add(TallyList *l, const char *name) {
    for (size_t i = 0; i < l->count; i++) {
        if (strcmp(l->items[i].name, name) == 0) {
            l->items[i].count++;
            return;
        }
    }
    if (l->count == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->items = realloc(l->items, l->cap * sizeof *l->items);
    }
    size_t n = strlen(name);
    l->items[l->count].name = malloc(n + 1);
    memcpy(l->items[l->count].name, name, n + 1);
    l->items[l->count].count = 1;
    l->count++;
}

/* most_common(): count descending, ties keeping insertion order -- a stable insertion sort,
 * the same shape (and for the same reason) as discover.c's profile sort. */
static void tally_sort(TallyList *l) {
    for (size_t i = 1; i < l->count; i++) {
        Tally key = l->items[i];
        size_t j = i;
        while (j > 0 && l->items[j - 1].count < key.count) {
            l->items[j] = l->items[j - 1];
            j--;
        }
        l->items[j] = key;
    }
}

static void tally_free(TallyList *l) {
    for (size_t i = 0; i < l->count; i++) free(l->items[i].name);
    free(l->items);
}

int cmd_kinds(const char *file) {
    SaveLoad sl = sas4_load(file);
    if (!sl.ok) {
        printf("%s\n", sl.error);
        sas4_load_free(&sl);
        return 1;
    }

    ScalarList scalars = {0};
    path_scalars(sl.document, "", &scalars);

    TallyList counts = {0};
    TallyList areas = {0};
    for (size_t i = 0; i < scalars.count; i++) {
        const char *kind = path_kind_of(scalars.items[i].value);
        tally_add(&counts, kind);
        if (strcmp(kind, "bool") == 0) {
            /* path.strip("/").split("/")[0].split("[")[0] -- the top-level section name,
             * with any array index dropped, so a 256-entry collection is one row not 256. */
            const char *p = scalars.items[i].path;
            while (*p == '/') p++;
            char head[256];
            size_t n = 0;
            while (p[n] && p[n] != '/' && p[n] != '[' && n + 1 < sizeof head) {
                head[n] = p[n];
                n++;
            }
            head[n] = '\0';
            tally_add(&areas, head);
        }
    }
    tally_sort(&counts);
    tally_sort(&areas);

    printf("by type:\n");
    for (size_t i = 0; i < counts.count; i++)
        printf("  %-8s %zu\n", counts.items[i].name, counts.items[i].count);
    printf("\nbooleans by top-level section:\n");
    for (size_t i = 0; i < areas.count; i++)
        printf("  %-28s %zu\n", areas.items[i].name, areas.items[i].count);

    tally_free(&counts);
    tally_free(&areas);
    path_scalars_free(&scalars);
    sas4_load_free(&sl);
    return 0;
}
