/*
 * Verification tool for task 7: prints every scalar's path and kind (path_kind_of), one per
 * line, in document order -- never the value itself. Also prints the kinds tally at the end,
 * matching cmd_kinds' "by type" section. Not part of sas4.py's CLI surface.
 */
#include "json.h"
#include "path.h"

#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);

    size_t cap = 1 << 20;
    size_t len = 0;
    uint8_t *buf = (uint8_t *)malloc(cap);
    size_t got;
    while ((got = fread(buf + len, 1, cap - len, stdin)) > 0) {
        len += got;
        if (len == cap) {
            cap *= 2;
            buf = (uint8_t *)realloc(buf, cap);
        }
    }

    JsonParseResult result = json_parse(buf, len);
    if (!result.value) {
        fprintf(stderr, "parse error\n");
        free(buf);
        return 1;
    }

    ScalarList scalars = {0};
    path_scalars(result.value, "", &scalars);
    for (size_t i = 0; i < scalars.count; i++) {
        printf("%s\t%s\n", scalars.items[i].path, path_kind_of(scalars.items[i].value));
    }

    KindCount *tally;
    size_t tally_count;
    path_kind_tally(&scalars, &tally, &tally_count);
    printf("---\n");
    for (size_t i = 0; i < tally_count; i++) {
        printf("%s\t%zu\n", tally[i].name, tally[i].count);
    }
    free(tally);

    path_scalars_free(&scalars);
    json_free(result.value);
    free(buf);
    return 0;
}
