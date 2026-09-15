/*
 * `watch` -- port of sas4.py's cmd_watch. Task 11 of the port-to-c-sas4-cli plan.
 *
 * Polls the profile's modification time and reports what moved each time the game rewrites
 * it. Like the Python, the loop is unbounded: it runs until the user interrupts it, which is
 * what "Ctrl+C to stop" in its own first line promises. A console control handler flushes
 * stdout before exiting, so a transcript being redirected to a file is not lost at the point
 * the user stops watching -- Python gets the same flush from interpreter shutdown.
 */
#ifndef SAS4_CMD_WATCH_H
#define SAS4_CMD_WATCH_H

#include <stdbool.h>

/* `saves_dir` is sas4.py's SAVES, where --archive puts each observed version. */
int cmd_watch(const char *file, bool archive, const char *saves_dir);

#endif
