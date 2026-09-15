"""
The CLI differential harness -- task 16 of port-to-c-sas4-cli. Runs every one of the
seventeen subcommands through both `py tools/sas4.py` and `c/sas4.exe` and reports one
summary. Exits non-zero on any difference.

    py c/tests/differential_cli.py
    py c/tests/differential_cli.py --online     also compare the `items` download

Requires the C tool already built (`cd c && make`).

PRIVACY: this file ships inside the public zip (see make_dist.py) and it runs read-only
commands against real saves when they are present on the machine running it. It must never
print a value, a decoded profile field, or a real save's own filename or path -- a save's
path carries the Steam account number, and Version/link and Version/analytics carry account
identifiers of their own. Real files are referred to only by an index ("real file #7"),
assigned by enumeration order, never by name. Command output is compared by SHA-256 and by
length; when it differs, only the line index and the two lengths are reported, never the
bytes. Every command that writes runs against a GENERATED save in a temporary directory,
never a real one, not even a copy.

SIDE EFFECTS, and how they are undone. The tools write to their own data directory, not
beside the --file they were given, so three real directories are touched and all three are
snapshotted and restored:

  * backups/  -- every write command takes a backup there. Only the delta is removed.
  * saves/    -- `watch --archive` copies each version it sees there. Only the delta.
  * ITEMS.md  -- `items --catalog` with no value writes it. Restored, or removed if it did
                 not exist.

`items` makes an HTTPS request to a public GitHub URL. A shipped test should not reach the
network unless asked, so by default this harness compares only the parts of `items` that
need no network; pass --online to compare the download itself. Nothing is ever sent but the
request.

Three decisions from the plan shape this file and must not be undone:

  * Decision 10 -- argparse's help text is width-dependent, so COLUMNS is pinned to 80 for
    every subprocess. Without it the --help comparisons measure the terminal, not the port.
  * Decision 11 -- a file that fails to load produces analogous but not byte-identical
    wording. Such a file is probed for and skipped WITH A PRINTED REASON, after confirming
    both implementations refuse it.
  * Decision 13 -- a redirected Python `watch` stopped with CTRL_BREAK loses its whole
    stdout buffer, so the Python side of that one comparison runs with -u.
"""
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))       # SAS4Trainer/c/tests
C_DIR = os.path.dirname(HERE)                            # SAS4Trainer/c
SAS4_ROOT = os.path.dirname(C_DIR)                        # SAS4Trainer
TOOLS = os.path.join(SAS4_ROOT, "tools")
SAS4_PY = os.path.join(TOOLS, "sas4.py")
BACKUPS = os.path.join(SAS4_ROOT, "backups")
SAVES = os.path.join(SAS4_ROOT, "saves")
ITEMS_MD = os.path.join(SAS4_ROOT, "ITEMS.md")

sys.path.insert(0, TOOLS)
import dgdata  # noqa: E402
import sas4_model  # noqa: E402

ONLINE = "--online" in sys.argv

ENV = dict(os.environ)
ENV["COLUMNS"] = "80"  # Decision 10


def c_exe():
    path = os.path.join(C_DIR, "sas4.exe")
    if not os.path.isfile(path):
        print("missing sas4.exe -- build the C tool first: cd c && make")
        sys.exit(2)
    return path


EXE = c_exe()
PY = [sys.executable, SAS4_PY]
C = [EXE]


# --- reporting ------------------------------------------------------------------------------

class Sweep:
    """One subcommand's comparisons. Labels only -- never a value, never a real path."""

    def __init__(self, name):
        self.name = name
        self.compared = 0
        self.mismatches = []

    def record(self, label, ok):
        self.compared += 1
        if not ok:
            self.mismatches.append(label)


ALL_SWEEPS = []


def run_sweep(name, fn, *args):
    sweep = Sweep(name)
    ALL_SWEEPS.append(sweep)
    fn(sweep, *args)
    flag = "" if not sweep.mismatches else "   <-- %d DIFFERENT" % len(sweep.mismatches)
    print("  %-34s %5d compared%s" % (sweep.name, sweep.compared, flag))
    for label in sweep.mismatches[:10]:
        print("      %s" % label)
    return sweep


# --- running the two implementations ---------------------------------------------------------

def both(args, stdin=b"", cwd=None, py_unbuffered=False):
    """(py_result, c_result) for the same argument list."""
    py_cmd = [sys.executable] + (["-u"] if py_unbuffered else []) + [SAS4_PY]
    out = []
    for cmd in (py_cmd, C):
        out.append(subprocess.run(cmd + args, input=stdin, capture_output=True, env=ENV,
                                   cwd=cwd or SAS4_ROOT))
    return out[0], out[1]


def digest(blob):
    return hashlib.sha256(blob).hexdigest()


def compare(sweep, label, a, b, mask=None):
    """Compare two CompletedProcess results by exit code and stdout, reporting structure only."""
    a_out, b_out = a.stdout, b.stdout
    if mask:
        a_out, b_out = mask(a_out), mask(b_out)
    ok = a.returncode == b.returncode and digest(a_out) == digest(b_out)
    if not ok:
        al, bl = a_out.split(b"\n"), b_out.split(b"\n")
        where = "line counts %d/%d" % (len(al), len(bl))
        for i in range(min(len(al), len(bl))):
            if al[i] != bl[i]:
                where = "first differing line %d, lengths %d/%d" % (i, len(al[i]), len(bl[i]))
                break
        label = "%s  (rc %d/%d, %s)" % (label, a.returncode, b.returncode, where)
    sweep.record(label, ok)
    return ok


# --- real saves, by index only -----------------------------------------------------------------

def find_real_saves():
    found = []
    for name in ("saves", "backups"):
        base = os.path.join(SAS4_ROOT, name)
        if not os.path.isdir(base):
            continue
        for dirpath, _dirs, files in os.walk(base):
            for filename in sorted(files):
                if filename.lower().endswith(".save"):
                    found.append(os.path.join(dirpath, filename))
    return found


def loadable_real_saves(paths):
    """(usable, skipped) -- Decision 11: a file that fails to load is skipped WITH a printed
    reason, after confirming BOTH implementations refuse it rather than assuming."""
    usable, skipped = [], []
    for index, path in enumerate(paths):
        a, b = both(["--file", path, "verify"])
        if a.returncode == 0 and b.returncode == 0:
            usable.append((index, path))
            continue
        a2, b2 = both(["--file", path, "get", "Version/Profile"])
        if a2.returncode != 0 and b2.returncode != 0:
            skipped.append(index)
        else:
            usable.append((index, path))
    return usable, skipped


# --- generated saves -----------------------------------------------------------------------

def write_generated(path, **kwargs):
    document = sas4_model.generate(**kwargs)
    plain = json.dumps(document, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    with open(path, "wb") as handle:
        handle.write(dgdata.encode(plain))
    return document


def fresh(tmp, name="profile.save", **kwargs):
    directory = os.path.join(tmp, name.replace(".", "_") + "-%d" % time.time_ns())
    os.makedirs(directory)
    path = os.path.join(directory, name)
    write_generated(path, **kwargs)
    return path


# --- the real directories these commands write into -------------------------------------------

def dir_snapshot(path):
    return set(os.listdir(path)) if os.path.isdir(path) else set()


def dir_restore(path, before):
    """Remove exactly what appeared since `before`. Never touches anything that was there."""
    removed = 0
    for name in sorted(dir_snapshot(path) - before):
        target = os.path.join(path, name)
        if os.path.isdir(target):
            shutil.rmtree(target, ignore_errors=True)
        else:
            os.remove(target)
        removed += 1
    return removed


def file_snapshot(path):
    if not os.path.exists(path):
        return (False, None)
    with open(path, "rb") as handle:
        return (True, handle.read())


def file_restore(path, state):
    existed, blob = state
    if existed:
        with open(path, "wb") as handle:
            handle.write(blob)
    elif os.path.exists(path):
        os.remove(path)


# --- masks ------------------------------------------------------------------------------------

BACKUP_LINE = re.compile(rb"backup\s+\S.*")
TIMESTAMP = re.compile(rb"=== \d\d:\d\d:\d\d ===")


def mask_backup(blob):
    """A backup path is timestamped to the second, so two runs can legitimately differ."""
    out = []
    for line in blob.split(b"\n"):
        at = line.find(b"backup")
        if at != -1 and BACKUP_LINE.match(line[at:]):
            line = line[:at] + b"backup <STAMPED>"
        out.append(line)
    return b"\n".join(out)


def mask_paths(*paths):
    def mask(blob):
        for index, path in enumerate(paths):
            blob = blob.replace(path.encode(), b"<PATH%d>" % index)
        return mask_backup(blob)
    return mask


# --- the sweeps ---------------------------------------------------------------------------------

def sweep_help(sweep):
    """Task 1's text comparisons: every subparser's help, and the error paths."""
    commands = ["where", "view", "list", "kinds", "get", "set", "give", "level", "mastery",
                "contribute", "items", "verify", "decode", "encode", "watch", "session",
                "graft"]
    compare(sweep, "top-level --help", *both(["--help"]))
    compare(sweep, "no arguments", *both([]))
    for command in commands:
        compare(sweep, "%s --help" % command, *both([command, "--help"]))
    for args, label in (
            (["nosuchcommand"], "unknown command"),
            (["view", "--nope"], "unknown option"),
            (["view", "--slot", "x"], "bad int"),
            (["view", "--section", "nope"], "bad choice"),
            (["get"], "missing positional"),
            (["give"], "missing item"),
            (["set", "only-one"], "missing value"),
            (["graft"], "missing source"),
            (["mastery", "--set", "1=1", "--all", "2"], "mutually exclusive"),
            (["--file"], "global option with no value"),
            (["view", "--file", "x"], "--file after the subcommand"),
    ):
        compare(sweep, label, *both(args))


def sweep_read_only(sweep, usable):
    """Every command that only reads, against every loadable real save."""
    for index, path in usable:
        mask = mask_paths(path)
        for slot in (0, 1):
            compare(sweep, "real #%d view slot %d" % (index, slot),
                    *both(["--file", path, "view", "--slot", str(slot)]), mask=mask)
        compare(sweep, "real #%d list" % index,
                *both(["--file", path, "list", "--type", "int"]), mask=mask)
        compare(sweep, "real #%d kinds" % index, *both(["--file", path, "kinds"]), mask=mask)
        compare(sweep, "real #%d verify" % index,
                *both(["--file", path, "verify"]), mask=mask)
        compare(sweep, "real #%d get" % index,
                *both(["--file", path, "get", "Version/Profile"]), mask=mask)
        compare(sweep, "real #%d mastery" % index,
                *both(["--file", path, "mastery"]), mask=mask)
        compare(sweep, "real #%d contribute --print" % index,
                *both(["--file", path, "contribute", "--print"]), mask=mask)


def sweep_where(sweep):
    compare(sweep, "where", *both(["where"]))


def sweep_decode_encode(sweep, usable, tmp):
    for index, path in usable[:6]:
        out_py = os.path.join(tmp, "decode-py-%d.json" % index)
        out_c = os.path.join(tmp, "decode-c-%d.json" % index)
        a = subprocess.run(PY + ["--file", path, "decode", out_py], capture_output=True,
                           env=ENV, cwd=SAS4_ROOT)
        b = subprocess.run(C + ["--file", path, "decode", out_c], capture_output=True,
                           env=ENV, cwd=SAS4_ROOT)
        same_text = digest(a.stdout.replace(out_py.encode(), b"<OUT>")) == \
            digest(b.stdout.replace(out_c.encode(), b"<OUT>"))
        same_file = (os.path.exists(out_py) and os.path.exists(out_c)
                     and digest(open(out_py, "rb").read()) == digest(open(out_c, "rb").read()))
        sweep.record("real #%d decode" % index,
                     a.returncode == b.returncode and same_text and same_file)

        enc_py = os.path.join(tmp, "encode-py-%d.save" % index)
        enc_c = os.path.join(tmp, "encode-c-%d.save" % index)
        a = subprocess.run(PY + ["encode", out_py, enc_py], capture_output=True, env=ENV,
                           cwd=SAS4_ROOT)
        b = subprocess.run(C + ["encode", out_c, enc_c], capture_output=True, env=ENV,
                           cwd=SAS4_ROOT)
        same_file = (os.path.exists(enc_py) and os.path.exists(enc_c)
                     and digest(open(enc_py, "rb").read()) == digest(open(enc_c, "rb").read()))
        sweep.record("real #%d encode" % index, a.returncode == b.returncode and same_file)


def write_case(sweep, label, tmp, args, stdin=b"", generated_kwargs=None):
    """One write command, run on a fresh generated save for each implementation.

    The two saves are byte-identical to start with, so comparing their SHA-256 afterwards
    compares what the command actually wrote.
    """
    results = []
    for cmd in (PY, C):
        path = fresh(tmp, **(generated_kwargs or {}))
        before_backups = dir_snapshot(BACKUPS)
        proc = subprocess.run(cmd + ["--file", path] + args, input=stdin,
                              capture_output=True, env=ENV, cwd=os.path.dirname(path))
        results.append((proc, digest(open(path, "rb").read()),
                        dir_restore(BACKUPS, before_backups),
                        mask_paths(path)(proc.stdout)))
    (pa, pf, pb, po), (ca, cf, cb, co) = results
    ok = (pa.returncode == ca.returncode and pf == cf and pb == cb and digest(po) == digest(co))
    if not ok:
        label = "%s  (rc %d/%d, same file %s, backups %d/%d)" % (
            label, pa.returncode, ca.returncode, pf == cf, pb, cb)
    sweep.record(label, ok)


def sweep_writes(sweep, tmp):
    cases = [
        ("set money", ["set", "/Inventory/Profile0/Money", "424242", "--force"], b""),
        ("set dry-run", ["set", "/Inventory/Profile0/Money", "1", "--force", "--dry-run"], b""),
        ("set bad path", ["set", "/No/Such", "1", "--force"], b""),
        ("set bad value", ["set", "/Inventory/Profile0/Money", "abc", "--force"], b""),
        ("level 55", ["level", "55", "--force"], b""),
        ("level dry-run", ["level", "55", "--force", "--dry-run"], b""),
        ("level out of range", ["level", "999", "--force"], b""),
        ("mastery --set", ["mastery", "--set", "3=5,7=2", "--force"], b""),
        ("mastery --all", ["mastery", "--all", "3", "--force"], b""),
        ("mastery bad track", ["mastery", "--set", "99=1", "--force"], b""),
        ("give weapon", ["give", "129", "--kind", "weapon", "--force"], b""),
        ("give equipment", ["give", "101", "--kind", "equipment", "--force"], b""),
        ("give ambiguous", ["give", "129", "--force"], b""),
        ("give unknown id", ["give", "999999", "--force"], b""),
        ("give dry-run", ["give", "129", "--kind", "weapon", "--force", "--dry-run"], b""),
    ]
    for label, args, stdin in cases:
        write_case(sweep, label, tmp, args, stdin)


def sweep_graft(sweep, tmp):
    """Both saves generated: graft copies fields between saves by design."""
    source = fresh(tmp, name="source.save", level=40, money=999999)
    money = "Inventory/Profile0/Money"
    level = "Inventory/Profile0/Skills/PlayerLevel"
    cases = [
        ("graft no fields", []),
        ("graft preview", ["--fields", money]),
        ("graft two fields", ["--fields", "%s,%s" % (money, level)]),
        ("graft unknown field", ["--fields", "No/Such"]),
        ("graft identity leaf", ["--fields", "Version/link"]),
        ("graft identity buried", ["--fields", "Version"]),
        ("graft apply", ["--fields", money, "--apply", "--force"]),
        ("graft apply buried identity", ["--fields", "Version", "--apply", "--force"]),
    ]
    for label, extra in cases:
        results = []
        for cmd in (PY, C):
            dest = fresh(tmp, name="profile.save")
            before_backups = dir_snapshot(BACKUPS)
            proc = subprocess.run(cmd + ["--file", dest, "graft", source] + extra,
                                  capture_output=True, env=ENV, cwd=os.path.dirname(dest))
            results.append((proc, digest(open(dest, "rb").read()),
                            dir_restore(BACKUPS, before_backups),
                            mask_paths(dest, source)(proc.stdout)))
        (pa, pf, pb, po), (ca, cf, cb, co) = results
        ok = (pa.returncode == ca.returncode and pf == cf and pb == cb
              and digest(po) == digest(co))
        if not ok:
            label = "%s  (rc %d/%d, same file %s)" % (label, pa.returncode, ca.returncode,
                                                       pf == cf)
        sweep.record(label, ok)


def sweep_session(sweep, tmp):
    """A generated session file -- never the real one, whose path carries the account."""
    fixtures = {
        "normal": {"user": {"nkapiID": "2200000000000001",
                            "sessionID": "aaaabbbbccccddddeeeeffff0000111122223333"},
                   "meta": {"version": 3, "region": "eu"}},
        "no_user": {"meta": {"version": 3}},
    }
    cases = [
        ("session read", "normal", [], b""),
        ("session read no user", "no_user", [], b""),
        ("session set no equals", "normal", ["--set", "nkapiID"], b""),
        ("session set unknown", "normal", ["--set", "nope=1", "--yes"], b""),
        ("session set --yes", "normal", ["--set", "nkapiID=2200000000000009", "--yes"], b""),
        ("session confirm y", "normal", ["--set", "nkapiID=2200000000000009"], b"y\n"),
        ("session confirm n", "normal", ["--set", "nkapiID=2200000000000009"], b"n\n"),
        ("session set region", "normal", ["--set", "region=us", "--yes"], b""),
    ]
    for label, fixture, extra, stdin in cases:
        results = []
        for cmd in (PY, C):
            directory = os.path.join(tmp, "session-%d" % time.time_ns())
            os.makedirs(directory)
            path = os.path.join(directory, "current.session")
            plain = json.dumps(fixtures[fixture], separators=(",", ":"),
                               ensure_ascii=False).encode("utf-8")
            with open(path, "wb") as handle:
                handle.write(dgdata.encode(plain))
            before_backups = dir_snapshot(BACKUPS)
            proc = subprocess.run(cmd + ["session", "--session", path] + extra, input=stdin,
                                  capture_output=True, env=ENV, cwd=directory)
            results.append((proc, digest(open(path, "rb").read()),
                            dir_restore(BACKUPS, before_backups),
                            mask_paths(path)(proc.stdout)))
        (pa, pf, pb, po), (ca, cf, cb, co) = results
        ok = (pa.returncode == ca.returncode and pf == cf and pb == cb
              and digest(po) == digest(co))
        if not ok:
            label = "%s  (rc %d/%d, same file %s)" % (label, pa.returncode, ca.returncode,
                                                       pf == cf)
        sweep.record(label, ok)


def sweep_watch(sweep, tmp):
    """Bounded: each watcher sees three versions and is stopped after ~11 seconds.

    Decision 13 -- the Python side runs with -u, because a redirected watcher stopped with
    CTRL_BREAK otherwise loses its entire stdout buffer and the comparison would pass while
    measuring two empty files. The C flushes from its own console control handler.
    """
    base = sas4_model.generate()
    versions = []
    for money in (100, 424242, 999999):
        document = json.loads(json.dumps(base))
        document["Inventory"]["Profile0"]["Money"] = money
        plain = json.dumps(document, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
        versions.append(dgdata.encode(plain))

    transcripts = []
    archived = []
    for label, cmd in (("py", [sys.executable, "-u", SAS4_PY]), ("c", C)):
        directory = os.path.join(tmp, "watch-" + label + "x" * (2 - len(label)))
        os.makedirs(directory)
        path = os.path.join(directory, "profile.save")
        with open(path, "wb") as handle:
            handle.write(versions[0])
        out_path = os.path.join(directory, "out.txt")
        out = open(out_path, "wb")
        before_saves = dir_snapshot(SAVES)
        proc = subprocess.Popen(cmd + ["--file", path, "watch", "--archive"], stdout=out,
                                stderr=subprocess.DEVNULL, cwd=directory, env=ENV,
                                creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
        try:
            for blob in versions[1:]:
                time.sleep(2.5)
                with open(path, "wb") as handle:
                    handle.write(blob)
            time.sleep(3.0)
        finally:
            proc.send_signal(signal.CTRL_BREAK_EVENT)
            try:
                proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            out.close()
        copies = sorted(dir_snapshot(SAVES) - before_saves)
        archived.append([digest(open(os.path.join(SAVES, n), "rb").read()) for n in copies])
        dir_restore(SAVES, before_saves)
        blob = open(out_path, "rb").read()
        blob = blob.replace(path.encode(), b"<SAVE>").replace(SAVES.encode(), b"<SAVES>")
        transcripts.append(TIMESTAMP.sub(b"=== <TIME> ===", blob))

    sweep.record("watch transcript (%d/%d bytes)" % (len(transcripts[0]), len(transcripts[1])),
                 len(transcripts[0]) > 0 and transcripts[0] == transcripts[1])
    sweep.record("watch --archive copies (%d/%d)" % (len(archived[0]), len(archived[1])),
                 archived[0] == archived[1])
    sweep.record("no watch-NNN.save left behind",
                 not [n for n in dir_snapshot(SAVES) if n.startswith("watch-")])


def sweep_items(sweep):
    """Offline by default: a shipped test should not reach the network unless asked."""
    compare(sweep, "items --help", *both(["items", "--help"]))
    compare(sweep, "items --catalog with a bad value", *both(["items", "--catalog", "--nope"]))
    if not ONLINE:
        return
    cache = os.path.join(SAS4_ROOT, "decoded", "items.json")
    cache_state = file_snapshot(cache)
    md_state = file_snapshot(ITEMS_MD)
    try:
        outputs, caches = [], []
        for cmd in (PY, C):
            proc = subprocess.run(cmd + ["items"], capture_output=True, env=ENV, cwd=SAS4_ROOT)
            outputs.append((proc.returncode, digest(mask_paths(cache)(proc.stdout))))
            caches.append(digest(open(cache, "rb").read()) if os.path.exists(cache) else None)
        sweep.record("items download (cache identical)", caches[0] == caches[1])
        sweep.record("items output", outputs[0] == outputs[1])
    finally:
        file_restore(cache, cache_state)
        file_restore(ITEMS_MD, md_state)


# --- main ---------------------------------------------------------------------------------------

def main():
    print("CLI differential -- sas4.py vs sas4.exe, COLUMNS=80")
    if not ONLINE:
        print("  (items' download not compared; pass --online to include it)")

    real = find_real_saves()
    usable, skipped = loadable_real_saves(real)
    print("%d real save(s) found, %d loadable" % (len(real), len(usable)))
    for index in skipped:
        print("  real file #%d skipped: it fails to load, and BOTH implementations refuse "
              "it -- their wording differs by Decision 11, so it has nothing comparable"
              % index)
    print()

    backups_before = dir_snapshot(BACKUPS)
    saves_before = dir_snapshot(SAVES)
    md_before = file_snapshot(ITEMS_MD)

    try:
        with tempfile.TemporaryDirectory(prefix="sas4-cli-diff-") as tmp:
            run_sweep("help and error paths", sweep_help)
            run_sweep("where", sweep_where)
            run_sweep("read-only commands", sweep_read_only, usable)
            run_sweep("decode / encode", sweep_decode_encode, usable, tmp)
            run_sweep("write commands (generated)", sweep_writes, tmp)
            run_sweep("graft (generated)", sweep_graft, tmp)
            run_sweep("session (generated)", sweep_session, tmp)
            run_sweep("watch (generated, bounded)", sweep_watch, tmp)
            run_sweep("items", sweep_items)
    finally:
        stray_backups = dir_restore(BACKUPS, backups_before)
        stray_saves = dir_restore(SAVES, saves_before)
        file_restore(ITEMS_MD, md_before)
        if stray_backups or stray_saves:
            print("\ncleaned up %d stray backup dir(s) and %d stray archived save(s)"
                  % (stray_backups, stray_saves))

    total_compared = sum(s.compared for s in ALL_SWEEPS)
    total_mismatches = sum(len(s.mismatches) for s in ALL_SWEEPS)
    empty = [s.name for s in ALL_SWEEPS if s.compared == 0]
    # Only the real-save sweeps may legitimately compare nothing -- on a machine with no SAS4
    # profile, which is the ordinary case for someone who just unzipped this.
    expected_empty = {"read-only commands", "decode / encode"}
    broken_empty = [n for n in empty if n not in expected_empty or usable]

    print("\n%d total comparisons across %d sweeps, %d difference(s)"
          % (total_compared, len(ALL_SWEEPS), total_mismatches))
    if empty and not broken_empty:
        print("  %d sweep(s) compared nothing because no loadable real save was available "
              "(expected on a machine with no SAS4 profile): %s"
              % (len(empty), ", ".join(empty)))
    if broken_empty:
        print("WARNING: these sweeps compared nothing at all: %s" % ", ".join(broken_empty))

    if total_mismatches or broken_empty:
        print("\nDIFFERENCES FOUND" if total_mismatches else "\nINCOMPLETE RUN")
        return 1
    print("\nALL MATCH")
    return 0


if __name__ == "__main__":
    sys.exit(main())
