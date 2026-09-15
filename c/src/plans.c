/*
 * Port of sas4.py's require_character/EmptySlot and level_plan -- task 11 of
 * port-to-c-sas4-core. See sas4.py's own docstrings for the reasoning; this file only
 * reproduces the behaviour and the error wording.
 */
#include "plans.h"
#include "model.h"
#include "path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void edit_plan_free(EditPlan *plan) {
    for (size_t i = 0; i < plan->count; i++) {
        free((char *)plan->items[i].path);
        json_free(plan->items[i].value);
    }
    free(plan->items);
    plan->items = NULL;
    plan->count = 0;
    plan->cap = 0;
}

static void plan_push(EditPlan *plan, const char *path, JsonValue *value) {
    if (plan->count == plan->cap) {
        plan->cap = plan->cap ? plan->cap * 2 : 4;
        plan->items = (EditPlanEntry *)realloc(plan->items, plan->cap * sizeof(EditPlanEntry));
    }
    plan->items[plan->count].path = strdup(path);
    plan->items[plan->count].value = value;
    plan->count++;
}

/* sorted(where.split("Profile")[-1] for where, _p in loaded_profiles(document)) -- the
 * digits after "Profile" in each loaded slot's "Inventory/ProfileN" label, lexicographically
 * sorted (matching Python's sorted() on strings; single-digit slot numbers 0-9 sort the same
 * way numerically or lexicographically, and every real save has at most 6 slots). */
static void sorted_loaded_slot_labels(const JsonValue *document, char joined[512]) {
    LoadedProfileList loaded;
    model_loaded_profiles(document, &loaded);

    const char **labels = (const char **)malloc(loaded.count * sizeof(char *));
    for (size_t i = 0; i < loaded.count; i++) {
        const char *p = strstr(loaded.items[i].where, "Profile");
        labels[i] = p ? p + strlen("Profile") : loaded.items[i].where;
    }
    for (size_t a = 1; a < loaded.count; a++) {
        const char *key = labels[a];
        size_t b = a;
        while (b > 0 && strcmp(labels[b - 1], key) > 0) {
            labels[b] = labels[b - 1];
            b--;
        }
        labels[b] = key;
    }

    joined[0] = '\0';
    size_t used = 0;
    if (loaded.count == 0) {
        snprintf(joined, 512, "none");
    } else {
        for (size_t i = 0; i < loaded.count; i++) {
            int n = snprintf(joined + used, 512 - used, "%s%s", i ? ", " : "", labels[i]);
            if (n > 0) {
                used += (size_t)n;
            }
        }
    }
    free(labels);
    model_loaded_profiles_free(&loaded);
}

RequireCharacterResult plans_require_character(const JsonValue *document, int slot,
                                                const char *flag, const JsonValue **out_profile) {
    RequireCharacterResult r = {0};
    *out_profile = NULL;

    char key[32];
    snprintf(key, sizeof(key), "Profile%d", slot);

    const JsonValue *inventory = (document && document->type == JSON_OBJECT)
                                      ? json_object_get(document, "Inventory") : NULL;
    const JsonValue *profile = (inventory && inventory->type == JSON_OBJECT)
                                    ? json_object_get(inventory, key) : NULL;

    if (!profile || profile->type != JSON_OBJECT) {
        r.ok = false;
        r.empty_slot = false;
        snprintf(r.error, sizeof(r.error), "no Profile%d in this save", slot);
        return r;
    }

    if (!model_py_truthy(json_object_get(profile, "Loaded"))) {
        char labels[512];
        sorted_loaded_slot_labels(document, labels);
        r.ok = false;
        r.empty_slot = true;
        snprintf(r.error, sizeof(r.error),
                 "character slot %d is empty -- nothing has been played in it.\n"
                 "Slots holding a character: %s. Pass %s with one of those.",
                 slot, labels, flag);
        return r;
    }

    r.ok = true;
    *out_profile = profile;
    return r;
}

LevelPlanResult plans_level_plan(const JsonValue *document, int level, int slot) {
    LevelPlanResult result = {0};

    if (!(1 <= level && level <= MODEL_MAX_LEVEL)) {
        result.ok = false;
        result.empty_slot = false;
        snprintf(result.error, sizeof(result.error), "level must be between 1 and %d",
                 MODEL_MAX_LEVEL);
        return result;
    }

    char where[64];
    snprintf(where, sizeof(where), "Inventory/Profile%d", slot);

    const JsonValue *profile;
    RequireCharacterResult req = plans_require_character(document, slot, "--slot", &profile);
    if (!req.ok) {
        result.ok = false;
        result.empty_slot = req.empty_slot;
        memcpy(result.error, req.error, sizeof(result.error));
        return result;
    }

    /* skills = profile.get("Skills") or {} -- falsy (missing, None, {}, etc.) collapses to
     * an empty object, same "or default" pattern model.c's rules already replicate. */
    const JsonValue *skills_raw = json_object_get(profile, "Skills");
    const JsonValue *skills = model_py_truthy(skills_raw) ? skills_raw : NULL;

    /* skills.get("SkillsArray") or [] -- same collapse, one level down. */
    const JsonValue *array_raw = skills ? json_object_get(skills, "SkillsArray") : NULL;
    const JsonValue *array = model_py_truthy(array_raw) ? array_raw : NULL;

    int64_t spent = 0;
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

    const JsonValue *global = (document && document->type == JSON_OBJECT)
                                   ? json_object_get(document, "Global") : NULL;
    const JsonValue *rank_v = (global && global->type == JSON_OBJECT)
                                   ? json_object_get(global, "HighestRank") : NULL;
    int64_t rank = model_py_is_int(rank_v) ? model_py_int_value(rank_v) : 0;

    int64_t available = level - spent;
    if (available < 0) {
        available = 0;
    }
    int64_t new_rank = level > rank ? level : rank;

    EditPlan plan = {0};
    char path[128];

    snprintf(path, sizeof(path), "%s/Skills/PlayerLevel", where);
    plan_push(&plan, path, json_new_int(level));

    snprintf(path, sizeof(path), "%s/Skills/PlayerTotalXp", where);
    plan_push(&plan, path, json_new_int(model_xp_for_level(level)));

    snprintf(path, sizeof(path), "%s/Skills/AvailableSkillPoints", where);
    plan_push(&plan, path, json_new_int(available));

    plan_push(&plan, "Global/HighestRank", json_new_int(new_rank));

    result.ok = true;
    result.empty_slot = false;
    result.plan = plan;
    result.spent = spent;
    return result;
}

void level_plan_result_free(LevelPlanResult *result) {
    edit_plan_free(&result->plan);
}

static int mastery_target_cmp(const void *a, const void *b) {
    int ia = ((const MasteryTarget *)a)->index;
    int ib = ((const MasteryTarget *)b)->index;
    return (ia > ib) - (ia < ib);
}

MasteryPlanResult plans_mastery_plan(JsonValue *document, const MasteryTarget *targets,
                                      size_t target_count, int slot) {
    MasteryPlanResult result = {0};

    /* mastery_plan calls require_character(document, slot) with no third argument, so the
     * empty-slot advice names "--slot" -- unlike grant_plan (task 13), which passes
     * "--slotprofile" explicitly. */
    const JsonValue *profile;
    RequireCharacterResult req = plans_require_character(document, slot, "--slot", &profile);
    if (!req.ok) {
        result.ok = false;
        result.empty_slot = req.empty_slot;
        memcpy(result.error, req.error, sizeof(result.error));
        return result;
    }

    char mastery_path[64];
    snprintf(mastery_path, sizeof(mastery_path), "MasteryProgress/MasteryProfile%d", slot);

    /* at_path's failure modes (a missing key, a non-subscriptable node) all collapse to one
     * ValueError in the Python; path_at's own ok=false already covers exactly that set
     * (task 7's own verification). */
    PathAtResult at = path_at(document, mastery_path);
    if (!at.ok) {
        result.ok = false;
        result.empty_slot = false;
        snprintf(result.error, sizeof(result.error),
                 "no MasteryProgress/MasteryProfile%d in this save", slot);
        return result;
    }
    if (at.value->type != JSON_ARRAY) {
        result.ok = false;
        result.empty_slot = false;
        snprintf(result.error, sizeof(result.error),
                 "MasteryProgress/MasteryProfile%d is not a list", slot);
        return result;
    }

    size_t n = at.value->as.array.count;
    JsonValue *updated = json_new_array();
    for (size_t i = 0; i < n; i++) {
        const JsonValue *entry = at.value->as.array.items[i];
        if (entry->type == JSON_OBJECT) {
            json_array_push(updated, json_clone(entry));
        } else {
            JsonValue *fresh = json_new_object();
            json_object_set(fresh, "MasteryXp", strlen("MasteryXp"), json_new_int(0));
            json_object_set(fresh, "MasteryLvl", strlen("MasteryLvl"), json_new_int(0));
            json_array_push(updated, fresh);
        }
    }

    MasteryTarget *sorted_targets = NULL;
    if (target_count > 0) {
        sorted_targets = (MasteryTarget *)malloc(target_count * sizeof(MasteryTarget));
        memcpy(sorted_targets, targets, target_count * sizeof(MasteryTarget));
        qsort(sorted_targets, target_count, sizeof(MasteryTarget), mastery_target_cmp);
    }

    for (size_t i = 0; i < target_count; i++) {
        int index = sorted_targets[i].index;
        int level = sorted_targets[i].level;
        if (!(0 <= index && (size_t)index < n)) {
            result.ok = false;
            result.empty_slot = false;
            snprintf(result.error, sizeof(result.error),
                     "track %d is out of range; this save has %zu of them", index, n);
            json_free(updated);
            free(sorted_targets);
            return result;
        }
        if (!(0 <= level && level <= MODEL_MASTERY_MAX_LEVEL)) {
            result.ok = false;
            result.empty_slot = false;
            snprintf(result.error, sizeof(result.error),
                     "mastery level %d is out of range (0-%d)", level, MODEL_MASTERY_MAX_LEVEL);
            json_free(updated);
            free(sorted_targets);
            return result;
        }
        JsonValue *replacement = json_new_object();
        json_object_set(replacement, "MasteryXp", strlen("MasteryXp"),
                         json_new_int(MODEL_MASTERY_LEVEL_XP[level]));
        json_object_set(replacement, "MasteryLvl", strlen("MasteryLvl"), json_new_int(level));
        json_free(updated->as.array.items[index]);
        updated->as.array.items[index] = replacement;
    }
    free(sorted_targets);

    EditPlan plan = {0};
    plan_push(&plan, mastery_path, updated);

    result.ok = true;
    result.empty_slot = false;
    result.plan = plan;
    return result;
}

void mastery_plan_result_free(MasteryPlanResult *result) {
    edit_plan_free(&result->plan);
}

void plans_mastery_rows(const JsonValue *document, int slot, MasteryRowList *out) {
    out->items = NULL;
    out->count = 0;
    out->cap = 0;

    char path[64];
    snprintf(path, sizeof(path), "MasteryProgress/MasteryProfile%d", slot);
    PathAtResult at = path_at((JsonValue *)document, path);
    if (!at.ok || at.value->type != JSON_ARRAY) {
        return;
    }

    for (size_t index = 0; index < at.value->as.array.count; index++) {
        const JsonValue *entry = at.value->as.array.items[index];
        if (entry->type != JSON_OBJECT) {
            continue;
        }
        const JsonValue *xp_raw = json_object_get(entry, "MasteryXp");
        const JsonValue *level_raw = json_object_get(entry, "MasteryLvl");
        /* `entry.get(key, 0) or 0` -- falsy (missing, None, 0, "", ...) collapses to the
         * literal int 0; a truthy value of any type passes through unchanged. */
        JsonValue *xp = model_py_truthy(xp_raw) ? json_clone(xp_raw) : json_new_int(0);
        JsonValue *level = model_py_truthy(level_raw) ? json_clone(level_raw) : json_new_int(0);

        bool has_supported = model_py_is_int(xp);
        int supported = has_supported ? model_mastery_level_for_xp(model_py_int_value(xp)) : 0;

        if (out->count == out->cap) {
            out->cap = out->cap ? out->cap * 2 : 32;
            out->items = (MasteryRow *)realloc(out->items, out->cap * sizeof(MasteryRow));
        }
        MasteryRow *row = &out->items[out->count++];
        row->index = index;
        row->xp = xp;
        row->level = level;
        row->has_supported = has_supported;
        row->supported = supported;
    }
}

void plans_mastery_rows_free(MasteryRowList *list) {
    for (size_t i = 0; i < list->count; i++) {
        json_free(list->items[i].xp);
        json_free(list->items[i].level);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

/* --- item table -------------------------------------------------------------------------- */

void item_table_add(ItemTable *table, int64_t id, const char *name, const char *category) {
    if (table->count == table->cap) {
        table->cap = table->cap ? table->cap * 2 : 16;
        table->items = (ItemEntry *)realloc(table->items, table->cap * sizeof(ItemEntry));
    }
    table->items[table->count].id = id;
    table->items[table->count].name = strdup(name);
    table->items[table->count].category = strdup(category);
    table->count++;
}

const ItemEntry *item_table_find(const ItemTable *table, int64_t id) {
    for (size_t i = 0; i < table->count; i++) {
        if (table->items[i].id == id) {
            return &table->items[i];
        }
    }
    return NULL;
}

static void item_table_free(ItemTable *table) {
    for (size_t i = 0; i < table->count; i++) {
        free(table->items[i].name);
        free(table->items[i].category);
    }
    free(table->items);
    table->items = NULL;
    table->count = 0;
    table->cap = 0;
}

/* Matches item_names()'s walk(): any object carrying "Name" (by key presence, whatever its
 * value) and an int-or-bool "ID" is an item; table.setdefault means the FIRST occurrence of
 * an id within a domain wins, matching document order (this module's JSON objects already
 * preserve insertion order). Everything else recurses: an object's values by key (the key
 * becomes the new category), an array's elements (category unchanged). */
static void item_walk(const JsonValue *node, const char *category, ItemTable *table) {
    if (node->type == JSON_OBJECT) {
        const JsonValue *name_v = json_object_get(node, "Name");
        const JsonValue *id_v = json_object_get(node, "ID");
        if (name_v && id_v && model_py_is_int(id_v)) {
            int64_t id = model_py_int_value(id_v);
            if (!item_table_find(table, id)) {
                /* Measured: every "Name" in the real 37KB item cache is a JSON string:
                 * assumed here rather than reproducing Python's str()-of-anything -- the
                 * rare non-string case falls back to an empty name instead. */
                const char *name = (name_v->type == JSON_STRING) ? name_v->as.string.data : "";
                item_table_add(table, id, name, category);
            }
            return;
        }
        for (size_t i = 0; i < node->as.object.count; i++) {
            const JsonMember *m = &node->as.object.members[i];
            item_walk(m->value, m->key, table);
        }
    } else if (node->type == JSON_ARRAY) {
        for (size_t i = 0; i < node->as.array.count; i++) {
            item_walk(node->as.array.items[i], category, table);
        }
    }
}

void items_load(const char *path, ItemNames *out) {
    memset(out, 0, sizeof(*out));

    FILE *f = fopen(path, "rb");
    if (!f) {
        return; /* matches `if not os.path.exists(ITEMS_CACHE): return {}` */
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)size);
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        free(buf);
        return;
    }

    JsonParseResult r = json_parse(buf, (size_t)size);
    free(buf);
    if (!r.value) {
        return; /* matches the except (OSError, ValueError) branch -- {} either way */
    }
    if (r.value->type != JSON_OBJECT) {
        /* Python's document.items() would raise AttributeError here, uncaught -- a defect
         * in the reference this port does not reproduce a crash for (same policy as
         * model.c's profile_skills). Treated as "no sections". */
        json_free(r.value);
        return;
    }

    for (size_t i = 0; i < r.value->as.object.count; i++) {
        const JsonMember *section = &r.value->as.object.members[i];
        out->section_count++;

        /* domain = section.replace("_info", "") -- the real cache's four section names
         * (weapon_info, equipment_info, turret_info, premium_info) only ever carry "_info"
         * as a trailing suffix, so a suffix-strip matches Python's full replace() for every
         * key this file actually has. */
        char domain[64];
        snprintf(domain, sizeof(domain), "%.*s", (int)section->key_len, section->key);
        static const char SUFFIX[] = "_info";
        size_t domain_len = strlen(domain);
        size_t suffix_len = strlen(SUFFIX);
        if (domain_len >= suffix_len &&
            strcmp(domain + domain_len - suffix_len, SUFFIX) == 0) {
            domain[domain_len - suffix_len] = '\0';
        }

        if (strcmp(domain, "weapon") == 0) {
            item_walk(section->value, domain, &out->weapon);
        } else if (strcmp(domain, "equipment") == 0) {
            item_walk(section->value, domain, &out->equipment);
        }
        /* turret / premium are counted (section_count) but not parsed -- grant_plan never
         * reads either domain. */
    }
    json_free(r.value);
}

void items_free(ItemNames *names) {
    item_table_free(&names->weapon);
    item_table_free(&names->equipment);
    names->section_count = 0;
}

/* --- weapon_entry / equipment_entry (sas4.py's, not sas4_model.py's near-namesakes) ------- */

/* Both build a four-element Claimed run: a bare tag, the item dict, then CLAIMED_TAIL
 * [8, 0]. Key order matches the Python dict literal, so a replacement reads the way the
 * rest of the file does. */
static JsonValue *build_weapon_entry(int64_t item_id, int64_t grade, int64_t bonus) {
    JsonValue *run = json_new_array();
    json_array_push(run, json_new_int(0));
    JsonValue *item = json_new_object();
    json_object_set(item, "ID", 2, json_new_int(item_id));
    json_object_set(item, "EquipVersion", strlen("EquipVersion"), json_new_int(0));
    json_object_set(item, "Grade", strlen("Grade"), json_new_int(grade));
    json_object_set(item, "AugmentSlots", strlen("AugmentSlots"), json_new_int(0));
    json_object_set(item, "BonusStatsLevel", strlen("BonusStatsLevel"), json_new_int(bonus));
    json_object_set(item, "InventoryIndex", strlen("InventoryIndex"), json_new_int(0));
    json_array_push(run, item);
    json_array_push(run, json_new_int(8));
    json_array_push(run, json_new_int(0));
    return run;
}

static JsonValue *build_equipment_entry(int64_t item_id, int64_t slot, int64_t grade,
                                         int64_t bonus) {
    JsonValue *run = json_new_array();
    json_array_push(run, json_new_int(1));
    JsonValue *item = json_new_object();
    json_object_set(item, "ID", 2, json_new_int(item_id));
    json_object_set(item, "EquipVersion", strlen("EquipVersion"), json_new_int(0));
    json_object_set(item, "Grade", strlen("Grade"), json_new_int(grade));
    json_object_set(item, "AugmentSlots", strlen("AugmentSlots"), json_new_int(0));
    json_object_set(item, "BonusStatsLevel", strlen("BonusStatsLevel"), json_new_int(bonus));
    json_object_set(item, "EquippedSlot", strlen("EquippedSlot"), json_new_int(slot));
    json_object_set(item, "InventoryIndex", strlen("InventoryIndex"), json_new_int(slot));
    json_array_push(run, item);
    json_array_push(run, json_new_int(8));
    json_array_push(run, json_new_int(0));
    return run;
}

/* Moves every element out of `run` (a 4-element array from build_weapon_entry/
 * build_equipment_entry) onto the end of `added`, then frees the now-empty shell -- matches
 * Python's `added.extend(weapon_entry(...))`, which flattens the run into the flat Claimed
 * list rather than nesting it. */
static void extend_with_run(JsonValue *added, JsonValue *run) {
    for (size_t i = 0; i < run->as.array.count; i++) {
        json_array_push(added, run->as.array.items[i]);
    }
    free(run->as.array.items); /* elements themselves were moved, not freed */
    free(run);
}

static char **labels_push(char **labels, size_t *count, size_t *cap, const char *text) {
    if (*count == *cap) {
        *cap = *cap ? *cap * 2 : 4;
        labels = (char **)realloc(labels, (*cap) * sizeof(char *));
    }
    labels[(*count)++] = strdup(text);
    return labels;
}

GrantPlanResult plans_grant_plan(JsonValue *document, const ItemNames *item_names,
                                  const GrantRequest *requests, size_t request_count,
                                  int slotprofile) {
    GrantPlanResult result = {0};

    if (item_names->section_count == 0) {
        result.ok = false;
        result.empty_slot = false;
        snprintf(result.error, sizeof(result.error),
                 "run `items` first so IDs can be resolved to weapon vs equipment");
        return result;
    }

    const JsonValue *profile;
    RequireCharacterResult req = plans_require_character(document, slotprofile, "--slotprofile",
                                                           &profile);
    if (!req.ok) {
        result.ok = false;
        result.empty_slot = req.empty_slot;
        memcpy(result.error, req.error, sizeof(result.error));
        return result;
    }

    char path[64];
    snprintf(path, sizeof(path), "Inventory/Profile%d/Strongboxes/Claimed", slotprofile);
    PathAtResult at = path_at(document, path);
    if (!at.ok) {
        result.ok = false;
        result.empty_slot = false;
        snprintf(result.error, sizeof(result.error), "no Strongboxes/Claimed in Profile%d",
                 slotprofile);
        return result;
    }
    if (at.value->type != JSON_ARRAY) {
        /* Python has no such guard here -- `claimed + added` would raise an uncaught
         * TypeError, not a ValueError. A defect in the reference this port turns into a
         * clean error instead of a crash (same policy as elsewhere in this port); never
         * exercised by a real or generate()d save, where Claimed is always a list. */
        result.ok = false;
        result.empty_slot = false;
        snprintf(result.error, sizeof(result.error),
                 "Strongboxes/Claimed in Profile%d is not a list", slotprofile);
        return result;
    }

    JsonValue *added = json_new_array();
    char **labels = NULL;
    size_t label_count = 0, label_cap = 0;

    for (size_t i = 0; i < request_count; i++) {
        const GrantRequest *rq = &requests[i];
        const ItemEntry *weapon_hit = item_table_find(&item_names->weapon, rq->item_id);
        const ItemEntry *equip_hit = item_table_find(&item_names->equipment, rq->item_id);
        bool in_weapon = weapon_hit != NULL;
        bool in_equip = equip_hit != NULL;

        const char *effective_kind = rq->kind;
        bool auto_found_none = false;
        if (strcmp(rq->kind, "auto") == 0) {
            if (in_weapon && in_equip) {
                result.ok = false;
                result.empty_slot = false;
                snprintf(result.error, sizeof(result.error),
                         "id %lld is both a weapon (%s) and equipment (%s) -- say which",
                         (long long)rq->item_id, weapon_hit->name, equip_hit->name);
                json_free(added);
                for (size_t j = 0; j < label_count; j++) free(labels[j]);
                free(labels);
                return result;
            }
            if (in_weapon) {
                effective_kind = "weapon";
            } else if (in_equip) {
                effective_kind = "equipment";
            } else {
                effective_kind = NULL;
                auto_found_none = true;
            }
        }

        if (!auto_found_none && effective_kind && strcmp(effective_kind, "weapon") == 0 &&
            in_weapon) {
            JsonValue *run = build_weapon_entry(rq->item_id, rq->grade, rq->bonus);
            extend_with_run(added, run);
            char label[300];
            snprintf(label, sizeof(label), "%s (weapon)", weapon_hit->name);
            labels = labels_push(labels, &label_count, &label_cap, label);
        } else if (!auto_found_none && effective_kind && strcmp(effective_kind, "equipment") == 0 &&
                   in_equip) {
            JsonValue *run = build_equipment_entry(rq->item_id, rq->slot, rq->grade, rq->bonus);
            extend_with_run(added, run);
            char label[300];
            snprintf(label, sizeof(label), "%s (equipment, slot %lld)", equip_hit->name,
                     (long long)rq->slot);
            labels = labels_push(labels, &label_count, &label_cap, label);
        } else {
            result.ok = false;
            result.empty_slot = false;
            snprintf(result.error, sizeof(result.error), "id %lld is not a known %s id",
                     (long long)rq->item_id,
                     effective_kind ? effective_kind : "weapon or equipment");
            json_free(added);
            for (size_t j = 0; j < label_count; j++) free(labels[j]);
            free(labels);
            return result;
        }
    }

    JsonValue *final_claimed = json_new_array();
    for (size_t i = 0; i < at.value->as.array.count; i++) {
        json_array_push(final_claimed, json_clone(at.value->as.array.items[i]));
    }
    for (size_t i = 0; i < added->as.array.count; i++) {
        json_array_push(final_claimed, added->as.array.items[i]);
    }
    free(added->as.array.items);
    free(added);

    EditPlan plan = {0};
    plan_push(&plan, path, final_claimed);

    result.ok = true;
    result.empty_slot = false;
    result.plan = plan;
    result.labels = labels;
    result.label_count = label_count;
    return result;
}

void grant_plan_result_free(GrantPlanResult *result) {
    edit_plan_free(&result->plan);
    for (size_t i = 0; i < result->label_count; i++) {
        free(result->labels[i]);
    }
    free(result->labels);
    result->labels = NULL;
    result->label_count = 0;
}

/* --- claimed_items / drop_claimed --------------------------------------------------------- */

#define CLAIMED_RUN 4

static bool tag_is(const JsonValue *v, int want) {
    if (v->type == JSON_INT) {
        return v->as.integer == want;
    }
    if (v->type == JSON_BOOL) {
        return (v->as.boolean ? 1 : 0) == want;
    }
    return false;
}

/* claimed[start] not in (0, 1) -- True/False are both members of that set (True==1,
 * False==0), a float 0.0/1.0 would be too (never appears in this format, Decision 6). */
static bool tag_is_0_or_1(const JsonValue *v) {
    return tag_is(v, 0) || tag_is(v, 1);
}

static int64_t object_get_int_or(const JsonValue *object, const char *key, int64_t fallback) {
    const JsonValue *v = json_object_get(object, key);
    if (!v) {
        return fallback;
    }
    /* entry.get(key, 0) only substitutes the default when the key is ABSENT -- a present
     * but non-int value would be carried through verbatim in Python (Grade/BonusStatsLevel
     * are always ints in every real and generate()d entry, so this falls back to `fallback`
     * for that case instead, a documented divergence never actually exercised). */
    return model_py_is_int(v) ? model_py_int_value(v) : fallback;
}

static void claimed_row_push(ClaimedRowList *out, size_t index, const char *kind, int64_t id,
                              const char *name, int64_t grade, int64_t bonus,
                              bool has_slot, int64_t slot) {
    if (out->count == out->cap) {
        out->cap = out->cap ? out->cap * 2 : 8;
        out->items = (ClaimedRow *)realloc(out->items, out->cap * sizeof(ClaimedRow));
    }
    ClaimedRow *row = &out->items[out->count++];
    row->index = index;
    snprintf(row->kind, sizeof(row->kind), "%s", kind);
    row->id = id;
    row->name = strdup(name);
    row->grade = grade;
    row->bonus = bonus;
    row->has_equipped_slot = has_slot;
    row->equipped_slot = slot;
}

void plans_claimed_items(const JsonValue *document, const ItemNames *item_names,
                          int slotprofile, ClaimedRowList *out) {
    out->items = NULL;
    out->count = 0;
    out->cap = 0;

    char path[64];
    snprintf(path, sizeof(path), "Inventory/Profile%d/Strongboxes/Claimed", slotprofile);
    PathAtResult at = path_at((JsonValue *)document, path);
    if (!at.ok || at.value->type != JSON_ARRAY) {
        return; /* matches the except-returns-[] and the isinstance(claimed, list) guard */
    }

    const JsonValue *claimed = at.value;
    size_t n = claimed->as.array.count;
    size_t i = 0;
    while (i < n) {
        const JsonValue *tag_v = claimed->as.array.items[i];
        const char *kind;
        if (tag_is(tag_v, 0)) {
            kind = "weapon";
        } else if (tag_is(tag_v, 1)) {
            kind = "equipment";
        } else {
            i += 1;
            continue;
        }
        if (i + CLAIMED_RUN > n) {
            break;
        }
        const JsonValue *entry = claimed->as.array.items[i + 1];
        const JsonValue *id_v = (entry->type == JSON_OBJECT) ? json_object_get(entry, "ID")
                                                              : NULL;
        if (entry->type == JSON_OBJECT && model_py_is_int(id_v)) {
            int64_t id = model_py_int_value(id_v);
            const ItemTable *table = (strcmp(kind, "weapon") == 0) ? &item_names->weapon
                                                                    : &item_names->equipment;
            const ItemEntry *known = item_table_find(table, id);
            char name_buf[300];
            if (known) {
                snprintf(name_buf, sizeof(name_buf), "%s", known->name);
            } else {
                snprintf(name_buf, sizeof(name_buf), "id %lld", (long long)id);
            }
            int64_t grade = object_get_int_or(entry, "Grade", 0);
            int64_t bonus = object_get_int_or(entry, "BonusStatsLevel", 0);
            const JsonValue *slot_v = json_object_get(entry, "EquippedSlot");
            bool has_slot = slot_v != NULL;
            int64_t slot = (has_slot && model_py_is_int(slot_v)) ? model_py_int_value(slot_v) : 0;
            claimed_row_push(out, i, kind, id, name_buf, grade, bonus, has_slot, slot);
        }
        i += CLAIMED_RUN;
    }
}

void plans_claimed_items_free(ClaimedRowList *list) {
    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i].name);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

DropClaimedResult plans_drop_claimed(JsonValue *document, const int64_t *indexes,
                                      size_t index_count, int slotprofile) {
    DropClaimedResult result = {0};

    char path[64];
    snprintf(path, sizeof(path), "Inventory/Profile%d/Strongboxes/Claimed", slotprofile);
    PathAtResult at = path_at(document, path);
    if (!at.ok) {
        result.ok = false;
        snprintf(result.error, sizeof(result.error), "no Strongboxes/Claimed in Profile%d",
                 slotprofile);
        return result;
    }
    if (at.value->type != JSON_ARRAY) {
        /* Python has no such guard -- len()/enumerate() on a non-list raises an uncaught
         * TypeError. Same documented divergence as elsewhere in this port; unexercised. */
        result.ok = false;
        snprintf(result.error, sizeof(result.error),
                 "Strongboxes/Claimed in Profile%d is not a list", slotprofile);
        return result;
    }

    size_t n = at.value->as.array.count;
    bool *doomed = (bool *)calloc(n ? n : 1, sizeof(bool));
    for (size_t k = 0; k < index_count; k++) {
        int64_t start = indexes[k];
        if (start < 0 || (size_t)start >= n) {
            continue;
        }
        if (!tag_is_0_or_1(at.value->as.array.items[start])) {
            continue;
        }
        size_t end = (size_t)start + CLAIMED_RUN;
        if (end > n) {
            end = n;
        }
        for (size_t j = (size_t)start; j < end; j++) {
            doomed[j] = true;
        }
    }

    JsonValue *kept = json_new_array();
    for (size_t i = 0; i < n; i++) {
        if (!doomed[i]) {
            json_array_push(kept, json_clone(at.value->as.array.items[i]));
        }
    }
    free(doomed);

    EditPlan plan = {0};
    plan_push(&plan, path, kept);

    result.ok = true;
    result.plan = plan;
    return result;
}

void drop_claimed_result_free(DropClaimedResult *result) {
    edit_plan_free(&result->plan);
}
