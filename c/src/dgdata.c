/*
 * DGDATA checksum -- port of dgdata.py's _table_entry() and checksum().
 *
 * The table generator reinterprets each intermediate value as a *signed* int32 before
 * shifting it right, so once a value's top bit is set, the shift fills from the left with
 * one-bits instead of zero-bits (ActionScript's `>>` on a negative number). A plain `>>` on
 * a C `uint32_t` is a logical shift -- it always fills with zero-bits -- so translating the
 * Python line for line with an unsigned type silently produces real CRC-32 instead, with no
 * warning from the compiler. This is the single most likely place for this port to go wrong
 * (see the plan's Risks section), which is why it gets its own task and its own pinned
 * constant (dg_table()[1] == 0x09073096, not CRC-32's 0x77073096) before anything else in
 * the format touches it.
 *
 * Rather than relying on C's implementation-defined behaviour for `>>` on a negative signed
 * int (which happens to be arithmetic on this toolchain, but the standard does not require
 * it), the shift is written out explicitly: a logical shift right by one, then the vacated
 * top bit is set back to whatever the original top bit was. That is bit-for-bit what
 * reinterpreting as a negative int32 and arithmetic-shifting it produces, without depending
 * on any signed-shift behaviour at all.
 */
#include "dgdata.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t table_entry_uncached(uint32_t index) {
    uint32_t value = index;
    for (int i = 0; i < 8; i++) {
        uint32_t lsb = value & 1u;
        uint32_t sign_bit = value & 0x80000000u;
        uint32_t shifted = (value >> 1) | sign_bit;
        value = lsb ? (shifted ^ DG_POLYNOMIAL) : shifted;
    }
    return value;
}

uint32_t dg_table_entry(uint32_t index) {
    return table_entry_uncached(index & 0xFFu);
}

const uint32_t *dg_table(void) {
    static uint32_t table[256];
    static int built = 0;
    if (!built) {
        for (int i = 0; i < 256; i++) {
            table[i] = table_entry_uncached((uint32_t)i);
        }
        built = 1;
    }
    return table;
}

uint32_t dg_checksum(const uint8_t *plain, size_t len) {
    const uint32_t *table = dg_table();
    uint32_t acc = 0;
    for (size_t i = 0; i < len; i++) {
        acc = ((acc >> 8) & 0xFFFFFFu) ^ table[(acc ^ plain[i]) & 0xFFu];
    }
    return acc;
}

bool dg_decode(const uint8_t *raw, size_t raw_len, uint8_t **out, size_t *out_len) {
    size_t header_len = strlen(DG_HEADER);
    if (raw_len < header_len || memcmp(raw, DG_HEADER, header_len) != 0) {
        return false;
    }
    /* Python's decode() slices `raw[HEADER_LENGTH:]`, which is simply empty (not an error)
     * if raw is shorter than the full 14-byte header -- so a file that starts with
     * "DGDATA" but has fewer than 14 bytes total decodes to an empty plaintext, not a
     * failure. Match that rather than treating it as malformed. */
    size_t body_len = raw_len > DG_HEADER_LENGTH ? raw_len - DG_HEADER_LENGTH : 0;
    const uint8_t *body = raw + DG_HEADER_LENGTH;
    uint8_t *plain = (uint8_t *)malloc(body_len > 0 ? body_len : 1);
    if (!plain) {
        return false;
    }
    for (size_t i = 0; i < body_len; i++) {
        plain[i] = (uint8_t)(body[i] - DG_BYTE_OFFSET - (int)(i % DG_CYCLE));
    }
    *out = plain;
    *out_len = body_len;
    return true;
}

void dg_encode(const uint8_t *plain, size_t len, uint8_t **out, size_t *out_len) {
    uint8_t *built = (uint8_t *)malloc(len + DG_HEADER_LENGTH);
    uint32_t sum = dg_checksum(plain, len);
    /* "DGDATA%08x" -- 6-byte literal header plus 8 lowercase hex digits, 14 bytes total.
     * Formatted into a scratch buffer first, not straight into `built`: snprintf always
     * writes a NUL terminator, which at built[14] would clobber the first real body byte
     * whenever len > 0. */
    char header[DG_HEADER_LENGTH + 1];
    snprintf(header, sizeof(header), "%s%08x", DG_HEADER, sum);
    memcpy(built, header, DG_HEADER_LENGTH);
    for (size_t i = 0; i < len; i++) {
        built[DG_HEADER_LENGTH + i] = (uint8_t)(plain[i] + DG_BYTE_OFFSET + (int)(i % DG_CYCLE));
    }
    *out = built;
    *out_len = len + DG_HEADER_LENGTH;
}

int dg_verify(const uint8_t *raw, size_t raw_len, char stored_out[9], char computed_out[9]) {
    uint8_t *plain;
    size_t plain_len;
    if (!dg_decode(raw, raw_len, &plain, &plain_len)) {
        return -1;
    }
    size_t header_len = strlen(DG_HEADER);
    /* raw[6:14] -- the 8 checksum digits, after the fixed "DGDATA" literal. dg_decode
     * already confirmed raw_len >= header_len (6), but the checksum digits themselves may
     * still be short or missing if the file is truncated right after the literal; copy
     * whatever is actually there and let the string comparison below decide. */
    size_t avail = raw_len > header_len ? raw_len - header_len : 0;
    size_t digits = avail < 8 ? avail : 8;
    memcpy(stored_out, raw + header_len, digits);
    stored_out[digits] = '\0';
    uint32_t sum = dg_checksum(plain, plain_len);
    free(plain);
    snprintf(computed_out, 9, "%08x", sum);
    return strcmp(stored_out, computed_out) == 0 ? 1 : 0;
}

int dg_load(const char *path, char stored_out[9], uint8_t **out_plain, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    uint8_t *raw = (uint8_t *)malloc((size_t)size > 0 ? (size_t)size : 1);
    if (!raw) {
        fclose(f);
        return -1;
    }
    size_t got = fread(raw, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        free(raw);
        return -1;
    }
    if (!dg_decode(raw, (size_t)size, out_plain, out_len)) {
        free(raw);
        return -2;
    }
    size_t header_len = strlen(DG_HEADER);
    size_t avail = (size_t)size > header_len ? (size_t)size - header_len : 0;
    size_t digits = avail < 8 ? avail : 8;
    memcpy(stored_out, raw + header_len, digits);
    stored_out[digits] = '\0';
    free(raw);
    return 0;
}

/* One UTF-8 code point (or, if invalid, one replaced byte) appended to *out at *o, growing
 * *out as needed. Mirrors Python's bytes.decode("utf-8", "replace"): any byte sequence that
 * is not valid UTF-8 becomes one U+FFFD (encoded EF BF BD) per invalid byte, and decoding
 * resumes at the very next byte -- CPython's own replace handler can consume more than one
 * bad byte per replacement in some cases, which this does not reproduce, but every real save
 * observed for this format is pure ASCII (see the plan's Context), so no real input reaches
 * this path at all; it exists so a genuinely malformed byte does not crash the tool. */
static void utf8_replace_append(const uint8_t *data, size_t len, char **out, size_t *o,
                                 size_t *cap) {
    size_t i = 0;
    while (i < len) {
        uint8_t b0 = data[i];
        int seqlen = 0;
        if (b0 < 0x80) {
            seqlen = 1;
        } else if ((b0 & 0xE0) == 0xC0) {
            seqlen = 2;
        } else if ((b0 & 0xF0) == 0xE0) {
            seqlen = 3;
        } else if ((b0 & 0xF8) == 0xF0) {
            seqlen = 4;
        }
        bool valid = seqlen > 0 && i + (size_t)seqlen <= len;
        for (int k = 1; valid && k < seqlen; k++) {
            if ((data[i + k] & 0xC0) != 0x80) {
                valid = false;
            }
        }
        size_t need = valid ? (size_t)seqlen : 3;
        if (*o + need + 1 > *cap) {
            *cap = (*cap + need + 1) * 2;
            *out = (char *)realloc(*out, *cap);
        }
        if (valid) {
            memcpy(*out + *o, data + i, (size_t)seqlen);
            *o += (size_t)seqlen;
            i += (size_t)seqlen;
        } else {
            (*out)[(*o)++] = (char)0xEF;
            (*out)[(*o)++] = (char)0xBF;
            (*out)[(*o)++] = (char)0xBD;
            i += 1;
        }
    }
}

static char *utf8_replace_decode(const uint8_t *data, size_t len) {
    size_t cap = len + 16;
    char *out = (char *)malloc(cap);
    size_t o = 0;
    utf8_replace_append(data, len, &out, &o, &cap);
    out[o] = '\0';
    return out;
}

void dg_changed_fields(const uint8_t *old_plain, size_t old_len,
                        const uint8_t *new_plain, size_t new_len, size_t max_span,
                        DgChangedField **out, size_t *out_count) {
    size_t limit = old_len < new_len ? old_len : new_len;

    /* Group differing offsets into runs the same way the Python does: a new offset joins
     * the current run if it is at most one past the run's last offset, else it starts a
     * new run. */
    size_t run_cap = 16;
    size_t *run_start = (size_t *)malloc(run_cap * sizeof(size_t));
    size_t *run_span = (size_t *)malloc(run_cap * sizeof(size_t));
    size_t run_count = 0;
    size_t last_offset = 0;
    bool have_run = false;

    for (size_t i = 0; i < limit; i++) {
        if (old_plain[i] == new_plain[i]) {
            continue;
        }
        if (have_run && i - last_offset <= 1) {
            run_span[run_count - 1] += 1;
        } else {
            if (run_count == run_cap) {
                run_cap *= 2;
                run_start = (size_t *)realloc(run_start, run_cap * sizeof(size_t));
                run_span = (size_t *)realloc(run_span, run_cap * sizeof(size_t));
            }
            run_start[run_count] = i;
            run_span[run_count] = 1;
            run_count++;
        }
        last_offset = i;
        have_run = true;
    }

    DgChangedField *fields = NULL;
    size_t field_count = 0;
    size_t field_cap = 0;
    for (size_t r = 0; r < run_count; r++) {
        size_t start = run_start[r];
        size_t span = run_span[r];
        if (span > max_span) {
            continue;
        }
        size_t pad = 12;
        size_t lo = start > pad ? start - pad : 0;
        size_t hi = start + span + pad;
        if (hi > limit) {
            hi = limit;
        }
        if (field_count == field_cap) {
            field_cap = field_cap ? field_cap * 2 : 8;
            fields = (DgChangedField *)realloc(fields, field_cap * sizeof(DgChangedField));
        }
        fields[field_count].offset = start;
        fields[field_count].span = span;
        fields[field_count].before = utf8_replace_decode(old_plain + lo, hi - lo);
        fields[field_count].after = utf8_replace_decode(new_plain + lo, hi - lo);
        field_count++;
    }

    free(run_start);
    free(run_span);
    *out = fields;
    *out_count = field_count;
}

void dg_free_changed_fields(DgChangedField *fields, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(fields[i].before);
        free(fields[i].after);
    }
    free(fields);
}
