/*
 * Port of sas4_model.py's XP curve, level/mastery constants, and check() -- task 10 of
 * port-to-c-sas4-core. Every rule below reproduces its Python counterpart's wording
 * character for character; see sas4_model.py for the reasoning behind each one.
 */
#include "model.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- XP curve ----------------------------------------------------------------------- */

/* Cumulative XP to reach each level, from the community wiki, cross-checked against a real
 * save -- see sas4_model.py's XP_TABLE comment. Kept only to cross-check the formula below
 * at startup; xp_for_level does not consult this directly. */
static const int64_t XP_TABLE[26] = {
    0, 0, 1071, 2359, 4014, 6190, 9045, 12741, 17445, 23328, 30565, 39335, 49821,
    62211, 76697, 93475, 112745, 134711, 159582, 187571, 218895, 253775, 292436,
    335108, 382025, 433425};

const int64_t MODEL_MASTERY_LEVEL_XP[6] = {0, 2400, 12400, 42400, 142400, 542400};

int64_t model_xp_per_level(int level) {
    /* (10000 + 707*level^2 + 7*level^3) / 10 with C's truncating integer division standing
     * in for Python's floor division (//): they agree here because level is never negative
     * in this port's callers, and truncation and floor are the same operation for a
     * nonnegative numerator over a positive divisor. */
    int64_t l = level;
    return (10000 + 707 * l * l + 7 * l * l * l) / 10;
}

/* Built once, lazily, the same shape as Python's module-level _CUMULATIVE: totals[0] and
 * totals[1] are both 0 (level 0 and level 1 both cost nothing to have reached), then each
 * further entry adds one more level's xp_per_level. Covers level 0 through MODEL_MAX_LEVEL +
 * 8 (108) directly; xp_for_level falls back to summing past that. */
#define CUMULATIVE_LEN (MODEL_MAX_LEVEL + 9)
static int64_t s_cumulative[CUMULATIVE_LEN];
static bool s_ready = false;

static void ensure_ready(void) {
    if (s_ready) {
        return;
    }
    s_cumulative[0] = 0;
    s_cumulative[1] = 0;
    for (int level = 1; level < MODEL_MAX_LEVEL + 8; level++) {
        s_cumulative[level + 1] = s_cumulative[level] + model_xp_per_level(level);
    }
    /* Import-time cross-check, matching sas4_model.py's own assert: the closed-form curve
     * has to reproduce every one of the 26 observed table values, or the formula and the
     * table disagree and one of them is wrong. */
    for (size_t i = 0; i < sizeof(XP_TABLE) / sizeof(XP_TABLE[0]); i++) {
        assert(s_cumulative[i] == XP_TABLE[i] && "xp_per_level no longer matches XP_TABLE");
    }
    s_ready = true;
}

void model_cross_check(void) {
    ensure_ready();
}

int64_t model_xp_for_level(int level) {
    ensure_ready();
    if (level < 0) {
        return 0;
    }
    if (level < CUMULATIVE_LEN) {
        return s_cumulative[level];
    }
    int64_t total = s_cumulative[CUMULATIVE_LEN - 1];
    for (int n = CUMULATIVE_LEN - 1; n < level; n++) {
        total += model_xp_per_level(n);
    }
    return total;
}

int model_mastery_level_for_xp(int64_t xp) {
    int level = 0;
    for (int candidate = 0; candidate < 6; candidate++) {
        if (xp >= MODEL_MASTERY_LEVEL_XP[candidate]) {
            level = candidate;
        }
    }
    return level;
}

const MasteryTrackName MODEL_MASTERY_TRACKS[2] = {
    {0, "pistols"},
    {9, "high damage ammo"},
};
const size_t MODEL_MASTERY_TRACKS_COUNT = 2;

const char *model_mastery_name(int index) {
    for (size_t i = 0; i < MODEL_MASTERY_TRACKS_COUNT; i++) {
        if (MODEL_MASTERY_TRACKS[i].index == index) {
            return MODEL_MASTERY_TRACKS[i].name;
        }
    }
    return NULL;
}

/* --- ProblemList ---------------------------------------------------------------------- */

void model_problem_list_free(ProblemList *list) {
    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

static void problem_list_push(ProblemList *out, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list args2;
    va_copy(args2, args);
    int needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (needed < 0) {
        va_end(args2);
        return;
    }
    char *msg = (char *)malloc((size_t)needed + 1);
    vsnprintf(msg, (size_t)needed + 1, fmt, args2);
    va_end(args2);

    if (out->count == out->cap) {
        out->cap = out->cap ? out->cap * 2 : 8;
        out->items = (char **)realloc(out->items, out->cap * sizeof(char *));
    }
    out->items[out->count++] = msg;
}

/* --- Python-semantics helpers ----------------------------------------------------------
 *
 * Python's bool is an int subclass, so isinstance(x, int) is True for a JSON true/false
 * too, and arithmetic on it uses 0/1. Matched here rather than treated as a separate case,
 * the same way json.c's json_equal does for `==`. */

bool model_py_is_int(const JsonValue *v) {
    return v && (v->type == JSON_INT || v->type == JSON_BOOL);
}

int64_t model_py_int_value(const JsonValue *v) {
    return v->type == JSON_BOOL ? (v->as.boolean ? 1 : 0) : v->as.integer;
}

bool model_py_truthy(const JsonValue *v) {
    if (!v) {
        return false; /* .get(key) with no default is None, which is falsy */
    }
    switch (v->type) {
        case JSON_NULL: return false;
        case JSON_BOOL: return v->as.boolean;
        case JSON_INT: return v->as.integer != 0;
        case JSON_FLOAT: return v->as.number != 0.0;
        case JSON_STRING: return v->as.string.len != 0;
        case JSON_ARRAY: return v->as.array.count != 0;
        case JSON_OBJECT: return v->as.object.count != 0;
    }
    return false;
}

/* --- a minimal Python repr(), for the two rule messages that use %r ---------------------
 *
 * Only the shapes that can actually appear in a JSON tree: None/bool/int/float/string/
 * list/dict. Strings use Python's own quote-preference rule (single quotes unless the
 * string holds one and not a double quote), with \\, \n, \r, \t and other control bytes
 * below 0x20 backslash-escaped -- printable non-ASCII passes through unescaped, matching
 * Python 3's repr(). */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

static void sb_init(StrBuf *sb) {
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

static void sb_reserve(StrBuf *sb, size_t extra) {
    if (sb->len + extra + 1 <= sb->cap) {
        return;
    }
    size_t want = sb->cap ? sb->cap * 2 : 64;
    while (want < sb->len + extra + 1) {
        want *= 2;
    }
    sb->data = (char *)realloc(sb->data, want);
    sb->cap = want;
}

static void sb_append(StrBuf *sb, const char *text, size_t len) {
    sb_reserve(sb, len);
    memcpy(sb->data + sb->len, text, len);
    sb->len += len;
    sb->data[sb->len] = '\0';
}

static void sb_str(StrBuf *sb, const char *text) {
    sb_append(sb, text, strlen(text));
}

static void sb_fmt(StrBuf *sb, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list args2;
    va_copy(args2, args);
    int needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    char *tmp = (char *)malloc((size_t)needed + 1);
    vsnprintf(tmp, (size_t)needed + 1, fmt, args2);
    va_end(args2);
    sb_append(sb, tmp, (size_t)needed);
    free(tmp);
}

static void py_repr_string(StrBuf *sb, const char *data, size_t len) {
    bool has_single = false, has_double = false;
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\'') has_single = true;
        if (data[i] == '"') has_double = true;
    }
    char quote = (has_single && !has_double) ? '"' : '\'';
    sb_append(sb, &quote, 1);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)data[i];
        if (c == (unsigned char)quote || c == '\\') {
            sb_fmt(sb, "\\%c", c);
        } else if (c == '\n') {
            sb_str(sb, "\\n");
        } else if (c == '\r') {
            sb_str(sb, "\\r");
        } else if (c == '\t') {
            sb_str(sb, "\\t");
        } else if (c < 0x20 || c == 0x7f) {
            sb_fmt(sb, "\\x%02x", c);
        } else {
            sb_append(sb, (const char *)&c, 1);
        }
    }
    sb_append(sb, &quote, 1);
}

static void py_repr(StrBuf *sb, const JsonValue *v) {
    if (!v || v->type == JSON_NULL) {
        sb_str(sb, "None");
        return;
    }
    switch (v->type) {
        case JSON_BOOL:
            sb_str(sb, v->as.boolean ? "True" : "False");
            return;
        case JSON_INT:
            sb_fmt(sb, "%lld", (long long)v->as.integer);
            return;
        case JSON_FLOAT:
            sb_fmt(sb, "%g", v->as.number);
            return;
        case JSON_STRING:
            py_repr_string(sb, v->as.string.data, v->as.string.len);
            return;
        case JSON_ARRAY:
            sb_str(sb, "[");
            for (size_t i = 0; i < v->as.array.count; i++) {
                if (i) sb_str(sb, ", ");
                py_repr(sb, v->as.array.items[i]);
            }
            sb_str(sb, "]");
            return;
        case JSON_OBJECT:
            sb_str(sb, "{");
            for (size_t i = 0; i < v->as.object.count; i++) {
                if (i) sb_str(sb, ", ");
                py_repr_string(sb, v->as.object.members[i].key, v->as.object.members[i].key_len);
                sb_str(sb, ": ");
                py_repr(sb, v->as.object.members[i].value);
            }
            sb_str(sb, "}");
            return;
        default:
            return;
    }
}

/* Python's %s calls str(), not repr() -- identical to py_repr for every JSON-representable
 * type except a top-level string, where str() gives the raw text back unquoted and
 * unescaped instead of repr()'s quoted, backslash-escaped form. (A string nested inside a
 * list or dict is still shown via repr() by Python's own str(list)/str(dict), which is why
 * py_repr is still what py_str calls for anything but a bare top-level string.) */
static void py_str(StrBuf *sb, const JsonValue *v) {
    if (v && v->type == JSON_STRING) {
        sb_append(sb, v->as.string.data, v->as.string.len);
        return;
    }
    py_repr(sb, v);
}

/* Python's int(text): optional surrounding whitespace, an optional sign, then decimal
 * digits with optional single underscores between them (PEP 515). Python's str.strip()
 * whitespace set includes tab, newline, carriage return, form feed and vertical tab, which
 * int() skips at both ends -- "3 " and " 3" are both valid ints there, and that is what
 * mastery --set's "3 = 5" pieces rely on. */
static bool is_py_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

bool model_py_parse_int(const char *s, long *out) {
    const char *p = s;
    while (is_py_space(*p)) p++;
    const char *start = p;
    if (*p == '+' || *p == '-') p++;
    bool any_digit = false, prev_digit = false;
    while (*p) {
        if (*p >= '0' && *p <= '9') { any_digit = true; prev_digit = true; p++; continue; }
        if (*p == '_' && prev_digit) { prev_digit = false; p++; continue; }
        break;
    }
    if (!any_digit || !prev_digit) return false;
    const char *digits_end = p;
    while (is_py_space(*p)) p++;
    if (*p != '\0') return false;
    char clean[64];
    size_t n = 0;
    for (const char *q = start; q < digits_end && n + 1 < sizeof clean; q++)
        if (*q != '_') clean[n++] = *q;
    clean[n] = '\0';
    char *end = NULL;
    long v = strtol(clean, &end, 10);
    if (end == clean || *end != '\0') return false;
    *out = v;
    return true;
}

char *model_py_repr(const JsonValue *v) {
    StrBuf sb;
    sb_init(&sb);
    py_repr(&sb, v);
    if (!sb.data) sb_str(&sb, ""); /* py_repr's default: case never appends -- keep it non-NULL */
    return sb.data;
}

char *model_py_str(const JsonValue *v) {
    StrBuf sb;
    sb_init(&sb);
    py_str(&sb, v);
    if (!sb.data) sb_str(&sb, "");
    return sb.data;
}

/* json.dumps(tag)[:20] for _check_strongboxes' "found %s" message -- json.dumps(tag) there
 * is called with NO separators= argument, so it is Python's DEFAULT ", " / ": " form, not
 * the compact form encode_document uses; json_serialize_default matches that (measured: an
 * early version of this reused the compact serializer and printed "{"ID":2}" where Python
 * prints "{"ID": 2}", caught by a hand-crafted bad-tag test case, not by any of the stock
 * ATTACKS). Falls back to a placeholder only for the float case the serializer refuses,
 * which this port's test data never produces. Truncation is by byte, matching Python's
 * slice on an ASCII-only json.dumps output. */
static void json_dumps_head20(const JsonValue *v, char *out, size_t out_cap) {
    char *serialized;
    size_t serialized_len;
    char err[128];
    if (json_serialize_default(v, &serialized, &serialized_len, err, sizeof(err))) {
        size_t take = serialized_len < 20 ? serialized_len : 20;
        size_t copy = take < out_cap - 1 ? take : out_cap - 1;
        memcpy(out, serialized, copy);
        out[copy] = '\0';
        free(serialized);
    } else {
        snprintf(out, out_cap, "<unrepresentable>");
    }
}

/* --- loaded_profiles ---------------------------------------------------------------------
 *
 * document.get("Inventory", {}).items(), keeping only the dict-typed, Loaded-truthy ones,
 * paired with "Inventory/%s" % slot -- exactly Python's generator, just visited eagerly
 * through a callback instead of yielded. */
typedef void (*ProfileVisitor)(const char *where, const JsonValue *profile, const void *ctx,
                                ProblemList *out);

static void for_loaded_profiles(const JsonValue *document, ProfileVisitor visit,
                                 const void *ctx, ProblemList *out) {
    if (!document || document->type != JSON_OBJECT) {
        return;
    }
    const JsonValue *inventory = json_object_get(document, "Inventory");
    if (!inventory || inventory->type != JSON_OBJECT) {
        return;
    }
    for (size_t i = 0; i < inventory->as.object.count; i++) {
        const JsonMember *m = &inventory->as.object.members[i];
        if (m->value->type != JSON_OBJECT) {
            continue;
        }
        if (!model_py_truthy(json_object_get(m->value, "Loaded"))) {
            continue;
        }
        char where[300];
        snprintf(where, sizeof(where), "Inventory/%.*s", (int)m->key_len, m->key);
        visit(where, m->value, ctx, out);
    }
}

void model_loaded_profiles(const JsonValue *document, LoadedProfileList *out) {
    out->items = NULL;
    out->count = 0;
    out->cap = 0;
    if (!document || document->type != JSON_OBJECT) {
        return;
    }
    const JsonValue *inventory = json_object_get(document, "Inventory");
    if (!inventory || inventory->type != JSON_OBJECT) {
        return;
    }
    for (size_t i = 0; i < inventory->as.object.count; i++) {
        const JsonMember *m = &inventory->as.object.members[i];
        if (m->value->type != JSON_OBJECT) {
            continue;
        }
        if (!model_py_truthy(json_object_get(m->value, "Loaded"))) {
            continue;
        }
        if (out->count == out->cap) {
            out->cap = out->cap ? out->cap * 2 : 8;
            out->items = (LoadedProfile *)realloc(out->items, out->cap * sizeof(LoadedProfile));
        }
        snprintf(out->items[out->count].where, sizeof(out->items[out->count].where),
                 "Inventory/%.*s", (int)m->key_len, m->key);
        out->items[out->count].profile = m->value;
        out->count++;
    }
}

void model_loaded_profiles_free(LoadedProfileList *list) {
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

/* Skills, resolved the way profile.get("Skills", {}) would be, for the callers below that
 * only ever call .get() on the result -- NULL (treated as an empty object) whenever "Skills"
 * is absent or is not itself an object. A save where "Skills" holds some other JSON type
 * would make the Python .get() calls that follow raise AttributeError; that is a defect in
 * the reference this port does not need to reproduce a crash for, so this returns NULL
 * (fields read from it come back absent, same as an empty dict) rather than aborting. */
static const JsonValue *profile_skills(const JsonValue *profile) {
    const JsonValue *skills = json_object_get(profile, "Skills");
    return (skills && skills->type == JSON_OBJECT) ? skills : NULL;
}

/* --- the nine rules ------------------------------------------------------------------- */

static void visit_xp_matches_level(const char *where, const JsonValue *profile,
                                    const void *ctx, ProblemList *out) {
    (void)ctx;
    const JsonValue *skills = profile_skills(profile);
    const JsonValue *level_v = skills ? json_object_get(skills, "PlayerLevel") : NULL;
    const JsonValue *xp_v = skills ? json_object_get(skills, "PlayerTotalXp") : NULL;
    if (model_py_is_int(level_v) && model_py_is_int(xp_v)) {
        int64_t level = model_py_int_value(level_v);
        int64_t xp = model_py_int_value(xp_v);
        int64_t low = model_xp_for_level((int)level);
        int64_t high = model_xp_for_level((int)level + 1);
        if (!(low <= xp && xp < high)) {
            problem_list_push(out,
                "%s: level %lld needs XP in [%lld, %lld), but PlayerTotalXp is %lld",
                where, (long long)level, (long long)low, (long long)high, (long long)xp);
        }
    }
}

static void rule_xp_matches_level(const JsonValue *document, ProblemList *out) {
    for_loaded_profiles(document, visit_xp_matches_level, NULL, out);
}

static void visit_skill_points_add_up(const char *where, const JsonValue *profile,
                                       const void *ctx, ProblemList *out) {
    (void)ctx;
    const JsonValue *skills = profile_skills(profile);
    const JsonValue *level_v = skills ? json_object_get(skills, "PlayerLevel") : NULL;
    const JsonValue *points_v = skills ? json_object_get(skills, "AvailableSkillPoints")
                                        : NULL;
    int64_t spent = 0;
    const JsonValue *array = skills ? json_object_get(skills, "SkillsArray") : NULL;
    if (array && array->type == JSON_ARRAY) {
        for (size_t i = 0; i < array->as.array.count; i++) {
            const JsonValue *entry = array->as.array.items[i];
            if (entry->type == JSON_OBJECT) {
                const JsonValue *lvl = json_object_get(entry, "SkillLevel");
                if (model_py_is_int(lvl)) {
                    spent += model_py_int_value(lvl);
                }
            }
        }
    }
    if (model_py_is_int(level_v) && model_py_is_int(points_v)) {
        int64_t level = model_py_int_value(level_v);
        int64_t points = model_py_int_value(points_v);
        if (points + spent > level) {
            problem_list_push(out,
                "%s: %lld skill points available + %lld spent = %lld, more than the %lld "
                "a level-%lld character is granted",
                where, (long long)points, (long long)spent, (long long)(points + spent),
                (long long)level, (long long)level);
        }
    }
}

static void rule_skill_points_add_up(const JsonValue *document, ProblemList *out) {
    for_loaded_profiles(document, visit_skill_points_add_up, NULL, out);
}

static void visit_rank_at_least_level(const char *where, const JsonValue *profile,
                                       const void *ctx, ProblemList *out) {
    const JsonValue *rank_v = (const JsonValue *)ctx;
    const JsonValue *skills = profile_skills(profile);
    const JsonValue *level_v = skills ? json_object_get(skills, "PlayerLevel") : NULL;
    if (model_py_is_int(rank_v) && model_py_is_int(level_v)) {
        int64_t rank = model_py_int_value(rank_v);
        int64_t level = model_py_int_value(level_v);
        if (rank < level) {
            problem_list_push(out, "%s: HighestRank %lld is below PlayerLevel %lld",
                               where, (long long)rank, (long long)level);
        }
    }
}

static void rule_rank_at_least_level(const JsonValue *document, ProblemList *out) {
    const JsonValue *global = json_object_get(document, "Global");
    const JsonValue *rank_v = (global && global->type == JSON_OBJECT)
                                   ? json_object_get(global, "HighestRank") : NULL;
    for_loaded_profiles(document, visit_rank_at_least_level, rank_v, out);
}

static void visit_money_in_range(const char *where, const JsonValue *profile,
                                  const void *ctx, ProblemList *out) {
    (void)ctx;
    const JsonValue *money_v = json_object_get(profile, "Money");
    if (model_py_is_int(money_v)) {
        int64_t money = model_py_int_value(money_v);
        if (!(money >= 0 && money < ((int64_t)1 << 31))) {
            problem_list_push(out, "%s: Money %lld is outside the range a 32-bit value holds",
                               where, (long long)money);
        }
    }
}

static void rule_money_in_range(const JsonValue *document, ProblemList *out) {
    for_loaded_profiles(document, visit_money_in_range, NULL, out);
}

/* Claimed is a flattened stream of runs, not a list of objects -- see sas4_model.py's
 * _check_strongboxes docstring. */
#define CLAIMED_RUN 4

static void check_strongboxes(const char *where, const JsonValue *claimed, ProblemList *out) {
    size_t n = claimed->as.array.count;
    size_t i = 0;
    while (i < n) {
        const JsonValue *tag_v = claimed->as.array.items[i];
        const char *kind;
        if (tag_v->type == JSON_INT && tag_v->as.integer == 0) {
            kind = "weapon";
        } else if (tag_v->type == JSON_INT && tag_v->as.integer == 1) {
            kind = "equipment";
        } else if (tag_v->type == JSON_BOOL && !tag_v->as.boolean) {
            kind = "weapon"; /* Python: 0 == False, so a literal `false` tag also matches */
        } else if (tag_v->type == JSON_BOOL && tag_v->as.boolean) {
            kind = "equipment"; /* likewise 1 == True */
        } else {
            char head[24];
            json_dumps_head20(tag_v, head, sizeof(head));
            problem_list_push(out, "%s/Strongboxes/Claimed[%zu]: expected a 0 or 1 tag, found %s",
                               where, i, head);
            i += 1;
            continue;
        }
        if (i + CLAIMED_RUN > n) {
            problem_list_push(out, "%s/Strongboxes/Claimed[%zu]: %s run is cut short",
                               where, i, kind);
            break;
        }
        const JsonValue *entry = claimed->as.array.items[i + 1];
        if (entry->type != JSON_OBJECT || !json_object_get(entry, "ID")) {
            problem_list_push(out,
                "%s/Strongboxes/Claimed[%zu]: %s tag not followed by an item dict",
                where, i, kind);
        }
        i += CLAIMED_RUN;
    }
}

static void visit_strongboxes_well_formed(const char *where, const JsonValue *profile,
                                           const void *ctx, ProblemList *out) {
    (void)ctx;
    const JsonValue *boxes = json_object_get(profile, "Strongboxes");
    const JsonValue *claimed = (boxes && boxes->type == JSON_OBJECT)
                                    ? json_object_get(boxes, "Claimed") : NULL;
    if (claimed && claimed->type == JSON_ARRAY) {
        check_strongboxes(where, claimed, out);
    }
}

static void rule_strongboxes_well_formed(const JsonValue *document, ProblemList *out) {
    for_loaded_profiles(document, visit_strongboxes_well_formed, NULL, out);
}

static void rule_masteries_consistent(const JsonValue *document, ProblemList *out) {
    const JsonValue *tracks = json_object_get(document, "MasteryProgress");
    if (!tracks || tracks->type != JSON_OBJECT) {
        return;
    }
    /* Python iterates `sorted(tracks)` -- the object's keys, lexicographically, not
     * insertion order. */
    size_t n = tracks->as.object.count;
    const JsonMember **sorted_members = (const JsonMember **)malloc(n * sizeof(JsonMember *));
    for (size_t i = 0; i < n; i++) {
        sorted_members[i] = &tracks->as.object.members[i];
    }
    for (size_t a = 1; a < n; a++) {
        const JsonMember *key = sorted_members[a];
        size_t b = a;
        while (b > 0 && strcmp(sorted_members[b - 1]->key, key->key) > 0) {
            sorted_members[b] = sorted_members[b - 1];
            b--;
        }
        sorted_members[b] = key;
    }

    for (size_t t = 0; t < n; t++) {
        const char *name = sorted_members[t]->key;
        const JsonValue *rows = sorted_members[t]->value;
        if (rows->type != JSON_ARRAY) {
            continue;
        }
        for (size_t index = 0; index < rows->as.array.count; index++) {
            const JsonValue *entry = rows->as.array.items[index];
            if (entry->type != JSON_OBJECT) {
                problem_list_push(out, "MasteryProgress/%s[%zu]: not an object", name, index);
                continue;
            }
            const JsonValue *xp_v = json_object_get(entry, "MasteryXp");
            const JsonValue *level_v = json_object_get(entry, "MasteryLvl");
            /* `entry.get("MasteryXp", 0) or 0` -- an explicit None/0/falsy stored value
             * collapses to 0, same as a missing key. */
            bool xp_present_nonint_nonfalsy = xp_v && !model_py_is_int(xp_v) && model_py_truthy(xp_v);
            int64_t xp = 0;
            if (model_py_is_int(xp_v) && model_py_truthy(xp_v)) {
                xp = model_py_int_value(xp_v);
            }
            if (xp_present_nonint_nonfalsy) {
                StrBuf sb; sb_init(&sb); py_repr(&sb, xp_v);
                problem_list_push(out, "MasteryProgress/%s[%zu]: MasteryXp is %s", name, index,
                                   sb.data ? sb.data : "");
                free(sb.data);
                continue;
            }
            if (xp < 0) {
                StrBuf sb; sb_init(&sb); py_repr(&sb, xp_v);
                problem_list_push(out, "MasteryProgress/%s[%zu]: MasteryXp is %s", name, index,
                                   sb.data ? sb.data : "");
                free(sb.data);
                continue;
            }

            bool level_present_nonint_nonfalsy = level_v && !model_py_is_int(level_v) &&
                                                  model_py_truthy(level_v);
            int64_t level = 0;
            if (model_py_is_int(level_v) && model_py_truthy(level_v)) {
                level = model_py_int_value(level_v);
            }
            if (level_present_nonint_nonfalsy) {
                StrBuf sb; sb_init(&sb); py_repr(&sb, level_v);
                problem_list_push(out, "MasteryProgress/%s[%zu]: MasteryLvl is %s", name, index,
                                   sb.data ? sb.data : "");
                free(sb.data);
                continue;
            }

            if (!(level >= 0 && level <= MODEL_MASTERY_MAX_LEVEL)) {
                StrBuf sb; sb_init(&sb); py_repr(&sb, level_v);
                problem_list_push(out, "MasteryProgress/%s[%zu]: level %s is outside 0-%d",
                                   name, index, sb.data ? sb.data : "", MODEL_MASTERY_MAX_LEVEL);
                free(sb.data);
                continue;
            }
            int fits = model_mastery_level_for_xp(xp);
            if (level != fits) {
                problem_list_push(out,
                    "MasteryProgress/%s[%zu]: level %lld but %lld XP only reaches %d",
                    name, index, (long long)level, (long long)xp, fits);
            }
        }
    }
    free(sorted_members);
}

static void visit_level_positive(const char *where, const JsonValue *profile,
                                  const void *ctx, ProblemList *out) {
    (void)ctx;
    const JsonValue *skills = profile_skills(profile);
    const JsonValue *level_v = skills ? json_object_get(skills, "PlayerLevel") : NULL;
    if (model_py_is_int(level_v)) {
        int64_t level = model_py_int_value(level_v);
        if (level < 1) {
            problem_list_push(out, "%s: PlayerLevel %lld is below 1", where, (long long)level);
        }
    }
}

static void rule_level_positive(const JsonValue *document, ProblemList *out) {
    for_loaded_profiles(document, visit_level_positive, NULL, out);
}

static void visit_skill_levels_sane(const char *where, const JsonValue *profile,
                                     const void *ctx, ProblemList *out) {
    (void)ctx;
    const JsonValue *skills = profile_skills(profile);
    const JsonValue *array = skills ? json_object_get(skills, "SkillsArray") : NULL;
    if (!array || array->type != JSON_ARRAY) {
        return;
    }
    for (size_t i = 0; i < array->as.array.count; i++) {
        const JsonValue *entry = array->as.array.items[i];
        if (entry->type != JSON_OBJECT) {
            continue;
        }
        const JsonValue *lvl_v = json_object_get(entry, "SkillLevel");
        if (model_py_is_int(lvl_v)) {
            int64_t lvl = model_py_int_value(lvl_v);
            if (!(lvl >= 0 && lvl <= 20)) {
                const JsonValue *name_v = json_object_get(entry, "SkillName");
                StrBuf sb; sb_init(&sb); py_str(&sb, name_v);
                problem_list_push(out, "%s: skill %s at level %lld, outside 0..20",
                                   where, sb.data ? sb.data : "None", (long long)lvl);
                free(sb.data);
            }
        }
    }
}

static void rule_skill_levels_sane(const JsonValue *document, ProblemList *out) {
    for_loaded_profiles(document, visit_skill_levels_sane, NULL, out);
}

/* seen: EquippedSlot -> item ID, in first-seen order, both kept as JsonValue* so slots
 * compare with Python-== semantics (json_equal) rather than by their printed form -- an int
 * slot and a same-valued bool slot are one Python dict key, not two. */
typedef struct { const JsonValue *slot; const JsonValue *id; } SeenSlot;

/* json_equal requires both sides non-NULL; a missing "EquippedSlot" key (item.get() with no
 * default, i.e. Python None) comes through this module as a NULL JsonValue*, so this treats
 * NULL as JSON_NULL on both sides of the comparison before delegating. */
static bool value_equal_or_missing(const JsonValue *a, const JsonValue *b) {
    if (!a && !b) return true;
    if (!a) return b->type == JSON_NULL;
    if (!b) return a->type == JSON_NULL;
    return json_equal(a, b);
}

static void visit_no_double_equipped_slot(const char *where, const JsonValue *profile,
                                           const void *ctx, ProblemList *out) {
    (void)ctx;
    const JsonValue *equipment = json_object_get(profile, "Equipment");
    if (!equipment || equipment->type != JSON_ARRAY) {
        return;
    }
    SeenSlot *seen = NULL;
    size_t seen_count = 0, seen_cap = 0;
    for (size_t i = 0; i < equipment->as.array.count; i++) {
        const JsonValue *item = equipment->as.array.items[i];
        if (item->type != JSON_OBJECT) {
            continue;
        }
        if (!model_py_truthy(json_object_get(item, "Equipped"))) {
            continue;
        }
        const JsonValue *slot_v = json_object_get(item, "EquippedSlot");
        const JsonValue *id_v = json_object_get(item, "ID");

        size_t found = seen_count;
        for (size_t s = 0; s < seen_count; s++) {
            if (value_equal_or_missing(seen[s].slot, slot_v)) {
                found = s;
                break;
            }
        }
        if (found < seen_count) {
            StrBuf seen_id; sb_init(&seen_id); py_str(&seen_id, seen[found].id);
            StrBuf new_id; sb_init(&new_id); py_str(&new_id, id_v);
            StrBuf slot_repr; sb_init(&slot_repr); py_str(&slot_repr, slot_v);
            problem_list_push(out, "%s: items %s and %s both equipped in slot %s",
                               where, seen_id.data ? seen_id.data : "None",
                               new_id.data ? new_id.data : "None",
                               slot_repr.data ? slot_repr.data : "None");
            free(seen_id.data);
            free(new_id.data);
            free(slot_repr.data);
            continue;
        }
        if (seen_count == seen_cap) {
            seen_cap = seen_cap ? seen_cap * 2 : 4;
            seen = (SeenSlot *)realloc(seen, seen_cap * sizeof(SeenSlot));
        }
        seen[seen_count].slot = slot_v;
        seen[seen_count].id = id_v;
        seen_count++;
    }
    free(seen);
}

static void rule_no_double_equipped_slot(const JsonValue *document, ProblemList *out) {
    for_loaded_profiles(document, visit_no_double_equipped_slot, NULL, out);
}

/* --- registry --------------------------------------------------------------------------- */

const ModelRule MODEL_RULES[9] = {
    {"xp-matches-level", rule_xp_matches_level},
    {"skill-points-add-up", rule_skill_points_add_up},
    {"rank-at-least-level", rule_rank_at_least_level},
    {"money-in-range", rule_money_in_range},
    {"strongboxes-well-formed", rule_strongboxes_well_formed},
    {"level-positive", rule_level_positive},
    {"skill-levels-sane", rule_skill_levels_sane},
    {"no-double-equipped-slot", rule_no_double_equipped_slot},
    {"masteries-consistent", rule_masteries_consistent},
};
const size_t MODEL_RULES_COUNT = 9;

void model_check_subset(const JsonValue *document, const ModelRule *rules, size_t rule_count,
                         ProblemList *out) {
    ensure_ready();
    for (size_t i = 0; i < rule_count; i++) {
        rules[i].fn(document, out);
    }
}

void model_check(const JsonValue *document, ProblemList *out) {
    model_check_subset(document, MODEL_RULES, MODEL_RULES_COUNT, out);
}
