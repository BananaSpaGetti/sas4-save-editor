/*
 * Ports TestByteEdit, TestClaimed and TestMasteries from tools/tests/test_sas4.py -- task 14
 * of port-to-c-sas4-core. Every fixture is fixtures_document()/fixtures_write_save(), and
 * every item id/name is invented (129/101, matching the Python suite's own WEAPON/EQUIP
 * constants -- a stand-in table, never the downloaded catalogue).
 */
#include "anchor.h"
#include "dgdata.h"
#include "edit.h"
#include "fixtures.h"
#include "model.h"
#include "plans.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check(const char *name, bool ok) {
    printf("  %s  %s\n", ok ? "ok  " : "FAIL", name);
    if (!ok) {
        failures++;
    }
}

static JsonValue *parse_plaintext(const uint8_t *plain, size_t len) {
    JsonParseResult r = json_parse(plain, len);
    return r.value;
}

static JsonValue *load_document(const char *path) {
    char stored[9];
    uint8_t *plain;
    size_t plain_len;
    if (dg_load(path, stored, &plain, &plain_len) != 0) {
        return NULL;
    }
    JsonValue *document = parse_plaintext(plain, plain_len);
    free(plain);
    return document;
}

static int64_t get_int(const JsonValue *obj, const char *key) {
    const JsonValue *v = json_object_get(obj, key);
    return v && v->type == JSON_INT ? v->as.integer : -999999;
}

/* mingw's runtime does not ship memmem; a small local stand-in for the one use here. */
static const uint8_t *find_bytes(const uint8_t *haystack, size_t haystack_len,
                                  const char *needle) {
    size_t needle_len = strlen(needle);
    if (needle_len == 0 || needle_len > haystack_len) {
        return NULL;
    }
    for (size_t i = 0; i + needle_len <= haystack_len; i++) {
        if (memcmp(haystack + i, needle, needle_len) == 0) {
            return haystack + i;
        }
    }
    return NULL;
}

static bool check_is_empty_wrapper(const JsonValue *document) {
    ProblemList problems = {0};
    model_check(document, &problems);
    bool empty = problems.count == 0;
    model_problem_list_free(&problems);
    return empty;
}

/* --- TestByteEdit ------------------------------------------------------------------------- */

static void test_byte_edit(void) {
    printf("\nTestByteEdit\n");
    const char *dir = fixtures_temp_dir();

    /* test_only_the_named_value_changes */
    {
        char path[600];
        snprintf(path, sizeof(path), "%s\\only-named.save", dir);
        char backups[700];
        snprintf(backups, sizeof(backups), "%s\\backups", dir);
        JsonValue *d = fixtures_document(5, 1000);
        fixtures_write_save(d, path);
        json_free(d);

        size_t before_raw_len;
        uint8_t *before_raw = fixtures_read_file(path, &before_raw_len);
        char stored[9];
        uint8_t *before;
        size_t before_len;
        dg_load(path, stored, &before, &before_len);
        free(before_raw);

        EditPlanEntry plan[] = {{"Inventory/Profile0/Money", json_new_int(4242)}};
        ApplyEditsResult result = edit_apply(path, backups, plan, 1);
        check("apply_edits succeeds", result.ok);
        json_free(plan[0].value);

        uint8_t *after;
        size_t after_len;
        dg_load(path, stored, &after, &after_len);

        DgChangedField *fields;
        size_t field_count;
        dg_changed_fields(before, before_len, after, after_len, 80, &fields, &field_count);
        check("exactly one run differs", field_count == 1);
        check("the run mentions Money",
              field_count == 1 && strstr(fields[0].before, "Money") != NULL);

        JsonValue *doc = parse_plaintext(after, after_len);
        JsonValue *profile0 = json_object_get(json_object_get(doc, "Inventory"), "Profile0");
        check("Money reads back as 4242", get_int(profile0, "Money") == 4242);
        json_free(doc);

        dg_free_changed_fields(fields, field_count);
        free(before);
        free(after);
    }

    /* test_length_change_shifts_nothing_before_it */
    {
        char path[600];
        snprintf(path, sizeof(path), "%s\\length-change.save", dir);
        char backups[700];
        snprintf(backups, sizeof(backups), "%s\\backups", dir);
        JsonValue *d = fixtures_document(5, 1000);
        fixtures_write_save(d, path);
        json_free(d);

        char stored[9];
        uint8_t *before;
        size_t before_len;
        dg_load(path, stored, &before, &before_len);

        EditPlanEntry plan[] = {{"Inventory/Profile0/Money", json_new_int(12345678)}};
        ApplyEditsResult result = edit_apply(path, backups, plan, 1);
        check("apply_edits (widening) succeeds", result.ok);
        json_free(plan[0].value);

        uint8_t *after;
        size_t after_len;
        dg_load(path, stored, &after, &after_len);

        const uint8_t *cut = find_bytes(before, before_len, "\"Money\"");
        check("the anchor for Money was found in the before-plaintext", cut != NULL);
        size_t cut_offset = cut ? (size_t)(cut - before) : 0;
        check("everything before the value is byte-identical",
              cut && cut_offset <= after_len &&
              memcmp(before, after, cut_offset) == 0);

        JsonValue *doc = parse_plaintext(after, after_len);
        JsonValue *profile0 = json_object_get(json_object_get(doc, "Inventory"), "Profile0");
        check("Money reads back as 12345678", get_int(profile0, "Money") == 12345678);
        json_free(doc);

        free(before);
        free(after);
    }

    /* test_anchor_refuses_what_it_cannot_pin_down */
    {
        char path[600];
        snprintf(path, sizeof(path), "%s\\anchor-refuse.save", dir);
        JsonValue *d = fixtures_document(5, 1000);
        fixtures_write_save(d, path);

        char stored[9];
        uint8_t *plain;
        size_t plain_len;
        dg_load(path, stored, &plain, &plain_len);

        AnchorResult ok_anchor = anchor_for(d, plain, plain_len, "Inventory/Profile0/Money");
        check("a real path resolves to a unique anchor", ok_anchor.ok);
        if (ok_anchor.ok) {
            size_t count = 0;
            for (size_t i = 0; i + ok_anchor.anchor_len <= plain_len; i++) {
                if (memcmp(plain + i, ok_anchor.anchor, ok_anchor.anchor_len) == 0) {
                    count++;
                }
            }
            check("that anchor appears exactly once in the plaintext", count == 1);
            check("the value length is positive", ok_anchor.value_len > 0);
            anchor_free(&ok_anchor);
        }

        AnchorResult bad_anchor = anchor_for(d, plain, plain_len,
                                              "Inventory/Profile0/NoSuchField");
        check("a made-up path is refused", !bad_anchor.ok);

        json_free(d);
        free(plain);
    }

    /* test_a_bad_path_leaves_the_file_untouched */
    {
        char path[600];
        snprintf(path, sizeof(path), "%s\\bad-path.save", dir);
        char backups[700];
        snprintf(backups, sizeof(backups), "%s\\backups", dir);
        JsonValue *d = fixtures_document(5, 1000);
        fixtures_write_save(d, path);
        json_free(d);

        size_t original_len;
        uint8_t *original = fixtures_read_file(path, &original_len);

        EditPlanEntry plan[] = {{"Inventory/Profile0/Money", json_new_int(50)},
                                 {"Inventory/Profile0/Nope", json_new_int(1)}};
        ApplyEditsResult result = edit_apply(path, backups, plan, 2);
        check("a plan with one bad path is refused", !result.ok);
        json_free(plan[0].value);
        json_free(plan[1].value);

        size_t after_len;
        uint8_t *after = fixtures_read_file(path, &after_len);
        check("the file is unchanged: a plan that cannot be applied writes nothing at all",
              after_len == original_len && memcmp(original, after, original_len) == 0);
        free(original);
        free(after);
    }
}

/* --- TestClaimed -------------------------------------------------------------------------- */

#define WEAPON 129
#define EQUIP 101

static ItemNames make_stand_in_item_names(void) {
    ItemNames names;
    memset(&names, 0, sizeof(names));
    names.section_count = 2; /* matches item_names() always creating a key per section seen */
    item_table_add(&names.weapon, WEAPON, "Test Gun", "smg");
    item_table_add(&names.equipment, EQUIP, "Test Vest", "vest");
    return names;
}

static void test_claimed(void) {
    printf("\nTestClaimed\n");
    const char *dir = fixtures_temp_dir();
    ItemNames names = make_stand_in_item_names();

    /* test_run_lengths */
    {
        JsonValue *d = fixtures_document(5, 1000);
        GrantRequest req = {WEAPON, "weapon", 0, 0, 2};
        GrantPlanResult r = plans_grant_plan(d, &names, &req, 1, 0);
        check("a weapon run is four elements",
              r.ok && r.plan.count == 1 && r.plan.items[0].value->as.array.count == 4);
        check("a weapon's label names it", r.ok && r.label_count == 1 &&
              strstr(r.labels[0], "Test Gun") != NULL);
        grant_plan_result_free(&r);

        GrantRequest req2 = {EQUIP, "equipment", 0, 0, 2};
        GrantPlanResult r2 = plans_grant_plan(d, &names, &req2, 1, 0);
        check("a piece of equipment is also four elements",
              r2.ok && r2.plan.count == 1 && r2.plan.items[0].value->as.array.count == 4);
        grant_plan_result_free(&r2);
        json_free(d);
    }

    /* test_grant_read_back_preserves_the_details */
    {
        char path[600];
        snprintf(path, sizeof(path), "%s\\grant-readback.save", dir);
        char backups[700];
        snprintf(backups, sizeof(backups), "%s\\backups", dir);
        JsonValue *d = fixtures_document(5, 1000);
        fixtures_write_save(d, path);
        json_free(d);

        JsonValue *loaded = load_document(path);
        GrantRequest reqs[] = {{WEAPON, "weapon", 12, 10, 2}, {EQUIP, "equipment", 5, 3, 4}};
        GrantPlanResult plan = plans_grant_plan(loaded, &names, reqs, 2, 0);
        check("grant_plan succeeds for two items", plan.ok);
        if (plan.ok) {
            ApplyEditsResult applied = edit_apply(path, backups, plan.plan.items,
                                                   plan.plan.count);
            check("applying the grant succeeds", applied.ok);
        }
        grant_plan_result_free(&plan);
        json_free(loaded);

        JsonValue *final_doc = load_document(path);
        ClaimedRowList rows;
        plans_claimed_items(final_doc, &names, 0, &rows);
        check("two rows come back", rows.count == 2);
        if (rows.count == 2) {
            check("row 0 is the weapon at the right grade/bonus",
                  strcmp(rows.items[0].kind, "weapon") == 0 && rows.items[0].id == WEAPON &&
                  rows.items[0].grade == 12 && rows.items[0].bonus == 10);
            check("row 1 is the equipment at the right grade/bonus",
                  strcmp(rows.items[1].kind, "equipment") == 0 && rows.items[1].id == EQUIP &&
                  rows.items[1].grade == 5 && rows.items[1].bonus == 3);
            check("equipment keeps its slot", rows.items[1].has_equipped_slot &&
                  rows.items[1].equipped_slot == 4);
            check("a weapon has no slot", !rows.items[0].has_equipped_slot);
        }
        plans_claimed_items_free(&rows);
        json_free(final_doc);
    }

    /* test_indexes_step_by_the_run_not_by_one */
    {
        char path[600];
        snprintf(path, sizeof(path), "%s\\interleave.save", dir);
        char backups[700];
        snprintf(backups, sizeof(backups), "%s\\backups", dir);
        JsonValue *d = fixtures_document(5, 1000);
        fixtures_write_save(d, path);
        json_free(d);

        JsonValue *loaded = load_document(path);
        GrantRequest reqs[] = {{WEAPON, "weapon", 0, 0, 2}, {EQUIP, "equipment", 0, 0, 2},
                                {WEAPON, "weapon", 1, 1, 2}, {EQUIP, "equipment", 2, 2, 2}};
        GrantPlanResult plan = plans_grant_plan(loaded, &names, reqs, 4, 0);
        if (plan.ok) {
            edit_apply(path, backups, plan.plan.items, plan.plan.count);
        }
        grant_plan_result_free(&plan);
        json_free(loaded);

        JsonValue *final_doc = load_document(path);
        ClaimedRowList rows;
        plans_claimed_items(final_doc, &names, 0, &rows);
        check("four rows, indexes step by four", rows.count == 4 && rows.count >= 4 &&
              rows.items[0].index == 0 && rows.items[1].index == 4 &&
              rows.items[2].index == 8 && rows.items[3].index == 12);
        check("kinds alternate weapon/equipment/weapon/equipment", rows.count == 4 &&
              strcmp(rows.items[0].kind, "weapon") == 0 &&
              strcmp(rows.items[1].kind, "equipment") == 0 &&
              strcmp(rows.items[2].kind, "weapon") == 0 &&
              strcmp(rows.items[3].kind, "equipment") == 0);
        plans_claimed_items_free(&rows);
        json_free(final_doc);
    }

    /* test_removing_a_run_leaves_no_tail_behind */
    {
        char path[600];
        snprintf(path, sizeof(path), "%s\\remove-run.save", dir);
        char backups[700];
        snprintf(backups, sizeof(backups), "%s\\backups", dir);
        JsonValue *d = fixtures_document(5, 1000);
        fixtures_write_save(d, path);
        json_free(d);

        JsonValue *loaded = load_document(path);
        GrantRequest reqs[] = {{WEAPON, "weapon", 0, 0, 2}, {EQUIP, "equipment", 0, 0, 2},
                                {WEAPON, "weapon", 1, 0, 2}};
        GrantPlanResult plan = plans_grant_plan(loaded, &names, reqs, 3, 0);
        if (plan.ok) {
            edit_apply(path, backups, plan.plan.items, plan.plan.count);
        }
        grant_plan_result_free(&plan);
        json_free(loaded);

        JsonValue *doc2 = load_document(path);
        ClaimedRowList rows;
        plans_claimed_items(doc2, &names, 0, &rows);
        int64_t middle_index = rows.count > 1 ? (int64_t)rows.items[1].index : -1;
        size_t original_count = rows.count;
        plans_claimed_items_free(&rows);

        DropClaimedResult drop = plans_drop_claimed(doc2, &middle_index, 1, 0);
        check("drop_claimed succeeds", drop.ok);
        if (drop.ok) {
            const JsonValue *kept = drop.plan.items[0].value;
            check("one whole run went (4 elements)",
                  kept->as.array.count == original_count * 4 - 4);
            bool shape_ok = true;
            for (size_t i = 0; i + 1 < kept->as.array.count; i += 4) {
                const JsonValue *tag = kept->as.array.items[i];
                const JsonValue *entry = kept->as.array.items[i + 1];
                if (!(tag->type == JSON_INT && (tag->as.integer == 0 || tag->as.integer == 1)) ||
                    entry->type != JSON_OBJECT) {
                    shape_ok = false;
                }
            }
            check("every remaining element still starts with a tag and a dict", shape_ok);
        }
        drop_claimed_result_free(&drop);
        json_free(doc2);
    }

    /* test_removing_the_middle_item_keeps_the_rest */
    {
        char path[600];
        snprintf(path, sizeof(path), "%s\\remove-middle.save", dir);
        char backups[700];
        snprintf(backups, sizeof(backups), "%s\\backups", dir);
        JsonValue *d = fixtures_document(5, 1000);
        fixtures_write_save(d, path);
        json_free(d);

        JsonValue *loaded = load_document(path);
        GrantRequest reqs[] = {{WEAPON, "weapon", 1, 0, 2}, {EQUIP, "equipment", 2, 0, 2},
                                {WEAPON, "weapon", 3, 0, 2}};
        GrantPlanResult plan = plans_grant_plan(loaded, &names, reqs, 3, 0);
        if (plan.ok) {
            edit_apply(path, backups, plan.plan.items, plan.plan.count);
        }
        grant_plan_result_free(&plan);
        json_free(loaded);

        JsonValue *doc2 = load_document(path);
        ClaimedRowList rows;
        plans_claimed_items(doc2, &names, 0, &rows);
        int64_t middle_index = rows.count > 1 ? (int64_t)rows.items[1].index : -1;
        plans_claimed_items_free(&rows);

        DropClaimedResult drop = plans_drop_claimed(doc2, &middle_index, 1, 0);
        if (drop.ok) {
            ApplyEditsResult applied = edit_apply(path, backups, drop.plan.items,
                                                   drop.plan.count);
            check("applying the removal succeeds", applied.ok);
        }
        drop_claimed_result_free(&drop);
        json_free(doc2);

        JsonValue *doc3 = load_document(path);
        ClaimedRowList left;
        plans_claimed_items(doc3, &names, 0, &left);
        check("the equipment in the middle went, both weapons stayed",
              left.count == 2 && strcmp(left.items[0].kind, "weapon") == 0 &&
              left.items[0].grade == 1 && strcmp(left.items[1].kind, "weapon") == 0 &&
              left.items[1].grade == 3);
        plans_claimed_items_free(&left);
        check("the resulting profile is still consistent", check_is_empty_wrapper(doc3));
        json_free(doc3);
    }

    /* test_an_unknown_id_is_refused_before_anything_is_built */
    {
        JsonValue *d = fixtures_document(5, 1000);
        GrantRequest req = {999999, "auto", 0, 0, 2};
        GrantPlanResult r = plans_grant_plan(d, &names, &req, 1, 0);
        check("an unknown id is refused", !r.ok);
        grant_plan_result_free(&r);
        json_free(d);
    }

    /* test_an_ambiguous_id_has_to_be_told_which */
    {
        ItemNames ambiguous;
        memset(&ambiguous, 0, sizeof(ambiguous));
        ambiguous.section_count = 2;
        item_table_add(&ambiguous.weapon, 7, "Gun Seven", "smg");
        item_table_add(&ambiguous.equipment, 7, "Vest Seven", "vest");

        JsonValue *d = fixtures_document(5, 1000);
        GrantRequest auto_req = {7, "auto", 0, 0, 2};
        GrantPlanResult auto_r = plans_grant_plan(d, &ambiguous, &auto_req, 1, 0);
        check("an ambiguous id via auto is refused", !auto_r.ok);
        grant_plan_result_free(&auto_r);

        GrantRequest explicit_req = {7, "weapon", 0, 0, 2};
        GrantPlanResult explicit_r = plans_grant_plan(d, &ambiguous, &explicit_req, 1, 0);
        check("naming the kind resolves it", explicit_r.ok && explicit_r.label_count == 1 &&
              strstr(explicit_r.labels[0], "Gun Seven") != NULL);
        grant_plan_result_free(&explicit_r);

        json_free(d);
        items_free(&ambiguous);
    }

    items_free(&names);
}

/* --- TestMasteries ------------------------------------------------------------------------ */

static void test_masteries(void) {
    printf("\nTestMasteries\n");
    const char *dir = fixtures_temp_dir();

    /* test_a_generated_profile_has_every_track */
    {
        JsonValue *d = fixtures_document(5, 1000);
        MasteryRowList rows;
        plans_mastery_rows(d, 0, &rows);
        check("every track is present", rows.count == MODEL_MASTERY_SLOTS);
        bool all_zero = true;
        for (size_t i = 0; i < rows.count; i++) {
            if (rows.items[i].xp->type != JSON_INT || rows.items[i].xp->as.integer != 0 ||
                rows.items[i].level->type != JSON_INT || rows.items[i].level->as.integer != 0) {
                all_zero = false;
            }
        }
        check("every track starts at xp 0, level 0", all_zero);
        plans_mastery_rows_free(&rows);
        json_free(d);
    }

    /* test_setting_a_level_writes_the_xp_that_supports_it */
    {
        JsonValue *d = fixtures_document(5, 1000);
        MasteryTarget targets[] = {{3, 5}, {7, 2}};
        MasteryPlanResult plan = plans_mastery_plan(d, targets, 2, 0);
        check("mastery_plan succeeds", plan.ok);
        if (plan.ok) {
            const JsonValue *tracks = plan.plan.items[0].value;
            check("one value replaced, however many tracks move", plan.plan.count == 1);
            const JsonValue *track3 = tracks->as.array.items[3];
            const JsonValue *track7 = tracks->as.array.items[7];
            const JsonValue *track0 = tracks->as.array.items[0];
            check("track 3 got level 5's XP", get_int(track3, "MasteryXp") ==
                  MODEL_MASTERY_LEVEL_XP[5] && get_int(track3, "MasteryLvl") == 5);
            check("track 7 got level 2's XP", get_int(track7, "MasteryXp") ==
                  MODEL_MASTERY_LEVEL_XP[2] && get_int(track7, "MasteryLvl") == 2);
            check("other tracks are untouched",
                  get_int(track0, "MasteryXp") == 0 && get_int(track0, "MasteryLvl") == 0);
            check("the whole list is still MASTERY_SLOTS long",
                  tracks->as.array.count == MODEL_MASTERY_SLOTS);
        }
        mastery_plan_result_free(&plan);
        json_free(d);
    }

    /* test_out_of_range_is_refused */
    {
        JsonValue *d = fixtures_document(5, 1000);
        struct { int index; int level; } bad[] = {
            {MODEL_MASTERY_SLOTS, 1}, {-1, 1}, {0, 6}, {0, -1}};
        bool all_refused = true;
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            MasteryTarget t = {bad[i].index, bad[i].level};
            MasteryPlanResult plan = plans_mastery_plan(d, &t, 1, 0);
            if (plan.ok) {
                all_refused = false;
            }
            mastery_plan_result_free(&plan);
        }
        check("every out-of-range target is refused", all_refused);
        json_free(d);
    }

    /* test_a_slot_with_no_character_is_refused */
    {
        JsonValue *d = fixtures_document(5, 1000);
        MasteryTarget t = {0, 5};
        MasteryPlanResult mp = plans_mastery_plan(d, &t, 1, 4);
        check("mastery_plan on an empty slot is refused", !mp.ok && mp.empty_slot);
        check("the message names the slot", strstr(mp.error, "4") != NULL);
        mastery_plan_result_free(&mp);

        LevelPlanResult lp = plans_level_plan(d, 40, 4);
        check("level_plan on an empty slot is refused too", !lp.ok && lp.empty_slot);
        level_plan_result_free(&lp);
        json_free(d);
    }

    /* test_granting_into_an_empty_slot_is_refused_by_name */
    {
        ItemNames names;
        memset(&names, 0, sizeof(names));
        names.section_count = 2;
        item_table_add(&names.weapon, 129, "Z-5 Heavy", "smg");

        JsonValue *d = fixtures_document(5, 1000);
        GrantRequest req = {129, "weapon", 0, 0, 0};
        GrantPlanResult gp = plans_grant_plan(d, &names, &req, 1, 4);
        check("granting into an empty slot is refused", !gp.ok && gp.empty_slot);
        check("the message names --slotprofile", strstr(gp.error, "--slotprofile") != NULL);
        grant_plan_result_free(&gp);
        json_free(d);
        items_free(&names);
    }

    /* test_an_empty_slot_cannot_be_reached_through_a_global */
    {
        JsonValue *d = fixtures_document(5, 1000);
        LevelPlanResult lp = plans_level_plan(d, 40, 4);
        check("level 40 into slot 4 is refused", !lp.ok);
        level_plan_result_free(&lp);
        const JsonValue *global = json_object_get(d, "Global");
        check("HighestRank is untouched", get_int(global, "HighestRank") == 5);
        json_free(d);
    }

    /* test_a_track_that_is_not_a_number_is_reported_not_raised */
    {
        const char *fields[] = {"MasteryXp", "MasteryLvl"};
        bool both_reported = true;
        for (size_t f = 0; f < 2; f++) {
            JsonValue *d = fixtures_document(5, 1000);
            JsonValue *tracks = json_object_get(json_object_get(d, "MasteryProgress"),
                                                 "MasteryProfile0");
            JsonValue *track0 = tracks->as.array.items[0];
            json_object_set(track0, fields[f], strlen(fields[f]), json_new_string("3", 1));

            ProblemList problems = {0};
            model_check(d, &problems);
            bool found = false;
            for (size_t i = 0; i < problems.count; i++) {
                if (strstr(problems.items[i], fields[f])) {
                    found = true;
                }
            }
            if (!found) {
                both_reported = false;
            }
            model_problem_list_free(&problems);
            json_free(d);
        }
        check("a non-numeric track field is reported, not raised", both_reported);
    }

    /* test_reading_a_track_that_is_not_a_number_does_not_crash */
    {
        JsonValue *d = fixtures_document(5, 1000);
        JsonValue *tracks = json_object_get(json_object_get(d, "MasteryProgress"),
                                             "MasteryProfile0");
        json_object_set(tracks->as.array.items[0], "MasteryXp", strlen("MasteryXp"),
                         json_new_string("3", 1));
        json_object_set(tracks->as.array.items[1], "MasteryLvl", strlen("MasteryLvl"),
                         json_new_null());

        MasteryRowList rows;
        plans_mastery_rows(d, 0, &rows);
        check("every track is still present", rows.count == MODEL_MASTERY_SLOTS);
        check("the raw string value is shown, not a stand-in",
              rows.count > 0 && rows.items[0].xp->type == JSON_STRING &&
              rows.items[0].xp->as.string.len == 1 && rows.items[0].xp->as.string.data[0] == '3');
        check("no level is claimed for an XP that is not a number",
              rows.count > 0 && !rows.items[0].has_supported);
        plans_mastery_rows_free(&rows);
        json_free(d);
    }

    /* test_the_whole_list_is_replaced_because_a_field_cannot_be_pinned */
    {
        char path[600];
        snprintf(path, sizeof(path), "%s\\mastery-anchor.save", dir);
        JsonValue *d = fixtures_document(5, 1000);
        fixtures_write_save(d, path);

        char stored[9];
        uint8_t *plain;
        size_t plain_len;
        dg_load(path, stored, &plain, &plain_len);

        AnchorResult single = anchor_for(d, plain, plain_len,
                                          "MasteryProgress/MasteryProfile0[5]/MasteryXp");
        check("a single duplicated field cannot be pinned down", !single.ok);

        AnchorResult whole = anchor_for(d, plain, plain_len, "MasteryProgress/MasteryProfile0");
        check("the whole list resolves to a unique anchor", whole.ok);
        if (whole.ok) {
            size_t count = 0;
            for (size_t i = 0; i + whole.anchor_len <= plain_len; i++) {
                if (memcmp(plain + i, whole.anchor, whole.anchor_len) == 0) {
                    count++;
                }
            }
            check("that anchor appears exactly once", count == 1);
            anchor_free(&whole);
        }
        json_free(d);
        free(plain);
    }

    /* test_writing_masteries_end_to_end_stays_consistent */
    {
        char path[600];
        snprintf(path, sizeof(path), "%s\\mastery-e2e.save", dir);
        char backups[700];
        snprintf(backups, sizeof(backups), "%s\\backups", dir);
        JsonValue *d = fixtures_document(5, 1000);
        fixtures_write_save(d, path);
        json_free(d);

        JsonValue *loaded = load_document(path);
        MasteryTarget targets[] = {{3, 5}, {7, 2}};
        MasteryPlanResult plan = plans_mastery_plan(loaded, targets, 2, 0);
        if (plan.ok) {
            ApplyEditsResult applied = edit_apply(path, backups, plan.plan.items,
                                                   plan.plan.count);
            check("applying the mastery plan succeeds", applied.ok);
        }
        mastery_plan_result_free(&plan);
        json_free(loaded);

        JsonValue *final_doc = load_document(path);
        MasteryRowList rows;
        plans_mastery_rows(final_doc, 0, &rows);
        bool track3_ok = false, track7_ok = false;
        for (size_t i = 0; i < rows.count; i++) {
            if (rows.items[i].index == 3 && rows.items[i].level->type == JSON_INT &&
                rows.items[i].level->as.integer == 5 && rows.items[i].xp->type == JSON_INT &&
                rows.items[i].xp->as.integer == MODEL_MASTERY_LEVEL_XP[5]) {
                track3_ok = true;
            }
            if (rows.items[i].index == 7 && rows.items[i].level->type == JSON_INT &&
                rows.items[i].level->as.integer == 2 && rows.items[i].xp->type == JSON_INT &&
                rows.items[i].xp->as.integer == MODEL_MASTERY_LEVEL_XP[2]) {
                track7_ok = true;
            }
        }
        check("track 3 reads back at level 5 with the matching XP", track3_ok);
        check("track 7 reads back at level 2 with the matching XP", track7_ok);
        plans_mastery_rows_free(&rows);
        check("the file is still fully consistent", check_is_empty_wrapper(final_doc));
        json_free(final_doc);
    }

    /* test_a_level_its_xp_cannot_reach_is_caught */
    {
        JsonValue *d = fixtures_document(5, 1000);
        JsonValue *tracks = json_object_get(json_object_get(d, "MasteryProgress"),
                                             "MasteryProfile0");
        JsonValue *bad = json_new_object();
        json_object_set(bad, "MasteryXp", strlen("MasteryXp"), json_new_int(0));
        json_object_set(bad, "MasteryLvl", strlen("MasteryLvl"), json_new_int(5));
        json_free(tracks->as.array.items[2]);
        tracks->as.array.items[2] = bad;

        ProblemList problems = {0};
        model_check(d, &problems);
        bool found = false;
        for (size_t i = 0; i < problems.count; i++) {
            if (strstr(problems.items[i], "MasteryProfile0[2]")) {
                found = true;
            }
        }
        check("a level its XP cannot reach is caught", found);
        model_problem_list_free(&problems);
        json_free(d);
    }

    /* test_named_tracks_are_real_indexes */
    {
        check("at least one track has been established", MODEL_MASTERY_TRACKS_COUNT > 0);
        bool all_valid = true;
        for (size_t i = 0; i < MODEL_MASTERY_TRACKS_COUNT; i++) {
            int index = MODEL_MASTERY_TRACKS[i].index;
            if (!(0 <= index && index < MODEL_MASTERY_SLOTS) ||
                strlen(MODEL_MASTERY_TRACKS[i].name) == 0) {
                all_valid = false;
            }
        }
        check("every named track is a real index with a real name", all_valid);
        bool unique = true;
        for (size_t i = 0; i < MODEL_MASTERY_TRACKS_COUNT; i++) {
            for (size_t j = i + 1; j < MODEL_MASTERY_TRACKS_COUNT; j++) {
                if (strcmp(MODEL_MASTERY_TRACKS[i].name, MODEL_MASTERY_TRACKS[j].name) == 0) {
                    unique = false;
                }
            }
        }
        check("no two indexes share a name", unique);
    }

    /* test_every_threshold_round_trips_through_the_level_it_names */
    {
        bool round_trips = true;
        for (int level = 0; level < 6; level++) {
            int64_t needed = MODEL_MASTERY_LEVEL_XP[level];
            if (model_mastery_level_for_xp(needed) != level) {
                round_trips = false;
            }
            if (level > 0 && model_mastery_level_for_xp(needed - 1) != level - 1) {
                round_trips = false;
            }
        }
        check("every threshold round-trips through the level it names", round_trips);
    }
}

int main(void) {
    test_byte_edit();
    test_claimed();
    test_masteries();

    printf("\n");
    if (failures) {
        printf("FAILED: %d check(s)\n", failures);
        return 1;
    }
    printf("all checks pass\n");
    return 0;
}
