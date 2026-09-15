/*
 * `set` -- port of sas4.py's cmd_set. Task 6 of the port-to-c-sas4-cli plan.
 *
 * cmd_set does NOT go through apply_edits: the Python spells the whole sequence out inline
 * (refuse while the game runs, refuse a file that does not verify, resolve the path, coerce
 * the value, back up, splice the bytes, encode, verify, write, re-read and verify again) and
 * prints as it goes, so this mirrors that rather than the plan-shaped apply_edits path the
 * level/mastery/give commands use.
 */
#ifndef SAS4_CMD_SET_H
#define SAS4_CMD_SET_H

#include <stdbool.h>

int cmd_set(const char *file, const char *path, const char *value, bool dry_run, bool force,
            const char *backups_dir);

#endif
