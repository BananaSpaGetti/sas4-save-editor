/*
 * Port of sas4.py's require_character/EmptySlot and level_plan -- task 11 of
 * port-to-c-sas4-core. mastery_plan and grant_plan (tasks 12/13) live here too, once built.
 */
#ifndef SAS4_PLANS_H
#define SAS4_PLANS_H

#include "edit.h"
#include "json.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A list of (path, value) writes, same shape apply_edits already takes -- built by owning
 * both the path and the value, unlike EditPlanEntry's normal borrowed-value use, since a
 * plan built here has nothing else keeping its values alive. */
typedef struct {
    EditPlanEntry *items; /* .path is malloc'd; .value is owned */
    size_t count;
    size_t cap;
} EditPlan;

void edit_plan_free(EditPlan *plan);

/* require_character(document, slot): whether `slot` holds a real character. On success,
 * *out_profile borrows the profile object from `document` and ok is true. On failure, ok is
 * false and error is filled -- empty_slot distinguishes EmptySlot (the slot exists but
 * nothing has been played in it, error carries the "pass --flag with one of those" advice)
 * from every other failure (the slot does not exist at all in this save). */
typedef struct {
    bool ok;
    bool empty_slot;
    char error[600];
} RequireCharacterResult;

RequireCharacterResult plans_require_character(const JsonValue *document, int slot,
                                                const char *flag, const JsonValue **out_profile);

/* level_plan(document, level, slot): the plan to move a character to `level`, and how many
 * skill points it already has spent (so a caller can report it, matching level_plan's own
 * (plan, spent) return). ok is false, with error/empty_slot filled the same way as
 * plans_require_character, on an out-of-range level or a slot problem -- plan is left
 * zeroed in that case. */
typedef struct {
    bool ok;
    bool empty_slot;
    char error[600];
    EditPlan plan;
    int64_t spent;
} LevelPlanResult;

LevelPlanResult plans_level_plan(const JsonValue *document, int level, int slot);
void level_plan_result_free(LevelPlanResult *result);

/* One (track index, target mastery level) pair from mastery_plan's `targets` dict. */
typedef struct {
    int index;
    int level;
} MasteryTarget;

/* mastery_plan(document, targets, slot): replaces the WHOLE MasteryProgress/MasteryProfileN
 * list in one edit (never individual entries -- see plans.c), applying `targets` in
 * ascending index order, same as Python's sorted(targets.items()). On failure (an out-of-
 * range track index or mastery level, a missing/non-list MasteryProgress/MasteryProfileN,
 * or a require_character failure), ok is false, error is filled, and plan is left zeroed. */
typedef struct {
    bool ok;
    bool empty_slot;
    char error[600];
    EditPlan plan; /* one entry: MasteryProgress/MasteryProfileN -> the whole replacement list */
} MasteryPlanResult;

MasteryPlanResult plans_mastery_plan(JsonValue *document, const MasteryTarget *targets,
                                      size_t target_count, int slot);
void mastery_plan_result_free(MasteryPlanResult *result);

/* One row of mastery_rows: the raw MasteryXp/MasteryLvl values (whatever JSON type they
 * actually are -- a hand-edited file's "3" where a count belongs shows as itself, not a
 * stand-in 0), and the mastery level that XP value supports (absent when MasteryXp, after
 * the same `x or 0` collapse mastery_plan uses, is not an int). */
typedef struct {
    size_t index;
    JsonValue *xp;    /* owned clone; any JSON type */
    JsonValue *level; /* owned clone; any JSON type */
    bool has_supported;
    int supported;
} MasteryRow;

typedef struct {
    MasteryRow *items;
    size_t count;
    size_t cap;
} MasteryRowList;

/* mastery_rows(document, slot) -- empty (never an error) when the path does not resolve or
 * is not a list, matching Python's own except-returns-[] and isinstance guard. */
void plans_mastery_rows(const JsonValue *document, int slot, MasteryRowList *out);
void plans_mastery_rows_free(MasteryRowList *list);

/* --- the item table (sas4.py's item_names()) and grant_plan -------------------------------
 *
 * Task 13. The item cache is a JSON tree of arbitrary nesting depth -- {domain: {tier:
 * {category: [item, ...]}}} in practice, but item_names() walks it structurally (any object
 * carrying both "Name" and an int-or-bool "ID" counts, whatever key path it sits under), so
 * this port does too rather than assuming the depth.
 */

typedef struct {
    int64_t id;
    char *name;     /* malloc'd */
    char *category; /* malloc'd -- the deepest object key above the item; unused by
                      * grant_plan itself, kept for a future item_catalog port */
} ItemEntry;

typedef struct {
    ItemEntry *items;
    size_t count;
    size_t cap;
} ItemTable;

typedef struct {
    ItemTable weapon;
    ItemTable equipment;
    /* How many top-level sections the cache file actually had (0 if the file is missing or
     * unreadable/corrupt) -- matches Python's `if not domains`, which is false as soon as
     * item_names() has seen ANY section, even one with zero items inside it. Sections other
     * than weapon/equipment (turret, premium) are counted here but not otherwise parsed --
     * grant_plan never reads them. */
    size_t section_count;
} ItemNames;

/* Loads item_names() from the cache file at `path`. Every domain table is empty and
 * section_count is 0 if the file is missing or fails to parse as JSON -- matching
 * item_names()'s own swallow-and-return-{} behaviour; the diagnostic it prints for a
 * present-but-corrupt file is a CLI-layer concern, not reproduced by this library call. */
void items_load(const char *path, ItemNames *out);
void items_free(ItemNames *names);

/* NULL if `id` is not in `table`, else a borrowed pointer into it. */
const ItemEntry *item_table_find(const ItemTable *table, int64_t id);

/* Adds one entry directly, bypassing items_load -- for a test standing in a small table for
 * the downloaded one (matches monkeypatching sas4._ITEM_CACHE in the Python suite), rather
 * than needing a real cache file on disk. */
void item_table_add(ItemTable *table, int64_t id, const char *name, const char *category);

/* One (item_id, kind, grade, bonus, slot) request from grant_plan's `requests` list. `kind`
 * is "auto", "weapon", "equipment", or (matching the Python's total lack of validation on
 * this string) anything else -- an arbitrary kind is carried through verbatim into the
 * "not a known %s id" message exactly the way Python's would be. Not owned; copied. */
typedef struct {
    int64_t item_id;
    const char *kind;
    int64_t grade;
    int64_t bonus;
    int64_t slot;
} GrantRequest;

typedef struct {
    bool ok;
    bool empty_slot;
    char error[600];
    EditPlan plan;    /* one entry on success */
    char **labels;    /* malloc'd array of malloc'd strings, one per request, in order */
    size_t label_count;
} GrantPlanResult;

GrantPlanResult plans_grant_plan(JsonValue *document, const ItemNames *item_names,
                                  const GrantRequest *requests, size_t request_count,
                                  int slotprofile);
void grant_plan_result_free(GrantPlanResult *result);

/* --- claimed_items / drop_claimed ---------------------------------------------------------
 *
 * The run-parsing helper task 13's own text calls for but this port did not yet have when
 * task 13 closed -- added here because task 14's TestClaimed assertions need it, the same
 * way task 11 needed model_loaded_profiles before task 10 had exported it.
 */

typedef struct {
    size_t index;   /* where the run starts in Claimed */
    char kind[16];  /* "weapon" or "equipment" */
    int64_t id;
    char *name;     /* malloc'd -- the known item name, or "id %lld" when unknown */
    int64_t grade;
    int64_t bonus;
    bool has_equipped_slot; /* false for a weapon run (no EquippedSlot key at all) */
    int64_t equipped_slot;  /* valid only when has_equipped_slot */
} ClaimedRow;

typedef struct {
    ClaimedRow *items;
    size_t count;
    size_t cap;
} ClaimedRowList;

/* claimed_items(document, slotprofile) -- empty (never an error) when the path does not
 * resolve or is not a list, matching the Python's own except-returns-[] and isinstance
 * guard. */
void plans_claimed_items(const JsonValue *document, const ItemNames *item_names,
                          int slotprofile, ClaimedRowList *out);
void plans_claimed_items_free(ClaimedRowList *list);

typedef struct {
    bool ok;
    char error[300];
    EditPlan plan; /* one entry on success */
} DropClaimedResult;

/* drop_claimed(document, indexes, slotprofile) -- removes the whole four-element run
 * starting at each index in `indexes` (silently ignoring one that is out of range or does
 * not actually start on a 0/1 tag, matching Python's own silent skips). A negative index is
 * refused rather than reproducing Python's negative-index wraparound on `claimed[start]`;
 * never exercised by a real or generate()d save, where every index comes from
 * plans_claimed_items and is never negative.
 *
 * A bad path (an unloaded or nonexistent slot) is the one place this deliberately diverges:
 * Python's drop_claimed has no try/except around its at_path() call at all (unlike
 * claimed_items, grant_plan, level_plan and mastery_plan, which all catch and convert to a
 * clean ValueError) -- a bad slotprofile there is an uncaught KeyError, not a catchable
 * error a caller's `except ValueError` would see. This port turns it into a clean ok=false
 * result instead, the same no-crash-reproduction policy used elsewhere in this file; never
 * exercised by any of this plan's callers, which only ever pass an index that
 * plans_claimed_items just read off the same document and slot. */
DropClaimedResult plans_drop_claimed(JsonValue *document, const int64_t *indexes,
                                      size_t index_count, int slotprofile);
void drop_claimed_result_free(DropClaimedResult *result);

#endif
