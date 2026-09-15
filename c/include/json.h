/*
 * A small JSON tree, parsed from and (task 5/6) serialized back to bytes that match
 * Python's json module exactly. See the port-to-c-sas4-core plan's Context for why this is
 * hand-written rather than vendored: anchor_for's correctness depends on the serializer's
 * output matching Python's json.dumps(..., separators=(",", ":"), ensure_ascii=False) byte
 * for byte, which no off-the-shelf C JSON library does out of the box.
 *
 * Real SAS4 saves contain only int, str, bool -- no float, no null, no non-ASCII string
 * (measured across all 26 decodable saves in the repo; see the plan). float is still
 * parsed and tagged, so a save containing one can be read; only the serializer (task 5)
 * refuses to write one. null is supported because it costs nothing to support.
 *
 * JSON_INT holds an int64_t; an integer literal outside that range is a parse error
 * (Decision 10) rather than silently truncated or promoted to float -- every real .save
 * file's largest integer measured at 13 digits, well inside int64_t's 19-digit range, but a
 * DGDATA-encoded file that is not a profile save (a session token, not shipped as sample
 * data) has been measured to hold a 20-digit one. Python's own int has no such limit, so
 * this is a genuine, narrow divergence, not a hypothetical one -- see the plan for why it is
 * accepted rather than fixed.
 */
#ifndef SAS4_JSON_H
#define SAS4_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_INT,
    JSON_FLOAT,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT,
} JsonType;

typedef struct JsonValue JsonValue;

/* One key/value pair inside an object, in the order it was inserted -- objects are stored
 * as an ordered list of members, not a hash table, because output order is compared byte
 * for byte against Python's dict insertion order (see Decision 9 of the plan). */
typedef struct {
    char *key;    /* malloc'd, NUL-terminated UTF-8 */
    size_t key_len;
    JsonValue *value;
} JsonMember;

struct JsonValue {
    JsonType type;
    union {
        bool boolean;
        int64_t integer;
        double number;
        struct {
            char *data; /* malloc'd, NUL-terminated UTF-8; data_len excludes the NUL */
            size_t len;
        } string;
        struct {
            JsonValue **items;
            size_t count;
            size_t cap;
        } array;
        struct {
            JsonMember *members;
            size_t count;
            size_t cap;
        } object;
    } as;
};

/* --- construction (all values are heap-allocated; free the whole tree with json_free) --- */

JsonValue *json_new_null(void);
JsonValue *json_new_bool(bool value);
JsonValue *json_new_int(int64_t value);
JsonValue *json_new_float(double value);
/* Copies data (does not take ownership); data need not be NUL-terminated. */
JsonValue *json_new_string(const char *data, size_t len);
JsonValue *json_new_array(void);
JsonValue *json_new_object(void);

void json_array_push(JsonValue *array, JsonValue *item);

/* Inserts at the end if key is new; if key already exists, replaces its value in place and
 * frees the old one, WITHOUT moving the key's position -- matching Python dict semantics
 * for a JSON object with a repeated key (json.loads keeps the last value, but the key's
 * position is wherever it was first seen). Takes ownership of value; copies key. */
void json_object_set(JsonValue *object, const char *key, size_t key_len, JsonValue *value);

/* NULL if the object has no such key. Does not transfer ownership. */
JsonValue *json_object_get(const JsonValue *object, const char *key);

void json_free(JsonValue *value);

/* A deep copy of `value`. */
JsonValue *json_clone(const JsonValue *value);

/* Matches Python's `==` for the JSON-representable types: an object compares as a set of
 * key/value pairs (order does not matter, unlike json_object_set's insertion-order
 * preservation), an array compares element-wise in order, and -- because Python's bool is
 * a subclass of int -- a bool and an int compare equal when the bool's 0/1 value matches
 * the int. int and float cross-compare by numeric value too, matching Python's own mixed
 * numeric-tower equality. */
bool json_equal(const JsonValue *a, const JsonValue *b);

/* --- parsing --- */

typedef struct {
    JsonValue *value;     /* NULL on error */
    size_t error_offset;  /* byte offset into the input; valid only when value is NULL */
    char error[160];      /* valid only when value is NULL */
} JsonParseResult;

/* Parses a whole UTF-8 byte buffer as a single JSON value (matching json.loads: trailing
 * whitespace is allowed after the value, anything else is not). The caller owns
 * result.value and must json_free() it. */
JsonParseResult json_parse(const uint8_t *data, size_t len);

/* --- serialization --- */

/* Matches json.dumps(value, separators=(",", ":"), ensure_ascii=False) byte for byte: no
 * space after ',' or ':'; object keys in insertion order; a string's '"' and '\' backslash-
 * escaped, \b \f \n \r \t used where they apply, \u00XX (lowercase hex) for every other
 * control character below 0x20, and every byte at 0x20 and above -- including a multi-byte
 * UTF-8 sequence -- emitted as-is. A JSON_FLOAT value anywhere in the tree is refused with
 * an explicit error rather than formatted (Decision 6 of the port-to-c-sas4-core plan: no
 * real save has ever contained one, and C has no shortest-round-trip float formatter to
 * match Python's repr()).
 *
 * On success returns true and allocates *out via malloc (caller frees), NUL-terminated with
 * *out_len excluding the NUL. On failure returns false and fills err (a human-readable
 * reason; err_cap should be at least 128). */
bool json_serialize_compact(const JsonValue *value, char **out, size_t *out_len, char *err,
                             size_t err_cap);

/* Matches json.dumps(value) with no separators= argument -- Python's default ", " / ": "
 * separators, still on one line (unlike json_serialize_indent). Same escaping, ordering and
 * float refusal as json_serialize_compact; only the separators differ. */
bool json_serialize_default(const JsonValue *value, char **out, size_t *out_len, char *err,
                             size_t err_cap);

/* Matches json.dumps(value, indent=2): two-space indent per level, ": " after keys, "," at
 * end of line, closing brackets at the parent's indentation, an empty object/array on one
 * line. Human-readable output only -- nothing parses this back, and (unlike the compact
 * form) it is not on anchor_for's critical path. Also refuses a JSON_FLOAT, for the same
 * reason and the same way. */
bool json_serialize_indent(const JsonValue *value, char **out, size_t *out_len, char *err,
                            size_t err_cap);

#endif
