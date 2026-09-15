/*
 * `items` -- port of sas4.py's cmd_items, item_catalog and write_catalog. Task 10 of the
 * port-to-c-sas4-cli plan.
 *
 * This is the one command that touches the network: it downloads the community item table
 * (someone else's data, deliberately not vendored) and caches it beside sas4.bat. It sends
 * nothing. See Decision 12 for the failure path -- Python's is an uncaught traceback, whose
 * wording is not reproduced; what is reproduced is that nothing is written and the exit code
 * is nonzero.
 */
#ifndef SAS4_CMD_ITEMS_H
#define SAS4_CMD_ITEMS_H

#include <stdbool.h>

/* sas4.py ITEMS_URL. */
#define SAS4_ITEMS_URL "https://raw.githubusercontent.com/0daxelagnia/SAS4Tool/main/items.json"

/* `url` is sas4.py's ITEMS_URL and `cache_path` its ITEMS_CACHE, both passed in so a test
 * can point them somewhere harmless. `catalog_path` is NULL unless --catalog was given. */
int cmd_items(const char *url, const char *cache_path, const char *catalog_path);

#endif
