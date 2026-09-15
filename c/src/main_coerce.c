/*
 * Verification tool for task 7's coerce: `coerce <current-json-literal> <text>` parses the
 * first argument as JSON to get a value of the "current" kind, then prints the result of
 * path_coerce(text, current) -- either the coerced value's compact JSON, or "ERROR: <msg>"
 * on stderr with exit 1. Not part of sas4.py's CLI surface.
 */
#include "json.h"
#include "path.h"

#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    /* Same Windows text-mode trap as every other stdio-based tool here: without this,
     * every \n this tool prints comes out \r\n, breaking a byte-exact comparison against
     * Python's \n-only output. */
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);

    if (argc != 3) {
        fprintf(stderr, "usage: coerce <current-json-literal> <text>\n");
        return 2;
    }
    JsonParseResult current_result = json_parse((const uint8_t *)argv[1], strlen(argv[1]));
    if (!current_result.value) {
        fprintf(stderr, "could not parse current value: %s\n", current_result.error);
        return 2;
    }
    CoerceResult r = path_coerce(argv[2], current_result.value);
    json_free(current_result.value);
    if (!r.ok) {
        fprintf(stderr, "ERROR: %s\n", r.error);
        return 1;
    }
    if (r.value->type == JSON_FLOAT) {
        /* json_serialize_compact refuses a float by design (Decision 6) -- coerce()
         * itself still needs to support float (the "current" value's own kind decides
         * which branch runs), it is just that nothing this port actually writes back to
         * a save can be a float. Print the raw value directly for this verification
         * tool's own purposes; no real command ever needs to serialize one. */
        printf("%.17g\n", r.value->as.number);
        json_free(r.value);
        return 0;
    }
    char *out;
    size_t out_len;
    char err[160];
    if (!json_serialize_compact(r.value, &out, &out_len, err, sizeof(err))) {
        fprintf(stderr, "ERROR: %s\n", err);
        json_free(r.value);
        return 1;
    }
    fwrite(out, 1, out_len, stdout);
    printf("\n");
    free(out);
    json_free(r.value);
    return 0;
}
