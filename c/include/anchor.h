/*
 * anchor_for -- port of sas4.py's anchor_for. Builds a byte string that appears exactly
 * once in a save's plaintext and ends with the value at a given path, so apply_edits (task
 * 9) can replace only that value's bytes. See the plan's Context and Risks: a one-byte
 * difference here from the compact serializer makes this return failure having found
 * nothing, silently -- not a crash, just a wrong-looking "path not found".
 */
#ifndef SAS4_ANCHOR_H
#define SAS4_ANCHOR_H

#include "json.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool ok;
    uint8_t *anchor;     /* malloc'd, NOT NUL-terminated (it is a byte string, and a save's
                           * plaintext can itself contain the byte 0x00) -- always paired
                           * with anchor_len. Caller frees on success. */
    size_t anchor_len;
    size_t value_len;    /* the length, in anchor's trailing bytes, of just the value's own
                           * serialized bytes -- what a caller replaces to change it. */
    char error[256];
} AnchorResult;

/* `plain` is the save's whole decoded plaintext (not just the value's own bytes) -- the
 * anchor is searched for, and its uniqueness checked, against the entire file. */
AnchorResult anchor_for(JsonValue *document, const uint8_t *plain, size_t plain_len,
                         const char *path);

void anchor_free(AnchorResult *result);

#endif
