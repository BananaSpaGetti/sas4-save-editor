/*
 * `session` -- port of sas4.py's cmd_session. Task 12 of the port-to-c-sas4-cli plan.
 *
 * current.session is a DGDATA file like the save, carrying the account's sessionID and
 * nkapiID. They are printed as lengths, never in full: this file is worth the same care as
 * a password, and the redaction is the whole point of the read path rather than a nicety.
 * Only the keys in SECRET_KEYS are hidden, and only where they sit directly under a
 * top-level section -- the Python does exactly that much and no more, so this does too.
 *
 * This is the only command in the port that reads stdin: editing a credential key asks for
 * confirmation unless --yes was passed.
 */
#ifndef SAS4_CMD_SESSION_H
#define SAS4_CMD_SESSION_H

#include <stdbool.h>

/* `set_expr` is NULL (or empty) for the read path, else "key=value". `backups_dir` is
 * sas4.py's BACKUPS, where the pre-write copy goes. */
int cmd_session(const char *path, const char *set_expr, bool yes, bool force,
                const char *backups_dir);

#endif
