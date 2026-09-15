/*
 * Ports TestExperience and TestRules from tools/tests/test_sas4.py onto model.c directly --
 * task 14 of port-to-c-sas4-core. Every document is fixtures_document(), never a real save.
 */
#include "fixtures.h"
#include "model.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check(const char *name, bool ok) {
    printf("  %s  %s\n", ok ? "ok  " : "FAIL", name);
    if (!ok) {
        failures++;
    }
}

/* The 26 community-wiki cumulative values sas4_model.py's XP_TABLE ships -- public game
 * data, not derived from any save, reproduced here the same way the Python test hardcodes
 * it rather than importing it. */
static const int64_t XP_TABLE[26] = {
    0, 0, 1071, 2359, 4014, 6190, 9045, 12741, 17445, 23328, 30565, 39335, 49821,
    62211, 76697, 93475, 112745, 134711, 159582, 187571, 218895, 253775, 292436,
    335108, 382025, 433425};

static void test_experience(void) {
    printf("\nTestExperience\n");

    bool table_matches = true;
    for (size_t level = 0; level < sizeof(XP_TABLE) / sizeof(XP_TABLE[0]); level++) {
        if (model_xp_for_level((int)level) != XP_TABLE[level]) {
            table_matches = false;
        }
    }
    check("the formula reproduces every observed XP_TABLE value", table_matches);

    bool rounding_ambiguous = false;
    for (int n = 1; n < MODEL_MAX_LEVEL + 20; n++) {
        int64_t l = n;
        if ((7 * l * l + 7 * l * l * l) % 10 == 5) {
            rounding_ambiguous = true;
        }
    }
    check("floor/round rounding is never ambiguous", !rounding_ambiguous);

    bool climbs = true;
    for (int level = 1; level <= MODEL_MAX_LEVEL; level++) {
        if (!(model_xp_for_level(level) < model_xp_for_level(level + 1))) {
            climbs = false;
        }
    }
    check("the curve climbs all the way to the cap", climbs);

    bool bands_ok = true;
    for (int level = 1; level <= MODEL_MAX_LEVEL; level++) {
        int64_t xp = model_xp_for_level(level);
        if (!(model_xp_for_level(level) <= xp && xp < model_xp_for_level(level + 1))) {
            bands_ok = false;
        }
    }
    check("every level lands inside its own band", bands_ok);
}

static bool check_is_empty(const JsonValue *document) {
    ProblemList problems = {0};
    model_check(document, &problems);
    bool empty = problems.count == 0;
    model_problem_list_free(&problems);
    return empty;
}

static bool check_is_nonempty(const JsonValue *document) {
    return !check_is_empty(document);
}

/* --- the nine structural attacks + two consistent ones, from sas4_model.py's ATTACKS ----- */

static void attack_mastery_level_without_xp(JsonValue *d) {
    JsonValue *tracks = json_object_get(json_object_get(d, "MasteryProgress"), "MasteryProfile0");
    JsonValue *track = json_new_object();
    json_object_set(track, "MasteryXp", strlen("MasteryXp"), json_new_int(0));
    json_object_set(track, "MasteryLvl", strlen("MasteryLvl"), json_new_int(5));
    json_free(tracks->as.array.items[3]);
    tracks->as.array.items[3] = track;
}

static JsonValue *profile0_skills(JsonValue *d) {
    JsonValue *inventory = json_object_get(d, "Inventory");
    JsonValue *profile0 = json_object_get(inventory, "Profile0");
    return json_object_get(profile0, "Skills");
}

static void attack_xp_far_above_level(JsonValue *d) {
    json_object_set(profile0_skills(d), "PlayerTotalXp", strlen("PlayerTotalXp"),
                     json_new_int(100000));
}

static void attack_money_over_int32(JsonValue *d) {
    JsonValue *profile0 = json_object_get(json_object_get(d, "Inventory"), "Profile0");
    json_object_set(profile0, "Money", strlen("Money"), json_new_int(5000000000LL));
}

static void attack_rank_below_level(JsonValue *d) {
    JsonValue *skills = profile0_skills(d);
    json_object_set(skills, "PlayerLevel", strlen("PlayerLevel"), json_new_int(20));
    json_object_set(skills, "PlayerTotalXp", strlen("PlayerTotalXp"),
                     json_new_int(model_xp_for_level(20)));
    json_object_set(json_object_get(d, "Global"), "HighestRank", strlen("HighestRank"),
                     json_new_int(3));
}

static void attack_too_many_skill_points(JsonValue *d) {
    json_object_set(profile0_skills(d), "AvailableSkillPoints", strlen("AvailableSkillPoints"),
                     json_new_int(999));
}

static void attack_malformed_strongbox(JsonValue *d) {
    JsonValue *profile0 = json_object_get(json_object_get(d, "Inventory"), "Profile0");
    JsonValue *strongboxes = json_object_get(profile0, "Strongboxes");
    JsonValue *claimed = json_new_array();
    json_array_push(claimed, json_new_int(0));
    json_array_push(claimed, json_new_string("not a dict", strlen("not a dict")));
    json_object_set(strongboxes, "Claimed", strlen("Claimed"), claimed);
}

static void attack_negative_level(JsonValue *d) {
    json_object_set(profile0_skills(d), "PlayerLevel", strlen("PlayerLevel"), json_new_int(-3));
}

static void attack_absurd_skill_level(JsonValue *d) {
    JsonValue *array = json_new_array();
    JsonValue *entry = json_new_object();
    json_object_set(entry, "SkillName", strlen("SkillName"),
                     json_new_string("holdtheline", strlen("holdtheline")));
    json_object_set(entry, "SkillLevel", strlen("SkillLevel"), json_new_int(500));
    json_array_push(array, entry);
    json_object_set(profile0_skills(d), "SkillsArray", strlen("SkillsArray"), array);
}

static void attack_double_equipped_slot(JsonValue *d) {
    JsonValue *profile0 = json_object_get(json_object_get(d, "Inventory"), "Profile0");
    JsonValue *equipment = json_new_array();
    JsonValue *a = json_new_object();
    json_object_set(a, "ID", 2, json_new_int(101));
    json_object_set(a, "EquippedSlot", strlen("EquippedSlot"), json_new_int(2));
    json_object_set(a, "Equipped", strlen("Equipped"), json_new_bool(true));
    JsonValue *b = json_new_object();
    json_object_set(b, "ID", 2, json_new_int(102));
    json_object_set(b, "EquippedSlot", strlen("EquippedSlot"), json_new_int(2));
    json_object_set(b, "Equipped", strlen("Equipped"), json_new_bool(true));
    json_array_push(equipment, a);
    json_array_push(equipment, b);
    json_object_set(profile0, "Equipment", strlen("Equipment"), equipment);
}

static void promote(JsonValue *d, int level) {
    JsonValue *skills = profile0_skills(d);
    json_object_set(skills, "PlayerLevel", strlen("PlayerLevel"), json_new_int(level));
    json_object_set(skills, "PlayerTotalXp", strlen("PlayerTotalXp"),
                     json_new_int(model_xp_for_level(level)));
    json_object_set(skills, "AvailableSkillPoints", strlen("AvailableSkillPoints"),
                     json_new_int(level));
    json_object_set(json_object_get(d, "Global"), "HighestRank", strlen("HighestRank"),
                     json_new_int(level));
}

static void attack_consistent_level_20(JsonValue *d) { promote(d, 20); }
static void attack_consistent_level_25(JsonValue *d) { promote(d, 25); }

typedef void (*Attack)(JsonValue *);

static void test_rules(void) {
    printf("\nTestRules\n");

    bool generated_consistent = true;
    int levels[] = {1, 5, 25, 99};
    for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        JsonValue *d = fixtures_document(levels[i], 1234);
        if (!check_is_empty(d)) {
            generated_consistent = false;
        }
        json_free(d);
    }
    check("a generated profile is consistent at every level", generated_consistent);

    struct { const char *name; Attack fn; } structural[] = {
        {"mastery-level-without-xp", attack_mastery_level_without_xp},
        {"xp-far-above-level", attack_xp_far_above_level},
        {"money-over-int32", attack_money_over_int32},
        {"rank-below-level", attack_rank_below_level},
        {"too-many-skill-points", attack_too_many_skill_points},
        {"malformed-strongbox", attack_malformed_strongbox},
        {"negative-level", attack_negative_level},
        {"absurd-skill-level", attack_absurd_skill_level},
        {"double-equipped-slot", attack_double_equipped_slot},
    };
    bool all_structural_caught = true;
    for (size_t i = 0; i < sizeof(structural) / sizeof(structural[0]); i++) {
        JsonValue *d = fixtures_document(1, 1000);
        structural[i].fn(d);
        if (!check_is_nonempty(d)) {
            printf("    %s slipped through\n", structural[i].name);
            all_structural_caught = false;
        }
        json_free(d);
    }
    check("every structural attack is caught", all_structural_caught);

    struct { const char *name; Attack fn; } consistent[] = {
        {"consistent-level-20", attack_consistent_level_20},
        {"consistent-level-25", attack_consistent_level_25},
    };
    bool all_consistent_clean = true;
    for (size_t i = 0; i < sizeof(consistent) / sizeof(consistent[0]); i++) {
        JsonValue *d = fixtures_document(1, 1000);
        consistent[i].fn(d);
        if (!check_is_empty(d)) {
            printf("    %s should have been undetectable\n", consistent[i].name);
            all_consistent_clean = false;
        }
        json_free(d);
    }
    check("a consistent edit is never caught -- that is the point", all_consistent_clean);
}

int main(void) {
    test_experience();
    test_rules();

    printf("\n");
    if (failures) {
        printf("FAILED: %d check(s)\n", failures);
        return 1;
    }
    printf("all checks pass\n");
    return 0;
}
