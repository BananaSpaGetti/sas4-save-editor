"""
Reader and writer for SAS4's `DGDATA` save files. The format is fully solved.

    file  =  "DGDATA"  +  8 hex characters  +  obfuscated body

The body is UTF-8 JSON with a per-index offset added to every byte, and the eight hex
characters are a CRC-style hash of the **plaintext**, before that offset is applied:

    stored[i] = plain[i] + 21 + (i % 6)        (mod 256)
    header    = "DGDATA%08x" % checksum(plain)

`checksum` looks like CRC-32 with polynomial 0xEDB88320, init 0 and no final xor, but it is
**not** CRC-32, and the difference is the whole reason twenty-five algorithm guesses missed
it. The table generator shifts an int32 *arithmetically*, the way ActionScript's `>>` does,
so once a value goes negative the sign bit is carried down instead of a zero. Entry 1 of
the table is `09073096` where a real CRC-32 has `77073096`.

Credit: the algorithm is from `dgdata.js` in hemisemidemipresent/NKsku, by way of the Python
port in SWFplayer/SAS4Tool. Verified here against the samples in `saves\\`: six of the seven
reproduce their stored checksum exactly, and the seventh is `profile-001`, the hand-edited
file the game rejected -- so the function correctly calls that one invalid.

    py dgdata.py verify    <save>              recompute and compare the header
    py dgdata.py decode    <save> [out.json]   write the plaintext JSON
    py dgdata.py encode    <json> <out.save>   build a save, checksum and all
    py dgdata.py roundtrip <save>              decode then re-encode, require byte equality

Before writing over a live profile: back it up, and close the game first. The game rewrites
the save on its own schedule and will overwrite an edit made underneath it.
"""

import json
import os
import sys

HEADER = b"DGDATA"
HEADER_LENGTH = 14
BYTE_OFFSET = 21
CYCLE = 6
POLYNOMIAL = 0xEDB88320


def _table_entry(index):
    """One entry of the hash table, with ActionScript's arithmetic int32 shift."""
    value = index & 0xFFFFFFFF
    for _ in range(8):
        signed = value - 0x100000000 if value & 0x80000000 else value
        if signed & 1:
            signed >>= 1
            signed ^= POLYNOMIAL
        else:
            signed >>= 1
        value = signed & 0xFFFFFFFF
    return value


TABLE = [_table_entry(i) for i in range(256)]


def checksum(plain):
    """The value the eight hex characters spell, computed over the plaintext."""
    acc = 0
    for byte in plain:
        acc = (((acc >> 8) & 0xFFFFFF) ^ TABLE[(acc ^ byte) & 0xFF]) & 0xFFFFFFFF
    return acc


def decode(raw):
    """Plaintext JSON bytes from a whole save file."""
    if not raw.startswith(HEADER):
        raise ValueError("not a DGDATA file")
    body = raw[HEADER_LENGTH:]
    return bytes((body[i] - BYTE_OFFSET - (i % CYCLE)) & 0xFF for i in range(len(body)))


def encode(plain):
    """A complete save file, header and checksum included, from plaintext JSON bytes."""
    header = ("DGDATA%08x" % checksum(plain)).encode("ascii")
    body = bytes((plain[i] + BYTE_OFFSET + (i % CYCLE)) & 0xFF for i in range(len(plain)))
    return header + body


def load(path):
    """(stored checksum, plaintext) for a save on disk."""
    raw = open(path, "rb").read()
    return raw[6:HEADER_LENGTH].decode("ascii"), decode(raw)


def verify(raw):
    """(stored, computed, ok) - whether the game would accept this file."""
    stored = raw[6:HEADER_LENGTH].decode("ascii")
    computed = "%08x" % checksum(decode(raw))
    return stored, computed, stored == computed


def changed_fields(old_plain, new_plain, max_span=80):
    """Runs of plaintext that differ, as (offset, span, before, after) with text snippets.

    Now that the body decodes to JSON, a changed run is readable on its own and there is no
    need to guess field widths. Runs longer than `max_span` are skipped: the save ends with
    a section that grows, and once it does every byte after it shifts.
    """
    limit = min(len(old_plain), len(new_plain))
    offsets = [i for i in range(limit) if old_plain[i] != new_plain[i]]

    runs = []
    for offset in offsets:
        if runs and offset - runs[-1][-1] <= 1:
            runs[-1].append(offset)
        else:
            runs.append([offset])

    out = []
    for run in runs:
        start, span = run[0], len(run)
        if span > max_span:
            continue
        pad = 12
        lo, hi = max(0, start - pad), min(limit, start + span + pad)
        out.append((start, span,
                    old_plain[lo:hi].decode("utf-8", "replace"),
                    new_plain[lo:hi].decode("utf-8", "replace")))
    return out


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    command, path = sys.argv[1], sys.argv[2]
    raw = open(path, "rb").read() if command != "encode" else None

    if command == "verify":
        stored, computed, ok = verify(raw)
        print("stored   %s" % stored)
        print("computed %s" % computed)
        print("VALID" if ok else "MISMATCH - the game would reject this file")
        return 0 if ok else 1

    if command == "decode":
        plain = decode(raw)
        out = sys.argv[3] if len(sys.argv) > 3 else os.path.splitext(path)[0] + ".json"
        with open(out, "wb") as handle:
            handle.write(plain)
        print("wrote %s, %d bytes" % (out, len(plain)))
        try:
            json.loads(plain)
            print("parses as JSON")
        except ValueError as problem:
            print("does NOT parse as JSON: %s" % problem)
        return 0

    if command == "encode":
        if len(sys.argv) < 4:
            print("encode needs an output path")
            return 2
        plain = open(path, "rb").read()
        json.loads(plain)                      # refuse to build a save from broken JSON
        built = encode(plain)
        with open(sys.argv[3], "wb") as handle:
            handle.write(built)
        print("wrote %s, %d bytes, header %s" % (sys.argv[3], len(built),
                                                 built[:HEADER_LENGTH].decode()))
        return 0

    if command == "roundtrip":
        rebuilt = encode(decode(raw))
        same = rebuilt == raw
        print("%d bytes in, %d bytes out" % (len(raw), len(rebuilt)))
        print("byte-identical" if same else "DIFFERS from the original")
        return 0 if same else 1

    print("unknown command %r" % command)
    return 2


if __name__ == "__main__":
    sys.exit(main())
