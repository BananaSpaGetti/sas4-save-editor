/*
 * Verification tool for task 12: `masteryplan <plaintext.json> <slot> <index:level>...`
 * prints the plan mastery_plan would write as "path<TAB>compact-json-value", or "ERROR
 * <empty_slot 0-or-1>\n<message>" on failure. Reads already-decoded plaintext only, never a
 * raw .save file. Not part of sas4.py's CLI surface.
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

    if (argc < 3) {
        fprintf(stderr, "usage: masteryplan <plaintext.json> <slot> [<index:level>]...\n");
        return 2;
    }
    int slot = atoi(argv[2]);
    size_t target_count = (size_t)(argc - 3);
    MasteryTarget *targets = (MasteryTarget *)malloc(
        (target_count ? target_count : 1) * sizeof(MasteryTarget));
    for (size_t i = 0; i < target_count; i++) {
        int index, level;
        if (sscanf(argv[3 + i], "%d:%d", &index, &level) != 2) {
            fprintf(stderr, "bad target '%s', want index:level\n", argv[3 + i]);
            return 2;
        }
        targets[i].index = index;
        targets[i].level = level;
    }

    size_t len;
    uint8_t *data = read_all(argv[1], &len);
    if (!data) {
        fprintf(stderr, "could not read '%s'\n", argv[1]);
        return 2;
    }
    JsonParseResult r = json_parse(data, len);
    free(data);
    if (!r.value) {
        fprintf(stderr, "not valid JSON: %s\n", r.error);
        return 2;
    }

    MasteryPlanResult plan = plans_mastery_plan(r.value, targets, target_count, slot);
    free(targets);
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

    mastery_plan_result_free(&plan);
    json_free(r.value);
    return 0;
}
