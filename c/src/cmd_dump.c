/*
 * `get`, `decode` and `encode` -- task 5 of the port-to-c-sas4-cli plan.
 */
#include "cmd_dump.h"
#include "dgdata.h"
#include "json.h"
#include "path.h"
#include "sas4load.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

int cmd_get(const char *file, const char *path) {
    SaveLoad sl = sas4_load(file);
    if (!sl.ok) {
        printf("%s\n", sl.error);
        sas4_load_free(&sl);
        return 1;
    }
    PathAtResult at = path_at(sl.document, path);
    if (!at.ok) {
        /* Python raises here; see cmd_dump.h. stdout stays empty, exit code 1. */
        fprintf(stderr, "%s\n", at.error);
        sas4_load_free(&sl);
        return 1;
    }
    char *out = NULL;
    size_t out_len = 0;
    char err[128];
    if (!json_serialize_indent(at.value, &out, &out_len, err, sizeof err)) {
        fprintf(stderr, "%s\n", err);
        sas4_load_free(&sl);
        return 1;
    }
    printf("%s\n", out);
    free(out);
    sas4_load_free(&sl);
    return 0;
}

/* os.makedirs(path, exist_ok=True). An empty path is a no-op here; Python would raise
 * FileNotFoundError on os.makedirs("") -- which is what `decode out.json` with no directory
 * component does there. Not reproduced (same family as Decision 11); this writes the file. */
static void make_dirs(const char *path) {
    if (!path || !path[0]) return;
    char buf[900];
    snprintf(buf, sizeof buf, "%.880s", path);
    size_t len = strlen(buf);
    for (size_t i = 1; i < len; i++) {
        if (buf[i] == '\\' || buf[i] == '/') {
            char saved = buf[i];
            buf[i] = '\0';
            if (!(strlen(buf) == 2 && buf[1] == ':')) CreateDirectoryA(buf, NULL);
            buf[i] = saved;
        }
    }
    CreateDirectoryA(buf, NULL);
}

static void dirname_of(const char *path, char *out, size_t cap) {
    snprintf(out, cap, "%s", path);
    char *s1 = strrchr(out, '\\');
    char *s2 = strrchr(out, '/');
    char *slash = s1 > s2 ? s1 : s2;
    if (slash) {
        *slash = '\0';
    } else {
        out[0] = '\0';
    }
}

int cmd_decode(const char *file, const char *out_arg, const char *data_dir) {
    SaveLoad sl = sas4_load(file);
    if (!sl.ok) {
        printf("%s\n", sl.error);
        sas4_load_free(&sl);
        return 1;
    }

    char out_path[900];
    if (out_arg) {
        snprintf(out_path, sizeof out_path, "%s", out_arg);
    } else {
        snprintf(out_path, sizeof out_path, "%.700s\\decoded\\profile.json", data_dir);
    }

    char dir[900];
    dirname_of(out_path, dir, sizeof dir);
    make_dirs(dir);

    char *text = NULL;
    size_t text_len = 0;
    char err[128];
    if (!json_serialize_indent(sl.document, &text, &text_len, err, sizeof err)) {
        fprintf(stderr, "%s\n", err);
        sas4_load_free(&sl);
        return 1;
    }

    /* Text mode, not binary: Python writes this through a text-mode file object, so every
     * '\n' json.dump emits becomes '\r\n' on Windows. Byte-identical output needs the same. */
    FILE *f = fopen(out_path, "w");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", out_path);
        free(text);
        sas4_load_free(&sl);
        return 1;
    }
    fwrite(text, 1, text_len, f);
    fclose(f);
    free(text);
    sas4_load_free(&sl);

    /* os.path.getsize(out) -- the size ON DISK, after newline translation, not the length
     * of the string that was written. */
    long long size = 0;
    FILE *sized = fopen(out_path, "rb");
    if (sized) {
        fseek(sized, 0, SEEK_END);
        size = ftell(sized);
        fclose(sized);
    }
    printf("wrote %s (%lld bytes)\n", out_path, size);
    return 0;
}

int cmd_encode(const char *json_path, const char *out) {
    FILE *f = fopen(json_path, "rb");
    if (!f) {
        fprintf(stderr, "cannot read %s\n", json_path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *plain = malloc((size_t)(size > 0 ? size : 1));
    size_t got = fread(plain, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        fprintf(stderr, "cannot read %s\n", json_path);
        free(plain);
        return 1;
    }

    /* json.loads(plain) -- a validation step whose value is discarded; a malformed file is
     * an uncaught exception in the Python (see cmd_dump.h on that family). */
    JsonParseResult parsed = json_parse(plain, got);
    if (!parsed.value) {
        fprintf(stderr, "%s\n", parsed.error);
        free(plain);
        return 1;
    }
    json_free(parsed.value);

    uint8_t *built = NULL;
    size_t built_len = 0;
    dg_encode(plain, got, &built, &built_len);
    free(plain);

    FILE *o = fopen(out, "wb");
    if (!o) {
        fprintf(stderr, "cannot write %s\n", out);
        free(built);
        return 1;
    }
    fwrite(built, 1, built_len, o);
    fclose(o);

    char header[15];
    size_t header_len = built_len < 14 ? built_len : 14;
    memcpy(header, built, header_len);
    header[header_len] = '\0';
    printf("wrote %s, %zu bytes, header %s\n", out, built_len, header);
    free(built);
    return 0;
}
