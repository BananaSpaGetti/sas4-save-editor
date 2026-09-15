/*
 * `give` -- task 8 of the port-to-c-sas4-cli plan.
 */
#include "cmd_give.h"
#include "dgdata.h"
#include "edit.h"
#include "json.h"
#include "path.h"
#include "sas4load.h"

#include <stdio.h>
#include <string.h>

#define GAME_PROCESS "SAS4-Win.exe"

int cmd_give(const char *file, const ItemNames *item_names, int64_t item, const char *kind,
             int64_t grade, int64_t bonus, int64_t slot, int slotprofile, bool dry_run,
             bool force, const char *backups_dir) {
    SaveLoad sl = sas4_load(file);
    if (!sl.ok) {
        printf("%s\n", sl.error);
        sas4_load_free(&sl);
        return 1;
    }
    char stored[9], computed[9];
    if (dg_verify(sl.raw, sl.raw_len, stored, computed) != 1 && !force) {
        printf("this file does not verify (%s vs %s) -- refusing to edit it\n", stored,
               computed);
        sas4_load_free(&sl);
        return 1;
    }

    GrantRequest request = {item, kind, grade, bonus, slot};
    GrantPlanResult planned = plans_grant_plan(sl.document, item_names, &request, 1,
                                                slotprofile);
    if (!planned.ok) {
        printf("%s\n", planned.error);
        /* The "list them with" hint is skipped for an EmptySlot failure, which is about the
         * character slot rather than the item id -- matching the isinstance check. */
        if (!planned.empty_slot) {
            printf("list them with:  py sas4.py items --catalog\n");
        }
        grant_plan_result_free(&planned);
        sas4_load_free(&sl);
        return 1;
    }

    /* len(at_path(d, plan[0][0])) -- the Claimed list's length before the edit, and
     * len(plan[0][1]) after it. */
    const char *claimed_path = planned.plan.items[0].path;
    PathAtResult at = path_at(sl.document, claimed_path);
    size_t old_len = (at.ok && at.value && at.value->type == JSON_ARRAY)
                         ? at.value->as.array.count
                         : 0;
    const JsonValue *new_list = planned.plan.items[0].value;
    size_t new_len = (new_list && new_list->type == JSON_ARRAY) ? new_list->as.array.count : 0;

    printf("grant %s  (id %lld, grade %lld, bonus %lld)\n", planned.labels[0],
           (long long)item, (long long)grade, (long long)bonus);
    printf("  Claimed: %zu entries -> %zu\n", old_len, new_len);

    if (dry_run) {
        printf("  (dry run, nothing written)\n");
        grant_plan_result_free(&planned);
        sas4_load_free(&sl);
        return 0;
    }
    if (edit_game_running() && !force) {
        printf("  %s is running -- close it first\n", GAME_PROCESS);
        grant_plan_result_free(&planned);
        sas4_load_free(&sl);
        return 1;
    }

    ApplyEditsResult applied = edit_apply(file, backups_dir, planned.plan.items,
                                           planned.plan.count);
    printf("  backup   %s\n", applied.has_backup_path ? applied.backup_path : "None");
    printf("  %s\n", applied.message);
    grant_plan_result_free(&planned);
    sas4_load_free(&sl);
    return applied.ok ? 0 : 1;
}
