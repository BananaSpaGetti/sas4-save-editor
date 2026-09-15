/*
 * Shared test fixtures for task 14's C unit suite -- a generated profile document (the C-side
 * equivalent of sas4_model.py's generate(), built directly rather than diffed against Python:
 * tasks 10-13's own verify scripts already did that exhaustively against the real oracle,
 * this only needs a document shaped well enough to exercise model.c/plans.c/edit.c/anchor.c
 * directly), a way to write it to a real DGDATA file, and a fresh per-process temp directory.
 *
 * Header-only (every function `static`) so it can be included from multiple test .c files
 * without the Makefile needing a shared object of its own -- each test binary gets its own
 * copy, which is fine at this size.
 *
 * EVERY value here is invented. None of this ever touches or resembles a real profile.
 */
#ifndef SAS4_TEST_FIXTURES_H
#define SAS4_TEST_FIXTURES_H

#include "dgdata.h"
#include "json.h"
#include "model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* A document with exactly the shape model.c's rules and plans.c's plan-builders read:
 * Inventory/Profile0 Loaded with Skills/Strongboxes/Equipment, Profile1-5 the {"Loaded":
 * false} stub every fresh account has, Global/HighestRank, and MasteryProgress for all six
 * slots at MASTERY_SLOTS tracks each (enough duplicate "MasteryXp":0 entries -- 6*27 = 162
 * of them -- to reproduce the real anchor_for ambiguity a single track hits). */
static inline JsonValue *fixtures_document(int level, int64_t money) {
    JsonValue *profile0 = json_new_object();
    json_object_set(profile0, "Loaded", strlen("Loaded"), json_new_bool(true));
    json_object_set(profile0, "Name", strlen("Name"), json_new_string("Test", 4));
    json_object_set(profile0, "Money", strlen("Money"), json_new_int(money));

    JsonValue *skills = json_new_object();
    json_object_set(skills, "PlayerLevel", strlen("PlayerLevel"), json_new_int(level));
    json_object_set(skills, "PlayerTotalXp", strlen("PlayerTotalXp"),
                     json_new_int(model_xp_for_level(level)));
    json_object_set(skills, "AvailableSkillPoints", strlen("AvailableSkillPoints"),
                     json_new_int(level));
    json_object_set(skills, "SkillsArray", strlen("SkillsArray"), json_new_array());
    json_object_set(profile0, "Skills", strlen("Skills"), skills);

    JsonValue *strongboxes = json_new_object();
    json_object_set(strongboxes, "Unopened", strlen("Unopened"), json_new_array());
    json_object_set(strongboxes, "Claimed", strlen("Claimed"), json_new_array());
    json_object_set(profile0, "Strongboxes", strlen("Strongboxes"), strongboxes);
    json_object_set(profile0, "Equipment", strlen("Equipment"), json_new_array());

    JsonValue *inventory = json_new_object();
    json_object_set(inventory, "Profile0", strlen("Profile0"), profile0);
    for (int i = 1; i <= 5; i++) {
        char key[16];
        snprintf(key, sizeof(key), "Profile%d", i);
        JsonValue *stub = json_new_object();
        json_object_set(stub, "Loaded", strlen("Loaded"), json_new_bool(false));
        json_object_set(inventory, key, strlen(key), stub);
    }

    JsonValue *global = json_new_object();
    json_object_set(global, "HighestRank", strlen("HighestRank"), json_new_int(level));

    JsonValue *mastery = json_new_object();
    for (int p = 0; p < 6; p++) {
        char key[24];
        snprintf(key, sizeof(key), "MasteryProfile%d", p);
        JsonValue *tracks = json_new_array();
        for (int t = 0; t < MODEL_MASTERY_SLOTS; t++) {
            JsonValue *track = json_new_object();
            json_object_set(track, "MasteryXp", strlen("MasteryXp"), json_new_int(0));
            json_object_set(track, "MasteryLvl", strlen("MasteryLvl"), json_new_int(0));
            json_array_push(tracks, track);
        }
        json_object_set(mastery, key, strlen(key), tracks);
    }

    JsonValue *document = json_new_object();
    json_object_set(document, "Inventory", strlen("Inventory"), inventory);
    json_object_set(document, "Global", strlen("Global"), global);
    json_object_set(document, "MasteryProgress", strlen("MasteryProgress"), mastery);
    return document;
}

/* Writes `document` to `path` as a real DGDATA file (compact-serialized, then dg_encode'd).
 * Returns true on success. */
static inline bool fixtures_write_save(const JsonValue *document, const char *path) {
    char *plain;
    size_t plain_len;
    char err[128];
    if (!json_serialize_compact(document, &plain, &plain_len, err, sizeof(err))) {
        return false;
    }
    uint8_t *encoded;
    size_t encoded_len;
    dg_encode((const uint8_t *)plain, plain_len, &encoded, &encoded_len);
    free(plain);

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(encoded);
        return false;
    }
    size_t wrote = fwrite(encoded, 1, encoded_len, f);
    fclose(f);
    free(encoded);
    return wrote == encoded_len;
}

/* A fresh, unique-per-process directory under the system temp path -- avoids the fixed-path
 * trap a persistent %TEMP%\sas4_test_edit style fixture has, where count-based assertions
 * (exactly one backup, exactly two backups) pass the first run and fail the second because
 * a prior run's files are still there. Not cleaned up afterward (Windows has no simple
 * recursive-delete call without pulling in shell32's SHFileOperation for it) -- the
 * uniqueness alone is what keeps count-based assertions correct across repeated runs. */
static inline const char *fixtures_temp_dir(void) {
    static char buf[MAX_PATH + 64];
    char base[MAX_PATH];
    DWORD n = GetTempPathA(sizeof(base), base);
    if (n > 0 && (base[n - 1] == '\\' || base[n - 1] == '/')) {
        base[n - 1] = '\0';
    }
    snprintf(buf, sizeof(buf), "%s\\sas4_ctest_%lu_%lu", base,
             (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount());
    CreateDirectoryA(buf, NULL);
    return buf;
}

/* Bytes of a whole file, malloc'd; *out_len set. NULL on failure. */
static inline uint8_t *fixtures_read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)size);
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        free(buf);
        return NULL;
    }
    *out_len = (size_t)size;
    return buf;
}

/* Counts the files (not directories) directly and recursively under `dir` -- enough for
 * "exactly N backups" assertions without a full directory-tree walker; backups never nest
 * more than one level (backups_dir/backup-YYYYMMDD-HHMMSS[-NN]). */
static inline size_t fixtures_count_files_recursive(const char *dir) {
    char pattern[900];
    snprintf(pattern, sizeof(pattern), "%.700s\\*", dir);
    WIN32_FIND_DATAA data;
    HANDLE h = FindFirstFileA(pattern, &data);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    size_t count = 0;
    do {
        if (strcmp(data.cFileName, ".") == 0 || strcmp(data.cFileName, "..") == 0) {
            continue;
        }
        char child[900];
        snprintf(child, sizeof(child), "%.600s\\%.255s", dir, data.cFileName);
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            count += fixtures_count_files_recursive(child);
        } else {
            count++;
        }
    } while (FindNextFileA(h, &data));
    FindClose(h);
    return count;
}

#endif
