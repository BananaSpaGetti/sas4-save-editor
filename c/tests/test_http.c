/*
 * The failure path of `items` -- task 10 of the port-to-c-sas4-cli plan.
 *
 * The success path needs the network and is measured against the Python by the differential
 * harness, not here. What this pins is the part that must hold with no network at all: a
 * download that fails must leave no cache behind, and must not truncate one that already
 * exists. A partial or empty decoded/items.json is worse than none, because `give` and
 * `view` read it for every item name.
 *
 * The unreachable address is 127.0.0.1 port 1 rather than a bogus hostname: it fails on
 * connect instead of waiting on a DNS lookup, so the test cannot hang on a machine with a
 * slow or hijacking resolver.
 */
#include "cmd_items.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

static void check(int condition, const char *what) {
    printf("%s  %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) failures++;
}

static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static long file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

#define UNREACHABLE "https://127.0.0.1:1/items.json"

int main(void) {
    char dir[1024];
    const char *tmp = getenv("TEMP");
    if (!tmp || !*tmp) tmp = ".";
    snprintf(dir, sizeof dir, "%.400s\\sas4-test-http", tmp);

    char cache[1024];
    snprintf(cache, sizeof cache, "%.480s\\items.json", dir);
    remove(cache);

    /* 1. A failed download creates nothing. */
    int rc = cmd_items(UNREACHABLE, cache, NULL);
    check(rc != 0, "an unreachable host exits nonzero");
    check(!file_exists(cache), "and writes no cache file");

    /* 2. A failed download leaves an existing cache exactly as it was. */
    FILE *f = fopen(cache, "wb");
    if (!f) {
        printf("FAIL  cannot create the fixture cache file\n");
        return 1;
    }
    static const char FIXTURE[] = "{\"weapon_info\":{\"normal\":{\"smg\":[]}}}";
    fwrite(FIXTURE, 1, sizeof FIXTURE - 1, f);
    fclose(f);

    rc = cmd_items(UNREACHABLE, cache, NULL);
    check(rc != 0, "an unreachable host still exits nonzero with a cache present");
    check(file_size(cache) == (long)(sizeof FIXTURE - 1),
          "and leaves the existing cache at its original size");

    char readback[256] = {0};
    f = fopen(cache, "rb");
    if (f) {
        size_t got = fread(readback, 1, sizeof readback - 1, f);
        readback[got] = '\0';
        fclose(f);
    }
    check(strcmp(readback, FIXTURE) == 0, "and byte-identical");
    remove(cache);

    printf("%s\n", failures ? "FAILURES" : "all ok");
    return failures ? 1 : 0;
}
