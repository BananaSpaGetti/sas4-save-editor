/*
 * dgdata -- native port of SAS4Trainer/tools/dgdata.py's own small CLI: verify, decode,
 * encode, roundtrip. This exists so the format layer has something runnable on its own,
 * the same way memscope.exe/ptrscan.exe do for MemScope's port; the full sas4.py CLI is a
 * separate follow-on plan (port-to-c-sas4-cli.md).
 *
 * decode and encode do not yet validate that the plaintext is well-formed JSON the way
 * dgdata.py's do (`json.loads(plain)` before writing/encoding) -- this task (task 3 of
 * port-to-c-sas4-core) comes before the JSON parser (task 4). Once that lands, this CLI
 * should gain the same check; noted rather than silently left out.
 */
#include "dgdata.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *read_whole_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)size > 0 ? (size_t)size : 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        free(buf);
        return NULL;
    }
    *out_len = (size_t)size;
    return buf;
}

static int write_whole_file(const char *path, const uint8_t *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        return -1;
    }
    size_t wrote = fwrite(data, 1, len, f);
    fclose(f);
    return wrote == len ? 0 : -1;
}

static char *ext_replace_json(const char *path) {
    /* os.path.splitext(path)[0] + ".json" -- everything before the last '.' in the final
     * path component, or the whole path if it has no '.'. */
    const char *slash1 = strrchr(path, '/');
    const char *slash2 = strrchr(path, '\\');
    const char *base = slash1 > slash2 ? slash1 : slash2;
    const char *dot = strrchr(base ? base : path, '.');
    size_t stem_len = dot ? (size_t)(dot - path) : strlen(path);
    char *out = (char *)malloc(stem_len + 6); /* ".json" + NUL */
    memcpy(out, path, stem_len);
    memcpy(out + stem_len, ".json", 6);
    return out;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: dgdata <verify|decode|encode|roundtrip> <path> [out]\n");
        return 2;
    }
    const char *command = argv[1];
    const char *path = argv[2];

    if (strcmp(command, "verify") == 0) {
        size_t raw_len;
        uint8_t *raw = read_whole_file(path, &raw_len);
        if (!raw) {
            fprintf(stderr, "could not read '%s'\n", path);
            return 1;
        }
        char stored[9], computed[9];
        int verdict = dg_verify(raw, raw_len, stored, computed);
        free(raw);
        if (verdict < 0) {
            fprintf(stderr, "not a DGDATA file\n");
            return 1;
        }
        printf("stored   %s\n", stored);
        printf("computed %s\n", computed);
        printf(verdict ? "VALID\n" : "MISMATCH - the game would reject this file\n");
        return verdict ? 0 : 1;
    }

    if (strcmp(command, "decode") == 0) {
        size_t raw_len;
        uint8_t *raw = read_whole_file(path, &raw_len);
        if (!raw) {
            fprintf(stderr, "could not read '%s'\n", path);
            return 1;
        }
        uint8_t *plain;
        size_t plain_len;
        if (!dg_decode(raw, raw_len, &plain, &plain_len)) {
            free(raw);
            fprintf(stderr, "not a DGDATA file\n");
            return 1;
        }
        free(raw);
        char *out_path = argc > 3 ? NULL : ext_replace_json(path);
        const char *dest = argc > 3 ? argv[3] : out_path;
        if (write_whole_file(dest, plain, plain_len) != 0) {
            free(plain);
            free(out_path);
            fprintf(stderr, "could not write '%s'\n", dest);
            return 1;
        }
        printf("wrote %s, %zu bytes\n", dest, plain_len);
        free(plain);
        free(out_path);
        return 0;
    }

    if (strcmp(command, "encode") == 0) {
        if (argc < 4) {
            printf("encode needs an output path\n");
            return 2;
        }
        size_t plain_len;
        uint8_t *plain = read_whole_file(path, &plain_len);
        if (!plain) {
            fprintf(stderr, "could not read '%s'\n", path);
            return 1;
        }
        uint8_t *built;
        size_t built_len;
        dg_encode(plain, plain_len, &built, &built_len);
        free(plain);
        if (write_whole_file(argv[3], built, built_len) != 0) {
            free(built);
            fprintf(stderr, "could not write '%s'\n", argv[3]);
            return 1;
        }
        printf("wrote %s, %zu bytes, header %.14s\n", argv[3], built_len, (char *)built);
        free(built);
        return 0;
    }

    if (strcmp(command, "roundtrip") == 0) {
        size_t raw_len;
        uint8_t *raw = read_whole_file(path, &raw_len);
        if (!raw) {
            fprintf(stderr, "could not read '%s'\n", path);
            return 1;
        }
        uint8_t *plain;
        size_t plain_len;
        if (!dg_decode(raw, raw_len, &plain, &plain_len)) {
            free(raw);
            fprintf(stderr, "not a DGDATA file\n");
            return 1;
        }
        uint8_t *rebuilt;
        size_t rebuilt_len;
        dg_encode(plain, plain_len, &rebuilt, &rebuilt_len);
        free(plain);
        bool same = rebuilt_len == raw_len && memcmp(rebuilt, raw, raw_len) == 0;
        printf("%zu bytes in, %zu bytes out\n", raw_len, rebuilt_len);
        printf(same ? "byte-identical\n" : "DIFFERS from the original\n");
        free(raw);
        free(rebuilt);
        return same ? 0 : 1;
    }

    fprintf(stderr, "unknown command '%s'\n", command);
    return 2;
}
