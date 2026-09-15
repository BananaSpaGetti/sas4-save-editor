/*
 * Verification tool for task 8: reads raw plaintext from stdin, parses it, then computes
 * anchor_for for each path given as argv[1..], printing one line per path (in argv order):
 * "<value_len> <anchor_len>" on success or "ERROR <message>" on failure. Never the anchor's
 * own bytes -- an anchor contains a value, and this walks over real saves. Takes explicit
 * paths rather than enumerating every scalar itself: anchor_for's sibling-extension loop
 * rescans the whole plaintext per extension, so a highly repeated value (MasteryXp:0) is
 * inherently expensive, and doing that for every one of a save's several thousand scalars
 * is a combinatorial blowup neither implementation is meant to survive in one pass -- the
 * caller decides how many and which paths to actually exercise. Not part of sas4.py's CLI
 * surface.
 */
#include "anchor.h"
#include "json.h"

#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
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

    JsonParseResult parsed = json_parse(buf, len);
    if (!parsed.value) {
        fprintf(stderr, "parse error\n");
        free(buf);
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        AnchorResult result = anchor_for(parsed.value, buf, len, argv[i]);
        if (result.ok) {
            printf("%zu %zu\n", result.value_len, result.anchor_len);
            anchor_free(&result);
        } else {
            printf("ERROR %s\n", result.error);
        }
        fflush(stdout);
    }

    json_free(parsed.value);
    free(buf);
    return 0;
}
