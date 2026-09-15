/*
 * `graft` -- port of sas4.py's cmd_graft. Task 13 of the port-to-c-sas4-cli plan.
 *
 * Copies named fields from another save into this one while leaving identity alone, so the
 * result is your account with someone else's progress. Two refusals carry that promise and
 * both are reproduced: a field whose own leaf name is an identity field is skipped, and so
 * is one whose VALUE contains an identity field anywhere nested inside it -- leaf-name
 * matching alone would let `--fields Version` copy Version.link straight through.
 *
 * The Python has no source-equals-destination check and no liveness check on the source,
 * and none is added here; the port reproduces what is there.
 */
#ifndef SAS4_CMD_GRAFT_H
#define SAS4_CMD_GRAFT_H

#include <stdbool.h>

int cmd_graft(const char *file, const char *source, const char *fields, bool apply,
              bool force, const char *backups_dir);

#endif
