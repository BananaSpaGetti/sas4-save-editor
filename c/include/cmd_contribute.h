/*
 * `contribute` -- port of sas4.py's contribution_report, scan_report_for_ids and
 * cmd_contribute. Tasks 14 and 15 of the port-to-c-sas4-cli plan.
 *
 * The report is built as an explicit sequence of NAMED FIELD READS, never as a walk over
 * the document with a blacklist. That is the whole design: a field a later patch adds
 * cannot appear in it by default, because nothing here asks the document what it holds.
 * The schema section is the single sanctioned exception, and it emits path names and type
 * names only -- never a value. Keep both properties if you touch this file.
 *
 * Nothing here sends anything anywhere, and it must not learn how to.
 */
#ifndef SAS4_CMD_CONTRIBUTE_H
#define SAS4_CMD_CONTRIBUTE_H

#include "json.h"

#include <stdbool.h>
#include <stddef.h>

/* contribution_report(document, slot) as one malloc'd, NUL-terminated string (no trailing
 * newline, matching "\n".join). NULL if the document's shape makes the Python raise. */
char *contribute_report(const JsonValue *document, int slot);

/* One thing in a report that is shaped like an identifier. */
typedef struct {
    const char *description; /* borrowed: a literal from ID_PATTERNS */
    char *match;             /* malloc'd: the text that matched */
} LeakedId;

/* scan_report_for_ids(text). The four patterns are implemented directly -- they are two
 * character-class runs and two literals, so a regex engine would be a dependency bought
 * for nothing. THE THRESHOLDS ARE LOAD-BEARING AND MUST NOT BE LOWERED: 15 digits rather
 * than 9 because a real profile holds fourteen legitimate long timestamps, and 24 hex
 * characters because Version/analytics wraps a 32-hex id. */
void contribute_scan(const char *text, LeakedId **out, size_t *out_count);
void contribute_scan_free(LeakedId *found, size_t count);

int cmd_contribute(const char *file, int slot, bool print_only, const char *data_dir);

#endif
