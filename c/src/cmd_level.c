/*
 * `level` and `mastery` -- task 7 of the port-to-c-sas4-cli plan.
 */
#include "cmd_level.h"
#include "dgdata.h"
#include "edit.h"
#include "json.h"
#include "model.h"
#include "path.h"
#include "plans.h"
#include "sas4load.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GAME_PROCESS "SAS4-Win.exe"

/* json.dumps(v) -- Python's default (ensure_ascii=True). Malloc'd. */
static char *dumps(const JsonValue *v) {
    char *out = NULL;
    size_t out_len = 0;
    char err[128];
    if (!json_serialize_default(v, &out, &out_len, err, sizeof err)) {
        out = malloc(2);
        out[0] = '?';
        out[1] = '\0';
    }
    return out;
}

static bool refuse_unverified(const SaveLoad *sl, bool force) {
    char stored[9], computed[9];
    if (dg_verify(sl->raw, sl->raw_len, stored, computed) != 1 && !force) {
        printf("this file does not verify (%s vs %s) -- refusing to edit it\n", stored,
               computed);
        return true;
    }
    return false;
}

int cmd_level(const char *file, int level, int slot, bool dry_run, bool force,
              const char *backups_dir) {
    SaveLoad sl = sas4_load(file);
    if (!sl.ok) {
        printf("%s\n", sl.error);
        sas4_load_free(&sl);
        return 1;
    }
    if (refuse_unverified(&sl, force)) {
        sas4_load_free(&sl);
        return 1;
    }

    LevelPlanResult planned = plans_level_plan(sl.document, level, slot);
    if (!planned.ok) {
        printf("%s\n", planned.error);
        level_plan_result_free(&planned);
        sas4_load_free(&sl);
        return 1;
    }

    printf("%s -> level %d\n", file, level);
    if (planned.spent) {
        long long granted = (long long)level - planned.spent;
        if (granted < 0) granted = 0;
        printf("  %lld skill point(s) already spent, so %lld are granted rather than %d\n",
               (long long)planned.spent, granted, level);
    }
    for (size_t i = 0; i < planned.plan.count; i++) {
        const char *path = planned.plan.items[i].path;
        PathAtResult at = path_at(sl.document, path);
        if (!at.ok) {
            printf("  skip %s -- not in this save\n", path);
            continue;
        }
        char *cur = dumps(at.value);
        char *new_ = dumps(planned.plan.items[i].value);
        printf("  %-46s %s -> %s%s\n", path, cur, new_,
               json_equal(at.value, planned.plan.items[i].value) ? "   (already)" : "");
        free(cur);
        free(new_);
    }

    PendingList pending = {0};
    edit_pending(sl.document, planned.plan.items, planned.plan.count, &pending);
    if (pending.count == 0) {
        printf("\nnothing to change\n");
        edit_pending_free(&pending);
        level_plan_result_free(&planned);
        sas4_load_free(&sl);
        return 0;
    }
    if (dry_run) {
        printf("\n  (dry run, nothing written)\n");
        edit_pending_free(&pending);
        level_plan_result_free(&planned);
        sas4_load_free(&sl);
        return 0;
    }
    if (edit_game_running() && !force) {
        printf("\n%s is running. It rewrites the save on its own schedule and would "
               "overwrite\n", GAME_PROCESS);
        printf("this edit. Close the game first, or pass --force if you know better.\n");
        edit_pending_free(&pending);
        level_plan_result_free(&planned);
        sas4_load_free(&sl);
        return 1;
    }

    /* apply_edits(args.file, changes) -- `changes` is pending()'s (path, new) pairs, not the
     * whole plan: an entry already at its target value is dropped before writing. */
    EditPlanEntry *changes = malloc(pending.count * sizeof *changes);
    for (size_t i = 0; i < pending.count; i++) {
        changes[i].path = pending.items[i].path;
        changes[i].value = pending.items[i].new_value;
    }
    ApplyEditsResult applied = edit_apply(file, backups_dir, changes, pending.count);
    free(changes);
    printf("\n  backup   %s\n", applied.has_backup_path ? applied.backup_path : "None");
    printf("  %s\n", applied.message);
    edit_pending_free(&pending);
    level_plan_result_free(&planned);
    sas4_load_free(&sl);
    if (!applied.ok) return 1;

    SaveLoad after = sas4_load(file);
    ProblemList problems = {0};
    if (after.ok) model_check(after.document, &problems);
    if (problems.count > 0) {
        printf("  but the result is not consistent:\n");
        for (size_t i = 0; i < problems.count; i++) printf("    - %s\n", problems.items[i]);
        model_problem_list_free(&problems);
        sas4_load_free(&after);
        return 1;
    }
    model_problem_list_free(&problems);
    sas4_load_free(&after);
    printf("  consistent: nothing a plausibility check would flag\n");
    return 0;
}

/* --- mastery ---------------------------------------------------------------------------- */

/* str(value) of a mastery row field, for the "%-9s"/"%-7s" columns -- the Python prints the
 * raw value with %s precisely so an odd one survives being printed. */
static char *row_str(const JsonValue *v) { return model_py_str(v); }

int cmd_mastery(const char *file, const char *set_spec, bool all_given, int all_level,
                int slot, bool dry_run, bool force, const char *backups_dir) {
    SaveLoad sl = sas4_load(file);
    if (!sl.ok) {
        printf("%s\n", sl.error);
        sas4_load_free(&sl);
        return 1;
    }
    if (refuse_unverified(&sl, force)) {
        sas4_load_free(&sl);
        return 1;
    }

    MasteryRowList rows = {0};
    plans_mastery_rows(sl.document, slot, &rows);
    if (rows.count == 0) {
        printf("no mastery tracks in slot %d of %s\n", slot, file);
        plans_mastery_rows_free(&rows);
        sas4_load_free(&sl);
        return 1;
    }

    MasteryTarget *targets = NULL;
    size_t target_count = 0;
    if (all_given) {
        targets = malloc(rows.count * sizeof *targets);
        for (size_t i = 0; i < rows.count; i++) {
            targets[target_count].index = (int)rows.items[i].index;
            targets[target_count].level = all_level;
            target_count++;
        }
    } else if (set_spec) {
        /* args.set.split(","), each "index=level"; a piece that is not two ints is an error
         * naming the piece with %r (Python's repr of the string). */
        size_t cap = 8;
        targets = malloc(cap * sizeof *targets);
        const char *p = set_spec;
        while (*p) {
            const char *comma = strchr(p, ',');
            size_t piece_len = comma ? (size_t)(comma - p) : strlen(p);
            char piece[128];
            size_t n = piece_len < sizeof piece - 1 ? piece_len : sizeof piece - 1;
            memcpy(piece, p, n);
            piece[n] = '\0';
            /* .strip() */
            char *start = piece;
            while (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r') start++;
            size_t slen = strlen(start);
            while (slen > 0 && (start[slen - 1] == ' ' || start[slen - 1] == '\t'
                                || start[slen - 1] == '\n' || start[slen - 1] == '\r')) {
                start[--slen] = '\0';
            }
            if (slen > 0) {
                char *eq = strchr(start, '=');
                char index_text[64] = {0};
                const char *level_text = "";
                if (eq) {
                    size_t ilen = (size_t)(eq - start);
                    if (ilen >= sizeof index_text) ilen = sizeof index_text - 1;
                    memcpy(index_text, start, ilen);
                    index_text[ilen] = '\0';
                    level_text = eq + 1;
                } else {
                    snprintf(index_text, sizeof index_text, "%s", start);
                }
                /* int(index) / int(level) -- Python's int(), which tolerates whitespace on
                 * either side, so "3 = 5" parses as 3 and 5 rather than being refused. */
                long idx = 0, lvl = 0;
                bool bad = !model_py_parse_int(index_text, &idx)
                           || !model_py_parse_int(level_text, &lvl);
                if (bad) {
                    JsonValue *as_str = json_new_string(start, slen);
                    char *repr = model_py_repr(as_str);
                    printf("cannot read %s -- write it as <track>=<level>, e.g. 3=5\n", repr);
                    free(repr);
                    json_free(as_str);
                    free(targets);
                    plans_mastery_rows_free(&rows);
                    sas4_load_free(&sl);
                    return 1;
                }
                /* A dict: a repeated index keeps the LAST value, at its first position. */
                bool replaced = false;
                for (size_t i = 0; i < target_count; i++) {
                    if (targets[i].index == (int)idx) {
                        targets[i].level = (int)lvl;
                        replaced = true;
                        break;
                    }
                }
                if (!replaced) {
                    if (target_count == cap) {
                        cap *= 2;
                        targets = realloc(targets, cap * sizeof *targets);
                    }
                    targets[target_count].index = (int)idx;
                    targets[target_count].level = (int)lvl;
                    target_count++;
                }
            }
            if (!comma) break;
            p = comma + 1;
        }
    }

    if (target_count == 0) {
        printf("%s, character slot %d\n", file, slot);
        printf("  track   XP        level   what it is\n");
        for (size_t i = 0; i < rows.count; i++) {
            const MasteryRow *r = &rows.items[i];
            const char *note;
            if (!r->has_supported) {
                note = "   <- this is not a number";
            } else if (!(model_py_is_int(r->level)
                         && model_py_int_value(r->level) == r->supported)) {
                note = "   <- level disagrees with the XP";
            } else {
                note = "";
            }
            char *xp = row_str(r->xp);
            char *lvl = row_str(r->level);
            const char *name = model_mastery_name((int)r->index);
            printf("  %5zu   %-9s %-7s %s%s\n", r->index, xp, lvl, name ? name : "?", note);
            free(xp);
            free(lvl);
        }
        printf("\n%zu track(s). Levels reach %lld XP.\n", rows.count,
               (long long)MODEL_MASTERY_LEVEL_XP[MODEL_MASTERY_MAX_LEVEL]);
        printf("%zu of them are named; the rest are not guessed. To name one, read its XP "
               "off\n", MODEL_MASTERY_TRACKS_COUNT);
        printf("the game's own mastery screen and find the track holding that number "
               "above.\n");
        free(targets);
        plans_mastery_rows_free(&rows);
        sas4_load_free(&sl);
        return 0;
    }

    MasteryPlanResult planned = plans_mastery_plan(sl.document, targets, target_count, slot);
    if (!planned.ok) {
        printf("%s\n", planned.error);
        mastery_plan_result_free(&planned);
        free(targets);
        plans_mastery_rows_free(&rows);
        sas4_load_free(&sl);
        return 1;
    }

    printf("%s, character slot %d\n", file, slot);
    /* sorted(targets.items()) -- ascending index. */
    for (size_t a = 0; a + 1 < target_count; a++) {
        for (size_t b = 0; b + 1 < target_count - a; b++) {
            if (targets[b].index > targets[b + 1].index) {
                MasteryTarget tmp = targets[b];
                targets[b] = targets[b + 1];
                targets[b + 1] = tmp;
            }
        }
    }
    for (size_t i = 0; i < target_count; i++) {
        const MasteryRow *was = NULL;
        for (size_t r = 0; r < rows.count; r++) {
            if ((int)rows.items[r].index == targets[i].index) {
                was = &rows.items[r];
                break;
            }
        }
        char *xp = was ? row_str(was->xp) : NULL;
        char *lvl = was ? row_str(was->level) : NULL;
        printf("  track %-3d  xp %-9s -> %-9lld   level %s -> %d\n", targets[i].index,
               xp ? xp : "?",
               (long long)MODEL_MASTERY_LEVEL_XP[targets[i].level], lvl ? lvl : "?",
               targets[i].level);
        free(xp);
        free(lvl);
    }

    PendingList pending = {0};
    edit_pending(sl.document, planned.plan.items, planned.plan.count, &pending);
    if (pending.count == 0) {
        printf("\nnothing to change\n");
        edit_pending_free(&pending);
        mastery_plan_result_free(&planned);
        free(targets);
        plans_mastery_rows_free(&rows);
        sas4_load_free(&sl);
        return 0;
    }
    if (dry_run) {
        printf("\n(dry run -- nothing written)\n");
        edit_pending_free(&pending);
        mastery_plan_result_free(&planned);
        free(targets);
        plans_mastery_rows_free(&rows);
        sas4_load_free(&sl);
        return 0;
    }
    if (edit_game_running() && !force) {
        printf("\n%s is running -- close it first\n", GAME_PROCESS);
        edit_pending_free(&pending);
        mastery_plan_result_free(&planned);
        free(targets);
        plans_mastery_rows_free(&rows);
        sas4_load_free(&sl);
        return 1;
    }

    /* apply_edits(args.file, plan) -- the WHOLE plan here, not pending()'s subset. */
    ApplyEditsResult applied = edit_apply(file, backups_dir, planned.plan.items,
                                           planned.plan.count);
    printf("\n%s\n", applied.message);
    edit_pending_free(&pending);
    mastery_plan_result_free(&planned);
    free(targets);
    plans_mastery_rows_free(&rows);
    sas4_load_free(&sl);
    if (!applied.ok) return 1;
    printf("backup   %s\n", applied.has_backup_path ? applied.backup_path : "None");

    SaveLoad after = sas4_load(file);
    ProblemList problems = {0};
    if (after.ok) model_check(after.document, &problems);
    if (problems.count > 0) {
        printf("check    the result is not consistent:\n");
        for (size_t i = 0; i < problems.count; i++) printf("           - %s\n",
                                                            problems.items[i]);
        printf("         the backup above is the way back.\n");
        model_problem_list_free(&problems);
        sas4_load_free(&after);
        return 1;
    }
    model_problem_list_free(&problems);
    sas4_load_free(&after);
    printf("check    clean\n");
    printf("\nThe server keeps its own copy of this profile. A row of maxed masteries is a\n"
           "plain diff on their side, however well the file agrees with itself.\n");
    return 0;
}
