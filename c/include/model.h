/*
 * Port of sas4_model.py's XP curve, level/mastery constants, and the check() registry --
 * task 10 of port-to-c-sas4-core. See sas4_model.py's module docstring and the comments on
 * xp_per_level/MASTERY_LEVEL_XP there for where every number came from; this file only
 * reproduces the arithmetic and the rule wording, not the reasoning.
 */
#ifndef SAS4_MODEL_H
#define SAS4_MODEL_H

#include "json.h"

#include <stddef.h>
#include <stdint.h>

#define MODEL_MAX_LEVEL 100
#define MODEL_MASTERY_SLOTS 27
#define MODEL_MASTERY_MAX_LEVEL 5

/* Index i is the XP needed to reach mastery level i from 0 (cumulative), matching Python's
 * MASTERY_LEVEL_XP list. */
extern const int64_t MODEL_MASTERY_LEVEL_XP[6];

/* XP needed to go from `level` to `level` + 1. Matches xp_per_level exactly, including its
 * integer (floor) division -- see the .c file for why truncation and floor agree here. */
int64_t model_xp_per_level(int level);

/* Cumulative XP to reach `level`. Matches xp_for_level: 0 for level < 0, a precomputed table
 * through level 108, computed on demand past that. */
int64_t model_xp_for_level(int level);

/* The highest mastery level `xp` reaches, per mastery_level_for_xp. */
int model_mastery_level_for_xp(int64_t xp);

/* What each track is, where that has actually been established by direct measurement (see
 * sas4_model.py's MASTERY_TRACKS comment) -- index 0 is pistols, index 9 is high damage
 * ammo. The other 25 stay unnamed rather than guessed, since a wrong name here would send
 * an edit to the wrong track. */
typedef struct {
    int index;
    const char *name;
} MasteryTrackName;

extern const MasteryTrackName MODEL_MASTERY_TRACKS[2];
extern const size_t MODEL_MASTERY_TRACKS_COUNT;

/* What track `index` is, or NULL when it has not been established. */
const char *model_mastery_name(int index);

/* Python-semantics primitives, shared with plans.c: Python's bool is an int subclass, so
 * isinstance(x, int) is True for a JSON true/false too (model_py_is_int), and its numeric
 * value is 0/1 (model_py_int_value, only meaningful when model_py_is_int is true).
 * model_py_truthy matches Python's general truthiness (None/False/0/0.0/""/[]/{} are
 * falsy, a missing value -- NULL here -- is treated as None). */
bool model_py_is_int(const JsonValue *v);
int64_t model_py_int_value(const JsonValue *v);
bool model_py_truthy(const JsonValue *v);

/* Python's repr() and str() of a JSON-representable value (None/bool/int/float/str/list/
 * dict), matching CPython's quote-preference rule for strings, \n/\r/\t/other-control-byte
 * escaping, and (model_py_str only) that a bare top-level string is emitted raw/unquoted
 * where repr() would quote it -- exactly what a bare `"%s" % value`-style format spec calls,
 * as opposed to `"%r"`. Malloc'd, NUL-terminated; caller frees. */
/* Python's int(text) for a decimal string: optional surrounding ASCII whitespace, an
 * optional sign, then digits with optional single underscores between them (PEP 515).
 * Returns false for anything else. Shared because both the argument parser (argparse's
 * type=int) and `mastery --set`'s "3=5" pieces go through Python's int() and must accept
 * exactly the same strings -- notably "3 " with trailing space, which int() accepts and a
 * bare strtol-plus-end-check does not. */
bool model_py_parse_int(const char *text, long *out);

char *model_py_repr(const JsonValue *v);
char *model_py_str(const JsonValue *v);

/* document.get("Inventory", {}).items(), keeping only the dict-typed, Loaded-truthy ones --
 * matches sas4_model.py's loaded_profiles generator. `where` is "Inventory/<key>";
 * `profile` borrows from `document` (do not free, do not outlive it). */
typedef struct {
    char where[300];
    const JsonValue *profile;
} LoadedProfile;

typedef struct {
    LoadedProfile *items;
    size_t count;
    size_t cap;
} LoadedProfileList;

void model_loaded_profiles(const JsonValue *document, LoadedProfileList *out);
void model_loaded_profiles_free(LoadedProfileList *list);

/* Runs an assertion-style cross-check of xp_per_level/xp_for_level against the 26 observed
 * XP_TABLE values sas4_model.py ships (see model.c), matching the Python's import-time
 * assert. Aborts the process (via assert()) if the formula and the table ever disagree.
 * Called automatically before any of the above run; exposed so a test or a CLI's startup
 * can call it explicitly too. */
void model_cross_check(void);

/* A dynamically-grown list of problem strings, same shape as edit.h's PendingList. */
typedef struct {
    char **items; /* malloc'd C strings, one per problem found */
    size_t count;
    size_t cap;
} ProblemList;

void model_problem_list_free(ProblemList *list);

typedef void (*ModelRuleFn)(const JsonValue *document, ProblemList *out);

typedef struct {
    const char *name;
    ModelRuleFn fn;
} ModelRule;

/* The nine rules, in the Python RULES list's order -- order matters for a --rules-count
 * subset (not exposed by this port's CLI yet, but check()'s own contract depends on it). */
extern const ModelRule MODEL_RULES[9];
extern const size_t MODEL_RULES_COUNT;

/* Every internal inconsistency `document` has, appended to `out` (which must start zeroed
 * or already-initialized; call model_problem_list_free first if reusing). Runs all nine
 * rules in order, matching check(document, rules=None). */
void model_check(const JsonValue *document, ProblemList *out);

/* Runs only `rules[0..rule_count)` -- matches check(document, rules=RULES[:n]) for redteam's
 * progression mode. */
void model_check_subset(const JsonValue *document, const ModelRule *rules, size_t rule_count,
                         ProblemList *out);

#endif
