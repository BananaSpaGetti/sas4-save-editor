/*
 * `give` -- port of sas4.py's cmd_give (and give_plan, its single-request wrapper around
 * grant_plan). Task 8 of the port-to-c-sas4-cli plan.
 */
#ifndef SAS4_CMD_GIVE_H
#define SAS4_CMD_GIVE_H

#include "plans.h"

#include <stdbool.h>

int cmd_give(const char *file, const ItemNames *item_names, int64_t item, const char *kind,
             int64_t grade, int64_t bonus, int64_t slot, int slotprofile, bool dry_run,
             bool force, const char *backups_dir);

#endif
