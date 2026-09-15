/*
 * Verification tool for task 5's node sweep: reads raw JSON bytes from stdin, parses them,
 * then walks the whole tree and serializes every node in isolation (not just the root),
 * printing "<compact bytes>\n" for each -- matching what the differential harness (below)
 * does on the Python side with json.dumps(node, separators=(",", ":"), ensure_ascii=False)
 * called on every node individually, including the whole Strongboxes/Claimed list task 13's
 * grant_plan actually writes. Pre-order, matching main_jsondump.c's own traversal order.
 */
#include "json.h"

#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>

static void walk(const JsonValue *v) {
    char *out;
    size_t out_len;
    char err[160];
    if (!json_serialize_compact(v, &out, &out_len, err, sizeof(err))) {
        printf("<error: %s>\n", err);
    } else {
        fwrite(out, 1, out_len, stdout);
        printf("\n");
        free(out);
    }
    if (v->type == JSON_ARRAY) {
        for (size_t i = 0; i < v->as.array.count; i++) {
            walk(v->as.array.items[i]);
        }
    } else if (v->type == JSON_OBJECT) {
        for (size_t i = 0; i < v->as.object.count; i++) {
            walk(v->as.object.members[i].value);
        }
    }
}

int main(void) {
    /* Both directions: text-mode stdout translates \n to \r\n, which would shift every
     * byte after the first line relative to what Python's own \n-only output uses --
     * caught empirically as a 100%, every-node "mismatch" that was really a framing bug
     * in this tool, not the serializer. */
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
        fprintf(stderr, "parse error at byte %zu: %s\n", result.error_offset, result.error);
        free(buf);
        return 1;
    }
    walk(result.value);
    json_free(result.value);
    free(buf);
    return 0;
}
