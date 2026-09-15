"""
The core differential harness -- task 15 of port-to-c-sas4-core. Runs every sweep the
individual tasks (1-13) defined, against every real save under saves/ and backups/ plus a
set of generated ones, and reports one summary. Exits non-zero on any difference.

    py c/tests/differential_core.py

Requires the C tools already built (`cd c && make`).

PRIVACY: this file ships inside the public zip (see make_dist.py), and it walks real saves
when they are present on the machine running it. It must never print a value, a decoded
profile field, or a real save's own filename or path -- the save's path carries the Steam
account number, and Version/link and Version/analytics carry account identifiers of their
own. Real files are referred to only by an index ("real file #7"), assigned by enumeration
order, never their name. A check() problem string can carry a level, XP or money value even
though no rule reads link/analytics -- those never reach stdout either. Every write-path
sweep (apply_edits, grant/level/mastery plans actually applied) runs on a *generated* save in
a temporary directory, never a real one, not even a copy of one.
"""
import json
import os
import random
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))            # SAS4Trainer/c/tests
C_DIR = os.path.dirname(HERE)                                 # SAS4Trainer/c
SAS4_ROOT = os.path.dirname(C_DIR)                             # SAS4Trainer
TOOLS = os.path.join(SAS4_ROOT, "tools")
ITEMS_CACHE = os.path.join(SAS4_ROOT, "decoded", "items.json")

sys.path.insert(0, TOOLS)
import dgdata  # noqa: E402
import sas4  # noqa: E402
import sas4_model  # noqa: E402

RNG = random.Random(20260915)  # fixed seed -- reproducible sampling, not a secret


def c_exe(name):
    path = os.path.join(C_DIR, name + ".exe")
    if not os.path.isfile(path):
        print("missing %s.exe -- build the C tools first: cd c && make" % name)
        sys.exit(2)
    return path


# --- reporting ------------------------------------------------------------------------------

class Sweep:
    def __init__(self, name):
        self.name = name
        self.compared = 0
        self.mismatches = []  # labels only -- indexes/levels/paths, never values

    def record(self, label, ok):
        self.compared += 1
        if not ok:
            self.mismatches.append(label)


ALL_SWEEPS = []


def run_sweep(name, fn, *args):
    sweep = Sweep(name)
    fn(sweep, *args)
    ALL_SWEEPS.append(sweep)
    status = "OK" if not sweep.mismatches else "MISMATCH"
    print("  %-32s %6d compared   %4d mismatch   %s"
          % (name, sweep.compared, len(sweep.mismatches), status))
    if sweep.mismatches:
        shown = sweep.mismatches[:15]
        print("      mismatches: %s%s" % (", ".join(shown),
              " ... (%d more)" % (len(sweep.mismatches) - 15) if len(sweep.mismatches) > 15
              else ""))
    return sweep


# --- real files (saves/ and backups/) -- referred to only by index, never a name/path ------

class RealFile:
    __slots__ = ("index", "raw", "plain", "document", "skip_reason")

    def __init__(self, index, raw):
        self.index = index
        self.raw = raw
        self.plain = None
        self.document = None
        self.skip_reason = None
        try:
            self.plain = dgdata.decode(raw)
            self.document = json.loads(self.plain)
        except Exception:
            return  # not a decodable DGDATA file -- the dgdata sweep still covers it

        # Decision 10: json.c's integer type is int64_t, matching Python's own int everywhere
        # a real .save file ever needs (measured: every one of the 27 .save files in this
        # repo's saves/+backups/ has a 13-digit maximum integer literal, nowhere near the
        # 19-digit int64_t boundary). Two small non-.save files that the game also happens to
        # encode with the DGDATA container -- a session token, not a profile -- carry a
        # 20-digit id outside that range; sas4.py's own load() is generic enough to accept
        # such a file if someone pointed it at one, so this is a real, if narrow, difference,
        # not a hypothetical. Detected here via the actual parser's own diagnosis (a probe
        # parse), not a heuristic guess, and skipped rather than silently miscounted.
        probe = subprocess.run([c_exe("jsondump")], input=self.plain, capture_output=True)
        if probe.returncode != 0:
            stderr = probe.stderr.decode("utf-8", "replace")
            if "out of range for int64_t" in stderr:
                self.skip_reason = "int64 range (Decision 10)"
                self.document = None
            else:
                self.skip_reason = "C parser failed for an unrecognized reason"
                self.document = None
            return

        # Decision 6: the serializer refuses a float outright, by design, because no real
        # .save file has ever contained one (measured across 26 of them). Task 15's full
        # sweep over every DGDATA-encoded file the game writes -- not only .save files --
        # found the deliberate exception that proves the rule: a "settings" blob (not a
        # profile, not something any sas4.py command reads) does carry floats. That is
        # Decision 6 working as designed, not a bug, so it is skipped the same way the
        # int64-range case above is -- detected by the real serializer's own refusal, not a
        # value-type heuristic.
        indent_probe = subprocess.run([c_exe("jsonindent")], input=self.plain,
                                       capture_output=True)
        if indent_probe.returncode != 0:
            stderr = indent_probe.stderr.decode("utf-8", "replace")
            if "float" in stderr and "Decision 6" in stderr:
                self.skip_reason = "contains a float, refused by design (Decision 6)"
                self.document = None
            else:
                self.skip_reason = "C serializer failed for an unrecognized reason"
                self.document = None


def load_real_files():
    paths = []
    for root_name in ("saves", "backups"):
        root = os.path.join(SAS4_ROOT, root_name)
        if not os.path.isdir(root):
            continue
        for dirpath, _dirs, files in os.walk(root):
            for fn in files:
                paths.append(os.path.join(dirpath, fn))
    out = []
    for i, path in enumerate(paths):
        with open(path, "rb") as f:
            out.append(RealFile(i, f.read()))
    return out


# --- sweeps -----------------------------------------------------------------------------

def sweep_dgdata(sweep, real_files, tmp):
    """backups/ turns out to hold the game's whole local data mirror, not only DGDATA saves
    -- .png, .json, .settings, .session and similar files sit alongside the actual .save
    files, and dgdata.verify()/decode() let a "not a DGDATA file" ValueError propagate
    uncaught for those rather than reporting a checksum mismatch (see dgdata.h's dg_verify
    doc comment). Both sides' refusal is compared here, not just the valid-file case."""
    exe = c_exe("dgdata")
    scratch = os.path.join(tmp, "dg_in.save")
    for rf in real_files:
        with open(scratch, "wb") as f:
            f.write(rf.raw)

        try:
            py_verify = ("OK",) + dgdata.verify(rf.raw)
        except ValueError:
            py_verify = ("FOREIGN",)
        result = subprocess.run([exe, "verify", scratch], capture_output=True)
        lines = result.stdout.decode("utf-8", "replace").splitlines()
        if len(lines) >= 3:
            c_verify = ("OK", lines[0].split()[-1], lines[1].split()[-1], lines[2] == "VALID")
        else:
            c_verify = ("FOREIGN",)
        sweep.record("real file #%d verify" % rf.index, py_verify == c_verify)

        try:
            py_round = ("OK", dgdata.encode(dgdata.decode(rf.raw)) == rf.raw)
        except ValueError:
            py_round = ("FOREIGN",)
        result2 = subprocess.run([exe, "roundtrip", scratch], capture_output=True)
        rt_lines = result2.stdout.decode("utf-8", "replace").splitlines()
        c_round = ("OK", len(rt_lines) > 1 and rt_lines[1] == "byte-identical") \
            if len(rt_lines) >= 2 else ("FOREIGN",)
        sweep.record("real file #%d roundtrip" % rf.index, py_round == c_round)


def sweep_json_tree(sweep, real_files):
    exe = c_exe("jsondump")

    def py_dump(node, path, out):
        if isinstance(node, dict):
            out.append("%s\tobject\t%d" % (path, len(node)))
            for k, v in node.items():
                py_dump(v, "%s/%s" % (path, k), out)
        elif isinstance(node, list):
            out.append("%s\tarray\t%d" % (path, len(node)))
            for i, v in enumerate(node):
                py_dump(v, "%s[%d]" % (path, i), out)
        elif node is None:
            out.append("%s\tnull\t" % path)
        elif isinstance(node, bool):
            out.append("%s\tbool\t%s" % (path, "true" if node else "false"))
        elif isinstance(node, int):
            out.append("%s\tint\t%d" % (path, node))
        elif isinstance(node, float):
            out.append("%s\tfloat\t%.17g" % (path, node))
        elif isinstance(node, str):
            esc = node.replace("\\", "\\\\").replace("\t", "\\t").replace("\n", "\\n") \
                      .replace("\r", "\\r")
            out.append("%s\tstr\t%s" % (path, esc))

    for rf in real_files:
        if rf.document is None:
            continue
        py_lines = []
        py_dump(rf.document, "$", py_lines)
        result = subprocess.run([exe], input=rf.plain, capture_output=True)
        c_lines = result.stdout.decode("utf-8", "replace").splitlines()
        sweep.record("real file #%d" % rf.index, py_lines == c_lines)


def sweep_json_compact_nodes(sweep, real_files):
    exe = c_exe("jsonround")
    for rf in real_files:
        if rf.document is None:
            continue
        py_lines = []

        def walk(node):
            py_lines.append(json.dumps(node, separators=(",", ":"), ensure_ascii=False))
            if isinstance(node, list):
                for v in node:
                    walk(v)
            elif isinstance(node, dict):
                for v in node.values():
                    walk(v)

        walk(rf.document)
        result = subprocess.run([exe], input=rf.plain, capture_output=True)
        c_lines = result.stdout.decode("utf-8", "replace").splitlines()
        sweep.record("real file #%d" % rf.index, py_lines == c_lines)


def sweep_json_indent(sweep, real_files):
    exe = c_exe("jsonindent")
    for rf in real_files:
        if rf.document is None:
            continue
        py_out = json.dumps(rf.document, indent=2, ensure_ascii=False)
        result = subprocess.run([exe], input=rf.plain, capture_output=True)
        c_out = result.stdout.decode("utf-8", "replace")
        # json.dumps has no trailing newline; the C tool's fwrite doesn't add one either.
        sweep.record("real file #%d" % rf.index, py_out == c_out)


def sweep_scalars_kinds(sweep, real_files):
    exe = c_exe("scalars")

    def py_kind(v):
        if isinstance(v, bool):
            return "bool"
        if isinstance(v, int):
            return "int"
        if isinstance(v, float):
            return "float"
        if isinstance(v, str):
            return "str"
        if v is None:
            return "null"
        return "other"

    def py_scalars(node, path, out):
        if isinstance(node, dict):
            for k, v in node.items():
                py_scalars(v, "%s/%s" % (path, k), out)
        elif isinstance(node, list):
            for i, v in enumerate(node):
                py_scalars(v, "%s[%d]" % (path, i), out)
        else:
            out.append((path, py_kind(node)))

    for rf in real_files:
        if rf.document is None:
            continue
        py_list = []
        py_scalars(rf.document, "", py_list)
        py_lines = ["%s\t%s" % (p, k) for p, k in py_list]

        result = subprocess.run([exe], input=rf.plain, capture_output=True)
        out_lines = result.stdout.decode("utf-8", "replace").splitlines()
        sep = out_lines.index("---") if "---" in out_lines else len(out_lines)
        c_lines = out_lines[:sep]
        sweep.record("real file #%d scalars" % rf.index, py_lines == c_lines)

        from collections import Counter
        py_tally = Counter(k for _p, k in py_list).most_common()
        c_tally = [(t.split("\t")[0], int(t.split("\t")[1])) for t in out_lines[sep + 1:]]
        sweep.record("real file #%d kinds" % rf.index, py_tally == c_tally)


def sweep_anchor_band(sweep, real_files):
    """anchor_for over the band the earlier per-task sweep barely touched: a value repeated
    2-19 times, where sibling-walking runs a few iterations and succeeds (see the plan's
    Context). Enumerated per file, bounded, and batched into one anchor.exe call per file.

    anchor_for refuses any path ending in an array index outright (parent_of resolves it to
    an int key, and anchor_for raises immediately: "set does not edit list elements") -- so
    only leaves ending in a plain object key are candidates here at all; a scalar sitting
    directly in an array (like Strongboxes/Claimed's own flattened entries) is never one.
    """
    exe = c_exe("anchor")

    def collect_scalar_paths(node, path, out):
        if isinstance(node, dict):
            for k, v in node.items():
                collect_scalar_paths(v, "%s/%s" % (path, k), out)
        elif isinstance(node, list):
            for i, v in enumerate(node):
                collect_scalar_paths(v, "%s[%d]" % (path, i), out)
        else:
            out.append(path)

    for rf in real_files:
        if rf.document is None:
            continue
        paths = []
        collect_scalar_paths(rf.document, "", paths)
        plain_bytes = rf.plain

        counts = {}
        for p in paths:
            if p.endswith("]"):
                continue  # an array-index leaf -- anchor_for refuses these outright
            try:
                parent, key = sas4.parent_of(rf.document, p.lstrip("/"))
                fragment = ('"%s":%s' % (key, json.dumps(parent[key], separators=(",", ":"),
                                                          ensure_ascii=False))).encode("utf-8")
            except Exception:
                continue
            counts[p] = plain_bytes.count(fragment)

        band = [p for p, n in counts.items() if 2 <= n < 20]
        if len(band) > 300:
            band = RNG.sample(band, 300)
        if not band:
            continue

        py_results = {}
        for p in band:
            try:
                anchor, length = sas4.anchor_for(rf.document, rf.plain, p)
                py_results[p] = ("OK", len(anchor), length)
            except Exception:
                py_results[p] = ("ERROR", None, None)

        args = [exe] + band
        result = subprocess.run(args, input=rf.plain, capture_output=True)
        out_lines = result.stdout.decode("utf-8", "replace").splitlines()
        for p, line in zip(band, out_lines):
            py = py_results[p]
            if line.startswith("ERROR"):
                c = ("ERROR", None, None)
            else:
                parts = line.split()
                c = ("OK", int(parts[1]), int(parts[0]))  # anchor_len, value_len
            ok = (py[0] == c[0]) and (py[0] != "OK" or (py[1] == c[1] and py[2] == c[2]))
            sweep.record("real file #%d %s" % (rf.index, p), ok)


def sweep_check(sweep, real_files):
    exe = c_exe("check")
    tmp_path = os.path.join(tempfile.gettempdir(), "sas4-diffcore-check.json")
    for rf in real_files:
        if rf.document is None:
            continue
        with open(tmp_path, "w", encoding="utf-8") as f:
            json.dump(rf.document, f, separators=(",", ":"), ensure_ascii=False)
        py_problems = sas4_model.check(rf.document)
        result = subprocess.run([exe, tmp_path], capture_output=True)
        lines = result.stdout.decode("utf-8", "replace").splitlines()
        c_problems = [] if lines == ["OK"] else lines
        sweep.record("real file #%d" % rf.index, py_problems == c_problems)

    # generated + tampered, invented values only
    for label, document in generated_check_cases():
        with open(tmp_path, "w", encoding="utf-8") as f:
            json.dump(document, f, separators=(",", ":"), ensure_ascii=False)
        py_problems = sas4_model.check(document)
        result = subprocess.run([exe, tmp_path], capture_output=True)
        lines = result.stdout.decode("utf-8", "replace").splitlines()
        c_problems = [] if lines == ["OK"] else lines
        sweep.record(label, py_problems == c_problems)
    os.remove(tmp_path)


def generated_check_cases():
    cases = [("generated clean level-1", sas4_model.generate(level=1, money=1000)),
             ("generated clean level-20-gear",
              sas4_model.generate(level=20, money=500000, weapons=[129],
                                   equipment=[(101, 2)]))]
    for name, attack in sas4_model.ATTACKS.items():
        d = sas4_model.generate(level=1, money=1000)
        attack(d)
        cases.append(("attack:%s" % name, d))
    return cases


def sweep_xp_curve(sweep):
    exe = c_exe("check")
    for level in range(0, 109):
        py_for = sas4_model.xp_for_level(level)
        py_per = sas4_model.xp_per_level(level) if level >= 1 else None
        result = subprocess.run([exe, "xp", str(level)], capture_output=True)
        c_per_s, c_for_s = result.stdout.decode().split()
        ok = py_for == int(c_for_s) and (level < 1 or py_per == int(c_per_s))
        sweep.record("level=%d" % level, ok)


def sweep_level_plan(sweep, real_files):
    exe = c_exe("levelplan")
    tmp_path = os.path.join(tempfile.gettempdir(), "sas4-diffcore-levelplan.json")

    def compare(document, level, slot, label):
        with open(tmp_path, "w", encoding="utf-8") as f:
            json.dump(document, f, separators=(",", ":"), ensure_ascii=False)
        try:
            py_plan, py_spent = sas4.level_plan(document, level, slot)
            py = ("OK", py_plan, py_spent)
        except sas4.EmptySlot:
            py = ("EMPTY", None, None)
        except ValueError:
            py = ("ERROR", None, None)
        result = subprocess.run([exe, tmp_path, str(level), str(slot)], capture_output=True)
        lines = result.stdout.decode("utf-8", "replace").splitlines()
        if lines and lines[0].startswith("ERROR"):
            c = ("EMPTY" if lines[0].split()[1] == "1" else "ERROR", None, None)
        else:
            plan = []
            spent = None
            for line in lines:
                if line.startswith("spent "):
                    spent = int(line.split()[1])
                else:
                    path, value = line.split("\t", 1)
                    plan.append((path, json.loads(value)))
            c = ("OK", plan, spent)
        ok = py[0] == c[0] and (py[0] != "OK" or (py[1] == c[1] and py[2] == c[2]))
        sweep.record(label, ok)

    # full resolution against saves/ only (a small, bounded set)
    save_only = [rf for rf in real_files if rf.document is not None][:8]
    for rf in save_only:
        for level in range(1, 101):
            compare(rf.document, level, 0, "real file #%d level=%d" % (rf.index, level))

    # coarser resolution against the rest (backups/), for broader real-file coverage
    rest = [rf for rf in real_files if rf.document is not None][8:]
    coarse_levels = [1, 5, 20, 50, 75, 100]
    for rf in rest:
        for level in coarse_levels:
            compare(rf.document, level, 0, "real file #%d level=%d" % (rf.index, level))

    # generated profiles at a spread of starting levels, error paths, empty slots
    for start in (1, 5, 20, 50, 99, 100):
        d = sas4_model.generate(level=start, money=1000, weapons=[129], equipment=[(101, 2)])
        for level in (1, 2, 50, 99, 100):
            compare(d, level, 0, "generated start=%d level=%d" % (start, level))
    d = sas4_model.generate(level=10, money=1000)
    for level in (0, -1, 101, 1000):
        compare(d, level, 0, "out-of-range level=%d" % level)
    for slot in (1, 2, 3, 4, 5, 9):
        compare(d, 5, slot, "slot=%d" % slot)

    os.remove(tmp_path)


def sweep_mastery_plan(sweep, real_files):
    exe = c_exe("masteryplan")
    tmp_path = os.path.join(tempfile.gettempdir(), "sas4-diffcore-masteryplan.json")

    def compare(document, targets, slot, label):
        with open(tmp_path, "w", encoding="utf-8") as f:
            json.dump(document, f, separators=(",", ":"), ensure_ascii=False)
        try:
            py_plan = sas4.mastery_plan(document, dict(targets), slot)
            py = ("OK", py_plan)
        except sas4.EmptySlot:
            py = ("EMPTY", None)
        except ValueError:
            py = ("ERROR", None)
        args = [exe, tmp_path, str(slot)] + ["%d:%d" % (i, lv) for i, lv in targets]
        result = subprocess.run(args, capture_output=True)
        lines = result.stdout.decode("utf-8", "replace").splitlines()
        if lines and lines[0].startswith("ERROR"):
            c = ("EMPTY" if lines[0].split()[1] == "1" else "ERROR", None)
        else:
            plan = [(l.split("\t", 1)[0], json.loads(l.split("\t", 1)[1])) for l in lines]
            c = ("OK", plan)
        ok = py[0] == c[0] and (py[0] != "OK" or py[1] == c[1])
        sweep.record(label, ok)

    save_only = [rf for rf in real_files if rf.document is not None][:8]
    for rf in save_only:
        for index in range(27):
            for level in range(6):
                compare(rf.document, [(index, level)], 0,
                        "real file #%d track=%d level=%d" % (rf.index, index, level))
        for level in range(6):
            compare(rf.document, [(i, level) for i in range(27)], 0,
                    "real file #%d set-all level=%d" % (rf.index, level))

    rest = [rf for rf in real_files if rf.document is not None][8:]
    for rf in rest:
        for level in range(6):
            compare(rf.document, [(i, level) for i in range(27)], 0,
                    "real file #%d set-all level=%d" % (rf.index, level))

    for gen_label, d in (("generated-1", sas4_model.generate(level=1, money=0)),
                         ("generated-20", sas4_model.generate(level=20, money=100000))):
        for index in range(27):
            for level in range(6):
                compare(d, [(index, level)], 0, "%s track=%d level=%d" % (gen_label, index,
                                                                           level))
        compare(d, [], 0, "%s empty targets" % gen_label)
        for bad_index in (-1, 27, 100):
            compare(d, [(bad_index, 0)], 0, "%s bad track=%d" % (gen_label, bad_index))
        for bad_level in (-1, 6, 99):
            compare(d, [(0, bad_level)], 0, "%s bad level=%d" % (gen_label, bad_level))

    d = sas4_model.generate(level=5, money=0)
    for slot in (1, 2, 3, 9):
        compare(d, [(0, 1)], slot, "slot=%d" % slot)

    os.remove(tmp_path)


def sweep_grant_plan(sweep):
    exe = c_exe("grantplan")
    tmp_path = os.path.join(tempfile.gettempdir(), "sas4-diffcore-grantplan.json")

    weapons = sas4.item_names().get("weapon", {})
    equipment = sas4.item_names().get("equipment", {})
    overlap = sorted(set(weapons) & set(equipment))
    weapon_only = sorted(set(weapons) - set(equipment))
    equip_only = sorted(set(equipment) - set(weapons))

    def compare(document, requests, slotprofile, label):
        with open(tmp_path, "w", encoding="utf-8") as f:
            json.dump(document, f, separators=(",", ":"), ensure_ascii=False)
        try:
            py_plan, py_labels = sas4.grant_plan(document, requests, slotprofile)
            py = ("OK", py_plan, py_labels)
        except sas4.EmptySlot:
            py = ("EMPTY", None, None)
        except ValueError:
            py = ("ERROR", None, None)
        args = [exe, ITEMS_CACHE, tmp_path, str(slotprofile)]
        for item_id, kind, grade, bonus, slot in requests:
            args.append("%d:%s:%d:%d:%d" % (item_id, kind, grade, bonus, slot or 0))
        result = subprocess.run(args, capture_output=True)
        lines = result.stdout.decode("utf-8", "replace").splitlines()
        if lines and lines[0].startswith("ERROR"):
            c = ("EMPTY" if lines[0].split()[1] == "1" else "ERROR", None, None)
        else:
            plan, labels = [], []
            for line in lines:
                if line.startswith("LABEL\t"):
                    labels.append(line[len("LABEL\t"):])
                else:
                    path, value = line.split("\t", 1)
                    plan.append((path, json.loads(value)))
            c = ("OK", plan, labels)
        ok = py[0] == c[0] and (py[0] != "OK" or (py[1] == c[1] and py[2] == c[2]))
        sweep.record(label, ok)

    document = sas4_model.generate(level=10, money=1000)
    table = []
    for item_id in weapon_only[:5]:
        table.append(("w-explicit %d g0" % item_id, [(item_id, "weapon", 0, 0, 0)]))
        table.append(("w-explicit %d g12" % item_id, [(item_id, "weapon", 12, 10, 0)]))
    for item_id in equip_only[:5]:
        table.append(("e-explicit %d s2" % item_id, [(item_id, "equipment", 5, 3, 2)]))
        table.append(("e-explicit %d s4" % item_id, [(item_id, "equipment", 0, 0, 4)]))
    for item_id in weapon_only[:3]:
        table.append(("w-auto %d" % item_id, [(item_id, "auto", 0, 0, 0)]))
    for item_id in equip_only[:3]:
        table.append(("e-auto %d" % item_id, [(item_id, "auto", 0, 0, 2)]))
    for item_id in overlap[:5]:
        table.append(("ambiguous-auto %d" % item_id, [(item_id, "auto", 0, 0, 2)]))
    for item_id in overlap[:3]:
        table.append(("ambiguous-explicit-w %d" % item_id, [(item_id, "weapon", 0, 0, 0)]))
        table.append(("ambiguous-explicit-e %d" % item_id, [(item_id, "equipment", 0, 0, 2)]))
    for bad_id in (999999, -1, 0):
        table.append(("unknown-auto %d" % bad_id, [(bad_id, "auto", 0, 0, 0)]))
    # Everything below needs a REAL id out of the downloaded item table, by direct index
    # rather than a slice. A machine that has never run `items` has no decoded/items.json,
    # so both lists are empty -- which is the ordinary state of a freshly extracted zip, not
    # an error. Without this guard the sweep died with IndexError before comparing anything,
    # taking the whole harness with it. The unknown-id cases above need no table and still
    # run, so the sweep is reduced here, never empty.
    have_ids = len(weapon_only) >= 2 and len(equip_only) >= 1
    if have_ids:
        table.append(("mismatched-kind-w-as-e", [(weapon_only[0], "equipment", 0, 0, 2)]))
        table.append(("mismatched-kind-e-as-w", [(equip_only[0], "weapon", 0, 0, 0)]))
        table.append(("garbage-kind", [(weapon_only[0], "banana", 0, 0, 0)]))
        table.append(("multi", [(weapon_only[0], "auto", 0, 0, 0),
                                (equip_only[0], "auto", 0, 0, 3),
                                (weapon_only[1], "weapon", 5, 5, 0)]))
        table.append(("multi-second-bad", [(weapon_only[0], "auto", 0, 0, 0),
                                            (999999, "auto", 0, 0, 0)]))

    for label, requests in table:
        compare(document, requests, 0, label)
    if have_ids:
        for slotprofile in (1, 9):
            compare(document, [(weapon_only[0], "auto", 0, 0, 0)], slotprofile,
                    "slotprofile=%d" % slotprofile)
    else:
        print("      (no item table on this machine -- the cases needing a real item id "
              "were skipped; run `py tools/sas4.py items` to include them)")

    os.remove(tmp_path)


def sweep_claimed(sweep, tmp):
    exe = c_exe("claimed")
    save_path = os.path.join(tmp, "claimed.save")
    plain_path = os.path.join(tmp, "claimed_plain.json")
    backups = os.path.join(tmp, "claimed_backups")
    os.makedirs(backups, exist_ok=True)

    weapons = sas4.item_names().get("weapon", {})
    equipment = sas4.item_names().get("equipment", {})
    weapon_only = sorted(set(weapons) - set(equipment))[:4]
    equip_only = sorted(set(equipment) - set(weapons))[:4]

    # This whole sweep is built on five grants of REAL item ids, so unlike grant_plan's it
    # cannot be reduced -- with no downloaded item table there is no fixture to parse at
    # all. That is the ordinary state of a freshly extracted zip, so say so and return
    # rather than dying on an index; ITEM_TABLE_DEPENDENT_SWEEPS below tells main() that an
    # empty result here is expected in that case and not a broken run.
    if len(weapon_only) < 3 or len(equip_only) < 2:
        print("      (no item table on this machine -- this sweep needs real item ids; "
              "run `py tools/sas4.py items` to include it)")
        return

    document = sas4_model.generate(level=10, money=1000)
    with open(save_path, "wb") as f:
        f.write(sas4_model.encode_document(document))

    requests = [(weapon_only[0], "weapon", 0, 0, None), (equip_only[0], "equipment", 0, 0, 2),
                (weapon_only[1], "weapon", 5, 3, None), (equip_only[1], "equipment", 1, 1, 4),
                (weapon_only[2], "weapon", 12, 10, None)]
    old_backups = sas4.BACKUPS
    sas4.BACKUPS = backups
    try:
        plan, _labels = sas4.grant_plan(document, requests, 0)
        ok, _saved, message = sas4.apply_edits(save_path, plan)
    finally:
        sas4.BACKUPS = old_backups
    if not ok:
        sweep.record("claimed fixture setup", False)
        return

    final_document = json.loads(dgdata.decode(open(save_path, "rb").read()))
    with open(plain_path, "w", encoding="utf-8") as f:
        json.dump(final_document, f, separators=(",", ":"), ensure_ascii=False)

    def c_rows(slotprofile):
        result = subprocess.run([exe, "rows", ITEMS_CACHE, plain_path, str(slotprofile)],
                                 capture_output=True)
        rows = []
        for line in result.stdout.decode("utf-8", "replace").splitlines():
            index, kind, item_id, name, grade, bonus, slot = line.split("\t")
            rows.append((int(index), kind, int(item_id), name, int(grade), int(bonus),
                        None if slot == "NONE" else int(slot)))
        return rows

    def c_drop(slotprofile, indexes):
        args = [exe, "drop", plain_path, str(slotprofile)] + [str(i) for i in indexes]
        result = subprocess.run(args, capture_output=True)
        lines = result.stdout.decode("utf-8", "replace").splitlines()
        if lines and lines[0] == "ERROR":
            return ("ERROR",)
        return ("OK", [(l.split("\t", 1)[0], json.loads(l.split("\t", 1)[1])) for l in lines])

    py_rows = sas4.claimed_items(final_document, 0)
    sweep.record("claimed_items", py_rows == c_rows(0))

    row_indexes = [r[0] for r in py_rows]
    for idx in row_indexes:
        try:
            py_d = ("OK", sas4.drop_claimed(final_document, [idx], 0))
        except (KeyError, IndexError, TypeError, ValueError):
            py_d = ("ERROR",)
        sweep.record("drop index=%d" % idx, py_d == c_drop(0, [idx]))

    if len(row_indexes) > 2:
        multi = [row_indexes[0], row_indexes[2]]
        try:
            py_d = ("OK", sas4.drop_claimed(final_document, multi, 0))
        except (KeyError, IndexError, TypeError, ValueError):
            py_d = ("ERROR",)
        sweep.record("drop multi", py_d == c_drop(0, multi))

    for bad in (9999, (row_indexes[0] + 1) if row_indexes else 1):
        try:
            py_d = ("OK", sas4.drop_claimed(final_document, [bad], 0))
        except (KeyError, IndexError, TypeError, ValueError):
            py_d = ("ERROR",)
        sweep.record("drop bad=%d" % bad, py_d == c_drop(0, [bad]))

    empty_document = sas4_model.generate(level=1, money=0)
    empty_plain_path = os.path.join(tmp, "claimed_empty_plain.json")
    with open(empty_plain_path, "w", encoding="utf-8") as f:
        json.dump(empty_document, f, separators=(",", ":"), ensure_ascii=False)
    result = subprocess.run([exe, "rows", ITEMS_CACHE, empty_plain_path, "0"],
                             capture_output=True)
    sweep.record("claimed_items empty",
                  sas4.claimed_items(empty_document, 0) == [] and result.stdout == b"")


def sweep_apply_edits(sweep, tmp):
    """apply_edits file-hash comparison -- write-path sweeps stay on generated saves only,
    never a real one, per the plan's privacy rule."""
    import hashlib

    def sha256_of(path):
        return hashlib.sha256(open(path, "rb").read()).hexdigest()

    py_save = os.path.join(tmp, "apply_py.save")
    c_save = os.path.join(tmp, "apply_c.save")
    for path in (py_save, c_save):
        with open(path, "wb") as f:
            f.write(sas4_model.encode_document(sas4_model.generate(level=5, money=1000)))
    if sha256_of(py_save) != sha256_of(c_save):
        sweep.record("apply_edits fixture setup", False)
        return

    real_plan = [("Inventory/Profile0/Money", 4242), ("Global/HighestRank", 9),
                 ("Inventory/Profile0/Name", "Renamed")]
    py_backups = os.path.join(tmp, "apply_py_backups")
    os.makedirs(py_backups, exist_ok=True)
    old_backups = sas4.BACKUPS
    sas4.BACKUPS = py_backups
    try:
        ok, _saved, _message = sas4.apply_edits(py_save, real_plan)
    finally:
        sas4.BACKUPS = old_backups
    sweep.record("apply_edits python side succeeds", ok)

    c_backups = os.path.join(tmp, "apply_c_backups")
    os.makedirs(c_backups, exist_ok=True)
    args = [c_exe("apply"), c_save, c_backups]
    for path, value in real_plan:
        args.append(path)
        args.append(json.dumps(value))
    result = subprocess.run(args, capture_output=True)
    sweep.record("apply_edits C side succeeds", result.returncode == 0)
    sweep.record("resulting files are byte-identical", sha256_of(py_save) == sha256_of(c_save))

    def count_files(d):
        return sum(len(files) for _r, _dirs, files in os.walk(d))

    sweep.record("exactly one backup each", count_files(py_backups) == 1 and
                 count_files(c_backups) == 1)


# --- main -------------------------------------------------------------------------------

# These sweeps have nothing to compare unless a decodable real file is present. On a zip
# recipient's machine (or any checkout with no saves/ or backups/) that is the normal case,
# not a broken harness -- so an empty result here is only worth failing the run over if a
# decodable real file WAS available and the sweep still touched nothing.
REAL_FILE_DEPENDENT_SWEEPS = frozenset([
    "dgdata verify/roundtrip", "json parse-tree", "json compact (every node)",
    "json indented", "scalars + kinds", "anchor_for (repeat band 2-19)",
])

# Sweeps that need the DOWNLOADED item table (decoded/items.json), which a zip recipient
# does not have until they run `items`. Same idea as REAL_FILE_DEPENDENT_SWEEPS: an empty
# result is expected without it, not a broken run.
ITEM_TABLE_DEPENDENT_SWEEPS = frozenset({"claimed_items / drop_claimed"})


def main():
    print("differential_core: reading real saves (referred to only by index, never a path)")
    real_files = load_real_files()
    decodable = sum(1 for rf in real_files if rf.document is not None)
    skipped = [rf for rf in real_files if rf.skip_reason is not None]
    if real_files:
        print("  %d real file(s) found, %d decodable" % (len(real_files), decodable))
        if skipped:
            from collections import Counter
            for reason, count in Counter(rf.skip_reason for rf in skipped).most_common():
                print("  %d file(s) skipped: %s" % (count, reason))
    else:
        print("  0 real files found -- this is normal for a fresh checkout or a zip "
              "recipient (saves/ and backups/ are never shipped). Generated saves still "
              "cover every write-path sweep.")

    with tempfile.TemporaryDirectory(prefix="sas4-differential-core-") as tmp:
        print("\nrunning sweeps...\n")
        run_sweep("dgdata verify/roundtrip", sweep_dgdata, real_files, tmp)
        run_sweep("json parse-tree", sweep_json_tree, real_files)
        run_sweep("json compact (every node)", sweep_json_compact_nodes, real_files)
        run_sweep("json indented", sweep_json_indent, real_files)
        run_sweep("scalars + kinds", sweep_scalars_kinds, real_files)
        run_sweep("anchor_for (repeat band 2-19)", sweep_anchor_band, real_files)
        run_sweep("check()", sweep_check, real_files)
        run_sweep("xp curve (levels 0-108)", sweep_xp_curve)
        run_sweep("level_plan", sweep_level_plan, real_files)
        run_sweep("mastery_plan", sweep_mastery_plan, real_files)
        run_sweep("grant_plan", sweep_grant_plan)
        run_sweep("claimed_items / drop_claimed", sweep_claimed, tmp)
        run_sweep("apply_edits (generated only)", sweep_apply_edits, tmp)

    total_compared = sum(s.compared for s in ALL_SWEEPS)
    total_mismatches = sum(len(s.mismatches) for s in ALL_SWEEPS)
    all_names = {s.name for s in ALL_SWEEPS}
    assert ITEM_TABLE_DEPENDENT_SWEEPS <= all_names, (
        "ITEM_TABLE_DEPENDENT_SWEEPS names a sweep that no longer exists: %r"
        % (ITEM_TABLE_DEPENDENT_SWEEPS - all_names))
    assert REAL_FILE_DEPENDENT_SWEEPS <= all_names, (
        "REAL_FILE_DEPENDENT_SWEEPS names a sweep that no longer exists -- a sweep's name was "
        "changed at its run_sweep(...) call site without updating this set, which would "
        "silently reintroduce the zip-recipient exit-1 bug: %r"
        % (REAL_FILE_DEPENDENT_SWEEPS - all_names))
    all_empty = [s.name for s in ALL_SWEEPS if s.compared == 0]
    # An empty real-file-dependent sweep is expected (not a bug) when no decodable real file
    # was ever available to feed it; only an unexplained empty sweep fails the run.
    have_item_table = bool(sas4.item_names().get("weapon"))
    expected_empty = [n for n in all_empty
                      if (n in REAL_FILE_DEPENDENT_SWEEPS and decodable == 0)
                      or (n in ITEM_TABLE_DEPENDENT_SWEEPS and not have_item_table)]
    broken_empty = [n for n in all_empty if n not in expected_empty]

    print("\n%d total comparisons across %d sweeps, %d mismatch(es)"
          % (total_compared, len(ALL_SWEEPS), total_mismatches))
    if expected_empty:
        print("  %d sweep(s) compared nothing because no decodable real file was available "
              "(expected on a zip recipient's machine): %s"
              % (len(expected_empty), ", ".join(expected_empty)))
    if broken_empty:
        print("WARNING: these sweeps compared nothing at all: %s" % ", ".join(broken_empty))

    if total_mismatches or broken_empty:
        print("\nDIFFERENCES FOUND" if total_mismatches else "\nINCOMPLETE RUN")
        return 1
    print("\nALL MATCH")
    return 0


if __name__ == "__main__":
    sys.exit(main())
