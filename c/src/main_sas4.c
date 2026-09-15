/* sas4.exe -- the command line entry point. Task 1 (port-to-c-sas4-cli plan) builds only the
 * argument parser and dispatch skeleton; each subcommand's actual behavior is wired in by its
 * own later task. Until a command is wired in, running it here prints a clearly-marked stub
 * line rather than silently doing nothing, so this is never mistaken for a finished command. */
#include "cli.h"
#include "cli_help_data.h"
#include "cmd_contribute.h"
#include "cmd_dump.h"
#include "cmd_give.h"
#include "cmd_graft.h"
#include "cmd_items.h"
#include "cmd_level.h"
#include "cmd_list.h"
#include "cmd_session.h"
#include "cmd_set.h"
#include "cmd_verify.h"
#include "cmd_watch.h"
#include "cmd_view.h"
#include "discover.h"
#include "plans.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Debug-only: prints the parsed CliArgs fields relevant to the selected command as
 * key=value lines, using Python's own str() spelling for bool/None so a Python-side
 * comparison script can diff the two side by side without a translation layer. Not
 * reachable via any documented flag -- gated on an env var so it can never surface in a
 * real invocation's output. See the port-to-c-sas4-cli plan, task 1's verification. */
static void dump_str(const char *key, const char *v) {
    if (v) printf("%s=%s\n", key, v);
    else printf("%s=None\n", key);
}
static void dump_int(const char *key, long v) { printf("%s=%ld\n", key, v); }
static void dump_bool(const char *key, bool v) { printf("%s=%s\n", key, v ? "True" : "False"); }

static void dump_args(const CliArgs *a) {
    dump_str("file", a->file);
    if (strcmp(a->command, "view") == 0) {
        dump_int("slot", a->view_slot);
        if (a->view_sections_count == 0) {
            printf("section=None\n");
        } else {
            printf("section=");
            for (int i = 0; i < a->view_sections_count; i++)
                printf("%s%s", i ? "," : "", a->view_sections[i]);
            printf("\n");
        }
    } else if (strcmp(a->command, "list") == 0) {
        dump_str("grep", a->list_grep);
        dump_str("type", a->list_type);
        dump_str("under", a->list_under);
    } else if (strcmp(a->command, "get") == 0) {
        dump_str("path", a->get_path);
    } else if (strcmp(a->command, "set") == 0) {
        dump_str("path", a->set_path);
        dump_str("value", a->set_value);
        dump_bool("dry_run", a->set_dry_run);
        dump_bool("force", a->set_force);
    } else if (strcmp(a->command, "give") == 0) {
        dump_int("item", a->give_item);
        dump_str("kind", a->give_kind);
        dump_int("grade", a->give_grade);
        dump_int("bonus", a->give_bonus);
        dump_int("slot", a->give_slot);
        dump_int("slotprofile", a->give_slotprofile);
        dump_bool("dry_run", a->give_dry_run);
        dump_bool("force", a->give_force);
    } else if (strcmp(a->command, "mastery") == 0) {
        dump_str("set", a->mastery_set);
        if (a->mastery_all_given) dump_int("all", a->mastery_all);
        else printf("all=None\n");
        dump_int("slot", a->mastery_slot);
        dump_bool("dry_run", a->mastery_dry_run);
        dump_bool("force", a->mastery_force);
    } else if (strcmp(a->command, "contribute") == 0) {
        dump_int("slot", a->contribute_slot);
        dump_bool("print_only", a->contribute_print_only);
    } else if (strcmp(a->command, "level") == 0) {
        dump_int("level", a->level_value);
        dump_int("slot", a->level_slot);
        dump_bool("dry_run", a->level_dry_run);
        dump_bool("force", a->level_force);
    } else if (strcmp(a->command, "items") == 0) {
        if (!a->items_catalog_given) printf("catalog=None\n");
        else dump_str("catalog", a->items_catalog); /* NULL here means "the const default" */
    } else if (strcmp(a->command, "decode") == 0) {
        dump_str("out", a->decode_out);
    } else if (strcmp(a->command, "encode") == 0) {
        dump_str("json", a->encode_json);
        dump_str("out", a->encode_out);
    } else if (strcmp(a->command, "watch") == 0) {
        dump_bool("archive", a->watch_archive);
    } else if (strcmp(a->command, "session") == 0) {
        dump_str("session", a->session_session);
        dump_str("set", a->session_set);
        dump_bool("yes", a->session_yes);
        dump_bool("force", a->session_force);
    } else if (strcmp(a->command, "graft") == 0) {
        dump_str("source", a->graft_source);
        dump_str("fields", a->graft_fields);
        dump_bool("apply", a->graft_apply);
        dump_bool("force", a->graft_force);
    }
    /* where/kinds/verify define no fields of their own beyond the global --file. */
}

int main(int argc, char **argv) {
    CliArgs args;
    int exit_code = 0;
    CliOutcome outcome = cli_parse(argc - 1, argv + 1, &args, &exit_code);
    if (outcome == CLI_EXIT) return exit_code;

    if (!args.command) {
        fputs(CLI_HELP_TOP, stdout);
        return 2;
    }

    if (getenv("SAS4_DEBUG_ARGS")) {
        dump_args(&args);
        return 0;
    }

    /* Task 2 (port-to-c-sas4-cli plan): profile discovery, matching sas4.py's main()'s own
     * "no profile, and this command needs one" check -- getattr(args, "file", "unset") is
     * None and args.command not in ("where", "encode"). */
    DiscoverContext ctx;
    discover_context_build(&ctx);

    const char *effective_file = args.file;
    if (!effective_file && ctx.live_found) effective_file = ctx.live;

    if (strcmp(args.command, "where") == 0) {
        int rc = discover_cmd_where(&ctx);
        discover_context_free(&ctx);
        return rc;
    }

    if (!effective_file && strcmp(args.command, "encode") != 0) {
        printf("no SAS4 profile found automatically on this machine.\n");
        printf("pass one with --file <path>, or run `py sas4.py where` to see what was "
               "found.\n");
        discover_context_free(&ctx);
        return 1;
    }

    if (strcmp(args.command, "view") == 0) {
        char items_path[900];
        snprintf(items_path, sizeof items_path, "%.500s\\decoded\\items.json", ctx.data);
        ItemNames names;
        items_load(items_path, &names);
        int rc = cmd_view(effective_file, args.view_slot, args.view_sections,
                           args.view_sections_count, &names);
        items_free(&names);
        discover_context_free(&ctx);
        return rc;
    }

    if (strcmp(args.command, "list") == 0) {
        int rc = cmd_list(effective_file, args.list_grep, args.list_type, args.list_under);
        discover_context_free(&ctx);
        return rc;
    }

    if (strcmp(args.command, "verify") == 0) {
        int rc = cmd_verify(effective_file);
        discover_context_free(&ctx);
        return rc;
    }

    if (strcmp(args.command, "give") == 0) {
        char backups_dir[900], items_path[900];
        snprintf(backups_dir, sizeof backups_dir, "%.700s\\backups", ctx.data);
        snprintf(items_path, sizeof items_path, "%.500s\\decoded\\items.json", ctx.data);
        ItemNames names;
        items_load(items_path, &names);
        int rc = cmd_give(effective_file, &names, args.give_item, args.give_kind,
                           args.give_grade, args.give_bonus, args.give_slot,
                           (int)args.give_slotprofile, args.give_dry_run,
                           args.give_force, backups_dir);
        items_free(&names);
        discover_context_free(&ctx);
        return rc;
    }

    if (strcmp(args.command, "level") == 0) {
        char backups_dir[900];
        snprintf(backups_dir, sizeof backups_dir, "%.700s\\backups", ctx.data);
        int rc = cmd_level(effective_file, (int)args.level_value, (int)args.level_slot,
                            args.level_dry_run, args.level_force, backups_dir);
        discover_context_free(&ctx);
        return rc;
    }

    if (strcmp(args.command, "mastery") == 0) {
        char backups_dir[900];
        snprintf(backups_dir, sizeof backups_dir, "%.700s\\backups", ctx.data);
        int rc = cmd_mastery(effective_file, args.mastery_set, args.mastery_all_given,
                              (int)args.mastery_all, (int)args.mastery_slot,
                              args.mastery_dry_run, args.mastery_force, backups_dir);
        discover_context_free(&ctx);
        return rc;
    }

    if (strcmp(args.command, "set") == 0) {
        char backups_dir[900];
        snprintf(backups_dir, sizeof backups_dir, "%.700s\\backups", ctx.data);
        int rc = cmd_set(effective_file, args.set_path, args.set_value,
                          args.set_dry_run, args.set_force, backups_dir);
        discover_context_free(&ctx);
        return rc;
    }

    if (strcmp(args.command, "get") == 0) {
        int rc = cmd_get(effective_file, args.get_path);
        discover_context_free(&ctx);
        return rc;
    }

    if (strcmp(args.command, "decode") == 0) {
        int rc = cmd_decode(effective_file, args.decode_out, ctx.data);
        discover_context_free(&ctx);
        return rc;
    }

    if (strcmp(args.command, "encode") == 0) {
        int rc = cmd_encode(args.encode_json, args.encode_out);
        discover_context_free(&ctx);
        return rc;
    }

    if (strcmp(args.command, "contribute") == 0) {
        int rc = cmd_contribute(effective_file, (int)args.contribute_slot,
                                 args.contribute_print_only, ctx.data);
        discover_context_free(&ctx);
        return rc;
    }

    if (strcmp(args.command, "graft") == 0) {
        char backups_dir[900];
        snprintf(backups_dir, sizeof backups_dir, "%.700s\\backups", ctx.data);
        int rc = cmd_graft(effective_file, args.graft_source, args.graft_fields,
                            args.graft_apply, args.graft_force, backups_dir);
        discover_context_free(&ctx);
        return rc;
    }

    if (strcmp(args.command, "session") == 0) {
        char backups_dir[900];
        snprintf(backups_dir, sizeof backups_dir, "%.700s\\backups", ctx.data);
        /* argparse's default for --session is SESSION, the current.session beside the live
         * profile; the parser leaves it NULL to say "the default", since only this layer
         * knows what discovery found. */
        const char *session_path = args.session_session;
        if (!session_path) session_path = ctx.session[0] ? ctx.session : NULL;
        if (!session_path) {
            /* SESSION is None when discovery found no profile, and open(None) is what the
             * Python would then hit -- a TypeError traceback. Decision 12's policy again:
             * nonzero exit, nothing written, wording not reproduced. */
            fprintf(stderr, "no current.session found -- pass --session <path>\n");
            discover_context_free(&ctx);
            return 1;
        }
        int rc = cmd_session(session_path, args.session_set, args.session_yes,
                              args.session_force, backups_dir);
        discover_context_free(&ctx);
        return rc;
    }

    if (strcmp(args.command, "watch") == 0) {
        char saves_dir[900];
        snprintf(saves_dir, sizeof saves_dir, "%.700s\\saves", ctx.data);
        int rc = cmd_watch(effective_file, args.watch_archive, saves_dir);
        discover_context_free(&ctx);
        return rc;
    }

    if (strcmp(args.command, "kinds") == 0) {
        int rc = cmd_kinds(effective_file);
        discover_context_free(&ctx);
        return rc;
    }

    if (strcmp(args.command, "items") == 0) {
        char items_path[900], catalog_default[900];
        snprintf(items_path, sizeof items_path, "%.500s\\decoded\\items.json", ctx.data);
        snprintf(catalog_default, sizeof catalog_default, "%.700s\\ITEMS.md", ctx.data);
        /* --catalog with no value carries argparse's const, which is DATA/ITEMS.md; the
         * parser leaves it NULL to say "the const", since only this layer knows DATA. */
        const char *catalog = NULL;
        if (args.items_catalog_given) {
            catalog = args.items_catalog ? args.items_catalog : catalog_default;
        }
        int rc = cmd_items(SAS4_ITEMS_URL, items_path, catalog);
        discover_context_free(&ctx);
        return rc;
    }

    discover_context_free(&ctx);
    printf("(stub) command not yet wired in: %s\n", args.command);
    return 0;
}
