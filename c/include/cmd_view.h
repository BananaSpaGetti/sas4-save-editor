/*
 * `view` -- port of sas4.py's cmd_view. Task 3 of the port-to-c-sas4-cli plan.
 */
#ifndef SAS4_CMD_VIEW_H
#define SAS4_CMD_VIEW_H

#include "plans.h"

/* `sections`/`sections_count` are the --section values in the order given (empty means "all
 * seven, in sas4.py's own order" -- matches `args.section or [...]`). `item_names` may be
 * NULL (no item cache loaded yet); Equipment/Weapons then print bare ids and the
 * "run `items`" note, matching item_names() returning {} when the cache file is absent.
 * Returns the process exit code (0, or 1 if the named profile slot is not loaded). */
int cmd_view(const char *file, int slot, const char *const *sections, int sections_count,
             const ItemNames *item_names);

#endif
