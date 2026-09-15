/*
 * Ports TestFormat from tools/tests/test_sas4.py -- task 14 of port-to-c-sas4-core (this
 * file started life as task 2's checksum-table pin; extended here to the rest of TestFormat).
 * If this format's arithmetic shift were ever accidentally reimplemented as a plain logical
 * shift, dg_table()[1] would read 0x77073096 (real CRC-32) instead of 0x09073096, and this
 * is the only thing here that would notice.
 */
#include "dgdata.h"
#include "fixtures.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check(const char *name, int ok) {
    printf("  %s  %s\n", ok ? "ok  " : "FAIL", name);
    if (!ok) {
        failures++;
    }
}

int main(void) {
    printf("dg_table()[1]\n");
    uint32_t entry1 = dg_table()[1];
    printf("  0x%08X\n", entry1);
    check("dg_table()[1] is 0x09073096 (arithmetic shift), not CRC-32's 0x77073096",
          entry1 == 0x09073096u);
    check("dg_table_entry(1) agrees with dg_table()[1]", dg_table_entry(1) == entry1);

    /* Invented, not a real profile -- the same golden pair test_sas4.py pins, chosen there
     * precisely because the format is what is being tested, not any real account's data. */
    printf("\ngolden vector: {\"Version\":{\"link\":\"0\"},\"n\":12345}\n");
    static const char plain[] = "{\"Version\":{\"link\":\"0\"},\"n\":12345}";
    uint32_t sum = dg_checksum((const uint8_t *)plain, strlen(plain));
    printf("  checksum = %08x\n", sum);
    char hex[9];
    snprintf(hex, sizeof(hex), "%08x", sum);
    check("checksum of the golden vector is 0a44022c", strcmp(hex, "0a44022c") == 0);

    /* The exact bytes the golden plaintext must produce -- a roundtrip test alone cannot
     * catch a changed DG_BYTE_OFFSET/DG_CYCLE, since encode and decode share the same
     * constant and would stay consistent with each other while producing a file the game
     * would not read. */
    static const uint8_t golden_expected[] = {
        0x44, 0x47, 0x44, 0x41, 0x54, 0x41, 0x30, 0x61, 0x34, 0x34, 0x30, 0x32, 0x32, 0x63,
        0x90, 0x38, 0x6d, 0x7d, 0x8b, 0x8d, 0x7e, 0x85, 0x85, 0x3a, 0x53, 0x95, 0x37, 0x82,
        0x80, 0x86, 0x84, 0x3c, 0x4f, 0x38, 0x47, 0x3a, 0x96, 0x46, 0x37, 0x84, 0x39, 0x52,
        0x4a, 0x4c, 0x48, 0x4a, 0x4c, 0x95};
    uint8_t *golden_encoded;
    size_t golden_encoded_len;
    dg_encode((const uint8_t *)plain, strlen(plain), &golden_encoded, &golden_encoded_len);
    check("dg_encode of the golden vector matches the pinned bytes exactly",
          golden_encoded_len == sizeof(golden_expected) &&
          memcmp(golden_encoded, golden_expected, sizeof(golden_expected)) == 0);
    uint8_t *golden_decoded;
    size_t golden_decoded_len;
    bool golden_decode_ok = dg_decode(golden_expected, sizeof(golden_expected), &golden_decoded,
                                       &golden_decoded_len);
    check("dg_decode of the pinned bytes reproduces the golden plaintext",
          golden_decode_ok && golden_decoded_len == strlen(plain) &&
          memcmp(golden_decoded, plain, strlen(plain)) == 0);
    check("DG_BYTE_OFFSET is 21", DG_BYTE_OFFSET == 21);
    check("DG_CYCLE is 6", DG_CYCLE == 6);
    free(golden_encoded);
    if (golden_decode_ok) {
        free(golden_decoded);
    }

    printf("\nroundtrip and decode/encode inverses\n");
    {
        const char *dir = fixtures_temp_dir();
        char path[600];
        snprintf(path, sizeof(path), "%s\\roundtrip.save", dir);
        JsonValue *d = fixtures_document(5, 1000);
        fixtures_write_save(d, path);
        json_free(d);

        size_t raw_len;
        uint8_t *raw = fixtures_read_file(path, &raw_len);
        uint8_t *decoded;
        size_t decoded_len;
        bool decode_ok = dg_decode(raw, raw_len, &decoded, &decoded_len);
        check("a generated save decodes", decode_ok);
        if (decode_ok) {
            uint8_t *reencoded;
            size_t reencoded_len;
            dg_encode(decoded, decoded_len, &reencoded, &reencoded_len);
            check("encode(decode(raw)) is byte-identical to raw",
                  reencoded_len == raw_len && memcmp(reencoded, raw, raw_len) == 0);
            free(reencoded);
            free(decoded);
        }
        free(raw);
    }
    {
        /* Invented plaintext holding a multi-byte UTF-8 sequence, to exercise the same path
         * a non-ASCII item name or player name would. */
        static const uint8_t utf8_plain[] = {'{', '"', 'a', '"', ':', '1', ',', '"', 'b', '"',
                                              ':', '"', 0xc3, 0xa9, '"', '}'};
        uint8_t *encoded;
        size_t encoded_len;
        dg_encode(utf8_plain, sizeof(utf8_plain), &encoded, &encoded_len);
        uint8_t *decoded;
        size_t decoded_len;
        bool ok = dg_decode(encoded, encoded_len, &decoded, &decoded_len);
        check("decode inverts encode for a UTF-8 plaintext",
              ok && decoded_len == sizeof(utf8_plain) &&
              memcmp(decoded, utf8_plain, sizeof(utf8_plain)) == 0);
        free(encoded);
        if (ok) {
            free(decoded);
        }
    }

    printf("\nverify\n");
    {
        const char *dir = fixtures_temp_dir();
        char path[600];
        snprintf(path, sizeof(path), "%s\\verify.save", dir);
        JsonValue *d = fixtures_document(5, 1000);
        fixtures_write_save(d, path);
        json_free(d);

        size_t raw_len;
        uint8_t *raw = fixtures_read_file(path, &raw_len);
        char stored[9], computed[9];
        int verdict = dg_verify(raw, raw_len, stored, computed);
        check("an unmodified save verifies", verdict == 1);

        /* One bit, in the body (past the 14-byte header). */
        raw[DG_HEADER_LENGTH + 5] ^= 0x01;
        int verdict2 = dg_verify(raw, raw_len, stored, computed);
        check("a flipped byte in the body fails verification", verdict2 == 0);
        check("the stored and computed checksums now disagree", strcmp(stored, computed) != 0);
        free(raw);
    }

    printf("\nforeign file\n");
    {
        uint8_t foreign[49];
        memcpy(foreign, "NOTDGDATA", 9);
        memset(foreign + 9, 0, sizeof(foreign) - 9);
        uint8_t *out;
        size_t out_len;
        check("decode refuses a file that does not start with DGDATA",
              !dg_decode(foreign, sizeof(foreign), &out, &out_len));
    }

    printf("\n");
    if (failures) {
        printf("FAILED: %d check(s)\n", failures);
        return 1;
    }
    printf("all checks pass\n");
    return 0;
}
