/*
 * `watch` -- task 11 of the port-to-c-sas4-cli plan.
 */
#include "cmd_watch.h"
#include "json.h"
#include "model.h"
#include "path.h"
#include "sas4load.h"

#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define POLL_MS 1000  /* time.sleep(1) at the bottom of the loop */
#define SETTLE_MS 400 /* time.sleep(0.4) -- let the game finish writing */
#define MAX_CHANGED_SHOWN 40
#define VALUE_WIDTH 24 /* json.dumps(...)[:24] */

/* --- dict(scalars(document)) ------------------------------------------------------------- */

/* Python builds a dict from the (path, value) pairs, so a path that somehow appeared twice
 * would keep its first position and its LAST value. Both are reproduced here. */
typedef struct {
    char *path;
    JsonValue *value; /* owned -- the document it came from is freed each round */
} Snap;

typedef struct {
    Snap *items;
    size_t count, cap;
} SnapMap;

static JsonValue *snap_get(const SnapMap *m, const char *path) {
    for (size_t i = 0; i < m->count; i++) {
        if (strcmp(m->items[i].path, path) == 0) return m->items[i].value;
    }
    return NULL;
}

static void snap_set(SnapMap *m, const char *path, const JsonValue *value) {
    for (size_t i = 0; i < m->count; i++) {
        if (strcmp(m->items[i].path, path) == 0) {
            json_free(m->items[i].value);
            m->items[i].value = json_clone(value);
            return;
        }
    }
    if (m->count == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 1024;
        m->items = (Snap *)realloc(m->items, m->cap * sizeof *m->items);
    }
    size_t n = strlen(path) + 1;
    m->items[m->count].path = (char *)malloc(n);
    memcpy(m->items[m->count].path, path, n);
    m->items[m->count].value = json_clone(value);
    m->count++;
}

static void snap_free(SnapMap *m) {
    for (size_t i = 0; i < m->count; i++) {
        free(m->items[i].path);
        json_free(m->items[i].value);
    }
    free(m->items);
    memset(m, 0, sizeof *m);
}

/* --- interrupt ---------------------------------------------------------------------------- */

static BOOL WINAPI on_interrupt(DWORD type) {
    (void)type;
    /* Python's interpreter shutdown flushes stdout on the way out of a KeyboardInterrupt;
     * without this the last lines of a redirected transcript would be lost, since the
     * default handler terminates the process outright. */
    fflush(stdout);
    ExitProcess(1);
    return TRUE;
}

/* --- helpers ------------------------------------------------------------------------------ */

/* os.path.getmtime as an opaque comparable stamp. Python compares floats of seconds; this
 * compares the FILETIME those floats are derived from, so it notices exactly the same
 * rewrites (and, in principle, a sub-microsecond one Python's float would round away -- not
 * reachable for a save the game writes). */
static bool file_mtime(const char *path, uint64_t *out) {
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &info)) return false;
    ULARGE_INTEGER t;
    t.LowPart = info.ftLastWriteTime.dwLowDateTime;
    t.HighPart = info.ftLastWriteTime.dwHighDateTime;
    *out = t.QuadPart;
    return true;
}

static void makedirs(const char *path) {
    if (!path || !*path) return;
    char buf[1024];
    snprintf(buf, sizeof buf, "%.1000s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '\\' || *p == '/') {
            char saved = *p;
            *p = '\0';
            if (!(strlen(buf) == 2 && buf[1] == ':')) CreateDirectoryA(buf, NULL);
            *p = saved;
        }
    }
    CreateDirectoryA(buf, NULL);
}

/* json.dumps(value)[:24] -- the bare form, so ensure_ascii=True (Decision from task 4).
 * The result is all ASCII, so a character slice and a byte slice are the same thing. */
static void dumps_clipped(const JsonValue *v, char *out, size_t out_cap) {
    char *text = NULL;
    size_t len = 0;
    char err[128];
    if (!json_serialize_default(v, &text, &len, err, sizeof err)) {
        snprintf(out, out_cap, "?");
        return;
    }
    size_t take = len < (size_t)VALUE_WIDTH ? len : (size_t)VALUE_WIDTH;
    if (take >= out_cap) take = out_cap - 1;
    memcpy(out, text, take);
    out[take] = '\0';
    free(text);
}

/* --- cmd_watch ---------------------------------------------------------------------------- */

int cmd_watch(const char *file, bool archive, const char *saves_dir) {
    SetConsoleCtrlHandler(on_interrupt, TRUE);

    printf("watching %s -- Ctrl+C to stop\n", file);
    if (archive) {
        makedirs(saves_dir);
        printf("archiving each version into %s\n", saves_dir);
    }

    int index = 0;
    bool has_previous = false;
    SnapMap previous = {0};
    uint64_t last = 0;

    for (;;) {
        uint64_t mtime;
        if (!file_mtime(file, &mtime)) {
            Sleep(POLL_MS);
            continue;
        }
        if (mtime != last) {
            last = mtime;
            Sleep(SETTLE_MS);

            SaveLoad sl = sas4_load(file);
            if (!sl.ok) {
                printf("  unreadable mid-write: %s\n", sl.error);
                sas4_load_free(&sl);
                continue; /* note: no sleep, matching the Python's bare `continue` */
            }

            if (archive) {
                char copy[1024];
                snprintf(copy, sizeof copy, "%.900s\\watch-%03d.save", saves_dir, index);
                CopyFileA(file, copy, FALSE);
                index++;
            }

            ScalarList scalars = {0};
            path_scalars(sl.document, "", &scalars);
            SnapMap current = {0};
            for (size_t i = 0; i < scalars.count; i++) {
                snap_set(&current, scalars.items[i].path, scalars.items[i].value);
            }
            path_scalars_free(&scalars);

            if (has_previous) {
                char stamp[32];
                time_t now = time(NULL);
                struct tm local;
                localtime_s(&local, &now);
                strftime(stamp, sizeof stamp, "%H:%M:%S", &local);
                printf("\n=== %s ===\n", stamp);

                size_t shown = 0;
                size_t changed_total = 0;
                for (size_t i = 0; i < current.count; i++) {
                    const JsonValue *before = snap_get(&previous, current.items[i].path);
                    if (!before || json_equal(before, current.items[i].value)) continue;
                    changed_total++;
                    if (shown >= MAX_CHANGED_SHOWN) continue;
                    shown++;
                    char a[VALUE_WIDTH + 1], b[VALUE_WIDTH + 1];
                    dumps_clipped(before, a, sizeof a);
                    dumps_clipped(current.items[i].value, b, sizeof b);
                    printf("  %-58s %s -> %s\n", current.items[i].path, a, b);
                }

                /* added = [k for k in current if k not in previous] */
                size_t added_total = 0;
                JsonValue *sample = json_new_array();
                for (size_t i = 0; i < current.count; i++) {
                    if (snap_get(&previous, current.items[i].path)) continue;
                    added_total++;
                    if (added_total <= 3) {
                        json_array_push(sample, json_new_string(current.items[i].path,
                                                                 strlen(current.items[i].path)));
                    }
                }
                if (added_total) {
                    char *repr = model_py_repr(sample);
                    printf("  %zu new paths, e.g. %s\n", added_total, repr);
                    free(repr);
                }
                json_free(sample);

                if (changed_total == 0 && added_total == 0) {
                    printf("  rewritten with no value change\n");
                }
            }

            snap_free(&previous);
            previous = current;
            has_previous = true;
            sas4_load_free(&sl);
        }
        Sleep(POLL_MS);
    }
}
