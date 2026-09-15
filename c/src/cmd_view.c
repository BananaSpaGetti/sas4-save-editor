/*
 * `view` -- port of sas4.py's cmd_view, line() and describe(). Task 3 of the
 * port-to-c-sas4-cli plan.
 */
#include "cmd_view.h"
#include "dgdata.h"
#include "json.h"
#include "model.h"
#include "plans.h"
#include "sas4load.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- small formatting helpers, matching sas4.py's line()/describe() ---------------------- */

static void view_line(const char *label, const char *value, int indent) {
    printf("%*s%-30s %s\n", indent, "", label, value);
}

/* obj.get(key): NULL for a missing key, and (unlike Python, which would raise) also NULL if
 * obj itself is not an object at all -- no real save ever puts the wrong shape here; see
 * sas4load.h's note on this port's no-crash-reproduction policy. */
static const JsonValue *jget(const JsonValue *obj, const char *key) {
    if (!obj || obj->type != JSON_OBJECT) return NULL;
    return json_object_get(obj, key);
}

static bool wants_section(const char *const *want, int want_count, const char *name) {
    for (int i = 0; i < want_count; i++)
        if (strcmp(want[i], name) == 0) return true;
    return false;
}

/* Python's "{:,}".format(n) for an int (bool included, per isinstance(x, int)). */
static char *format_comma(int64_t n) {
    char raw[32];
    snprintf(raw, sizeof raw, "%lld", (long long)n);
    bool neg = raw[0] == '-';
    const char *digits = neg ? raw + 1 : raw;
    size_t dlen = strlen(digits);
    size_t ngroups = dlen ? (dlen - 1) / 3 : 0;
    char *out = malloc(dlen + ngroups + (neg ? 1 : 0) + 1);
    size_t oi = 0;
    if (neg) out[oi++] = '-';
    for (size_t i = 0; i < dlen; i++) {
        if (i > 0 && (dlen - i) % 3 == 0) out[oi++] = ',';
        out[oi++] = digits[i];
    }
    out[oi] = '\0';
    return out;
}

/* "{:,}".format(value) if value is int-like (Python's isinstance(x, int), true for bool
 * too), else model_py_str(value) -- matches the skills-table ternary exactly. Malloc'd. */
static char *format_maybe_comma(const JsonValue *value) {
    if (model_py_is_int(value)) return format_comma(model_py_int_value(value));
    return model_py_str(value);
}

static char *describe(const JsonValue *id_value, const ItemTable *table) {
    char *id_str = model_py_str(id_value);
    if (model_py_is_int(id_value) && table) {
        const ItemEntry *entry = item_table_find(table, model_py_int_value(id_value));
        if (entry) {
            char *out = malloc(strlen(id_str) + strlen(entry->name) + strlen(entry->category) +
                                16);
            sprintf(out, "%s -- %s (%s)", id_str, entry->name, entry->category);
            free(id_str);
            return out;
        }
    }
    return id_str;
}

/* --- the command ---------------------------------------------------------------------------- */

int cmd_view(const char *file, int slot, const char *const *sections, int sections_count,
              const ItemNames *item_names) {
    SaveLoad sl = sas4_load(file);
    if (!sl.ok) {
        printf("%s\n", sl.error);
        sas4_load_free(&sl);
        return 1;
    }

    char stored[9], computed[9];
    int verify_rc = dg_verify(sl.raw, sl.raw_len, stored, computed);
    printf("file      %s\n", file);
    printf("checksum  %s / %s -- %s\n", stored, computed,
           verify_rc == 1 ? "VALID" : "MISMATCH");

    const JsonValue *document = sl.document;
    char profile_key[32];
    snprintf(profile_key, sizeof profile_key, "Profile%d", slot);
    const JsonValue *inventory = jget(document, "Inventory");
    const JsonValue *profile = jget(inventory, profile_key);
    if (!model_py_truthy(profile)) {
        printf("no Profile%d in this save\n", slot);
        sas4_load_free(&sl);
        return 1;
    }

    static const char *const ALL_SECTIONS[] = {"identity", "currency", "skills", "equipment",
                                                "weapons", "boxes", "global"};
    const char *const *want = sections;
    int want_count = sections_count;
    if (want_count == 0) {
        want = ALL_SECTIONS;
        want_count = 7;
    }

    const JsonValue *version = jget(document, "Version");
    const JsonValue *global = jget(document, "Global");
    const JsonValue *skills = jget(profile, "Skills");

    if (wants_section(want, want_count, "identity")) {
        printf("\n-- identity --\n");
        char *name = model_py_str(jget(profile, "Name"));
        char namebuf[600];
        snprintf(namebuf, sizeof namebuf, "%s (slot %d)", name, slot);
        free(name);
        view_line("character", namebuf, 2);
        char *link = model_py_str(jget(version, "link"));
        view_line("account link", link, 2);
        free(link);
        char *lastgame = model_py_str(jget(version, "LastGame"));
        view_line("game version", lastgame, 2);
        free(lastgame);
        char *upload = model_py_str(jget(global, "TimeOfLastUpload"));
        view_line("last upload", upload, 2);
        free(upload);
        char *hackcheck = model_py_str(jget(document, "HackCheck"));
        view_line("HackCheck", hackcheck, 2);
        free(hackcheck);

        char slots_in_use[2048] = {0};
        size_t used = 0;
        bool any = false;
        if (inventory && inventory->type == JSON_OBJECT) {
            for (size_t i = 0; i < inventory->as.object.count; i++) {
                const JsonValue *v = inventory->as.object.members[i].value;
                if (model_py_truthy(jget(v, "Loaded"))) {
                    used += (size_t)snprintf(slots_in_use + used, sizeof(slots_in_use) - used,
                                              "%s%s", any ? ", " : "",
                                              inventory->as.object.members[i].key);
                    any = true;
                }
            }
        }
        view_line("slots in use", any ? slots_in_use : "none", 2);
    }

    if (wants_section(want, want_count, "currency")) {
        printf("\n-- currency and tickets --\n");
        const JsonValue *money = jget(profile, "Money");
        char *money_str = format_comma(money ? model_py_int_value(money) : 0);
        /* Money defaults to 0, but only when absent -- if present and non-int (never true
         * for a real save), Python's isinstance check inside {:,} would raise; not chased,
         * see sas4load.h's note. */
        view_line("Money", money_str, 2);
        free(money_str);
        static const char *const SKILL_KEYS[] = {"AvailableBlackKeys",
                                                   "AvailableEliteAugmentCores",
                                                   "AvailableNightmareTickets"};
        for (int i = 0; i < 3; i++) {
            char *v = model_py_str(jget(skills, SKILL_KEYS[i]));
            view_line(SKILL_KEYS[i], v, 2);
            free(v);
        }
        static const char *const GLOBAL_KEYS[] = {"ReviveTokens", "AvailablePremiumTickets"};
        for (int i = 0; i < 2; i++) {
            char *v = model_py_str(jget(global, GLOBAL_KEYS[i]));
            view_line(GLOBAL_KEYS[i], v, 2);
            free(v);
        }
        char *fwc = model_py_str(jget(document, "FactionWarCredits"));
        view_line("FactionWarCredits", fwc, 2);
        free(fwc);
    }

    if (wants_section(want, want_count, "skills")) {
        printf("\n-- level and skills --\n");
        static const char *const KEYS[] = {"Class", "PlayerLevel", "PlayerTotalXp",
                                            "AvailableSkillPoints"};
        for (int i = 0; i < 4; i++) {
            char *v = format_maybe_comma(jget(skills, KEYS[i]));
            view_line(KEYS[i], v, 2);
            free(v);
        }
        char *rank = model_py_str(jget(global, "HighestRank"));
        view_line("HighestRank", rank, 2);
        free(rank);
        const JsonValue *skills_array = jget(skills, "SkillsArray");
        if (skills_array && skills_array->type == JSON_ARRAY) {
            for (size_t i = 0; i < skills_array->as.array.count; i++) {
                const JsonValue *entry = skills_array->as.array.items[i];
                char *name = model_py_str(jget(entry, "SkillName"));
                char label[900];
                snprintf(label, sizeof label, "  %s", name);
                free(name);
                char *level = model_py_str(jget(entry, "SkillLevel"));
                char value[900];
                snprintf(value, sizeof value, "level %s", level);
                free(level);
                view_line(label, value, 4);
            }
        }
    }

    if (wants_section(want, want_count, "equipment")) {
        printf("\n-- equipment --\n");
        const JsonValue *equipment = jget(profile, "Equipment");
        if (equipment && equipment->type == JSON_ARRAY) {
            for (size_t i = 0; i < equipment->as.array.count; i++) {
                const JsonValue *item = equipment->as.array.items[i];
                char *label = describe(jget(item, "ID"), item_names ? &item_names->equipment
                                                                     : NULL);
                char *grade = model_py_str(jget(item, "Grade"));
                char *bonus = model_py_str(jget(item, "BonusStatsLevel"));
                bool equipped = model_py_truthy(jget(item, "Equipped"));
                char value[900];
                snprintf(value, sizeof value, "grade %s, bonus %s, %s", grade, bonus,
                         equipped ? "equipped" : "stored");
                view_line(label, value, 2);
                free(label);
                free(grade);
                free(bonus);
            }
        }
        if (!item_names || item_names->section_count == 0) {
            view_line("", "(run `py sas4.py items` to show names instead of IDs)", 2);
        }
    }

    if (wants_section(want, want_count, "weapons")) {
        printf("\n-- weapons --\n");
        const JsonValue *weapons = jget(profile, "Weapons");
        size_t count = (weapons && weapons->type == JSON_ARRAY) ? weapons->as.array.count : 0;
        char count_str[32];
        snprintf(count_str, sizeof count_str, "%zu", count);
        view_line("count", count_str, 2);
        size_t shown = 0;
        if (weapons && weapons->type == JSON_ARRAY) {
            for (size_t i = 0; i < weapons->as.array.count && shown < 15; i++) {
                const JsonValue *item = weapons->as.array.items[i];
                shown++; /* Python slices weapons[:15] BEFORE the isinstance check. */
                if (!item || item->type != JSON_OBJECT) continue;
                char *label = describe(jget(item, "ID"), item_names ? &item_names->weapon
                                                                     : NULL);
                char *grade = model_py_str(jget(item, "Grade"));
                char *bonus = model_py_str(jget(item, "BonusStatsLevel"));
                char value[900];
                snprintf(value, sizeof value, "grade %s, bonus %s", grade, bonus);
                view_line(label, value, 2);
                free(label);
                free(grade);
                free(bonus);
            }
        }
    }

    if (wants_section(want, want_count, "boxes")) {
        printf("\n-- strongboxes --\n");
        const JsonValue *boxes = jget(profile, "Strongboxes");
        const JsonValue *unopened = jget(boxes, "Unopened");
        size_t unopened_count = (unopened && unopened->type == JSON_ARRAY)
                                     ? unopened->as.array.count
                                     : 0;
        JsonValue *null_placeholder = unopened ? NULL : json_new_null();
        char *dumped = NULL;
        size_t dumped_len = 0;
        char err[128];
        bool serialized = json_serialize_default(unopened ? unopened : null_placeholder,
                                                   &dumped, &dumped_len, err, sizeof err);
        char snippet[64];
        if (serialized) {
            size_t take = dumped_len < 50 ? dumped_len : 50;
            memcpy(snippet, dumped, take);
            snippet[take] = '\0';
            free(dumped);
        } else {
            snprintf(snippet, sizeof snippet, "null");
        }
        if (null_placeholder) json_free(null_placeholder);
        char unopened_value[128];
        snprintf(unopened_value, sizeof unopened_value, "%zu  %s", unopened_count, snippet);
        view_line("Unopened", unopened_value, 2);

        const JsonValue *claimed = jget(boxes, "Claimed");
        size_t claimed_count =
            (claimed && claimed->type == JSON_ARRAY) ? claimed->as.array.count : 0;
        char claimed_str[32];
        snprintf(claimed_str, sizeof claimed_str, "%zu", claimed_count);
        view_line("Claimed", claimed_str, 2);

        char *opened = model_py_str(jget(profile, "StrongboxesOpened"));
        view_line("StrongboxesOpened", opened, 2);
        free(opened);
    }

    if (wants_section(want, want_count, "global")) {
        printf("\n-- global --\n");
        if (global && global->type == JSON_OBJECT) {
            for (size_t i = 0; i < global->as.object.count; i++) {
                char *v = model_py_str(global->as.object.members[i].value);
                view_line(global->as.object.members[i].key, v, 2);
                free(v);
            }
        }
    }

    sas4_load_free(&sl);
    return 0;
}
