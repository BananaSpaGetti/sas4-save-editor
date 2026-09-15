#include "sas4load.h"
#include "dgdata.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool read_whole_file(const char *path, uint8_t **out, size_t *out_len, char *err,
                             size_t err_cap) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, err_cap, "cannot read %.400s\n  could not open the file", path);
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        snprintf(err, err_cap, "cannot read %.400s\n  could not read the file", path);
        return false;
    }
    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        snprintf(err, err_cap, "cannot read %.400s\n  could not read the file", path);
        return false;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)size > 0 ? (size_t)size : 1);
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        free(buf);
        snprintf(err, err_cap, "cannot read %.400s\n  could not read the file", path);
        return false;
    }
    *out = buf;
    *out_len = (size_t)size;
    return true;
}

SaveLoad sas4_load(const char *path) {
    SaveLoad sl;
    memset(&sl, 0, sizeof sl);

    if (!read_whole_file(path, &sl.raw, &sl.raw_len, sl.error, sizeof sl.error)) {
        return sl;
    }
    if (!dg_decode(sl.raw, sl.raw_len, &sl.plain, &sl.plain_len)) {
        snprintf(sl.error, sizeof sl.error, "cannot read %.400s\n  not a DGDATA file", path);
        free(sl.raw);
        sl.raw = NULL;
        return sl;
    }
    JsonParseResult parsed = json_parse(sl.plain, sl.plain_len);
    if (!parsed.value) {
        snprintf(sl.error, sizeof sl.error, "cannot read %.400s\n  %.150s", path,
                 parsed.error);
        free(sl.raw);
        sl.raw = NULL;
        free(sl.plain);
        sl.plain = NULL;
        return sl;
    }
    sl.document = parsed.value;
    sl.ok = true;
    return sl;
}

void sas4_load_free(SaveLoad *sl) {
    free(sl->raw);
    free(sl->plain);
    if (sl->document) json_free(sl->document);
    memset(sl, 0, sizeof *sl);
}
