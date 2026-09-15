/*
 * `verify` -- port of sas4.py's cmd_verify. Task 9 of the port-to-c-sas4-cli plan.
 *
 * Reads the raw bytes directly rather than through sas4_load: the Python's cmd_verify never
 * decodes or parses, so a file that is DGDATA-shaped but holds unparseable JSON still gets a
 * checksum verdict here rather than a "cannot read" message.
 */
#include "cmd_verify.h"
#include "dgdata.h"

#include <stdio.h>
#include <stdlib.h>

int cmd_verify(const char *file) {
    FILE *f = fopen(file, "rb");
    if (!f) {
        /* open() raises OSError in the Python, uncaught -- the Decision 11 family. */
        fprintf(stderr, "cannot read %s\n", file);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *raw = malloc((size_t)(size > 0 ? size : 1));
    size_t got = fread(raw, 1, (size_t)size, f);
    fclose(f);

    char stored[9], computed[9];
    int rc = dg_verify(raw, got, stored, computed);
    free(raw);
    if (rc < 0) {
        /* dgdata.verify lets decode()'s ValueError propagate for a non-DGDATA file. */
        fprintf(stderr, "not a DGDATA file\n");
        return 1;
    }
    printf("stored   %s\ncomputed %s\n%s\n", stored, computed,
           rc == 1 ? "VALID" : "MISMATCH - the game would reject this file");
    return rc == 1 ? 0 : 1;
}
