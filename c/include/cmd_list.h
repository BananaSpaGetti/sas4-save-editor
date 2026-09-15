/*
 * `list` and `kinds` -- port of sas4.py's cmd_list and cmd_kinds. Task 4 of the
 * port-to-c-sas4-cli plan.
 */
#ifndef SAS4_CMD_LIST_H
#define SAS4_CMD_LIST_H

/* Each filter may be NULL, meaning "not given": --grep is a case-insensitive substring test
 * on the path, --type is one of bool/int/str/float/null, --under restricts to a subtree by
 * case-insensitive path prefix. Returns the process exit code. */
int cmd_list(const char *file, const char *grep, const char *type, const char *under);

int cmd_kinds(const char *file);

#endif
