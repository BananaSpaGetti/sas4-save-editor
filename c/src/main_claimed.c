/*
 * Verification tool for the claimed_items/drop_claimed gap closed while porting task 14:
 * `claimed <items_cache.json|-> <plaintext.json> <slotprofile>` prints one
 * "index<TAB>kind<TAB>id<TAB>name<TAB>grade<TAB>bonus<TAB>slot-or-NONE" line per row.
 * `dropclaimed <plaintext.json> <slotprofile> <index>...` prints the resulting plan the
 * same way main_masteryplan.c does, or "ERROR\n<message>" on failure. Reads already-
 * decoded plaintext only, never a raw .save file. Not part of sas4.py's CLI surface.
 */
#include "json.h"
#include "plans.h"

#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *read_all(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)size);
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        free(buf);
        return NULL;
    }
    *out_len = (size_t)size;
    return buf;
}

static JsonValue *load_json(const char *path) {
    size_t len;
    uint8_t *data = read_all(path, &len);
    if (!data) {
        fprintf(stderr, "could not read '%s'\n", path);
        exit(2);
    }
    JsonParseResult r = json_parse(data, len);
    free(data);
    if (!r.value) {
        fprintf(stderr, "not valid JSON: %s\n", r.error);
        exit(2);
    }
    return r.value;
}

static int cmd_claimed(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: claimed <items_cache.json|-> <plaintext.json> <slotprofile>\n");
        return 2;
    }
    ItemNames names;
    if (strcmp(argv[2], "-") == 0) {
        memset(&names, 0, sizeof(names));
    } else {
        items_load(argv[2], &names);
    }
    JsonValue *document = load_json(argv[3]);
    int slotprofile = atoi(argv[4]);

    ClaimedRowList rows;
    plans_claimed_items(document, &names, slotprofile, &rows);
    for (size_t i = 0; i < rows.count; i++) {
        ClaimedRow *r = &rows.items[i];
        printf("%zu\t%s\t%lld\t%s\t%lld\t%lld\t", r->index, r->kind, (long long)r->id,
               r->name, (long long)r->grade, (long long)r->bonus);
        if (r->has_equipped_slot) {
            printf("%lld\n", (long long)r->equipped_slot);
        } else {
            printf("NONE\n");
        }
    }
    plans_claimed_items_free(&rows);
    items_free(&names);
    json_free(document);
    return 0;
}

static int cmd_drop(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: dropclaimed <plaintext.json> <slotprofile> <index>...\n");
        return 2;
    }
    JsonValue *document = load_json(argv[2]);
    int slotprofile = atoi(argv[3]);
    size_t index_count = (size_t)(argc - 4);
    int64_t *indexes = (int64_t *)malloc((index_count ? index_count : 1) * sizeof(int64_t));
    for (size_t i = 0; i < index_count; i++) {
        indexes[i] = atoll(argv[4 + i]);
    }

    DropClaimedResult result = plans_drop_claimed(document, indexes, index_count, slotprofile);
    free(indexes);
    if (!result.ok) {
        printf("ERROR\n%s\n", result.error);
        json_free(document);
        return 1;
    }
    for (size_t i = 0; i < result.plan.count; i++) {
        char *out;
        size_t out_len;
        char err[128];
        if (!json_serialize_compact(result.plan.items[i].value, &out, &out_len, err,
                                     sizeof(err))) {
            fprintf(stderr, "serialize failed: %s\n", err);
            return 2;
        }
        printf("%s\t%s\n", result.plan.items[i].path, out);
        free(out);
    }
    drop_claimed_result_free(&result);
    json_free(document);
    return 0;
}

int main(int argc, char **argv) {
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);

    if (argc < 2) {
        fprintf(stderr, "usage: claimed <mode: rows|drop> ...\n");
        return 2;
    }
    if (strcmp(argv[1], "rows") == 0) {
        return cmd_claimed(argc, argv);
    }
    if (strcmp(argv[1], "drop") == 0) {
        return cmd_drop(argc, argv);
    }
    fprintf(stderr, "unknown mode '%s'\n", argv[1]);
    return 2;
}
