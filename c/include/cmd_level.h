/*
 * `level` and `mastery` -- port of sas4.py's cmd_level and cmd_mastery. Task 7 of the
 * port-to-c-sas4-cli plan. Both write through the core's apply_edits (edit_apply), unlike
 * `set`, which splices inline.
 */
#ifndef SAS4_CMD_LEVEL_H
#define SAS4_CMD_LEVEL_H

#include <stdbool.h>

int cmd_level(const char *file, int level, int slot, bool dry_run, bool force,
              const char *backups_dir);

/* `set_spec` is --set's raw "3=5,7=2" string (NULL if not given); all_level applies to every
 * track when all_given. With neither, this prints the mastery table and writes nothing. */
int cmd_mastery(const char *file, const char *set_spec, bool all_given, int all_level,
                int slot, bool dry_run, bool force, const char *backups_dir);

#endif
