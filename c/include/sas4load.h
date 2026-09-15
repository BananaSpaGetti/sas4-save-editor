/*
 * Shared read path -- port of sas4.py's load(path): read the whole file, DGDATA-decode it,
 * parse the plaintext as JSON. Every read/write subcommand from task 3 onward needs exactly
 * this, so it lives here once rather than being copied into each cmd_*.c.
 *
 * Error text fidelity: "not a DGDATA file" is dgdata.py's own exact ValueError text and is
 * reproduced verbatim. A file that cannot be opened (OSError) or whose plaintext is not
 * valid JSON produces an analogous but NOT byte-identical message to Python's -- CPython's
 * OSError and json.JSONDecodeError text is platform- and version-specific in a way no task
 * in this plan asks to be reproduced (unlike task 1's argparse text, which the plan's own
 * done-when criteria require byte-for-byte). Every real save measured across the whole
 * port-to-c-sas4-core plan decodes and parses cleanly, so this divergence is confined to
 * paths a real save never takes. Decision 11 of the port-to-c-sas4-cli plan.
 */
#ifndef SAS4_LOAD_H
#define SAS4_LOAD_H

#include "json.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool ok;
    uint8_t *raw;
    size_t raw_len;
    uint8_t *plain;
    size_t plain_len;
    JsonValue *document; /* NULL if !ok */
    char error[600];     /* "cannot read %s\n  %s", filled only if !ok */
} SaveLoad;

SaveLoad sas4_load(const char *path);
void sas4_load_free(SaveLoad *sl);

#endif
