#include "cli.h"
#include "cli_help_data.h"
#include "model.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROG "sas4.py" /* hardcoded -- see cli.h's top comment (Decision 9) */

/* --- small dynamic list of "extra" tokens argparse would report as unrecognized ---------- */

typedef struct {
    char **items;
    int count;
    int cap;
} StrList;

static void strlist_push(StrList *l, const char *s) {
    if (l->count == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 4;
        l->items = realloc(l->items, (size_t)l->cap * sizeof(char *));
    }
    l->items[l->count++] = (char *)s;
}

/* --- token classification, matching argparse's own rules --------------------------------- */

/* argparse treats a token starting with '-' as an option string UNLESS it is exactly a
 * negative number (^-\d+$ or ^-\d*\.\d+$) and the parser defines no option strings that look
 * like negative numbers -- true here, none of sas4.py's flags do. */
static bool looks_like_negative_number(const char *s) {
    if (s[0] != '-' || s[1] == '\0') return false;
    const char *p = s + 1;
    bool any_digit = false, seen_dot = false;
    for (; *p; p++) {
        if (*p >= '0' && *p <= '9') { any_digit = true; continue; }
        if (*p == '.' && !seen_dot) { seen_dot = true; continue; }
        return false;
    }
    return any_digit;
}

static bool looks_like_option(const char *s) {
    if (s[0] != '-' || s[1] == '\0') return false;
    if (looks_like_negative_number(s)) return false;
    return true;
}

/* --- error printing ------------------------------------------------------------------------ */

static int usage_error(const char *usage, const char *errprog, const char *fmt, ...) {
    fputs(usage, stderr);
    fprintf(stderr, "%s: error: ", errprog);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    return 2;
}

/* Renders "(choose from a, b, c)" the way argparse does for an invalid choice. */
static void format_choices(char *buf, size_t cap, const char *const *choices, int n) {
    size_t used = 0;
    used += (size_t)snprintf(buf + used, cap - used, "(choose from ");
    for (int i = 0; i < n; i++) {
        used += (size_t)snprintf(buf + used, cap - used, "%s%s", i ? ", " : "", choices[i]);
    }
    snprintf(buf + used, cap - used, ")");
}

static bool in_choices(const char *value, const char *const *choices, int n) {
    for (int i = 0; i < n; i++)
        if (strcmp(value, choices[i]) == 0) return true;
    return false;
}

/* Python's int(str) for argparse's type=int -- model_py_parse_int is the shared
 * implementation (mastery --set's "3=5" pieces go through the same int()). */
static bool py_parse_int(const char *s, long *out) {
    return model_py_parse_int(s, out);
}

/* --- one subcommand's parse: shared machinery -------------------------------------------- */

typedef struct {
    const char *name;   /* e.g. "path", "item" -- used in "the following arguments are required" */
    bool optional;       /* nargs="?" */
    bool is_int;
    char **str_slot;      /* exactly one of str_slot/int_slot is non-NULL */
    long *int_slot;
    bool *given_slot;    /* set true once filled; NULL if not tracked */
} PosSpec;

/* Consumes positionals from `vals` (already-collected non-option tokens, in order) into
 * pos[], filling required ones first; extras beyond declared positionals are pushed to
 * `extra_positionals`. Returns NULL on success, or a malloc'd error message body (just the
 * missing-names list) on a required positional being unfilled -- caller wraps it. */
static char *fill_positionals(PosSpec *pos, int n_pos, StrList *vals, int vals_used_start,
                               StrList *extra_positionals, const char *usage,
                               const char *errprog, int *out_exit) {
    int vi = vals_used_start;
    for (int i = 0; i < n_pos; i++) {
        if (vi >= vals->count) continue; /* leave unfilled; checked below */
        const char *tok = vals->items[vi++];
        if (pos[i].is_int) {
            long v;
            if (!py_parse_int(tok, &v)) {
                *out_exit = usage_error(usage, errprog, "argument %s: invalid int value: '%s'",
                                         pos[i].name, tok);
                return (char *)(intptr_t)-1; /* sentinel: already errored */
            }
            *pos[i].int_slot = v;
        } else {
            *pos[i].str_slot = (char *)tok;
        }
        if (pos[i].given_slot) *pos[i].given_slot = true;
    }
    for (; vi < vals->count; vi++) strlist_push(extra_positionals, vals->items[vi]);

    StrList missing = {0};
    for (int i = 0; i < n_pos; i++) {
        if (pos[i].optional) continue;
        if (pos[i].given_slot && !*pos[i].given_slot) strlist_push(&missing, pos[i].name);
    }
    if (missing.count == 0) { free(missing.items); return NULL; }
    size_t cap = 256;
    char *buf = malloc(cap);
    size_t used = 0;
    for (int i = 0; i < missing.count; i++)
        used += (size_t)snprintf(buf + used, cap - used, "%s%s", i ? ", " : "", missing.items[i]);
    free(missing.items);
    *out_exit = usage_error(usage, errprog, "the following arguments are required: %s", buf);
    free(buf);
    return (char *)(intptr_t)-1;
}

#define ERRORED ((char *)(intptr_t)-1)

/* --- per-subcommand parsers --------------------------------------------------------------- */
/* Each returns true to continue (out filled), false if it already printed and set *exit_code. */

typedef bool (*SubParseFn)(StrList *argv, CliArgs *out, StrList *sub_extras,
                            int *exit_code);

static bool parse_where(StrList *a, CliArgs *out, StrList *sub_extras, int *exit_code) {
    (void)out;
    StrList extras = {0};
    for (int i = 0; i < a->count; i++) {
        if (strcmp(a->items[i], "-h") == 0 || strcmp(a->items[i], "--help") == 0) {
            fputs(CLI_HELP_WHERE, stdout);
            *exit_code = 0;
            return false;
        }
        strlist_push(&extras, a->items[i]);
    }
    for (int i = 0; i < extras.count; i++) strlist_push(sub_extras, extras.items[i]);
    return true;
}

/* Generic scan: recognizes -h/--help (prints subcommand help, exit 0), a set of flag/value
 * options via a small hand-written table per command below, collects positionals in order
 * and everything else as extras (bubbled to the caller for the top-level unrecognized
 * check). Kept as separate per-command bodies (not one data-driven engine) so each one stays
 * readable against the exact sas4.py subparser it mirrors. */

static bool want_value(StrList *a, int *i, const char *usage, const char *errprog,
                        const char *optname, const char **out_val, int *exit_code) {
    if (*i + 1 >= a->count || looks_like_option(a->items[*i + 1])) {
        *exit_code = usage_error(usage, errprog, "argument %s: expected one argument", optname);
        return false;
    }
    (*i)++;
    *out_val = a->items[*i];
    return true;
}

static bool want_int(StrList *a, int *i, const char *usage, const char *errprog,
                      const char *optname, long *out_val, int *exit_code) {
    const char *sval;
    if (!want_value(a, i, usage, errprog, optname, &sval, exit_code)) return false;
    if (!py_parse_int(sval, out_val)) {
        *exit_code = usage_error(usage, errprog, "argument %s: invalid int value: '%s'",
                                  optname, sval);
        return false;
    }
    return true;
}

static bool want_choice(StrList *a, int *i, const char *usage, const char *errprog,
                         const char *optname, const char *const *choices, int n_choices,
                         const char **out_val, int *exit_code) {
    const char *sval;
    if (!want_value(a, i, usage, errprog, optname, &sval, exit_code)) return false;
    if (!in_choices(sval, choices, n_choices)) {
        char cbuf[256];
        format_choices(cbuf, sizeof cbuf, choices, n_choices);
        *exit_code = usage_error(usage, errprog, "argument %s: invalid choice: '%s' %s",
                                  optname, sval, cbuf);
        return false;
    }
    *out_val = sval;
    return true;
}

static bool parse_view(StrList *a, CliArgs *out, StrList *sub_extras, int *exit_code) {
    static const char *SECTIONS[] = {"identity", "currency", "skills", "equipment", "weapons",
                                      "boxes", "global"};
    const char *usage = CLI_USAGE_VIEW;
    const char *errprog = PROG " view";
    StrList extras = {0};
    for (int i = 0; i < a->count; i++) {
        const char *tok = a->items[i];
        if (strcmp(tok, "-h") == 0 || strcmp(tok, "--help") == 0) {
            fputs(CLI_HELP_VIEW, stdout);
            *exit_code = 0;
            return false;
        } else if (strcmp(tok, "--slot") == 0) {
            long v;
            if (!want_int(a, &i, usage, errprog, "--slot", &v, exit_code)) return false;
            out->view_slot = (int)v;
        } else if (strcmp(tok, "--section") == 0) {
            const char *v = NULL;
            if (!want_choice(a, &i, usage, errprog, "--section", SECTIONS, 7, &v, exit_code))
                return false;
            if (out->view_sections_count < CLI_SECTION_MAX)
                out->view_sections[out->view_sections_count++] = v;
        } else {
            strlist_push(&extras, tok);
        }
    }
    for (int i = 0; i < extras.count; i++) strlist_push(sub_extras, extras.items[i]);
    return true;
}

static bool parse_list(StrList *a, CliArgs *out, StrList *sub_extras, int *exit_code) {
    static const char *TYPES[] = {"bool", "float", "int", "null", "str"};
    const char *usage = CLI_USAGE_LIST;
    const char *errprog = PROG " list";
    StrList extras = {0};
    for (int i = 0; i < a->count; i++) {
        const char *tok = a->items[i];
        if (strcmp(tok, "-h") == 0 || strcmp(tok, "--help") == 0) {
            fputs(CLI_HELP_LIST, stdout);
            *exit_code = 0;
            return false;
        } else if (strcmp(tok, "--grep") == 0) {
            if (!want_value(a, &i, usage, errprog, "--grep", &out->list_grep, exit_code))
                return false;
        } else if (strcmp(tok, "--type") == 0) {
            if (!want_choice(a, &i, usage, errprog, "--type", TYPES, 5, &out->list_type,
                              exit_code))
                return false;
        } else if (strcmp(tok, "--under") == 0) {
            if (!want_value(a, &i, usage, errprog, "--under", &out->list_under, exit_code))
                return false;
        } else {
            strlist_push(&extras, tok);
        }
    }
    for (int i = 0; i < extras.count; i++) strlist_push(sub_extras, extras.items[i]);
    return true;
}

static bool parse_kinds(StrList *a, CliArgs *out, StrList *sub_extras, int *exit_code) {
    (void)out;
    for (int i = 0; i < a->count; i++) {
        if (strcmp(a->items[i], "-h") == 0 || strcmp(a->items[i], "--help") == 0) {
            fputs(CLI_HELP_KINDS, stdout);
            *exit_code = 0;
            return false;
        }
    }
    return parse_where(a, out, sub_extras, exit_code); /* same "no options" shape */
}

static bool parse_verify(StrList *a, CliArgs *out, StrList *sub_extras, int *exit_code) {
    for (int i = 0; i < a->count; i++) {
        if (strcmp(a->items[i], "-h") == 0 || strcmp(a->items[i], "--help") == 0) {
            fputs(CLI_HELP_VERIFY, stdout);
            *exit_code = 0;
            return false;
        }
    }
    return parse_where(a, out, sub_extras, exit_code);
}

static bool simple_positional_only(StrList *a, CliArgs *out, StrList *sub_extras,
                                    int *exit_code, const char *help,
                                    const char *usage, const char *errprog, PosSpec *pos,
                                    int n_pos) {
    (void)out;
    StrList vals = {0}, extras = {0};
    for (int i = 0; i < a->count; i++) {
        const char *tok = a->items[i];
        if (strcmp(tok, "-h") == 0 || strcmp(tok, "--help") == 0) {
            fputs(help, stdout);
            *exit_code = 0;
            return false;
        } else if (looks_like_option(tok)) {
            strlist_push(&extras, tok);
        } else {
            strlist_push(&vals, tok);
        }
    }
    char *err = fill_positionals(pos, n_pos, &vals, 0, &extras, usage, errprog, exit_code);
    if (err == ERRORED) return false;
    for (int i = 0; i < extras.count; i++) strlist_push(sub_extras, extras.items[i]);
    return true;
}

static bool parse_get(StrList *a, CliArgs *out, StrList *sub_extras, int *exit_code) {
    bool given = false;
    PosSpec pos[] = {{"path", false, false, (char **)&out->get_path, NULL, &given}};
    return simple_positional_only(a, out, sub_extras, exit_code, CLI_HELP_GET, CLI_USAGE_GET,
                                   PROG " get", pos, 1);
}

static bool parse_set(StrList *a, CliArgs *out, StrList *sub_extras, int *exit_code) {
    const char *usage = CLI_USAGE_SET;
    const char *errprog = PROG " set";
    StrList vals = {0}, extras = {0};
    for (int i = 0; i < a->count; i++) {
        const char *tok = a->items[i];
        if (strcmp(tok, "-h") == 0 || strcmp(tok, "--help") == 0) {
            fputs(CLI_HELP_SET, stdout);
            *exit_code = 0;
            return false;
        } else if (strcmp(tok, "--dry-run") == 0) {
            out->set_dry_run = true;
        } else if (strcmp(tok, "--force") == 0) {
            out->set_force = true;
        } else if (looks_like_option(tok)) {
            strlist_push(&extras, tok);
        } else {
            strlist_push(&vals, tok);
        }
    }
    bool have_path = false, have_value = false;
    PosSpec pos[] = {
        {"path", false, false, (char **)&out->set_path, NULL, &have_path},
        {"value", false, false, (char **)&out->set_value, NULL, &have_value},
    };
    char *err = fill_positionals(pos, 2, &vals, 0, &extras, usage, errprog, exit_code);
    if (err == ERRORED) return false;
    for (int i = 0; i < extras.count; i++) strlist_push(sub_extras, extras.items[i]);
    return true;
}

static bool parse_give(StrList *a, CliArgs *out, StrList *sub_extras, int *exit_code) {
    static const char *KINDS[] = {"auto", "weapon", "equipment"};
    const char *usage = CLI_USAGE_GIVE;
    const char *errprog = PROG " give";
    out->give_kind = "auto";
    StrList vals = {0}, extras = {0};
    for (int i = 0; i < a->count; i++) {
        const char *tok = a->items[i];
        if (strcmp(tok, "-h") == 0 || strcmp(tok, "--help") == 0) {
            fputs(CLI_HELP_GIVE, stdout);
            *exit_code = 0;
            return false;
        } else if (strcmp(tok, "--kind") == 0) {
            if (!want_choice(a, &i, usage, errprog, "--kind", KINDS, 3, &out->give_kind,
                              exit_code))
                return false;
        } else if (strcmp(tok, "--grade") == 0) {
            if (!want_int(a, &i, usage, errprog, "--grade", &out->give_grade, exit_code))
                return false;
        } else if (strcmp(tok, "--bonus") == 0) {
            if (!want_int(a, &i, usage, errprog, "--bonus", &out->give_bonus, exit_code))
                return false;
        } else if (strcmp(tok, "--slot") == 0) {
            if (!want_int(a, &i, usage, errprog, "--slot", &out->give_slot, exit_code))
                return false;
        } else if (strcmp(tok, "--slotprofile") == 0) {
            if (!want_int(a, &i, usage, errprog, "--slotprofile", &out->give_slotprofile,
                           exit_code))
                return false;
        } else if (strcmp(tok, "--dry-run") == 0) {
            out->give_dry_run = true;
        } else if (strcmp(tok, "--force") == 0) {
            out->give_force = true;
        } else if (looks_like_option(tok)) {
            strlist_push(&extras, tok);
        } else {
            strlist_push(&vals, tok);
        }
    }
    bool have_item = false;
    PosSpec pos[] = {{"item", false, true, NULL, &out->give_item, &have_item}};
    char *err = fill_positionals(pos, 1, &vals, 0, &extras, usage, errprog, exit_code);
    if (err == ERRORED) return false;
    for (int i = 0; i < extras.count; i++) strlist_push(sub_extras, extras.items[i]);
    return true;
}

static bool parse_mastery(StrList *a, CliArgs *out, StrList *sub_extras, int *exit_code) {
    const char *usage = CLI_USAGE_MASTERY;
    const char *errprog = PROG " mastery";
    StrList extras = {0};
    bool saw_set = false, saw_all = false;
    for (int i = 0; i < a->count; i++) {
        const char *tok = a->items[i];
        if (strcmp(tok, "-h") == 0 || strcmp(tok, "--help") == 0) {
            fputs(CLI_HELP_MASTERY, stdout);
            *exit_code = 0;
            return false;
        } else if (strcmp(tok, "--set") == 0) {
            if (saw_all) {
                *exit_code = usage_error(usage, errprog,
                                          "argument --set: not allowed with argument --all");
                return false;
            }
            if (!want_value(a, &i, usage, errprog, "--set", &out->mastery_set, exit_code))
                return false;
            saw_set = true;
        } else if (strcmp(tok, "--all") == 0) {
            if (saw_set) {
                *exit_code = usage_error(usage, errprog,
                                          "argument --all: not allowed with argument --set");
                return false;
            }
            if (!want_int(a, &i, usage, errprog, "--all", &out->mastery_all, exit_code))
                return false;
            out->mastery_all_given = true;
            saw_all = true;
        } else if (strcmp(tok, "--slot") == 0) {
            if (!want_int(a, &i, usage, errprog, "--slot", &out->mastery_slot, exit_code))
                return false;
        } else if (strcmp(tok, "--dry-run") == 0) {
            out->mastery_dry_run = true;
        } else if (strcmp(tok, "--force") == 0) {
            out->mastery_force = true;
        } else {
            strlist_push(&extras, tok);
        }
    }
    for (int i = 0; i < extras.count; i++) strlist_push(sub_extras, extras.items[i]);
    return true;
}

static bool parse_contribute(StrList *a, CliArgs *out, StrList *sub_extras, int *exit_code) {
    const char *usage = CLI_USAGE_CONTRIBUTE;
    const char *errprog = PROG " contribute";
    StrList extras = {0};
    for (int i = 0; i < a->count; i++) {
        const char *tok = a->items[i];
        if (strcmp(tok, "-h") == 0 || strcmp(tok, "--help") == 0) {
            fputs(CLI_HELP_CONTRIBUTE, stdout);
            *exit_code = 0;
            return false;
        } else if (strcmp(tok, "--slot") == 0) {
            if (!want_int(a, &i, usage, errprog, "--slot", &out->contribute_slot, exit_code))
                return false;
        } else if (strcmp(tok, "--print") == 0) {
            out->contribute_print_only = true;
        } else {
            strlist_push(&extras, tok);
        }
    }
    for (int i = 0; i < extras.count; i++) strlist_push(sub_extras, extras.items[i]);
    return true;
}

static bool parse_level(StrList *a, CliArgs *out, StrList *sub_extras, int *exit_code) {
    const char *usage = CLI_USAGE_LEVEL;
    const char *errprog = PROG " level";
    StrList vals = {0}, extras = {0};
    for (int i = 0; i < a->count; i++) {
        const char *tok = a->items[i];
        if (strcmp(tok, "-h") == 0 || strcmp(tok, "--help") == 0) {
            fputs(CLI_HELP_LEVEL, stdout);
            *exit_code = 0;
            return false;
        } else if (strcmp(tok, "--slot") == 0) {
            if (!want_int(a, &i, usage, errprog, "--slot", &out->level_slot, exit_code))
                return false;
        } else if (strcmp(tok, "--dry-run") == 0) {
            out->level_dry_run = true;
        } else if (strcmp(tok, "--force") == 0) {
            out->level_force = true;
        } else if (looks_like_option(tok)) {
            strlist_push(&extras, tok);
        } else {
            strlist_push(&vals, tok);
        }
    }
    bool have_level = false;
    PosSpec pos[] = {{"level", false, true, NULL, &out->level_value, &have_level}};
    char *err = fill_positionals(pos, 1, &vals, 0, &extras, usage, errprog, exit_code);
    if (err == ERRORED) return false;
    for (int i = 0; i < extras.count; i++) strlist_push(sub_extras, extras.items[i]);
    return true;
}

static bool parse_items(StrList *a, CliArgs *out, StrList *sub_extras, int *exit_code) {
    StrList extras = {0};
    for (int i = 0; i < a->count; i++) {
        const char *tok = a->items[i];
        if (strcmp(tok, "-h") == 0 || strcmp(tok, "--help") == 0) {
            fputs(CLI_HELP_ITEMS, stdout);
            *exit_code = 0;
            return false;
        } else if (strcmp(tok, "--catalog") == 0) {
            out->items_catalog_given = true;
            if (i + 1 < a->count && !looks_like_option(a->items[i + 1])) {
                i++;
                out->items_catalog = a->items[i];
            } else {
                out->items_catalog = NULL; /* caller substitutes the const default path */
            }
        } else {
            strlist_push(&extras, tok);
        }
    }
    for (int i = 0; i < extras.count; i++) strlist_push(sub_extras, extras.items[i]);
    return true;
}

static bool parse_decode(StrList *a, CliArgs *out, StrList *sub_extras, int *exit_code) {
    bool given = false;
    PosSpec pos[] = {{"out", true, false, (char **)&out->decode_out, NULL, &given}};
    return simple_positional_only(a, out, sub_extras, exit_code, CLI_HELP_DECODE,
                                   CLI_USAGE_DECODE, PROG " decode", pos, 1);
}

static bool parse_encode(StrList *a, CliArgs *out, StrList *sub_extras, int *exit_code) {
    bool have_json = false, have_out = false;
    PosSpec pos[] = {
        {"json", false, false, (char **)&out->encode_json, NULL, &have_json},
        {"out", false, false, (char **)&out->encode_out, NULL, &have_out},
    };
    return simple_positional_only(a, out, sub_extras, exit_code, CLI_HELP_ENCODE,
                                   CLI_USAGE_ENCODE, PROG " encode", pos, 2);
}

static bool parse_watch(StrList *a, CliArgs *out, StrList *sub_extras, int *exit_code) {
    const char *usage = CLI_USAGE_WATCH;
    StrList extras = {0};
    for (int i = 0; i < a->count; i++) {
        const char *tok = a->items[i];
        if (strcmp(tok, "-h") == 0 || strcmp(tok, "--help") == 0) {
            fputs(CLI_HELP_WATCH, stdout);
            *exit_code = 0;
            return false;
        } else if (strcmp(tok, "--archive") == 0) {
            out->watch_archive = true;
        } else {
            strlist_push(&extras, tok);
        }
    }
    (void)usage;
    for (int i = 0; i < extras.count; i++) strlist_push(sub_extras, extras.items[i]);
    return true;
}

static bool parse_session(StrList *a, CliArgs *out, StrList *sub_extras, int *exit_code) {
    const char *usage = CLI_USAGE_SESSION;
    const char *errprog = PROG " session";
    StrList extras = {0};
    for (int i = 0; i < a->count; i++) {
        const char *tok = a->items[i];
        if (strcmp(tok, "-h") == 0 || strcmp(tok, "--help") == 0) {
            fputs(CLI_HELP_SESSION, stdout);
            *exit_code = 0;
            return false;
        } else if (strcmp(tok, "--session") == 0) {
            if (!want_value(a, &i, usage, errprog, "--session", &out->session_session,
                             exit_code))
                return false;
        } else if (strcmp(tok, "--set") == 0) {
            if (!want_value(a, &i, usage, errprog, "--set", &out->session_set, exit_code))
                return false;
        } else if (strcmp(tok, "--yes") == 0) {
            out->session_yes = true;
        } else if (strcmp(tok, "--force") == 0) {
            out->session_force = true;
        } else {
            strlist_push(&extras, tok);
        }
    }
    for (int i = 0; i < extras.count; i++) strlist_push(sub_extras, extras.items[i]);
    return true;
}

static bool parse_graft(StrList *a, CliArgs *out, StrList *sub_extras, int *exit_code) {
    const char *usage = CLI_USAGE_GRAFT;
    const char *errprog = PROG " graft";
    StrList vals = {0}, extras = {0};
    for (int i = 0; i < a->count; i++) {
        const char *tok = a->items[i];
        if (strcmp(tok, "-h") == 0 || strcmp(tok, "--help") == 0) {
            fputs(CLI_HELP_GRAFT, stdout);
            *exit_code = 0;
            return false;
        } else if (strcmp(tok, "--fields") == 0) {
            if (!want_value(a, &i, usage, errprog, "--fields", &out->graft_fields, exit_code))
                return false;
        } else if (strcmp(tok, "--apply") == 0) {
            out->graft_apply = true;
        } else if (strcmp(tok, "--force") == 0) {
            out->graft_force = true;
        } else if (looks_like_option(tok)) {
            strlist_push(&extras, tok);
        } else {
            strlist_push(&vals, tok);
        }
    }
    bool have_source = false;
    PosSpec pos[] = {{"source", false, false, (char **)&out->graft_source, NULL, &have_source}};
    char *err = fill_positionals(pos, 1, &vals, 0, &extras, usage, errprog, exit_code);
    if (err == ERRORED) return false;
    for (int i = 0; i < extras.count; i++) strlist_push(sub_extras, extras.items[i]);
    return true;
}

/* --- top-level dispatch -------------------------------------------------------------------- */

static const char *const COMMAND_NAMES[] = {
    "where", "view", "list", "kinds", "get", "set", "give", "mastery", "contribute",
    "level", "items", "verify", "decode", "encode", "watch", "session", "graft",
};
#define N_COMMANDS 17

CliOutcome cli_parse(int argc, char **argv, CliArgs *out, int *exit_code) {
    memset(out, 0, sizeof *out);
    out->give_slot = 2;

    StrList top_extras = {0};
    int i = 0;
    for (; i < argc; i++) {
        const char *tok = argv[i];
        if (strcmp(tok, "-h") == 0 || strcmp(tok, "--help") == 0) {
            fputs(CLI_HELP_TOP, stdout);
            *exit_code = 0;
            return CLI_EXIT;
        } else if (strcmp(tok, "--file") == 0) {
            if (i + 1 >= argc || looks_like_option(argv[i + 1])) {
                *exit_code = usage_error(CLI_USAGE_TOP, PROG,
                                          "argument --file: expected one argument");
                return CLI_EXIT;
            }
            out->file = argv[++i];
        } else if (strncmp(tok, "--file=", 7) == 0) {
            out->file = tok + 7;
        } else if (looks_like_option(tok)) {
            strlist_push(&top_extras, tok);
        } else {
            /* first non-option token: the subcommand name */
            if (!in_choices(tok, COMMAND_NAMES, N_COMMANDS)) {
                char cbuf[512];
                format_choices(cbuf, sizeof cbuf, COMMAND_NAMES, N_COMMANDS);
                *exit_code = usage_error(CLI_USAGE_TOP, PROG,
                                          "argument command: invalid choice: '%s' %s", tok,
                                          cbuf);
                return CLI_EXIT;
            }
            out->command = tok;
            i++;
            break;
        }
    }

    if (out->command) {
        StrList rest = {0};
        for (; i < argc; i++) strlist_push(&rest, argv[i]);

        SubParseFn fn = NULL;
        if (strcmp(out->command, "where") == 0) fn = parse_where;
        else if (strcmp(out->command, "view") == 0) fn = parse_view;
        else if (strcmp(out->command, "list") == 0) fn = parse_list;
        else if (strcmp(out->command, "kinds") == 0) fn = parse_kinds;
        else if (strcmp(out->command, "get") == 0) fn = parse_get;
        else if (strcmp(out->command, "set") == 0) fn = parse_set;
        else if (strcmp(out->command, "give") == 0) fn = parse_give;
        else if (strcmp(out->command, "mastery") == 0) fn = parse_mastery;
        else if (strcmp(out->command, "contribute") == 0) fn = parse_contribute;
        else if (strcmp(out->command, "level") == 0) fn = parse_level;
        else if (strcmp(out->command, "items") == 0) fn = parse_items;
        else if (strcmp(out->command, "verify") == 0) fn = parse_verify;
        else if (strcmp(out->command, "decode") == 0) fn = parse_decode;
        else if (strcmp(out->command, "encode") == 0) fn = parse_encode;
        else if (strcmp(out->command, "watch") == 0) fn = parse_watch;
        else if (strcmp(out->command, "session") == 0) fn = parse_session;
        else if (strcmp(out->command, "graft") == 0) fn = parse_graft;

        /* argparse reports every leftover token together, in original argv order, once
         * parsing as a whole finishes -- top-level extras (e.g. an unrecognized flag before
         * the command) come first since they appeared first in argv, then whatever the
         * subcommand itself could not place. A fatal parse error inside the subcommand
         * (missing value, bad int, bad choice, mutex conflict, missing required positional,
         * or its own --help) still takes priority and is reported immediately by fn(). */
        StrList sub_extras = {0};
        bool ok = fn(&rest, out, &sub_extras, exit_code);
        if (!ok) return CLI_EXIT;

        StrList combined = {0};
        for (int k = 0; k < top_extras.count; k++) strlist_push(&combined, top_extras.items[k]);
        for (int k = 0; k < sub_extras.count; k++) strlist_push(&combined, sub_extras.items[k]);
        if (combined.count) {
            char buf[512] = {0};
            size_t used = 0;
            for (int k = 0; k < combined.count; k++)
                used += (size_t)snprintf(buf + used, sizeof buf - used, "%s%s", k ? " " : "",
                                          combined.items[k]);
            *exit_code = usage_error(CLI_USAGE_TOP, PROG, "unrecognized arguments: %s", buf);
            return CLI_EXIT;
        }
        return CLI_CONTINUE;
    }

    if (top_extras.count) {
        char buf[512] = {0};
        size_t used = 0;
        for (int k = 0; k < top_extras.count; k++)
            used += (size_t)snprintf(buf + used, sizeof buf - used, "%s%s", k ? " " : "",
                                      top_extras.items[k]);
        *exit_code = usage_error(CLI_USAGE_TOP, PROG, "unrecognized arguments: %s", buf);
        return CLI_EXIT;
    }

    out->command = NULL;
    return CLI_CONTINUE;
}
