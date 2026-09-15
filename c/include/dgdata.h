/*
 * DGDATA -- the SAS4 save container format. Port of SAS4Trainer/tools/dgdata.py.
 *
 * This task (task 2 of port-to-c-sas4-core) covers only the checksum: the 256-entry table
 * and the accumulator. encode/decode/verify/load land in task 3.
 */
#ifndef SAS4_DGDATA_H
#define SAS4_DGDATA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DG_HEADER "DGDATA"
#define DG_HEADER_LENGTH 14
#define DG_BYTE_OFFSET 21
#define DG_CYCLE 6
#define DG_POLYNOMIAL 0xEDB88320u

/* One entry of the checksum table, computed with ActionScript's arithmetic int32 shift --
 * see dgdata.c for why a plain `>>` on a uint32_t is the wrong operation here. Exposed on
 * its own (not just through dg_table()) so a caller, or a test, can check one entry without
 * needing the whole table built first. */
uint32_t dg_table_entry(uint32_t index);

/* The full 256-entry table, built once on first call and cached; dg_table()[1] must be
 * 0x09073096, not 0x77073096 (real CRC-32's value there) -- the single discriminating
 * constant this whole format hinges on. */
const uint32_t *dg_table(void);

/* The value the save's eight hex header characters spell, computed over the plaintext
 * (before DG_BYTE_OFFSET is applied). Matches dgdata.py's checksum() exactly. */
uint32_t dg_checksum(const uint8_t *plain, size_t len);

/* Plaintext JSON bytes from a whole save file's raw bytes. Allocates *out via malloc (the
 * caller frees it); on success *out_len is the plaintext length. Returns false, touching
 * neither out param, if raw does not start with "DGDATA" -- dgdata.py's decode() raises
 * ValueError there. */
bool dg_decode(const uint8_t *raw, size_t raw_len, uint8_t **out, size_t *out_len);

/* A complete save file (14-byte header, "DGDATA" + 8 hex checksum digits, plus the
 * obfuscated body) from plaintext JSON bytes. Allocates *out via malloc (caller frees);
 * *out_len is always len + DG_HEADER_LENGTH. */
void dg_encode(const uint8_t *plain, size_t len, uint8_t **out, size_t *out_len);

/* (stored checksum, computed checksum, whether they agree) for a whole raw save file.
 * stored_out and computed_out must each be at least 9 bytes (8 hex digits + NUL). Returns
 * -1, touching neither buffer, if raw does not start with "DGDATA" -- matches dgdata.py's
 * verify() letting decode()'s exception propagate uncaught rather than reporting a
 * checksum mismatch for a file that is not this format at all. Returns 0 (mismatch) or 1
 * (valid) otherwise, filling both buffers either way -- a mismatch is an ordinary, expected
 * outcome, not a failure the caller needs to distinguish by anything other than the digits
 * disagreeing. */
int dg_verify(const uint8_t *raw, size_t raw_len, char stored_out[9], char computed_out[9]);

/* (stored checksum, plaintext) for a save on disk. Allocates *out_plain via malloc (caller
 * frees); stored_out must be at least 9 bytes. Returns 0 on success, -1 if the file could
 * not be opened or fully read, -2 if its contents do not start with "DGDATA". */
int dg_load(const char *path, char stored_out[9], uint8_t **out_plain, size_t *out_len);

/* One run of plaintext bytes that differs between an old and a new decode -- the C
 * equivalent of dgdata.py's changed_fields() tuple. before/after are UTF-8, decoded with
 * an invalid-byte-replaced-by-U+FFFD fallback matching Python's "replace" error handler,
 * NUL-terminated, and allocated via malloc; free them (and the array itself, via
 * dg_free_changed_fields()) when done. */
typedef struct {
    size_t offset;
    size_t span;
    char *before;
    char *after;
} DgChangedField;

/* Runs of plaintext that differ, matching dgdata.py's changed_fields(max_span=80 there).
 * *out receives a malloc'd array of *out_count entries (0 if there are no differing runs
 * within max_span, or none at all); free with dg_free_changed_fields(). */
void dg_changed_fields(const uint8_t *old_plain, size_t old_len,
                        const uint8_t *new_plain, size_t new_len, size_t max_span,
                        DgChangedField **out, size_t *out_count);

void dg_free_changed_fields(DgChangedField *fields, size_t count);

#endif
