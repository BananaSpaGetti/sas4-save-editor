/*
 * `items` -- task 10 of the port-to-c-sas4-cli plan.
 */
#include "cmd_items.h"
#include "http.h"
#include "json.h"
#include "model.h"

#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- small helpers ---------------------------------------------------------------------- */

static char *dupstr(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* os.makedirs(path, exist_ok=True) -- same shape as cmd_dump.c's. */
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

/* The directory part of a path, or "" when it has none -- os.path.dirname. */
static void dirname_of(const char *path, char *out, size_t cap) {
    snprintf(out, cap, "%.*s", (int)(cap - 1), path);
    char *cut = NULL;
    for (char *p = out; *p; p++) {
        if (*p == '\\' || *p == '/') cut = p;
    }
    if (cut) {
        *cut = '\0';
    } else {
        out[0] = '\0';
    }
}

/* Python's str() for a "Name" that is not a JSON string. Measured: every Name in the real
 * cache is a string, so this only guards against a future table shape. */
static const char *name_of(const JsonValue *v) {
    return (v && v->type == JSON_STRING) ? v->as.string.data : "";
}

/* --- item_names(): {domain: {id: (name, category)}} ------------------------------------- */

typedef struct {
    int64_t id;
    char *name;
    char *category;
} NamedItem;

typedef struct {
    char *domain;
    NamedItem *items;
    size_t count, cap;
} DomainTable;

typedef struct {
    DomainTable *domains;
    size_t count, cap;
} NameTable;

static DomainTable *table_for(NameTable *t, const char *domain) {
    for (size_t i = 0; i < t->count; i++) {
        if (strcmp(t->domains[i].domain, domain) == 0) return &t->domains[i];
    }
    if (t->count == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 8;
        t->domains = (DomainTable *)realloc(t->domains, t->cap * sizeof *t->domains);
    }
    DomainTable *d = &t->domains[t->count++];
    memset(d, 0, sizeof *d);
    d->domain = dupstr(domain);
    return d;
}

/* setdefault -- the first entry for an id wins, and insertion order is kept. */
static void domain_add(DomainTable *d, int64_t id, const char *name, const char *category) {
    for (size_t i = 0; i < d->count; i++) {
        if (d->items[i].id == id) return;
    }
    if (d->count == d->cap) {
        d->cap = d->cap ? d->cap * 2 : 64;
        d->items = (NamedItem *)realloc(d->items, d->cap * sizeof *d->items);
    }
    NamedItem *it = &d->items[d->count++];
    it->id = id;
    it->name = dupstr(name);
    it->category = dupstr(category);
}

static void names_walk(const JsonValue *node, const char *category, DomainTable *d) {
    if (node->type == JSON_OBJECT) {
        const JsonValue *name_v = json_object_get(node, "Name");
        const JsonValue *id_v = json_object_get(node, "ID");
        if (name_v && id_v && model_py_is_int(id_v)) {
            domain_add(d, model_py_int_value(id_v), name_of(name_v), category);
            return;
        }
        for (size_t i = 0; i < node->as.object.count; i++) {
            names_walk(node->as.object.members[i].value, node->as.object.members[i].key, d);
        }
    } else if (node->type == JSON_ARRAY) {
        for (size_t i = 0; i < node->as.array.count; i++) {
            names_walk(node->as.array.items[i], category, d);
        }
    }
}

static void name_table_free(NameTable *t) {
    for (size_t i = 0; i < t->count; i++) {
        for (size_t j = 0; j < t->domains[i].count; j++) {
            free(t->domains[i].items[j].name);
            free(t->domains[i].items[j].category);
        }
        free(t->domains[i].items);
        free(t->domains[i].domain);
    }
    free(t->domains);
    memset(t, 0, sizeof *t);
}

/* section.replace("_info", "") -- the four real section names only ever carry it as a
 * trailing suffix (same reasoning as plans.c's items_load). */
static void strip_info(char *s) {
    static const char SUFFIX[] = "_info";
    size_t n = strlen(s), m = strlen(SUFFIX);
    if (n >= m && strcmp(s + n - m, SUFFIX) == 0) s[n - m] = '\0';
}

static void build_name_table(const JsonValue *document, NameTable *out) {
    memset(out, 0, sizeof *out);
    if (!document || document->type != JSON_OBJECT) return;
    for (size_t i = 0; i < document->as.object.count; i++) {
        const JsonMember *section = &document->as.object.members[i];
        char *domain = dupstr(section->key);
        strip_info(domain);
        DomainTable *d = table_for(out, domain);
        names_walk(section->value, domain, d);
        free(domain);
    }
}

/* --- item_catalog(): [(domain, tier, category, id, name)] -------------------------------- */

typedef struct {
    char *domain, *tier, *category, *name;
    int64_t id;
} CatalogRow;

typedef struct {
    CatalogRow *rows;
    size_t count, cap;
} Catalog;

static void catalog_add(Catalog *c, const char *domain, const char *tier,
                        const char *category, int64_t id, const char *name) {
    if (c->count == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 128;
        c->rows = (CatalogRow *)realloc(c->rows, c->cap * sizeof *c->rows);
    }
    CatalogRow *r = &c->rows[c->count++];
    r->domain = dupstr(domain);
    r->tier = dupstr(tier);
    r->category = dupstr(category);
    r->id = id;
    r->name = dupstr(name);
}

/* `tier or "-"` / `category or "-"` -- Python's None becomes NULL here, and the empty
 * string is falsy in Python too, so both collapse to "-". */
static const char *or_dash(const char *s) {
    return (s && *s) ? s : "-";
}

static void catalog_walk(const JsonValue *node, const char *domain, const char *tier,
                         const char *category, Catalog *c) {
    if (node->type == JSON_OBJECT) {
        const JsonValue *name_v = json_object_get(node, "Name");
        const JsonValue *id_v = json_object_get(node, "ID");
        if (name_v && id_v && model_py_is_int(id_v)) {
            catalog_add(c, domain, or_dash(tier), or_dash(category),
                        model_py_int_value(id_v), name_of(name_v));
            return;
        }
        for (size_t i = 0; i < node->as.object.count; i++) {
            const JsonMember *m = &node->as.object.members[i];
            /* walk(value, domain, tier or key, key if tier else category) */
            const char *next_tier = (tier && *tier) ? tier : m->key;
            const char *next_category = (tier && *tier) ? m->key : category;
            catalog_walk(m->value, domain, next_tier, next_category, c);
        }
    } else if (node->type == JSON_ARRAY) {
        for (size_t i = 0; i < node->as.array.count; i++) {
            catalog_walk(node->as.array.items[i], domain, tier, category, c);
        }
    }
}

static void build_catalog(const JsonValue *document, Catalog *out) {
    memset(out, 0, sizeof *out);
    if (!document || document->type != JSON_OBJECT) return;
    for (size_t i = 0; i < document->as.object.count; i++) {
        const JsonMember *section = &document->as.object.members[i];
        char *domain = dupstr(section->key);
        strip_info(domain);
        catalog_walk(section->value, domain, NULL, NULL, out);
        free(domain);
    }
}

static void catalog_free(Catalog *c) {
    for (size_t i = 0; i < c->count; i++) {
        free(c->rows[i].domain);
        free(c->rows[i].tier);
        free(c->rows[i].category);
        free(c->rows[i].name);
    }
    free(c->rows);
    memset(c, 0, sizeof *c);
}

/* --- write_catalog ----------------------------------------------------------------------- */

/* One (domain, tier, category) bucket, holding the (id, name) pairs in encounter order. */
typedef struct {
    const char *domain, *tier, *category;
    const CatalogRow **rows;
    size_t count, cap;
} Group;

static int group_key_cmp(const Group *a, const Group *b) {
    /* sorted(picked.items()) on the (domain, tier, category) tuple. Every one of these is
     * ASCII in the real table, so strcmp orders them the way Python's code-point compare
     * does. `picked` is already filtered to one domain, but compare it anyway for the same
     * total order. */
    int c = strcmp(a->domain, b->domain);
    if (c) return c;
    c = strcmp(a->tier, b->tier);
    if (c) return c;
    return strcmp(a->category, b->category);
}

static int row_cmp(const void *pa, const void *pb) {
    /* sorted(items) on the (id, name) tuple. */
    const CatalogRow *a = *(const CatalogRow *const *)pa;
    const CatalogRow *b = *(const CatalogRow *const *)pb;
    if (a->id < b->id) return -1;
    if (a->id > b->id) return 1;
    return strcmp(a->name, b->name);
}

static size_t write_catalog(const char *path, const Catalog *catalog) {
    if (catalog->count == 0) return 0;

    Group *groups = NULL;
    size_t gcount = 0, gcap = 0;
    for (size_t i = 0; i < catalog->count; i++) {
        const CatalogRow *r = &catalog->rows[i];
        Group *g = NULL;
        for (size_t j = 0; j < gcount; j++) {
            if (strcmp(groups[j].domain, r->domain) == 0
                && strcmp(groups[j].tier, r->tier) == 0
                && strcmp(groups[j].category, r->category) == 0) {
                g = &groups[j];
                break;
            }
        }
        if (!g) {
            if (gcount == gcap) {
                gcap = gcap ? gcap * 2 : 32;
                groups = (Group *)realloc(groups, gcap * sizeof *groups);
            }
            g = &groups[gcount++];
            memset(g, 0, sizeof *g);
            g->domain = r->domain;
            g->tier = r->tier;
            g->category = r->category;
        }
        if (g->count == g->cap) {
            g->cap = g->cap ? g->cap * 2 : 16;
            g->rows = (const CatalogRow **)realloc(g->rows, g->cap * sizeof *g->rows);
        }
        g->rows[g->count++] = r;
    }

    /* Text mode: Python's open(path, "w") writes through a text-mode file object, so every
     * newline below becomes CRLF on Windows. Writing this in binary mode would be a byte
     * difference in every line. */
    FILE *out = fopen(path, "w");
    if (!out) {
        for (size_t j = 0; j < gcount; j++) free(groups[j].rows);
        free(groups);
        return 0;
    }
    fputs("# What `give` can grant\n\n", out);
    fprintf(out, "%zu items, from the community table `py sas4.py items` downloads.\n\n",
            catalog->count);
    fputs("Grant one with its ID:\n\n```\npy sas4.py give <id>\n"
          "py sas4.py give <id> --kind weapon      when the id is both a weapon and equipment\n"
          "py sas4.py give <id> --grade 12 --bonus 10\n```\n\n"
          "IDs are unique inside a domain but not across them: equipment 101 and\n"
          "weapon 101 are different things, which is what `--kind` settles.\n\n"
          "Tiers: **normal**, **red**, **black**, **factions**.\n\n", out);

    static const char *DOMAIN_ORDER[] = {"weapon", "equipment", "turret", "premium"};
    for (size_t d = 0; d < sizeof DOMAIN_ORDER / sizeof *DOMAIN_ORDER; d++) {
        const char *domain = DOMAIN_ORDER[d];
        size_t picked_n = 0, total = 0;
        for (size_t j = 0; j < gcount; j++) {
            if (strcmp(groups[j].domain, domain) == 0) {
                picked_n++;
                total += groups[j].count;
            }
        }
        if (picked_n == 0) continue;
        fprintf(out, "\n## %s  (%zu)\n", domain, total);

        /* sorted(picked.items()) -- an insertion sort over just this domain's groups. */
        Group **picked = (Group **)malloc(picked_n * sizeof *picked);
        size_t n = 0;
        for (size_t j = 0; j < gcount; j++) {
            if (strcmp(groups[j].domain, domain) != 0) continue;
            size_t k = n++;
            while (k > 0 && group_key_cmp(picked[k - 1], &groups[j]) > 0) {
                picked[k] = picked[k - 1];
                k--;
            }
            picked[k] = &groups[j];
        }
        for (size_t j = 0; j < picked_n; j++) {
            Group *g = picked[j];
            fprintf(out, "\n### %s / %s  (%zu)\n\n", g->tier, g->category, g->count);
            fputs("| ID | Name |\n|---:|------|\n", out);
            const CatalogRow **sorted =
                (const CatalogRow **)malloc(g->count * sizeof *sorted);
            memcpy(sorted, g->rows, g->count * sizeof *sorted);
            qsort(sorted, g->count, sizeof *sorted, row_cmp);
            for (size_t k = 0; k < g->count; k++) {
                fprintf(out, "| %lld | %s |\n", (long long)sorted[k]->id, sorted[k]->name);
            }
            free(sorted);
        }
        free(picked);
    }
    fclose(out);

    for (size_t j = 0; j < gcount; j++) free(groups[j].rows);
    free(groups);
    return catalog->count;
}

/* --- cmd_items --------------------------------------------------------------------------- */

static JsonValue *read_json_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) {
        fclose(f);
        return NULL;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)size + 1);
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        free(buf);
        return NULL;
    }
    JsonParseResult r = json_parse(buf, got);
    free(buf);
    return r.value;
}

int cmd_items(const char *url, const char *cache_path, const char *catalog_path) {
    char dir[1024];
    dirname_of(cache_path, dir, sizeof dir);
    makedirs(dir);

    printf("fetching %s\n", url);
    fflush(stdout);

    unsigned char *body = NULL;
    size_t body_len = 0;
    char err[256] = {0};
    if (!http_get(url, &body, &body_len, err, sizeof err)) {
        /* Decision 12: Python raises here and the traceback goes to stderr uncaught, so its
         * wording is not reproduced. What is reproduced is the observable contract -- stdout
         * carries only the "fetching" line, the exit code is nonzero, and no cache file is
         * created or truncated. */
        fprintf(stderr, "download failed: %s\n", err);
        return 1;
    }

    JsonParseResult parsed = json_parse(body, body_len); /* also rejects a broken download */
    if (!parsed.value) {
        fprintf(stderr, "the downloaded item table is not valid JSON\n");
        free(body);
        return 1;
    }

    /* Stored indented, not as the one long line it arrives as, so the cache is readable.
     * json.dump writes through a text-mode file object -- CRLF on Windows. */
    char *pretty = NULL;
    size_t pretty_len = 0;
    char serr[256] = {0};
    if (!json_serialize_indent(parsed.value, &pretty, &pretty_len, serr, sizeof serr)) {
        fprintf(stderr, "cannot re-serialize the item table: %s\n", serr);
        json_free(parsed.value);
        free(body);
        return 1;
    }
    json_free(parsed.value);
    FILE *cache = fopen(cache_path, "w");
    if (!cache) {
        fprintf(stderr, "cannot write %s\n", cache_path);
        free(pretty);
        free(body);
        return 1;
    }
    fwrite(pretty, 1, pretty_len, cache);
    fclose(cache);
    free(pretty);

    /* table = item_names() -- read back from the cache that was just written, which is what
     * the Python does too (its module-level memo is still unset in a fresh process). */
    JsonValue *cached = read_json_file(cache_path);
    NameTable table;
    build_name_table(cached, &table);

    size_t total = 0;
    for (size_t i = 0; i < table.count; i++) total += table.domains[i].count;
    printf("wrote %s (%zu bytes, %zu items)\n", cache_path, body_len, total);
    free(body);

    if (catalog_path && *catalog_path) {
        Catalog catalog;
        build_catalog(cached, &catalog);
        size_t written = write_catalog(catalog_path, &catalog);
        printf("wrote %s (%zu items, grouped by tier and category)\n", catalog_path, written);
        catalog_free(&catalog);
    }
    json_free(cached);

    /* sorted(table.items(), key=lambda kv: -len(kv[1])) -- stable, so equal-sized domains
     * keep the order the cache file listed them in. */
    DomainTable **order = (DomainTable **)malloc((table.count ? table.count : 1)
                                                 * sizeof *order);
    size_t n = 0;
    for (size_t i = 0; i < table.count; i++) {
        size_t k = n++;
        while (k > 0 && order[k - 1]->count < table.domains[i].count) {
            order[k] = order[k - 1];
            k--;
        }
        order[k] = &table.domains[i];
    }
    for (size_t i = 0; i < n; i++) {
        DomainTable *d = order[i];

        /* categories[c] += 1 in id order, then sorted by -count, stable. */
        const char **cat_names = (const char **)malloc((d->count ? d->count : 1)
                                                       * sizeof *cat_names);
        size_t *cat_counts = (size_t *)malloc((d->count ? d->count : 1) * sizeof *cat_counts);
        size_t cat_n = 0;
        for (size_t j = 0; j < d->count; j++) {
            size_t k = 0;
            for (; k < cat_n; k++) {
                if (strcmp(cat_names[k], d->items[j].category) == 0) break;
            }
            if (k == cat_n) {
                cat_names[cat_n] = d->items[j].category;
                cat_counts[cat_n] = 0;
                cat_n++;
            }
            cat_counts[k]++;
        }
        size_t *idx = (size_t *)malloc((cat_n ? cat_n : 1) * sizeof *idx);
        size_t m = 0;
        for (size_t j = 0; j < cat_n; j++) {
            size_t k = m++;
            while (k > 0 && cat_counts[idx[k - 1]] < cat_counts[j]) {
                idx[k] = idx[k - 1];
                k--;
            }
            idx[k] = j;
        }

        printf("  %-12s %3zu  ", d->domain, d->count);
        for (size_t j = 0; j < m; j++) {
            printf("%s%s %zu", j ? ", " : "", cat_names[idx[j]], cat_counts[idx[j]]);
        }
        printf("\n");
        free(idx);
        free(cat_names);
        free(cat_counts);
    }
    free(order);
    name_table_free(&table);
    return 0;
}
