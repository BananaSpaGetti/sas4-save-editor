/*
 * Path navigation over a JsonValue tree -- port of sas4.py's at_path, parent_of, scalars,
 * coerce and the kind_of/Counter machinery cmd_kinds uses. A path is slash-separated
 * ("Inventory/Profile0/Money"), and any segment may end with one or more "[N]" index
 * suffixes ("Strongboxes/Claimed[3]").
 */
#ifndef SAS4_PATH_H
#define SAS4_PATH_H

#include "json.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- at_path -------------------------------------------------------------------------- */

typedef struct {
    bool ok;
    JsonValue *value;   /* points into the document; not owned, do not free */
    char error[192];
} PathAtResult;

PathAtResult path_at(JsonValue *document, const char *path);

/* --- parent_of ------------------------------------------------------------------------ */

typedef struct {
    bool ok;
    JsonValue *parent;      /* the container (object or array); not owned */
    bool is_index;          /* true: key_index is the field to use; false: key_str is */
    char key_str[256];
    int64_t key_index;
    char error[192];
} PathParentResult;

PathParentResult path_parent_of(JsonValue *document, const char *path);

/* --- scalars ---------------------------------------------------------------------------- */

typedef struct {
    char *path;         /* malloc'd */
    JsonValue *value;    /* points into the document; not owned */
} ScalarEntry;

typedef struct {
    ScalarEntry *items;
    size_t count;
    size_t cap;
} ScalarList;

/* Every scalar (non-object, non-array leaf) in `node`, in document order, with its path
 * relative to `node` (path_prefix is prepended to every entry -- pass "" at the top). */
void path_scalars(JsonValue *node, const char *path_prefix, ScalarList *out);
void path_scalars_free(ScalarList *list);

/* --- kind_of / kinds tally --------------------------------------------------------------- */

/* "bool", "int", "str", "float", "null", or "other" (only reachable for an object/array,
 * which scalars() never yields, but kind_of accepts any JsonValue for completeness). */
const char *path_kind_of(const JsonValue *value);

typedef struct {
    char name[8];
    size_t count;
} KindCount;

/* Tallies path_kind_of() over every entry in `scalars`, sorted by count descending with
 * ties broken by first-seen order -- matching collections.Counter.most_common() exactly
 * (Python's sort is stable, and Counter iterates in insertion order for equal counts).
 * Allocates *out via malloc (caller frees). */
void path_kind_tally(const ScalarList *scalars, KindCount **out, size_t *out_count);

/* --- coerce --------------------------------------------------------------------------- */

typedef struct {
    bool ok;
    JsonValue *value;   /* newly allocated on success; caller owns (json_free) */
    char error[192];
} CoerceResult;

/* Reads `text` as whatever kind `current` is -- bool needs "true"/"false" (case-
 * insensitive); int uses the same base-0 auto-detection as Python's int(text, 0) (a "0x"/
 * "0X" prefix is hex, "0o"/"0O" octal, "0b"/"0B" binary, a bare "0" or "-0" is zero, and
 * anything else with a leading zero is rejected, matching Python's own leading-zero
 * restriction); float uses strtod; str is copied as-is; anything else (object, array,
 * null) is refused -- matches sas4.py's coerce() exactly, including its exact error text
 * for the shapes this port tests against. */
CoerceResult path_coerce(const char *text, const JsonValue *current);

#endif
