/*
 * Verification tool for task 11: `levelplan <plaintext.json> <level> <slot>` prints the plan
 * level_plan would write, one "path<TAB>compact-json-value" line per entry, then a final
 * "spent <n>" line -- or "ERROR <empty_slot 0-or-1>\n<message>" on failure. Reads already-
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

int main(int argc, char **argv) {
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);

    if (argc != 4) {
        fprintf(stderr, "usage: levelplan <plaintext.json> <level> <slot>\n");
        return 2;
    }
    int level = atoi(argv[2]);
    int slot = atoi(argv[3]);

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

    LevelPlanResult plan = plans_level_plan(r.value, level, slot);
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
    printf("spent %lld\n", (long long)plan.spent);

    level_plan_result_free(&plan);
    json_free(r.value);
    return 0;
}
