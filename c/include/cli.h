/*
 * A hand-rolled reproduction of sas4.py's argparse-based command line, byte-for-byte on
 * --help, usage lines and error messages -- verified against captured goldens rather than
 * against a re-implementation of argparse's HelpFormatter (see the port-to-c-sas4-cli plan,
 * task 1). PROG is hardcoded to "sas4.py" rather than derived from argv[0] (Decision 9 of
 * that plan): this makes every diff against the Python oracle byte-identical regardless of
 * what the compiled binary is actually named or invoked as, which is what the plan's
 * verification method (byte diff, not "looks right") actually needs.
 *
 * Help/usage text is width-frozen at COLUMNS=80 (Decision 10): argparse wraps to the live
 * terminal width, but the C build has no such formatter, so its output matches the Python's
 * only when the Python side is captured with COLUMNS=80 too -- the differential harness
 * (task 16) must pin COLUMNS=80 before invoking the oracle.
 */
#ifndef SAS4_CLI_H
#define SAS4_CLI_H

#include <stdbool.h>

/* argparse's action="append" has no cap and allows the same choice repeated -- there are
 * only 7 distinct --section values, but nothing stops `--section skills` 64 times, and
 * silently truncating would diverge from the oracle rather than merely being an unlikely
 * invocation. 64 is a generous, arbitrary bound chosen to never be reached in practice while
 * still being a fixed-size array rather than a dynamic one. */
#define CLI_SECTION_MAX 64

typedef struct {
    const char *file;    /* --file; NULL if not given (caller resolves the live-profile default) */
    const char *command; /* subcommand name, or NULL if none was given on the command line */

    /* view */
    int view_slot;
    const char *view_sections[CLI_SECTION_MAX];
    int view_sections_count;

    /* list */
    const char *list_grep;
    const char *list_type;
    const char *list_under;

    /* get */
    const char *get_path;

    /* set */
    const char *set_path;
    const char *set_value;
    bool set_dry_run;
    bool set_force;

    /* give */
    long give_item;
    const char *give_kind;
    long give_grade;
    long give_bonus;
    long give_slot;
    long give_slotprofile;
    bool give_dry_run;
    bool give_force;

    /* mastery */
    const char *mastery_set;    /* NULL if --set not given */
    bool mastery_all_given;
    long mastery_all;
    long mastery_slot;
    bool mastery_dry_run;
    bool mastery_force;

    /* contribute */
    long contribute_slot;
    bool contribute_print_only;

    /* level */
    long level_value;
    long level_slot;
    bool level_dry_run;
    bool level_force;

    /* items */
    bool items_catalog_given; /* --catalog present at all */
    const char *items_catalog; /* value if given with one, else the const default */

    /* decode */
    const char *decode_out; /* NULL if not given (nargs="?") */

    /* encode */
    const char *encode_json;
    const char *encode_out;

    /* watch */
    bool watch_archive;

    /* session */
    const char *session_session; /* NULL means "use the default" */
    const char *session_set;     /* NULL if --set not given */
    bool session_yes;
    bool session_force;

    /* graft */
    const char *graft_source;
    const char *graft_fields; /* NULL if --fields not given */
    bool graft_apply;
    bool graft_force;
} CliArgs;

typedef enum {
    CLI_CONTINUE, /* out is filled in; dispatch to the command (out->command may be NULL) */
    CLI_EXIT,     /* --help was printed, or an error was printed; exit(*exit_code) immediately */
} CliOutcome;

/* argv excludes the program name (pass argv+1, argc-1 from main). Prints to stdout (--help)
 * or stderr (an error), exactly matching sas4.py's argparse, on any CLI_EXIT outcome. */
CliOutcome cli_parse(int argc, char **argv, CliArgs *out, int *exit_code);

#endif
