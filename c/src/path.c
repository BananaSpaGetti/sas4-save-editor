/*
 * Path navigation -- port of sas4.py's at_path, parent_of, scalars, coerce and the
 * kind_of/Counter machinery cmd_kinds uses. Task 7 of port-to-c-sas4-core.
 */
#include "path.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- shared segment-splitting helpers ---------------------------------------------------- */

/* [p for p in path.strip("/").split("/") if p] -- strips leading/trailing '/', splits on
 * '/', drops empty segments (so "//a//b/" and "a/b" split the same way). Returns a malloc'd
 * array of malloc'd segment strings; free each, then the array. */
static char **split_segments(const char *path, size_t *out_count) {
    size_t len = strlen(path);
    size_t start = 0, end = len;
    while (start < end && path[start] == '/') start++;
    while (end > start && path[end - 1] == '/') end--;

    size_t cap = 8;
    char **segs = (char **)malloc(cap * sizeof(char *));
    size_t count = 0;
    size_t i = start;
    while (i < end) {
        size_t seg_start = i;
        while (i < end && path[i] != '/') i++;
        size_t seg_len = i - seg_start;
        if (seg_len > 0) {
            if (count == cap) {
                cap *= 2;
                segs = (char **)realloc(segs, cap * sizeof(char *));
            }
            char *seg = (char *)malloc(seg_len + 1);
            memcpy(seg, path + seg_start, seg_len);
            seg[seg_len] = '\0';
            segs[count++] = seg;
        }
        i++; /* skip the '/' */
    }
    *out_count = count;
    return segs;
}

static void free_segments(char **segs, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(segs[i]);
    }
    free(segs);
}

/* Rightmost '[' in s[0..len): (before_len, after, after_len, found), matching Python's
 * str.rpartition("[") -- if not found, before_len=0 and after/after_len is the whole input
 * (rpartition's own not-found convention: ("", "", original)). */
static void rpartition_bracket(const char *s, size_t len, size_t *before_len,
                                const char **after, size_t *after_len, bool *found) {
    for (size_t i = len; i > 0; i--) {
        if (s[i - 1] == '[') {
            *before_len = i - 1;
            *after = s + i;
            *after_len = len - i;
            *found = true;
            return;
        }
    }
    *before_len = 0;
    *after = s;
    *after_len = len;
    *found = false;
}

/* One "/"-separated segment, possibly ending in "[N]". Dereferences `*node` through it,
 * matching at_path's per-segment logic exactly -- including that a segment with more than
 * one bracket ("b[2][3]") does NOT chain two index lookups: Python's own loop reassigns and
 * then unconditionally clears its `part` variable each time through, so only a single
 * rpartition ever runs per segment, and "b[2]" (brackets included) is looked up as one
 * literal key before the final "[3]" index -- reproduced here exactly, bug and all, rather
 * than "fixed" into the nested-index behavior the code visually suggests but does not have. */
static bool navigate_segment(JsonValue **node, const char *seg, size_t seg_len, char *err,
                              size_t err_cap) {
    if (seg_len > 0 && seg[seg_len - 1] == ']') {
        size_t before_len;
        const char *after;
        size_t after_len;
        bool found;
        rpartition_bracket(seg, seg_len - 1, &before_len, &after, &after_len, &found);

        if (before_len > 0) {
            if ((*node)->type != JSON_OBJECT) {
                snprintf(err, err_cap, "cannot index a %s by key",
                         (*node)->type == JSON_ARRAY ? "list" : "scalar");
                return false;
            }
            char key[256];
            size_t n = before_len < sizeof(key) - 1 ? before_len : sizeof(key) - 1;
            memcpy(key, seg, n);
            key[n] = '\0';
            JsonValue *next = json_object_get(*node, key);
            if (!next) {
                snprintf(err, err_cap, "key not found: '%.150s'", key);
                return false;
            }
            *node = next;
        }

        char index_buf[32];
        size_t n = after_len < sizeof(index_buf) - 1 ? after_len : sizeof(index_buf) - 1;
        memcpy(index_buf, after, n);
        index_buf[n] = '\0';
        char *end;
        errno = 0;
        long long idx = strtoll(index_buf, &end, 10);
        if (end == index_buf || *end != '\0' || errno == ERANGE) {
            snprintf(err, err_cap, "invalid list index: '%s'", index_buf);
            return false;
        }
        if ((*node)->type != JSON_ARRAY) {
            snprintf(err, err_cap, "cannot index a %s by position",
                     (*node)->type == JSON_OBJECT ? "object" : "scalar");
            return false;
        }
        long long count = (long long)(*node)->as.array.count;
        long long effective = idx < 0 ? count + idx : idx;
        if (effective < 0 || effective >= count) {
            snprintf(err, err_cap, "list index out of range");
            return false;
        }
        *node = (*node)->as.array.items[effective];
        return true;
    }

    if ((*node)->type != JSON_OBJECT) {
        snprintf(err, err_cap, "cannot index a %s by key",
                 (*node)->type == JSON_ARRAY ? "list" : "scalar");
        return false;
    }
    char key[256];
    size_t n = seg_len < sizeof(key) - 1 ? seg_len : sizeof(key) - 1;
    memcpy(key, seg, n);
    key[n] = '\0';
    JsonValue *next = json_object_get(*node, key);
    if (!next) {
        snprintf(err, err_cap, "key not found: '%.150s'", key);
        return false;
    }
    *node = next;
    return true;
}

/* --- at_path ------------------------------------------------------------------------------ */

PathAtResult path_at(JsonValue *document, const char *path) {
    PathAtResult result;
    result.ok = true;
    result.value = document;
    result.error[0] = '\0';

    size_t count;
    char **segs = split_segments(path, &count);
    for (size_t i = 0; i < count; i++) {
        if (!navigate_segment(&result.value, segs[i], strlen(segs[i]), result.error,
                               sizeof(result.error))) {
            result.ok = false;
            result.value = NULL;
            break;
        }
    }
    free_segments(segs, count);
    return result;
}

/* --- parent_of ------------------------------------------------------------------------- */

PathParentResult path_parent_of(JsonValue *document, const char *path) {
    PathParentResult result;
    result.ok = true;
    result.parent = NULL;
    result.is_index = false;
    result.key_str[0] = '\0';
    result.key_index = 0;
    result.error[0] = '\0';

    size_t count;
    char **segs = split_segments(path, &count);
    if (count == 0) {
        snprintf(result.error, sizeof(result.error), "empty path");
        result.ok = false;
        free_segments(segs, count);
        return result;
    }

    JsonValue *parent = document;
    for (size_t i = 0; i + 1 < count; i++) {
        if (!navigate_segment(&parent, segs[i], strlen(segs[i]), result.error,
                               sizeof(result.error))) {
            result.ok = false;
            free_segments(segs, count);
            return result;
        }
    }

    const char *tail = segs[count - 1];
    size_t tail_len = strlen(tail);
    if (tail_len > 0 && tail[tail_len - 1] == ']') {
        size_t before_len;
        const char *after;
        size_t after_len;
        bool found;
        rpartition_bracket(tail, tail_len - 1, &before_len, &after, &after_len, &found);
        if (before_len > 0) {
            if (parent->type != JSON_OBJECT) {
                snprintf(result.error, sizeof(result.error), "cannot index a %s by key",
                         parent->type == JSON_ARRAY ? "list" : "scalar");
                result.ok = false;
                free_segments(segs, count);
                return result;
            }
            char key[256];
            size_t n = before_len < sizeof(key) - 1 ? before_len : sizeof(key) - 1;
            memcpy(key, tail, n);
            key[n] = '\0';
            JsonValue *next = json_object_get(parent, key);
            if (!next) {
                snprintf(result.error, sizeof(result.error), "key not found: '%.150s'", key);
                result.ok = false;
                free_segments(segs, count);
                return result;
            }
            parent = next;
        }
        char index_buf[32];
        size_t n = after_len < sizeof(index_buf) - 1 ? after_len : sizeof(index_buf) - 1;
        memcpy(index_buf, after, n);
        index_buf[n] = '\0';
        char *end;
        errno = 0;
        long long idx = strtoll(index_buf, &end, 10);
        if (end == index_buf || *end != '\0' || errno == ERANGE) {
            snprintf(result.error, sizeof(result.error), "invalid list index: '%s'", index_buf);
            result.ok = false;
            free_segments(segs, count);
            return result;
        }
        result.parent = parent;
        result.is_index = true;
        result.key_index = (int64_t)idx;
    } else {
        result.parent = parent;
        result.is_index = false;
        size_t n = tail_len < sizeof(result.key_str) - 1 ? tail_len : sizeof(result.key_str) - 1;
        memcpy(result.key_str, tail, n);
        result.key_str[n] = '\0';
    }

    free_segments(segs, count);
    return result;
}

/* --- scalars ------------------------------------------------------------------------------ */

static void scalars_append(ScalarList *out, const char *path, JsonValue *value) {
    if (out->count == out->cap) {
        out->cap = out->cap ? out->cap * 2 : 16;
        out->items = (ScalarEntry *)realloc(out->items, out->cap * sizeof(ScalarEntry));
    }
    out->items[out->count].path = strdup(path);
    out->items[out->count].value = value;
    out->count++;
}

void path_scalars(JsonValue *node, const char *path_prefix, ScalarList *out) {
    if (node->type == JSON_OBJECT) {
        for (size_t i = 0; i < node->as.object.count; i++) {
            JsonMember *m = &node->as.object.members[i];
            char child[4096];
            snprintf(child, sizeof(child), "%s/%s", path_prefix, m->key);
            path_scalars(m->value, child, out);
        }
    } else if (node->type == JSON_ARRAY) {
        for (size_t i = 0; i < node->as.array.count; i++) {
            char child[4096];
            snprintf(child, sizeof(child), "%s[%zu]", path_prefix, i);
            path_scalars(node->as.array.items[i], child, out);
        }
    } else {
        scalars_append(out, path_prefix, node);
    }
}

void path_scalars_free(ScalarList *list) {
    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i].path);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

/* --- kind_of / kinds tally ------------------------------------------------------------- */

const char *path_kind_of(const JsonValue *value) {
    /* TYPES = {"bool": bool, "int": int, "str": str, "float": float, "null": type(None)} --
     * checked in that order, bool before int, matching kind_of()'s own order (which also
     * special-cases skipping int for a bool value, redundant with checking bool first, but
     * the net effect is identical either way). */
    switch (value->type) {
        case JSON_BOOL: return "bool";
        case JSON_INT: return "int";
        case JSON_STRING: return "str";
        case JSON_FLOAT: return "float";
        case JSON_NULL: return "null";
        default: return "other";
    }
}

void path_kind_tally(const ScalarList *scalars, KindCount **out, size_t *out_count) {
    KindCount *tally = NULL;
    size_t count = 0;
    size_t cap = 0;
    for (size_t i = 0; i < scalars->count; i++) {
        const char *kind = path_kind_of(scalars->items[i].value);
        size_t j;
        for (j = 0; j < count; j++) {
            if (strcmp(tally[j].name, kind) == 0) {
                tally[j].count++;
                break;
            }
        }
        if (j == count) {
            if (count == cap) {
                cap = cap ? cap * 2 : 8;
                tally = (KindCount *)realloc(tally, cap * sizeof(KindCount));
            }
            snprintf(tally[count].name, sizeof(tally[count].name), "%s", kind);
            tally[count].count = 1;
            count++;
        }
    }
    /* Stable sort by count descending -- ties keep first-seen order, matching
     * collections.Counter.most_common()'s own stable sort. A simple insertion sort is
     * plenty: `count` is at most 6 (the five real kinds plus "other"). */
    for (size_t i = 1; i < count; i++) {
        KindCount key = tally[i];
        size_t j = i;
        while (j > 0 && tally[j - 1].count < key.count) {
            tally[j] = tally[j - 1];
            j--;
        }
        tally[j] = key;
    }
    *out = tally;
    *out_count = count;
}

/* --- coerce -------------------------------------------------------------------------------- */

/* Python's int(text, 0): strip whitespace, optional sign, then base auto-detected from a
 * "0x"/"0o"/"0b" prefix (case-insensitive); a bare "0" is zero; anything else starting with
 * '0' is rejected (PEP 3127 removed legacy 0-prefixed octal) rather than silently read as
 * octal the way C's own strtoll(text, &end, 0) would. */
static bool py_int_base0(const char *text, int64_t *out, char *err, size_t err_cap) {
    const char *s = text;
    while (isspace((unsigned char)*s)) s++;
    const char *end_trim = s + strlen(s);
    while (end_trim > s && isspace((unsigned char)end_trim[-1])) end_trim--;
    size_t len = (size_t)(end_trim - s);

    bool bad = false;
    if (len == 0) {
        bad = true;
    } else {
        const char *digits = s;
        size_t digits_len = len;
        bool neg = false;
        if (*digits == '+' || *digits == '-') {
            neg = (*digits == '-');
            digits++;
            digits_len--;
        }
        int base = 10;
        if (digits_len >= 2 && digits[0] == '0' &&
            (digits[1] == 'x' || digits[1] == 'X')) {
            base = 16; digits += 2; digits_len -= 2;
        } else if (digits_len >= 2 && digits[0] == '0' &&
                   (digits[1] == 'o' || digits[1] == 'O')) {
            base = 8; digits += 2; digits_len -= 2;
        } else if (digits_len >= 2 && digits[0] == '0' &&
                   (digits[1] == 'b' || digits[1] == 'B')) {
            base = 2; digits += 2; digits_len -= 2;
        } else if (digits_len >= 1 && digits[0] == '0') {
            /* A bare "0" is fine; "0" followed by more characters is a rejected leading
             * zero UNLESS every character is also '0' (Python accepts "00", "000", ...). */
            bool all_zero = true;
            for (size_t i = 0; i < digits_len; i++) {
                if (digits[i] != '0') { all_zero = false; break; }
            }
            if (!all_zero) {
                bad = true;
            }
            base = 10;
        }
        if (!bad) {
            if (digits_len == 0) {
                bad = true;
            } else {
                for (size_t i = 0; i < digits_len && !bad; i++) {
                    char c = digits[i];
                    int d = (c >= '0' && c <= '9') ? c - '0'
                           : (c >= 'a' && c <= 'z') ? c - 'a' + 10
                           : (c >= 'A' && c <= 'Z') ? c - 'A' + 10
                           : -1;
                    if (d < 0 || d >= base) {
                        bad = true;
                    }
                }
            }
            if (!bad) {
                errno = 0;
                char *strtoll_end;
                long long v = strtoll(digits, &strtoll_end, base);
                if (strtoll_end != digits + digits_len || errno == ERANGE) {
                    bad = true;
                } else {
                    *out = neg ? -(int64_t)v : (int64_t)v;
                }
            }
        }
    }
    if (bad) {
        snprintf(err, err_cap, "invalid literal for int() with base 0: '%s'", text);
        return false;
    }
    return true;
}

CoerceResult path_coerce(const char *text, const JsonValue *current) {
    CoerceResult result;
    result.ok = false;
    result.value = NULL;
    result.error[0] = '\0';

    if (current->type == JSON_BOOL) {
        char lower[16];
        size_t n = strlen(text);
        if (n < sizeof(lower)) {
            for (size_t i = 0; i < n; i++) {
                lower[i] = (char)tolower((unsigned char)text[i]);
            }
            lower[n] = '\0';
            if (strcmp(lower, "true") == 0) {
                result.ok = true;
                result.value = json_new_bool(true);
                return result;
            }
            if (strcmp(lower, "false") == 0) {
                result.ok = true;
                result.value = json_new_bool(false);
                return result;
            }
        }
        snprintf(result.error, sizeof(result.error), "bool is a boolean; pass true or false");
        return result;
    }

    if (current->type == JSON_INT) {
        int64_t v;
        if (py_int_base0(text, &v, result.error, sizeof(result.error))) {
            result.ok = true;
            result.value = json_new_int(v);
        }
        return result;
    }

    if (current->type == JSON_FLOAT) {
        char *end;
        errno = 0;
        double v = strtod(text, &end);
        /* strtod skips leading whitespace itself, matching float()'s own strip(). */
        while (end && isspace((unsigned char)*end)) end++;
        if (end == text || (end && *end != '\0')) {
            snprintf(result.error, sizeof(result.error),
                     "could not convert string to float: '%s'", text);
            return result;
        }
        result.ok = true;
        result.value = json_new_float(v);
        return result;
    }

    if (current->type == JSON_STRING) {
        result.ok = true;
        result.value = json_new_string(text, strlen(text));
        return result;
    }

    const char *type_name = current->type == JSON_ARRAY ? "list"
                           : current->type == JSON_OBJECT ? "dict"
                           : "NoneType";
    snprintf(result.error, sizeof(result.error),
             "cannot set a %s directly; edit it with decode/encode", type_name);
    return result;
}
