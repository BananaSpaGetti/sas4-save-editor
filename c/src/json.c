/*
 * The JSON parser -- task 4 of port-to-c-sas4-core. The compact and indented serializers
 * are tasks 5 and 6, in this same file.
 *
 * Matches Python's json.loads(text, strict=True) (the default, and what sas4.py uses):
 *   - whitespace between tokens is exactly space, tab, \n, \r;
 *   - a number is a float if its literal contains '.', 'e' or 'E', otherwise an int --
 *     classified the same way Python's own NUMBER_RE groups do, not by the value itself;
 *   - a string may not contain a raw (unescaped) control character (0x00-0x1F) -- strict
 *     mode rejects that, rather than silently accepting it;
 *   - trailing bytes after the one top-level value are a parse error ("Extra data" in
 *     Python; this reports the same condition with its own wording and a byte offset).
 *
 * Two departures from Python's own arbitrary precision, both already decided by the plan:
 * integers are int64_t, not unbounded (Context: "Integers are 64-bit" -- no real save
 * comes close), and a lone (unpaired) \uXXXX surrogate is encoded as if it were a valid
 * three-byte UTF-8 sequence rather than rejected or combined -- CPython's own decoder does
 * not validate surrogate pairing either, and no real save contains one at all (zero
 * non-ASCII strings measured across every real save; see the plan's Context).
 */
#include "json.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- construction ------------------------------------------------------------------------ */

static JsonValue *alloc_value(JsonType type) {
    JsonValue *v = (JsonValue *)malloc(sizeof(JsonValue));
    v->type = type;
    return v;
}

JsonValue *json_new_null(void) {
    return alloc_value(JSON_NULL);
}

JsonValue *json_new_bool(bool value) {
    JsonValue *v = alloc_value(JSON_BOOL);
    v->as.boolean = value;
    return v;
}

JsonValue *json_new_int(int64_t value) {
    JsonValue *v = alloc_value(JSON_INT);
    v->as.integer = value;
    return v;
}

JsonValue *json_new_float(double value) {
    JsonValue *v = alloc_value(JSON_FLOAT);
    v->as.number = value;
    return v;
}

JsonValue *json_new_string(const char *data, size_t len) {
    JsonValue *v = alloc_value(JSON_STRING);
    char *copy = (char *)malloc(len + 1);
    if (len > 0) {
        memcpy(copy, data, len);
    }
    copy[len] = '\0';
    v->as.string.data = copy;
    v->as.string.len = len;
    return v;
}

JsonValue *json_new_array(void) {
    JsonValue *v = alloc_value(JSON_ARRAY);
    v->as.array.items = NULL;
    v->as.array.count = 0;
    v->as.array.cap = 0;
    return v;
}

JsonValue *json_new_object(void) {
    JsonValue *v = alloc_value(JSON_OBJECT);
    v->as.object.members = NULL;
    v->as.object.count = 0;
    v->as.object.cap = 0;
    return v;
}

void json_array_push(JsonValue *array, JsonValue *item) {
    if (array->as.array.count == array->as.array.cap) {
        array->as.array.cap = array->as.array.cap ? array->as.array.cap * 2 : 4;
        array->as.array.items = (JsonValue **)realloc(
            array->as.array.items, array->as.array.cap * sizeof(JsonValue *));
    }
    array->as.array.items[array->as.array.count++] = item;
}

void json_object_set(JsonValue *object, const char *key, size_t key_len, JsonValue *value) {
    for (size_t i = 0; i < object->as.object.count; i++) {
        JsonMember *m = &object->as.object.members[i];
        if (m->key_len == key_len && memcmp(m->key, key, key_len) == 0) {
            json_free(m->value);
            m->value = value;
            return;
        }
    }
    if (object->as.object.count == object->as.object.cap) {
        object->as.object.cap = object->as.object.cap ? object->as.object.cap * 2 : 4;
        object->as.object.members = (JsonMember *)realloc(
            object->as.object.members, object->as.object.cap * sizeof(JsonMember));
    }
    JsonMember *m = &object->as.object.members[object->as.object.count++];
    m->key = (char *)malloc(key_len + 1);
    memcpy(m->key, key, key_len);
    m->key[key_len] = '\0';
    m->key_len = key_len;
    m->value = value;
}

JsonValue *json_object_get(const JsonValue *object, const char *key) {
    size_t key_len = strlen(key);
    for (size_t i = 0; i < object->as.object.count; i++) {
        const JsonMember *m = &object->as.object.members[i];
        if (m->key_len == key_len && memcmp(m->key, key, key_len) == 0) {
            return m->value;
        }
    }
    return NULL;
}

void json_free(JsonValue *value) {
    if (!value) {
        return;
    }
    switch (value->type) {
        case JSON_STRING:
            free(value->as.string.data);
            break;
        case JSON_ARRAY:
            for (size_t i = 0; i < value->as.array.count; i++) {
                json_free(value->as.array.items[i]);
            }
            free(value->as.array.items);
            break;
        case JSON_OBJECT:
            for (size_t i = 0; i < value->as.object.count; i++) {
                free(value->as.object.members[i].key);
                json_free(value->as.object.members[i].value);
            }
            free(value->as.object.members);
            break;
        default:
            break;
    }
    free(value);
}

JsonValue *json_clone(const JsonValue *value) {
    switch (value->type) {
        case JSON_NULL:
            return json_new_null();
        case JSON_BOOL:
            return json_new_bool(value->as.boolean);
        case JSON_INT:
            return json_new_int(value->as.integer);
        case JSON_FLOAT:
            return json_new_float(value->as.number);
        case JSON_STRING:
            return json_new_string(value->as.string.data, value->as.string.len);
        case JSON_ARRAY: {
            JsonValue *out = json_new_array();
            for (size_t i = 0; i < value->as.array.count; i++) {
                json_array_push(out, json_clone(value->as.array.items[i]));
            }
            return out;
        }
        case JSON_OBJECT: {
            JsonValue *out = json_new_object();
            for (size_t i = 0; i < value->as.object.count; i++) {
                JsonMember *m = &value->as.object.members[i];
                json_object_set(out, m->key, m->key_len, json_clone(m->value));
            }
            return out;
        }
    }
    return json_new_null();
}

static bool numeric_value(const JsonValue *v, double *out) {
    if (v->type == JSON_BOOL) {
        *out = v->as.boolean ? 1.0 : 0.0;
        return true;
    }
    if (v->type == JSON_INT) {
        *out = (double)v->as.integer;
        return true;
    }
    if (v->type == JSON_FLOAT) {
        *out = v->as.number;
        return true;
    }
    return false;
}

bool json_equal(const JsonValue *a, const JsonValue *b) {
    /* bool/int/float all compare by numeric value against each other, matching Python's
     * bool-is-an-int-subclass and its mixed int/float equality -- but only among
     * themselves; a bool or number never equals a string, array, object or null, matching
     * Python there too. */
    double a_num, b_num;
    if (numeric_value(a, &a_num) && numeric_value(b, &b_num)) {
        /* An exact int/int or bool/bool/int comparison should not go through floating
         * point at all (a double cannot represent every int64_t exactly) -- only fall
         * back to the numeric comparison when either side is genuinely a float. */
        if (a->type != JSON_FLOAT && b->type != JSON_FLOAT) {
            int64_t av = a->type == JSON_BOOL ? (a->as.boolean ? 1 : 0) : a->as.integer;
            int64_t bv = b->type == JSON_BOOL ? (b->as.boolean ? 1 : 0) : b->as.integer;
            return av == bv;
        }
        return a_num == b_num;
    }
    if (a->type != b->type) {
        return false;
    }
    switch (a->type) {
        case JSON_NULL:
            return true;
        case JSON_STRING:
            return a->as.string.len == b->as.string.len &&
                   memcmp(a->as.string.data, b->as.string.data, a->as.string.len) == 0;
        case JSON_ARRAY:
            if (a->as.array.count != b->as.array.count) {
                return false;
            }
            for (size_t i = 0; i < a->as.array.count; i++) {
                if (!json_equal(a->as.array.items[i], b->as.array.items[i])) {
                    return false;
                }
            }
            return true;
        case JSON_OBJECT:
            if (a->as.object.count != b->as.object.count) {
                return false;
            }
            for (size_t i = 0; i < a->as.object.count; i++) {
                JsonMember *m = &a->as.object.members[i];
                JsonValue *other = json_object_get(b, m->key);
                if (!other || !json_equal(m->value, other)) {
                    return false;
                }
            }
            return true;
        default:
            return false;
    }
}

/* --- parsing ------------------------------------------------------------------------------ */

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
    bool failed;
    size_t error_offset;
    char error[160];
} Parser;

static void fail(Parser *p, size_t offset, const char *message) {
    if (p->failed) {
        return; /* keep the first error */
    }
    p->failed = true;
    p->error_offset = offset;
    snprintf(p->error, sizeof(p->error), "%s", message);
}

static void skip_ws(Parser *p) {
    while (p->pos < p->len) {
        uint8_t c = p->data[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            p->pos++;
        } else {
            break;
        }
    }
}

static JsonValue *parse_value(Parser *p);

static bool literal(Parser *p, const char *word) {
    size_t n = strlen(word);
    if (p->pos + n > p->len) {
        return false;
    }
    if (memcmp(p->data + p->pos, word, n) != 0) {
        return false;
    }
    p->pos += n;
    return true;
}

static int hex_digit(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Appends one UTF-8-encoded codepoint to a growable buffer. */
static void append_utf8(char **buf, size_t *len, size_t *cap, uint32_t cp) {
    char enc[4];
    int n;
    if (cp < 0x80) {
        enc[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        enc[0] = (char)(0xC0 | (cp >> 6));
        enc[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        enc[0] = (char)(0xE0 | (cp >> 12));
        enc[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        enc[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        enc[0] = (char)(0xF0 | (cp >> 18));
        enc[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        enc[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        enc[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    if (*len + (size_t)n + 1 > *cap) {
        *cap = (*cap + (size_t)n + 1) * 2;
        *buf = (char *)realloc(*buf, *cap);
    }
    memcpy(*buf + *len, enc, (size_t)n);
    *len += (size_t)n;
}

/* Pure lookahead, no side effects: whether 4 valid hex digits sit at byte offset `pos`, and
 * if so their value. Used for both the escape itself and the surrogate-pair lookahead below,
 * so a low-surrogate check that turns out to be wrong never has to un-fail the parser --
 * nothing failed in the first place, because this never calls fail() or moves p->pos. */
static bool peek_hex4(const Parser *p, size_t pos, int32_t *out) {
    if (pos + 4 > p->len) {
        return false;
    }
    int32_t cp = 0;
    for (int i = 0; i < 4; i++) {
        int d = hex_digit(p->data[pos + (size_t)i]);
        if (d < 0) {
            return false;
        }
        cp = (cp << 4) | d;
    }
    *out = cp;
    return true;
}

/* p->pos is at the opening '"'. Returns malloc'd UTF-8 bytes (NUL-terminated) and *out_len,
 * or NULL on error. */
/* The length of the valid UTF-8 sequence starting at data[pos] (1 for plain ASCII, 2-4 for
 * a multi-byte sequence), or 0 if data[pos] is not a legal UTF-8 lead byte, the sequence
 * runs past `len`, a continuation byte is out of the 0x80-0xBF range, or -- the three
 * "shortest form" traps a naive lead-byte-plus-continuation-count check misses -- the
 * sequence is an overlong encoding (C0/C1, or E0/F0 followed by a continuation byte below
 * their respective minimums) or encodes a UTF-16 surrogate half (ED followed by
 * 0xA0-0xBF). This is the same table CPython's own strict UTF-8 decoder uses. */
static int utf8_seq_len(const uint8_t *data, size_t len, size_t pos) {
    uint8_t c = data[pos];
    if (c < 0x80) {
        return 1;
    }
    if (c < 0xC2 || c > 0xF4) {
        return 0;
    }
    int seqlen;
    uint8_t lo1 = 0x80, hi1 = 0xBF;
    if (c <= 0xDF) {
        seqlen = 2;
    } else if (c <= 0xEF) {
        seqlen = 3;
        if (c == 0xE0) lo1 = 0xA0;
        else if (c == 0xED) hi1 = 0x9F;
    } else {
        seqlen = 4;
        if (c == 0xF0) lo1 = 0x90;
        else if (c == 0xF4) hi1 = 0x8F;
    }
    if (pos + (size_t)seqlen > len) {
        return 0;
    }
    if (data[pos + 1] < lo1 || data[pos + 1] > hi1) {
        return 0;
    }
    for (int i = 2; i < seqlen; i++) {
        if (data[pos + (size_t)i] < 0x80 || data[pos + (size_t)i] > 0xBF) {
            return 0;
        }
    }
    return seqlen;
}

static char *parse_string_raw(Parser *p, size_t *out_len) {
    size_t start = p->pos;
    p->pos++; /* opening quote */
    size_t cap = 16;
    size_t len = 0;
    char *buf = (char *)malloc(cap);

    for (;;) {
        if (p->pos >= p->len) {
            fail(p, start, "unterminated string");
            free(buf);
            return NULL;
        }
        uint8_t c = p->data[p->pos];
        if (c == '"') {
            p->pos++;
            buf[len] = '\0';
            *out_len = len;
            return buf;
        }
        if (c == '\\') {
            size_t esc_pos = p->pos;
            p->pos++;
            if (p->pos >= p->len) {
                fail(p, esc_pos, "unterminated escape");
                free(buf);
                return NULL;
            }
            uint8_t e = p->data[p->pos];
            switch (e) {
                case '"': append_utf8(&buf, &len, &cap, '"'); p->pos++; break;
                case '\\': append_utf8(&buf, &len, &cap, '\\'); p->pos++; break;
                case '/': append_utf8(&buf, &len, &cap, '/'); p->pos++; break;
                case 'b': append_utf8(&buf, &len, &cap, '\b'); p->pos++; break;
                case 'f': append_utf8(&buf, &len, &cap, '\f'); p->pos++; break;
                case 'n': append_utf8(&buf, &len, &cap, '\n'); p->pos++; break;
                case 'r': append_utf8(&buf, &len, &cap, '\r'); p->pos++; break;
                case 't': append_utf8(&buf, &len, &cap, '\t'); p->pos++; break;
                case 'u': {
                    size_t hex_pos = p->pos + 1;
                    int32_t cp;
                    if (!peek_hex4(p, hex_pos, &cp)) {
                        fail(p, esc_pos, "invalid \\uXXXX escape");
                        free(buf);
                        return NULL;
                    }
                    p->pos = hex_pos + 4;
                    /* A high surrogate followed immediately by a low surrogate combines
                     * into one codepoint above U+FFFF -- the JSON encoding of a character
                     * outside the BMP. An unpaired half is passed through as its own
                     * (technically invalid, but never-occurring in real data) three-byte
                     * sequence rather than rejected. */
                    int32_t low;
                    if (cp >= 0xD800 && cp <= 0xDBFF && p->pos + 1 < p->len &&
                        p->data[p->pos] == '\\' && p->data[p->pos + 1] == 'u' &&
                        peek_hex4(p, p->pos + 2, &low) && low >= 0xDC00 && low <= 0xDFFF) {
                        p->pos += 6;
                        uint32_t combined =
                            0x10000 + (((uint32_t)cp - 0xD800) << 10) + ((uint32_t)low - 0xDC00);
                        append_utf8(&buf, &len, &cap, combined);
                        break;
                    }
                    append_utf8(&buf, &len, &cap, (uint32_t)cp);
                    break;
                }
                default:
                    fail(p, esc_pos, "invalid escape character");
                    free(buf);
                    return NULL;
            }
            continue;
        }
        if (c < 0x20) {
            fail(p, p->pos, "invalid control character in string");
            free(buf);
            return NULL;
        }
        if (c < 0x80) {
            if (len + 2 > cap) {
                cap = (cap + 2) * 2;
                buf = (char *)realloc(buf, cap);
            }
            buf[len++] = (char)c;
            p->pos++;
            continue;
        }
        /* A byte >= 0x80 starts a multi-byte UTF-8 sequence -- validated here, not just
         * passed through. json.loads(bytes) decodes strictly via UTF-8 first and raises
         * UnicodeDecodeError on anything malformed; a save file that is genuinely
         * corrupted (found empirically: one of the real saves has an invalid checksum and
         * fails to parse in Python for exactly this reason) must fail here too, not
         * silently accept garbage bytes into the tree. */
        int seqlen = utf8_seq_len(p->data, p->len, p->pos);
        if (seqlen == 0) {
            fail(p, p->pos, "invalid UTF-8 byte sequence in string");
            free(buf);
            return NULL;
        }
        if (len + (size_t)seqlen + 1 > cap) {
            cap = (cap + (size_t)seqlen + 1) * 2;
            buf = (char *)realloc(buf, cap);
        }
        memcpy(buf + len, p->data + p->pos, (size_t)seqlen);
        len += (size_t)seqlen;
        p->pos += (size_t)seqlen;
    }
}

static JsonValue *parse_string(Parser *p) {
    size_t len;
    char *data = parse_string_raw(p, &len);
    if (!data) {
        return NULL;
    }
    JsonValue *v = alloc_value(JSON_STRING);
    v->as.string.data = data;
    v->as.string.len = len;
    return v;
}

static JsonValue *parse_number(Parser *p) {
    size_t start = p->pos;
    bool is_float = false;

    if (p->pos < p->len && p->data[p->pos] == '-') {
        p->pos++;
    }
    if (p->pos >= p->len || p->data[p->pos] < '0' || p->data[p->pos] > '9') {
        fail(p, start, "invalid number");
        return NULL;
    }
    if (p->data[p->pos] == '0') {
        p->pos++;
    } else {
        while (p->pos < p->len && p->data[p->pos] >= '0' && p->data[p->pos] <= '9') {
            p->pos++;
        }
    }
    if (p->pos < p->len && p->data[p->pos] == '.') {
        is_float = true;
        p->pos++;
        if (p->pos >= p->len || p->data[p->pos] < '0' || p->data[p->pos] > '9') {
            fail(p, start, "invalid number");
            return NULL;
        }
        while (p->pos < p->len && p->data[p->pos] >= '0' && p->data[p->pos] <= '9') {
            p->pos++;
        }
    }
    if (p->pos < p->len && (p->data[p->pos] == 'e' || p->data[p->pos] == 'E')) {
        is_float = true;
        p->pos++;
        if (p->pos < p->len && (p->data[p->pos] == '+' || p->data[p->pos] == '-')) {
            p->pos++;
        }
        if (p->pos >= p->len || p->data[p->pos] < '0' || p->data[p->pos] > '9') {
            fail(p, start, "invalid number");
            return NULL;
        }
        while (p->pos < p->len && p->data[p->pos] >= '0' && p->data[p->pos] <= '9') {
            p->pos++;
        }
    }

    size_t token_len = p->pos - start;
    char token[512];
    if (token_len >= sizeof(token)) {
        fail(p, start, "number literal too long");
        return NULL;
    }
    memcpy(token, p->data + start, token_len);
    token[token_len] = '\0';

    if (is_float) {
        char *end;
        double d = strtod(token, &end);
        if (end != token + token_len) {
            fail(p, start, "invalid number");
            return NULL;
        }
        return json_new_float(d);
    }
    errno = 0;
    char *end;
    long long v = strtoll(token, &end, 10);
    if (end != token + token_len || errno == ERANGE) {
        fail(p, start, "integer literal out of range for int64_t");
        return NULL;
    }
    return json_new_int((int64_t)v);
}

static JsonValue *parse_array(Parser *p) {
    p->pos++; /* '[' */
    JsonValue *arr = json_new_array();
    skip_ws(p);
    if (p->pos < p->len && p->data[p->pos] == ']') {
        p->pos++;
        return arr;
    }
    for (;;) {
        skip_ws(p);
        JsonValue *item = parse_value(p);
        if (!item) {
            json_free(arr);
            return NULL;
        }
        json_array_push(arr, item);
        skip_ws(p);
        if (p->pos >= p->len) {
            fail(p, p->pos, "unterminated array");
            json_free(arr);
            return NULL;
        }
        if (p->data[p->pos] == ',') {
            p->pos++;
            continue;
        }
        if (p->data[p->pos] == ']') {
            p->pos++;
            return arr;
        }
        fail(p, p->pos, "expected ',' or ']'");
        json_free(arr);
        return NULL;
    }
}

static JsonValue *parse_object(Parser *p) {
    p->pos++; /* '{' */
    JsonValue *obj = json_new_object();
    skip_ws(p);
    if (p->pos < p->len && p->data[p->pos] == '}') {
        p->pos++;
        return obj;
    }
    for (;;) {
        skip_ws(p);
        if (p->pos >= p->len || p->data[p->pos] != '"') {
            fail(p, p->pos, "expected a string key");
            json_free(obj);
            return NULL;
        }
        size_t key_len;
        char *key = parse_string_raw(p, &key_len);
        if (!key) {
            json_free(obj);
            return NULL;
        }
        skip_ws(p);
        if (p->pos >= p->len || p->data[p->pos] != ':') {
            fail(p, p->pos, "expected ':'");
            free(key);
            json_free(obj);
            return NULL;
        }
        p->pos++;
        skip_ws(p);
        JsonValue *value = parse_value(p);
        if (!value) {
            free(key);
            json_free(obj);
            return NULL;
        }
        json_object_set(obj, key, key_len, value);
        free(key);
        skip_ws(p);
        if (p->pos >= p->len) {
            fail(p, p->pos, "unterminated object");
            json_free(obj);
            return NULL;
        }
        if (p->data[p->pos] == ',') {
            p->pos++;
            continue;
        }
        if (p->data[p->pos] == '}') {
            p->pos++;
            return obj;
        }
        fail(p, p->pos, "expected ',' or '}'");
        json_free(obj);
        return NULL;
    }
}

static JsonValue *parse_value(Parser *p) {
    skip_ws(p);
    if (p->pos >= p->len) {
        fail(p, p->pos, "unexpected end of input");
        return NULL;
    }
    uint8_t c = p->data[p->pos];
    if (c == '{') return parse_object(p);
    if (c == '[') return parse_array(p);
    if (c == '"') return parse_string(p);
    /* Python's json.loads accepts NaN, Infinity and -Infinity as constants (its own
     * extension, on by default, not part of the JSON grammar) -- matched here for parity
     * even though no real save has ever contained a float at all (see the plan's
     * Context), let alone one of these three. -Infinity has to be checked before falling
     * into parse_number just because it starts with '-'. */
    if (c == '-' && literal(p, "-Infinity")) return json_new_float(-INFINITY);
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number(p);
    if (literal(p, "true")) return json_new_bool(true);
    if (literal(p, "false")) return json_new_bool(false);
    if (literal(p, "null")) return json_new_null();
    if (literal(p, "NaN")) return json_new_float(NAN);
    if (literal(p, "Infinity")) return json_new_float(INFINITY);
    fail(p, p->pos, "unexpected character");
    return NULL;
}

JsonParseResult json_parse(const uint8_t *data, size_t len) {
    Parser p = {data, len, 0, false, 0, {0}};
    JsonValue *value = parse_value(&p);
    if (value && !p.failed) {
        skip_ws(&p);
        if (p.pos != p.len) {
            fail(&p, p.pos, "extra data after the top-level value");
            json_free(value);
            value = NULL;
        }
    }
    JsonParseResult result;
    if (p.failed || !value) {
        result.value = NULL;
        result.error_offset = p.error_offset;
        snprintf(result.error, sizeof(result.error), "%s",
                 p.error[0] ? p.error : "parse error");
        if (value) {
            json_free(value);
        }
    } else {
        result.value = value;
        result.error_offset = 0;
        result.error[0] = '\0';
    }
    return result;
}

/* --- serialization: task 5's compact form (json.dumps(..., separators=(",", ":"))) ------- */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} JsonBuf;

static void buf_init(JsonBuf *b) {
    b->cap = 64;
    b->data = (char *)malloc(b->cap);
    b->len = 0;
}

static void buf_reserve(JsonBuf *b, size_t extra) {
    if (b->len + extra + 1 > b->cap) {
        while (b->len + extra + 1 > b->cap) {
            b->cap *= 2;
        }
        b->data = (char *)realloc(b->data, b->cap);
    }
}

static void buf_push_byte(JsonBuf *b, char c) {
    buf_reserve(b, 1);
    b->data[b->len++] = c;
}

static void buf_push(JsonBuf *b, const char *s, size_t n) {
    buf_reserve(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
}

static void buf_push_cstr(JsonBuf *b, const char *s) {
    buf_push(b, s, strlen(s));
}

/* Matches json.dumps' string escaping under ensure_ascii=False, pinned against every one of
 * the 256 single-byte cases (plus a few multi-byte codepoints) in
 * c/tests/json_escape_golden.tsv -- see that file's own generation for exactly what was
 * measured rather than assumed. '"' and '\' are backslash-escaped; \b \f \n \r \t are used
 * where they apply; every other control character below 0x20 becomes \u00XX with lowercase
 * hex digits; everything else, 0x20 and above (including '/', DEL 0x7F, and any UTF-8
 * multi-byte sequence), is copied through unchanged. */
static void serialize_string(JsonBuf *b, const char *data, size_t len) {
    buf_push_byte(b, '"');
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)data[i];
        switch (c) {
            case '"': buf_push(b, "\\\"", 2); break;
            case '\\': buf_push(b, "\\\\", 2); break;
            case '\b': buf_push(b, "\\b", 2); break;
            case '\f': buf_push(b, "\\f", 2); break;
            case '\n': buf_push(b, "\\n", 2); break;
            case '\r': buf_push(b, "\\r", 2); break;
            case '\t': buf_push(b, "\\t", 2); break;
            default:
                if (c < 0x20) {
                    char tmp[7];
                    snprintf(tmp, sizeof(tmp), "\\u%04x", c);
                    buf_push(b, tmp, 6);
                } else {
                    buf_push_byte(b, (char)c);
                }
        }
    }
    buf_push_byte(b, '"');
}

/* The same escaping under ensure_ascii=True -- Python's json.dumps DEFAULT, which every bare
 * json.dumps(x) call in sas4.py uses (cmd_list's value column, cmd_view's Unopened line,
 * cmd_set's before/after messages, sas4_model's strongbox-tag message). Identical to
 * serialize_string below 0x80; every codepoint at 0x80 and above becomes \uXXXX with
 * lowercase hex, as a UTF-16 surrogate pair when it is outside the BMP. A byte sequence that
 * is not valid UTF-8 cannot come from this port's own parser (which rejects it) and is
 * emitted byte-for-byte rather than guessed at. */
static void serialize_string_ascii(JsonBuf *b, const char *data, size_t len) {
    buf_push_byte(b, '"');
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)data[i];
        if (c < 0x80) {
            switch (c) {
                case '"': buf_push(b, "\\\"", 2); break;
                case '\\': buf_push(b, "\\\\", 2); break;
                case '\b': buf_push(b, "\\b", 2); break;
                case '\f': buf_push(b, "\\f", 2); break;
                case '\n': buf_push(b, "\\n", 2); break;
                case '\r': buf_push(b, "\\r", 2); break;
                case '\t': buf_push(b, "\\t", 2); break;
                default:
                    /* Python's ensure_ascii=True escape set is "anything outside
                     * 0x20-0x7E", so DEL (0x7F) is escaped here even though the
                     * ensure_ascii=False form above passes it through -- measured:
                     * that is the single byte below 0x80 where the two differ. */
                    if (c < 0x20 || c == 0x7F) {
                        char tmp[7];
                        snprintf(tmp, sizeof(tmp), "\\u%04x", c);
                        buf_push(b, tmp, 6);
                    } else {
                        buf_push_byte(b, (char)c);
                    }
            }
            continue;
        }
        /* Decode one UTF-8 sequence. */
        uint32_t cp = 0;
        size_t extra = 0;
        if ((c & 0xE0) == 0xC0) { cp = c & 0x1Fu; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0Fu; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07u; extra = 3; }
        else { buf_push_byte(b, (char)c); continue; }
        if (i + extra >= len) { buf_push_byte(b, (char)c); continue; }
        bool valid = true;
        for (size_t k = 1; k <= extra; k++) {
            unsigned char cont = (unsigned char)data[i + k];
            if ((cont & 0xC0) != 0x80) { valid = false; break; }
            cp = (cp << 6) | (cont & 0x3Fu);
        }
        if (!valid) { buf_push_byte(b, (char)c); continue; }
        i += extra;
        char tmp[16];
        if (cp > 0xFFFF) {
            uint32_t v = cp - 0x10000u;
            snprintf(tmp, sizeof(tmp), "\\u%04x\\u%04x", 0xD800u + (v >> 10),
                     0xDC00u + (v & 0x3FFu));
            buf_push_cstr(b, tmp);
        } else {
            snprintf(tmp, sizeof(tmp), "\\u%04x", cp);
            buf_push_cstr(b, tmp);
        }
    }
    buf_push_byte(b, '"');
}

static bool serialize_compact(JsonBuf *b, const JsonValue *v, char *err, size_t err_cap) {
    switch (v->type) {
        case JSON_NULL:
            buf_push_cstr(b, "null");
            return true;
        case JSON_BOOL:
            buf_push_cstr(b, v->as.boolean ? "true" : "false");
            return true;
        case JSON_INT: {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "%" PRId64, v->as.integer);
            buf_push_cstr(b, tmp);
            return true;
        }
        case JSON_FLOAT:
            /* Decision 6 of the port-to-c-sas4-core plan: no real save has ever contained
             * a float, and C has no shortest-round-trip formatter to match Python's
             * repr(), so writing one is refused outright rather than risking a value that
             * would not round-trip. */
            snprintf(err, err_cap,
                     "cannot serialize a float value (Decision 6: not supported by this port)");
            return false;
        case JSON_STRING:
            serialize_string(b, v->as.string.data, v->as.string.len);
            return true;
        case JSON_ARRAY:
            buf_push_byte(b, '[');
            for (size_t i = 0; i < v->as.array.count; i++) {
                if (i > 0) {
                    buf_push_byte(b, ',');
                }
                if (!serialize_compact(b, v->as.array.items[i], err, err_cap)) {
                    return false;
                }
            }
            buf_push_byte(b, ']');
            return true;
        case JSON_OBJECT:
            buf_push_byte(b, '{');
            for (size_t i = 0; i < v->as.object.count; i++) {
                if (i > 0) {
                    buf_push_byte(b, ',');
                }
                serialize_string(b, v->as.object.members[i].key, v->as.object.members[i].key_len);
                buf_push_byte(b, ':');
                if (!serialize_compact(b, v->as.object.members[i].value, err, err_cap)) {
                    return false;
                }
            }
            buf_push_byte(b, '}');
            return true;
    }
    snprintf(err, err_cap, "unknown value type");
    return false;
}

bool json_serialize_compact(const JsonValue *value, char **out, size_t *out_len, char *err,
                             size_t err_cap) {
    JsonBuf b;
    buf_init(&b);
    if (!serialize_compact(&b, value, err, err_cap)) {
        free(b.data);
        return false;
    }
    b.data[b.len] = '\0';
    *out = b.data;
    *out_len = b.len;
    return true;
}

/* json.dumps(value) with NO separators= argument -- Python's default, ", " after a comma
 * and ": " after a key's colon, still on one line. Not the same as the compact form above
 * (encode_document's json.dumps always passes separators=(",", ":")); this is what a bare
 * json.dumps(x) call elsewhere in the Python -- e.g. _check_strongboxes' diagnostic message
 * -- actually produces. */
static bool serialize_default(JsonBuf *b, const JsonValue *v, char *err, size_t err_cap) {
    switch (v->type) {
        case JSON_ARRAY:
            buf_push_byte(b, '[');
            for (size_t i = 0; i < v->as.array.count; i++) {
                if (i > 0) {
                    buf_push(b, ", ", 2);
                }
                if (!serialize_default(b, v->as.array.items[i], err, err_cap)) {
                    return false;
                }
            }
            buf_push_byte(b, ']');
            return true;
        case JSON_OBJECT:
            buf_push_byte(b, '{');
            for (size_t i = 0; i < v->as.object.count; i++) {
                if (i > 0) {
                    buf_push(b, ", ", 2);
                }
                serialize_string_ascii(b, v->as.object.members[i].key,
                                        v->as.object.members[i].key_len);
                buf_push(b, ": ", 2);
                if (!serialize_default(b, v->as.object.members[i].value, err, err_cap)) {
                    return false;
                }
            }
            buf_push_byte(b, '}');
            return true;
        case JSON_STRING:
            /* ensure_ascii=True, unlike the compact form -- see serialize_string_ascii. */
            serialize_string_ascii(b, v->as.string.data, v->as.string.len);
            return true;
        default:
            /* every other scalar formats identically with either separator scheme, and has
             * no string content for ensure_ascii to apply to */
            return serialize_compact(b, v, err, err_cap);
    }
}

bool json_serialize_default(const JsonValue *value, char **out, size_t *out_len, char *err,
                             size_t err_cap) {
    JsonBuf b;
    buf_init(&b);
    if (!serialize_default(&b, value, err, err_cap)) {
        free(b.data);
        return false;
    }
    b.data[b.len] = '\0';
    *out = b.data;
    *out_len = b.len;
    return true;
}

/* --- serialization: task 6's indented form (json.dumps(..., indent=2)) ------------------- */

static void buf_push_indent(JsonBuf *b, int depth) {
    for (int i = 0; i < depth * 2; i++) {
        buf_push_byte(b, ' ');
    }
}

static bool serialize_indent(JsonBuf *b, const JsonValue *v, int depth, char *err,
                              size_t err_cap) {
    switch (v->type) {
        case JSON_NULL:
            buf_push_cstr(b, "null");
            return true;
        case JSON_BOOL:
            buf_push_cstr(b, v->as.boolean ? "true" : "false");
            return true;
        case JSON_INT: {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "%" PRId64, v->as.integer);
            buf_push_cstr(b, tmp);
            return true;
        }
        case JSON_FLOAT:
            snprintf(err, err_cap,
                     "cannot serialize a float value (Decision 6: not supported by this port)");
            return false;
        case JSON_STRING:
            serialize_string(b, v->as.string.data, v->as.string.len);
            return true;
        case JSON_ARRAY:
            if (v->as.array.count == 0) {
                buf_push_cstr(b, "[]");
                return true;
            }
            buf_push_byte(b, '[');
            buf_push_byte(b, '\n');
            for (size_t i = 0; i < v->as.array.count; i++) {
                buf_push_indent(b, depth + 1);
                if (!serialize_indent(b, v->as.array.items[i], depth + 1, err, err_cap)) {
                    return false;
                }
                if (i + 1 < v->as.array.count) {
                    buf_push_byte(b, ',');
                }
                buf_push_byte(b, '\n');
            }
            buf_push_indent(b, depth);
            buf_push_byte(b, ']');
            return true;
        case JSON_OBJECT:
            if (v->as.object.count == 0) {
                buf_push_cstr(b, "{}");
                return true;
            }
            buf_push_byte(b, '{');
            buf_push_byte(b, '\n');
            for (size_t i = 0; i < v->as.object.count; i++) {
                buf_push_indent(b, depth + 1);
                serialize_string(b, v->as.object.members[i].key, v->as.object.members[i].key_len);
                buf_push_cstr(b, ": ");
                if (!serialize_indent(b, v->as.object.members[i].value, depth + 1, err,
                                       err_cap)) {
                    return false;
                }
                if (i + 1 < v->as.object.count) {
                    buf_push_byte(b, ',');
                }
                buf_push_byte(b, '\n');
            }
            buf_push_indent(b, depth);
            buf_push_byte(b, '}');
            return true;
    }
    snprintf(err, err_cap, "unknown value type");
    return false;
}

bool json_serialize_indent(const JsonValue *value, char **out, size_t *out_len, char *err,
                            size_t err_cap) {
    JsonBuf b;
    buf_init(&b);
    if (!serialize_indent(&b, value, 0, err, err_cap)) {
        free(b.data);
        return false;
    }
    b.data[b.len] = '\0';
    *out = b.data;
    *out_len = b.len;
    return true;
}
