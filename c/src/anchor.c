/*
 * anchor_for -- task 8 of port-to-c-sas4-core.
 */
#include "anchor.h"
#include "path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Non-overlapping occurrences of needle in haystack -- matches bytes.count()'s own
 * semantics (a match consumes its bytes before scanning continues, so "aaa".count("aa")
 * is 1, not 2). needle_len == 0 is not a case this module ever produces (a key is never
 * empty) so it is left undefined here rather than special-cased. */
static size_t count_occurrences(const uint8_t *haystack, size_t haystack_len,
                                 const uint8_t *needle, size_t needle_len) {
    size_t count = 0;
    size_t i = 0;
    while (i + needle_len <= haystack_len) {
        if (memcmp(haystack + i, needle, needle_len) == 0) {
            count++;
            i += needle_len;
        } else {
            i++;
        }
    }
    return count;
}

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} Bytes;

static void bytes_init(Bytes *b) {
    b->cap = 64;
    b->data = (uint8_t *)malloc(b->cap);
    b->len = 0;
}

static void bytes_reserve(Bytes *b, size_t extra) {
    if (b->len + extra > b->cap) {
        while (b->len + extra > b->cap) {
            b->cap *= 2;
        }
        b->data = (uint8_t *)realloc(b->data, b->cap);
    }
}

static void bytes_append(Bytes *b, const uint8_t *data, size_t len) {
    bytes_reserve(b, len);
    memcpy(b->data + b->len, data, len);
    b->len += len;
}

static void bytes_prepend(Bytes *b, const uint8_t *data, size_t len) {
    bytes_reserve(b, len);
    memmove(b->data + len, b->data, b->len);
    memcpy(b->data, data, len);
    b->len += len;
}

/* '"' + key (UTF-8) + '":' + the compact serialization of value -- the anchor's own shape,
 * and the shape of a "previous sibling" prefix minus its trailing ','. Appends into `out`;
 * on failure (only the float-refusal from json_serialize_compact can fail here) fills err
 * and returns false without modifying `out`. *out_value_len, if non-NULL, receives the
 * value's own serialized length (not the whole "key":value's). */
static bool build_key_value(Bytes *out, const char *key, const JsonValue *value, char *err,
                             size_t err_cap, size_t *out_value_len) {
    char *compact;
    size_t compact_len;
    if (!json_serialize_compact(value, &compact, &compact_len, err, err_cap)) {
        return false;
    }
    bytes_append(out, (const uint8_t *)"\"", 1);
    bytes_append(out, (const uint8_t *)key, strlen(key));
    bytes_append(out, (const uint8_t *)"\":", 2);
    bytes_append(out, (const uint8_t *)compact, compact_len);
    free(compact);
    if (out_value_len) {
        *out_value_len = compact_len;
    }
    return true;
}

AnchorResult anchor_for(JsonValue *document, const uint8_t *plain, size_t plain_len,
                         const char *path) {
    AnchorResult result;
    result.ok = false;
    result.anchor = NULL;
    result.anchor_len = 0;
    result.value_len = 0;
    result.error[0] = '\0';

    PathParentResult parent_result = path_parent_of(document, path);
    if (!parent_result.ok) {
        snprintf(result.error, sizeof(result.error), "%s", parent_result.error);
        return result;
    }
    if (parent_result.is_index) {
        snprintf(result.error, sizeof(result.error),
                 "set does not edit list elements; use a path ending in a key");
        return result;
    }
    JsonValue *parent = parent_result.parent;
    const char *key = parent_result.key_str;
    if (parent->type != JSON_OBJECT) {
        snprintf(result.error, sizeof(result.error), "cannot index a %s by key",
                 parent->type == JSON_ARRAY ? "list" : "scalar");
        return result;
    }
    JsonValue *value = json_object_get(parent, key);
    if (!value) {
        snprintf(result.error, sizeof(result.error), "key not found: '%.200s'", key);
        return result;
    }

    Bytes anchor;
    bytes_init(&anchor);
    size_t old_len;
    if (!build_key_value(&anchor, key, value, result.error, sizeof(result.error), &old_len)) {
        free(anchor.data);
        return result;
    }

    size_t count = count_occurrences(plain, plain_len, anchor.data, anchor.len);
    if (count != 1) {
        /* Walk backwards through the preceding sibling keys, prepending
         * "\"sibling\":<value>," until the anchor becomes unique or the siblings run out --
         * matching anchor_for's own loop exactly, including that it keeps going even from
         * a zero-match start (the 0-vs-1 distinction is not special-cased in the Python
         * either; both end up at the same "still not unique" failure if siblings run out). */
        size_t position = 0;
        bool found_position = false;
        for (size_t i = 0; i < parent->as.object.count; i++) {
            if (strcmp(parent->as.object.members[i].key, key) == 0) {
                position = i;
                found_position = true;
                break;
            }
        }
        if (!found_position) {
            snprintf(result.error, sizeof(result.error),
                     "key not found among its own parent's members: '%.180s'", key);
            free(anchor.data);
            return result;
        }
        while (count != 1 && position > 0) {
            position--;
            JsonMember *sibling = &parent->as.object.members[position];
            Bytes prefix;
            bytes_init(&prefix);
            if (!build_key_value(&prefix, sibling->key, sibling->value, result.error,
                                  sizeof(result.error), NULL)) {
                free(prefix.data);
                free(anchor.data);
                return result;
            }
            bytes_append(&prefix, (const uint8_t *)",", 1);
            bytes_prepend(&anchor, prefix.data, prefix.len);
            free(prefix.data);
            count = count_occurrences(plain, plain_len, anchor.data, anchor.len);
        }
        if (count != 1) {
            snprintf(result.error, sizeof(result.error),
                     "cannot pin down %.180s in the file: %zu matches even with context",
                     path, count);
            free(anchor.data);
            return result;
        }
    }

    result.ok = true;
    result.anchor = anchor.data;
    result.anchor_len = anchor.len;
    result.value_len = old_len;
    return result;
}

void anchor_free(AnchorResult *result) {
    free(result->anchor);
    result->anchor = NULL;
    result->anchor_len = 0;
}
