/*
 * Verification tool for task 9: `apply <file> <backups_dir> <path1> <value1-json> ...`
 * applies the given (path, value) pairs to the DGDATA save at <file> via edit_apply, and
 * prints "OK <backup_path>\n<message>" or "FAIL <backup_path-or-NONE>\n<message>". Never
 * writes to a real save -- every caller of this tool works on a generated one, in the
 * scratchpad directory. Not part of sas4.py's CLI surface.
 */
#include "edit.h"
#include "json.h"

#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);

    if (argc < 3 || (argc - 3) % 2 != 0) {
        fprintf(stderr, "usage: apply <file> <backups_dir> [<path> <value-json>]...\n");
        return 2;
    }
    const char *file_path = argv[1];
    const char *backups_dir = argv[2];
    size_t plan_count = (size_t)(argc - 3) / 2;

    EditPlanEntry *plan = (EditPlanEntry *)malloc(plan_count * sizeof(EditPlanEntry));
    JsonValue **owned = (JsonValue **)malloc(plan_count * sizeof(JsonValue *));
    for (size_t i = 0; i < plan_count; i++) {
        const char *path = argv[3 + 2 * i];
        const char *value_json = argv[3 + 2 * i + 1];
        JsonParseResult r = json_parse((const uint8_t *)value_json, strlen(value_json));
        if (!r.value) {
            fprintf(stderr, "could not parse value for %s: %s\n", path, r.error);
            return 2;
        }
        plan[i].path = path;
        plan[i].value = r.value;
        owned[i] = r.value;
    }

    ApplyEditsResult result = edit_apply(file_path, backups_dir, plan, plan_count);

    printf("%s %s\n%s\n", result.ok ? "OK" : "FAIL",
           result.has_backup_path ? result.backup_path : "NONE", result.message);

    for (size_t i = 0; i < plan_count; i++) {
        json_free(owned[i]);
    }
    free(owned);
    free(plan);
    return result.ok ? 0 : 1;
}
