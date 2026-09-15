/*
 * Verification tool for task 5 (and 6) of port-to-c-sas4-core: reads raw UTF-8 bytes from
 * stdin, treats them as the CONTENTS of one JSON string (not JSON syntax -- no quotes, no
 * escapes expected in the input), and prints its compact serialization. Lets the differential
 * harness pin json_serialize_compact's string escaping against Python's json.dumps one
 * codepoint at a time without going through the parser at all. Not part of sas4.py's CLI
 * surface; a test tool only.
 */
#include "json.h"

#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    /* Windows's C runtime opens stdin in text mode by default, which does two things a
     * raw-byte reader cannot tolerate: it treats 0x1A (Ctrl-Z) as an end-of-file marker
     * mid-stream (silently truncating input right there -- caught empirically: codepoint
     * 26 in the escape golden table came back as an empty string), and it can translate
     * \r\n sequences. Binary mode disables both. */
    _setmode(_fileno(stdin), _O_BINARY);

    size_t cap = 4096;
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

    JsonValue *v = json_new_string((const char *)buf, len);
    free(buf);

    char *out;
    size_t out_len;
    char err[160];
    if (!json_serialize_compact(v, &out, &out_len, err, sizeof(err))) {
        fprintf(stderr, "%s\n", err);
        json_free(v);
        return 1;
    }
    fwrite(out, 1, out_len, stdout);
    free(out);
    json_free(v);
    return 0;
}
