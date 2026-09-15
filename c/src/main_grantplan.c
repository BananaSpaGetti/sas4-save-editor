/*
 * Verification tool for task 13: `grantplan <items_cache.json> <plaintext.json> <slotprofile>
 * <item_id:kind:grade:bonus:slot>...` prints the plan grant_plan would write as "path<TAB>
 * compact-json-value", then each label as "LABEL<TAB>text", or "ERROR <empty_slot 0-or-1>\n
 * <message>" on failure. Reads already-decoded plaintext only, never a raw .save file; the
 * item cache is game data, not a save, so it is never sensitive. Not part of sas4.py's CLI
 * surface.
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

int main(int argc, char **argv) {
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);

    if (argc < 4) {
        fprintf(stderr,
                "usage: grantplan <items_cache.json|-> <plaintext.json> <slotprofile> "
                "[<item_id:kind:grade:bonus:slot>]...\n");
        return 2;
    }
    const char *cache_path = argv[1];
    const char *doc_path = argv[2];
    int slotprofile = atoi(argv[3]);
    size_t request_count = (size_t)(argc - 4);

    ItemNames names;
    if (strcmp(cache_path, "-") == 0) {
        memset(&names, 0, sizeof(names)); /* section_count 0 -- "no cache" case */
    } else {
        items_load(cache_path, &names);
    }

    GrantRequest *requests = (GrantRequest *)malloc(
        (request_count ? request_count : 1) * sizeof(GrantRequest));
    char **kind_storage = (char **)malloc((request_count ? request_count : 1) * sizeof(char *));
    for (size_t i = 0; i < request_count; i++) {
        long long item_id, grade, bonus, slot;
        char kind[64];
        /* item_id:kind:grade:bonus:slot -- kind has no ':' or digits-only ambiguity since
         * sscanf's %63[^:] stops at the next colon. */
        if (sscanf(argv[4 + i], "%lld:%63[^:]:%lld:%lld:%lld", &item_id, kind, &grade, &bonus,
                   &slot) != 5) {
            fprintf(stderr, "bad request '%s', want id:kind:grade:bonus:slot\n", argv[4 + i]);
            return 2;
        }
        kind_storage[i] = strdup(kind);
        requests[i].item_id = item_id;
        requests[i].kind = kind_storage[i];
        requests[i].grade = grade;
        requests[i].bonus = bonus;
        requests[i].slot = slot;
    }

    size_t len;
    uint8_t *data = read_all(doc_path, &len);
    if (!data) {
        fprintf(stderr, "could not read '%s'\n", doc_path);
        return 2;
    }
    JsonParseResult r = json_parse(data, len);
    free(data);
    if (!r.value) {
        fprintf(stderr, "not valid JSON: %s\n", r.error);
        return 2;
    }

    GrantPlanResult plan = plans_grant_plan(r.value, &names, requests, request_count,
                                             slotprofile);
    for (size_t i = 0; i < request_count; i++) {
        free(kind_storage[i]);
    }
    free(kind_storage);
    free(requests);
    items_free(&names);

    if (!plan.ok) {
        printf("ERROR %d\n%s\n", plan.empty_slot ? 1 : 0, plan.error);
        json_free(r.value);
        return 1;
    }

    for (size_t i = 0; i < plan.plan.count; i++) {
        char *out;
        size_t out_len;
        char err[128];
        if (!json_serialize_compact(plan.plan.items[i].value, &out, &out_len, err, sizeof(err))) {
            fprintf(stderr, "serialize failed: %s\n", err);
            return 2;
        }
        printf("%s\t%s\n", plan.plan.items[i].path, out);
        free(out);
    }
    for (size_t i = 0; i < plan.label_count; i++) {
        printf("LABEL\t%s\n", plan.labels[i]);
    }

    grant_plan_result_free(&plan);
    json_free(r.value);
    return 0;
}
