/*
 * A canonical structural dump of a JSON tree -- type, key order and scalar values at every
 * path -- for the differential harness to diff against Python's own json.loads. Not part of
 * sas4.py's own CLI surface (json.py has none); exists purely as this port's verification
 * tool for task 4 (and, later, task 15's full harness).
 *
 * Output: one line per node, "<path>\t<type>\t<value>" in a fixed pre-order (object keys in
 * insertion order, array items by index), reading the whole file from stdin as raw bytes.
 * `path` uses "/" for object keys and "[N]" for array indices, matching path.c's own
 * convention (task 7) closely enough to be useful without depending on it here.
 */
#include "json.h"

#include <fcntl.h>
#include <inttypes.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The dump format is tab-separated, one record per line -- but a real string scalar can
 * legitimately contain a literal tab or newline once JSON's own \t/\n escapes are decoded,
 * which would otherwise split one record into pieces. Escape those two bytes (and the
 * escape character itself) for this dump's purposes only; this has nothing to do with
 * JSON's own escaping (that is task 5/6's job) and this tool ships nowhere. */
static void print_escaped(const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '\\') {
            fputs("\\\\", stdout);
        } else if (*p == '\t') {
            fputs("\\t", stdout);
        } else if (*p == '\n') {
            fputs("\\n", stdout);
        } else if (*p == '\r') {
            fputs("\\r", stdout);
        } else {
            fputc(*p, stdout);
        }
    }
}

static void dump(const JsonValue *v, const char *path) {
    switch (v->type) {
        case JSON_NULL:
            printf("%s\tnull\t\n", path);
            break;
        case JSON_BOOL:
            printf("%s\tbool\t%s\n", path, v->as.boolean ? "true" : "false");
            break;
        case JSON_INT:
            printf("%s\tint\t%" PRId64 "\n", path, v->as.integer);
            break;
        case JSON_FLOAT:
            printf("%s\tfloat\t%.17g\n", path, v->as.number);
            break;
        case JSON_STRING:
            printf("%s\tstr\t", path);
            print_escaped(v->as.string.data);
            printf("\n");
            break;
        case JSON_ARRAY:
            printf("%s\tarray\t%zu\n", path, v->as.array.count);
            for (size_t i = 0; i < v->as.array.count; i++) {
                char child[4096];
                snprintf(child, sizeof(child), "%s[%zu]", path, i);
                dump(v->as.array.items[i], child);
            }
            break;
        case JSON_OBJECT:
            printf("%s\tobject\t%zu\n", path, v->as.object.count);
            for (size_t i = 0; i < v->as.object.count; i++) {
                char child[4096];
                snprintf(child, sizeof(child), "%s/%s", path, v->as.object.members[i].key);
                dump(v->as.object.members[i].value, child);
            }
            break;
    }
}

int main(void) {
    /* See main_jsonstr.c's comment: Windows's text-mode stdin treats 0x1A as an end-of-file
     * marker mid-stream and can translate \r\n, either of which would silently truncate or
     * corrupt a real save's plaintext read this way. Text-mode stdout has the matching
     * output-side problem: it turns every \n this tool prints into \r\n, which a byte-exact
     * comparison against Python's \n-only lines (rather than a line-ending-tolerant one)
     * would see as every single line differing. */
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
    dump(result.value, "$");
    json_free(result.value);
    free(buf);
    return 0;
}
