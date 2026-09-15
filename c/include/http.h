/*
 * A single blocking HTTPS GET over WinHTTP -- what sas4.py gets from
 * `urllib.request.urlopen(url, timeout=30).read()`. Task 10 of the port-to-c-sas4-cli plan.
 *
 * This is the only outbound network call in the program, and it sends nothing but the
 * request itself: no body, no headers of our own, no identifiers. See Decision 12 in the
 * plan for what is and is not reproduced when it fails.
 */
#ifndef SAS4_HTTP_H
#define SAS4_HTTP_H

#include <stdbool.h>
#include <stddef.h>

/* Fetches `url` into a malloc'd buffer. On success returns true and fills in both out
 * parameters (the
 * buffer is NUL-terminated one past *out_len for convenience; the NUL is not counted).
 * On failure returns false, leaves *out NULL, and writes a short reason into `err`.
 *
 * A non-2xx status is a failure, matching urlopen()'s HTTPError. Nothing is written to disk
 * either way -- a caller that caches the response must only do so after this returns true,
 * so a failed or truncated transfer cannot leave a partial cache behind. */
bool http_get(const char *url, unsigned char **out, size_t *out_len, char *err, size_t err_len);

#endif
