/*
 * Verification tool for task 10: `check <plaintext.json>` runs model_check on the JSON tree
 * at that path and prints one problem string per line, then "OK" if there were none, or
 * `xp <level>` which prints "<xp_per_level> <xp_for_level>" for that level. Not part of
 * sas4.py's CLI surface -- reads already-decoded plaintext, never a raw .save file, so it
 * never touches DGDATA or a real save.
 */
#include "json.h"
#include "model.h"

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

    model_cross_check();

    if (argc == 3 && strcmp(argv[1], "xp") == 0) {
        int level = atoi(argv[2]);
        printf("%lld %lld\n", (long long)model_xp_per_level(level),
               (long long)model_xp_for_level(level));
        return 0;
    }

    if (argc != 2) {
        fprintf(stderr, "usage: check <plaintext.json>\n       check xp <level>\n");
        return 2;
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

    ProblemList problems = {0};
    model_check(r.value, &problems);
    for (size_t i = 0; i < problems.count; i++) {
        printf("%s\n", problems.items[i]);
    }
    if (problems.count == 0) {
        printf("OK\n");
    }
    model_problem_list_free(&problems);
    json_free(r.value);
    return 0;
}
