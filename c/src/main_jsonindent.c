/*
 * Verification tool for task 6 (indented serializer): reads raw JSON bytes from stdin,
 * parses them, and prints json_serialize_indent's output for the whole document (not a
 * per-node sweep like task 5's jsonround.exe -- nothing parses this form back, so only the
 * whole-document output that a real command would print needs to match). Not part of
 * sas4.py's CLI surface; a test tool only.
 */
#include "json.h"

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
        fprintf(stderr, "parse error at byte %zu: %s\n", result.error_offset, result.error);
        free(buf);
        return 1;
    }
    char *out;
    size_t out_len;
    char err[160];
    if (!json_serialize_indent(result.value, &out, &out_len, err, sizeof(err))) {
        fprintf(stderr, "%s\n", err);
        json_free(result.value);
        free(buf);
        return 1;
    }
    fwrite(out, 1, out_len, stdout);
    free(out);
    json_free(result.value);
    free(buf);
    return 0;
}
