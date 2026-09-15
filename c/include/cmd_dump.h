/*
 * `get`, `decode` and `encode` -- port of sas4.py's cmd_get, cmd_decode and cmd_encode.
 * Task 5 of the port-to-c-sas4-cli plan.
 */
#ifndef SAS4_CMD_DUMP_H
#define SAS4_CMD_DUMP_H

/* json.dumps(at_path(d, path), indent=2, ensure_ascii=False) on stdout.
 *
 * A path that does not resolve is an uncaught KeyError/IndexError/TypeError in the Python --
 * a traceback on stderr, empty stdout, exit 1. This prints its own message on stderr instead,
 * keeping stdout empty and the exit code 1 so everything a caller can compare still matches;
 * the traceback text itself is not reproduced (the same family as Decision 11). */
int cmd_get(const char *file, const char *path);

/* `out` may be NULL, meaning DECODED/profile.json (data_dir()/decoded/profile.json).
 * The file is written in text mode, so its newlines are CRLF on Windows exactly as
 * Python's own json.dump(..., handle) through a text-mode file object produces. */
int cmd_decode(const char *file, const char *out, const char *data_dir);

int cmd_encode(const char *json_path, const char *out);

#endif
