"""
A stand-in for a SAS4 profile: generate a valid one from nothing, and check one for
internal consistency. It is not the game -- it is a model of what the game considers a
well-formed save, so tools can be repaired and tested against it without touching a real
profile or the live account.

Why this exists separately from `sas4.py`: `sas4.py` edits a file and trusts the caller to
know what a sensible value is. This knows the *relationships* -- that the XP has to match
the level, that skill points have to add up, that a strongbox entry has to be tagged -- and
so it can both build a save that satisfies them and point at one that does not.

    py sas4_model.py generate out.save            a fresh, valid, minimal profile
    py sas4_model.py generate out.save --level 20 --money 1000000
    py sas4_model.py generate out.save --weapon 129 --equip 101:2
    py sas4_model.py check <some.save>            list every inconsistency it can find
    py sas4_model.py check <some.save> --strict   exit non-zero if any are found
    py sas4_model.py redteam                      how much of a catalog of tampering check() catches
    py sas4_model.py redteam --progression        coverage climbing as rules are added
    py sas4_model.py dataset out.jsonl --count 2000   labelled features for training a detector

The checks are deliberately the ones a server-side validator would also find easy: a level
whose XP is impossible, skill points that do not correspond to levels gained, a malformed
inventory. Passing them is necessary, not sufficient -- the point is to stop a tool from
producing a save that is obviously wrong, not to defeat anything.

`redteam` is a closed detection-engineering exercise: it applies a catalog of tampering
strategies and reports which `check` catches. It is built to be run against `check`, to find
gaps and close them -- the deliberately "plausible" attacks are there to make the point that
some things no single-file check can reach, because only the server has the history and the
cross-account view. A save that passes every local check is still not a save the server
must accept.
"""

import argparse
import importlib.util
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("dgdata", os.path.join(HERE, "dgdata.py"))
dgdata = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(dgdata)

# Cumulative XP to reach each level, from the community wiki, cross-checked against a real
# save (level 3 needs 2,359; a real profile at that level held 2,872, between 3 and 4).
XP_TABLE = [0, 0, 1071, 2359, 4014, 6190, 9045, 12741, 17445, 23328, 30565, 39335, 49821,
            62211, 76697, 93475, 112745, 134711, 159582, 187571, 218895, 253775, 292436,
            335108, 382025, 433425]


def xp_for_level(level):
    if level < len(XP_TABLE):
        return XP_TABLE[level]
    # beyond the tabulated range, extend by the last observed step; approximate but ordered
    step = XP_TABLE[-1] - XP_TABLE[-2]
    return XP_TABLE[-1] + step * (level - (len(XP_TABLE) - 1))


# --- generate ----------------------------------------------------------------------------

def weapon_entry(item_id, grade=0, bonus=0):
    """A gun for Strongboxes.Claimed. A bare 0 tags it, then the dict, then 8, 0."""
    return [0, {"ID": item_id, "EquipVersion": 0, "Grade": grade, "AugmentSlots": 0,
                "BonusStatsLevel": bonus, "InventoryIndex": 0}, 8, 0]


def equipment_entry(item_id, slot, grade=0, bonus=0):
    """A piece of equipment for Strongboxes.Claimed. A bare 1 tags it, then the dict."""
    return [1, {"ID": item_id, "EquipVersion": 0, "Grade": grade, "AugmentSlots": 0,
                "BonusStatsLevel": bonus, "EquippedSlot": slot, "InventoryIndex": slot}]


def generate(level=1, money=0, name="Test", weapons=(), equipment=()):
    """A profile that verifies and is internally consistent.

    weapons is a list of item ids; equipment is a list of (item_id, slot) pairs. Both are
    placed into Strongboxes.Claimed with the tagging the game uses, so `check` on the
    result exercises the strongbox rules too.
    """
    xp = xp_for_level(level)
    claimed = []
    for item_id in weapons:
        claimed.extend(weapon_entry(item_id))
    for item_id, slot in equipment:
        claimed.extend(equipment_entry(item_id, slot))
    profile = {
        "Loaded": True,
        "Ammo": {},
        "Name": name,
        "Money": money,
        "RewardDay": 1,
        "RewardLastClaimDate": 0,
        "ForceGiveWeapon": False,
        "FirstPlayFlow": False,
        "FreeSkillsReset": False,
        "UnlockedDailyRewards": False,
        "StrongboxesOpened": 0,
        "HasReceivedCryo": False,
        "PerkKillingMachine": False,
        "PerkHighRoller": False,
        "PerkTank": False,
        "PerkCost": 0,
        "Weapons": [],
        "Equipment": [],
        "Strongboxes": {"Unopened": [], "Claimed": claimed},
        "Skills": {
            "Class": 0,
            "PlayerLevel": level,
            "PlayerTotalXp": xp,
            # Points granted equals the level: a real level-3 profile had three accounted
            # for (paygrade 2 + holdtheline 1, zero unspent). None spent here, so all sit
            # available.
            "AvailableSkillPoints": level,
            "AvailableBlackKeys": 0,
            "AvailableEliteAugmentCores": 0,
            "AvailableBlackStrongboxes": [],
            "SkillsArray": [],
        },
        "Turrets": [],
    }
    document = {
        "Version": {"LastGame": [2, 2, 2], "Profile": [1, 0, 0], "link": "0" * 24},
        "Global": {"HighestRank": level, "HasPlayedGame": True},
        "HackCheck": 0,
        "Inventory": {"Profile0": profile,
                      **{"Profile%d" % i: {"Loaded": False} for i in range(1, 6)}},
        "Settings": {"MusicOn": True, "SFXOn": True},
        "StatsData": [],
    }
    return document


def encode_document(document):
    plain = json.dumps(document, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    return dgdata.encode(plain)


# --- check: a registry of named rules ----------------------------------------------------
#
# Each rule is a function of the whole document returning a list of problem strings. check()
# runs them all. Keeping them separate is what makes the red/blue loop legible: a rule maps
# to the attacks it catches, and adding one visibly raises coverage.

def loaded_profiles(document):
    for slot, profile in document.get("Inventory", {}).items():
        if isinstance(profile, dict) and profile.get("Loaded"):
            yield "Inventory/%s" % slot, profile


def rule_xp_matches_level(document):
    out = []
    for where, profile in loaded_profiles(document):
        skills = profile.get("Skills", {})
        level, xp = skills.get("PlayerLevel"), skills.get("PlayerTotalXp")
        if isinstance(level, int) and isinstance(xp, int):
            low, high = xp_for_level(level), xp_for_level(level + 1)
            if not (low <= xp < high):
                out.append("%s: level %d needs XP in [%d, %d), but PlayerTotalXp is %d"
                           % (where, level, low, high, xp))
    return out


def rule_skill_points_add_up(document):
    out = []
    for where, profile in loaded_profiles(document):
        skills = profile.get("Skills", {})
        level, points = skills.get("PlayerLevel"), skills.get("AvailableSkillPoints")
        spent = sum(e.get("SkillLevel", 0) for e in skills.get("SkillsArray", [])
                    if isinstance(e, dict))
        if isinstance(level, int) and isinstance(points, int) and points + spent > level:
            # Granted equals the level (one real level-3 profile carried three); fewer is
            # fine since events can hand out extras this does not model, more is not.
            out.append("%s: %d skill points available + %d spent = %d, more than the %d a "
                       "level-%d character is granted"
                       % (where, points, spent, points + spent, level, level))
    return out


def rule_rank_at_least_level(document):
    out = []
    rank = document.get("Global", {}).get("HighestRank")
    for where, profile in loaded_profiles(document):
        level = profile.get("Skills", {}).get("PlayerLevel")
        if isinstance(rank, int) and isinstance(level, int) and rank < level:
            out.append("%s: HighestRank %d is below PlayerLevel %d" % (where, rank, level))
    return out


def rule_money_in_range(document):
    out = []
    for where, profile in loaded_profiles(document):
        money = profile.get("Money")
        if isinstance(money, int) and not (0 <= money < 2**31):
            out.append("%s: Money %d is outside the range a 32-bit value holds" % (where, money))
    return out


def rule_strongboxes_well_formed(document):
    out = []
    for where, profile in loaded_profiles(document):
        claimed = profile.get("Strongboxes", {}).get("Claimed")
        if isinstance(claimed, list):
            out.extend(_check_strongboxes(where, claimed))
    return out


def rule_level_positive(document):
    """Added in the red/blue loop: level and rank must be at least 1."""
    out = []
    for where, profile in loaded_profiles(document):
        level = profile.get("Skills", {}).get("PlayerLevel")
        if isinstance(level, int) and level < 1:
            out.append("%s: PlayerLevel %d is below 1" % (where, level))
    return out


def rule_skill_levels_sane(document):
    """Added in the red/blue loop: no skill sits at a negative or absurd level."""
    out = []
    for where, profile in loaded_profiles(document):
        for entry in profile.get("Skills", {}).get("SkillsArray", []):
            if isinstance(entry, dict):
                skill_level = entry.get("SkillLevel")
                if isinstance(skill_level, int) and not (0 <= skill_level <= 20):
                    out.append("%s: skill %s at level %d, outside 0..20"
                               % (where, entry.get("SkillName"), skill_level))
    return out


def rule_no_double_equipped_slot(document):
    """Added in the red/blue loop: two items cannot occupy one equipped slot."""
    out = []
    for where, profile in loaded_profiles(document):
        seen = {}
        for item in profile.get("Equipment", []):
            if isinstance(item, dict) and item.get("Equipped"):
                slot = item.get("EquippedSlot")
                if slot in seen:
                    out.append("%s: items %s and %s both equipped in slot %s"
                               % (where, seen[slot], item.get("ID"), slot))
                seen[slot] = item.get("ID")
    return out


# Order matters only for reading; a lower --rules count uses the first n, which is how the
# loop is shown climbing. The last three were added after the first redteam run exposed
# attacks the original five missed.
RULES = [
    ("xp-matches-level", rule_xp_matches_level),
    ("skill-points-add-up", rule_skill_points_add_up),
    ("rank-at-least-level", rule_rank_at_least_level),
    ("money-in-range", rule_money_in_range),
    ("strongboxes-well-formed", rule_strongboxes_well_formed),
    ("level-positive", rule_level_positive),
    ("skill-levels-sane", rule_skill_levels_sane),
    ("no-double-equipped-slot", rule_no_double_equipped_slot),
]


def check(document, rules=None):
    """Every internal inconsistency found, as a list of strings. Empty means consistent."""
    problems = []
    for _name, rule in (rules if rules is not None else RULES):
        problems.extend(rule(document))
    return problems


def _check_strongboxes(where, claimed):
    """Claimed is a flattened stream of runs, not a list of objects.

    A weapon is four elements -- `0, {dict}, 8, 0` -- and a piece of equipment is two --
    `1, {dict}`. The run length is what makes it parseable: a weapon's trailing `0` would
    otherwise look like the start of another weapon. So consume whole runs, not positions.
    """
    problems = []
    i = 0
    while i < len(claimed):
        tag = claimed[i]
        if tag == 0:
            run, kind = 4, "weapon"
        elif tag == 1:
            run, kind = 2, "equipment"
        else:
            problems.append("%s/Strongboxes/Claimed[%d]: expected a 0 or 1 tag, found %s"
                            % (where, i, json.dumps(tag)[:20]))
            i += 1
            continue
        if i + run > len(claimed):
            problems.append("%s/Strongboxes/Claimed[%d]: %s run is cut short" % (where, i, kind))
            break
        entry = claimed[i + 1]
        if not isinstance(entry, dict) or "ID" not in entry:
            problems.append("%s/Strongboxes/Claimed[%d]: %s tag not followed by an item dict"
                            % (where, i, kind))
        i += run
    return problems


# --- commands ----------------------------------------------------------------------------

def cmd_generate(args):
    equipment = []
    for pair in args.equip:
        item_id, _, slot = pair.partition(":")
        equipment.append((int(item_id), int(slot) if slot else 2))
    document = generate(level=args.level, money=args.money, name=args.name,
                        weapons=args.weapon, equipment=equipment)
    problems = check(document)
    if problems:
        print("refusing to write: the generated profile is inconsistent (a bug here):")
        for line in problems:
            print("  " + line)
        return 1
    built = encode_document(document)
    stored, _, ok = dgdata.verify(built)
    if not ok:
        print("refusing to write: the generated profile does not verify (a bug here)")
        return 1
    with open(args.out, "wb") as handle:
        handle.write(built)
    print("wrote %s (%d bytes, header %s)" % (args.out, len(built), stored))
    print("  level %d, XP %d, %d skill points, money %d"
          % (args.level, xp_for_level(args.level), args.level, args.money))
    print("  consistent and verifies")
    return 0


def cmd_check(args):
    raw = open(args.file, "rb").read()
    stored, computed, ok = dgdata.verify(raw)
    print("checksum  %s / %s -- %s" % (stored, computed, "VALID" if ok else "MISMATCH"))
    try:
        document = json.loads(dgdata.decode(raw))
    except ValueError as problem:
        print("not decodable as JSON: %s" % problem)
        return 2
    problems = check(document)
    if not ok:
        problems.insert(0, "checksum does not match: the game would reject this file")
    if problems:
        print("\n%d problem(s):" % len(problems))
        for line in problems:
            print("  - " + line)
        return 1 if args.strict else 0
    print("\nconsistent: nothing a plausibility check would flag")
    return 0


# --- red/blue learning harness -----------------------------------------------------------
#
# A closed exercise for detection engineering. Each "attack" is a way of tampering with a
# clean profile; the "detector" is check() above. redteam applies every attack and reports
# which the detector catches and which slip through. The loop it is built for is a defensive
# one: a miss is a gap in check(), so you add a rule and the next run catches it.
#
# The lesson the misses teach is the real one: a local detector only knows this one file.
# The things it cannot check -- whether this level was reached at a plausible rate, whether
# the account's history supports the inventory, how this profile compares to millions of
# others -- are exactly what a server has and a local model never will. Passing every check
# here says nothing about a server-side pass.

def _set(document, path, value):
    node = document
    parts = [p for p in path.strip("/").split("/") if p]
    for part in parts[:-1]:
        node = node[part]
    node[parts[-1]] = value


def _promote(document, level):
    """A *consistent* edit to a level -- the kind that passes every local check."""
    _set(document, "Inventory/Profile0/Skills/PlayerLevel", level)
    _set(document, "Inventory/Profile0/Skills/PlayerTotalXp", xp_for_level(level))
    _set(document, "Inventory/Profile0/Skills/AvailableSkillPoints", level)
    _set(document, "Global/HighestRank", level)


def _double_equip(document):
    _set(document, "Inventory/Profile0/Equipment",
         [{"ID": 101, "EquippedSlot": 2, "Equipped": True},
          {"ID": 102, "EquippedSlot": 2, "Equipped": True}])


# Each attack tampers with a clean profile. The three at the end were what the first five
# rules missed and motivated the last three rules -- the loop in one place.
ATTACKS = {
    "xp-far-above-level":
        lambda d: _set(d, "Inventory/Profile0/Skills/PlayerTotalXp", 100_000),
    "money-over-int32":
        lambda d: _set(d, "Inventory/Profile0/Money", 5_000_000_000),
    "rank-below-level":
        lambda d: (_set(d, "Inventory/Profile0/Skills/PlayerLevel", 20),
                   _set(d, "Inventory/Profile0/Skills/PlayerTotalXp", xp_for_level(20)),
                   _set(d, "Global/HighestRank", 3)),
    "too-many-skill-points":
        lambda d: _set(d, "Inventory/Profile0/Skills/AvailableSkillPoints", 999),
    "malformed-strongbox":
        lambda d: _set(d, "Inventory/Profile0/Strongboxes/Claimed", [0, "not a dict"]),
    "negative-level":
        lambda d: _set(d, "Inventory/Profile0/Skills/PlayerLevel", -3),
    "absurd-skill-level":
        lambda d: _set(d, "Inventory/Profile0/Skills/SkillsArray",
                       [{"SkillName": "holdtheline", "SkillLevel": 500}]),
    "double-equipped-slot":
        _double_equip,
    # deliberately consistent -- no consistency rule can flag these, ever:
    "consistent-level-20":
        lambda d: _promote(d, 20),
    "consistent-level-25":
        lambda d: _promote(d, 25),
}


def _apply(attack):
    document = generate(level=1, money=1000)
    attack(document)
    return document


def cmd_redteam(args):
    if args.progression:
        print("coverage as rules are added, one at a time\n")
        for count in range(1, len(RULES) + 1):
            active = RULES[:count]
            caught = sum(bool(check(_apply(a), active)) for a in ATTACKS.values())
            newest = RULES[count - 1][0]
            print("  %d rule%s  ->  %d/%d caught   (added: %s)"
                  % (count, " " if count == 1 else "s", caught, len(ATTACKS), newest))
        print("\n  Coverage climbs as rules are added, then stops: the last two attacks are")
        print("  internally consistent, and no rule over a single file can catch those.")
        return 0

    print("detector coverage over %d tampering strategies, %d rules\n"
          % (len(ATTACKS), len(RULES)))
    caught = 0
    for name, attack in ATTACKS.items():
        document = _apply(attack)
        firing = [rule_name for rule_name, rule in RULES if rule(document)]
        if firing:
            caught += 1
            print("  CAUGHT  %-24s by %s" % (name, ", ".join(firing)))
        else:
            print("  missed  %-24s consistent to every rule" % name)
    print("\n  %d caught, %d missed" % (caught, len(ATTACKS) - caught))
    print("\n  The misses are consistent edits. A single-file check cannot reach them --")
    print("  only the server can, with the rate the level was reached at and the shape of")
    print("  millions of other accounts. A locally valid save is not a safe one.")
    return 0


# --- dataset for ML ----------------------------------------------------------------------

FEATURE_NAMES = ["level", "xp", "xp_over_band_low", "xp_band_span", "skill_points",
                 "skill_spent", "rank", "rank_minus_level", "money", "claimed_len",
                 "num_violations"]


def features(document):
    """A fixed-length numeric vector describing one profile -- the input an ML model sees.

    These are the same quantities the rules look at, turned into numbers. A model trained on
    them is learning the consistency boundary the rules encode; it is a way to study that
    boundary, and a detector, not a generator of anything.
    """
    profile = next((p for _w, p in loaded_profiles(document)), {})
    skills = profile.get("Skills", {})
    level = skills.get("PlayerLevel", 0) or 0
    xp = skills.get("PlayerTotalXp", 0) or 0
    points = skills.get("AvailableSkillPoints", 0) or 0
    spent = sum(e.get("SkillLevel", 0) for e in skills.get("SkillsArray", [])
                if isinstance(e, dict))
    rank = document.get("Global", {}).get("HighestRank", 0) or 0
    money = profile.get("Money", 0) or 0
    claimed = profile.get("Strongboxes", {}).get("Claimed", [])
    band_low = xp_for_level(level) if isinstance(level, int) and level >= 0 else 0
    band_span = (xp_for_level(level + 1) - band_low) if isinstance(level, int) and level >= 0 else 0
    return {
        "level": level, "xp": xp,
        "xp_over_band_low": xp - band_low,
        "xp_band_span": band_span,
        "skill_points": points, "skill_spent": spent,
        "rank": rank, "rank_minus_level": rank - level,
        "money": money,
        "claimed_len": len(claimed) if isinstance(claimed, list) else -1,
        "num_violations": len(check(document)),
    }


STRUCTURAL_ATTACKS = [a for name, a in ATTACKS.items() if not name.startswith("consistent")]


def tamper(document, rng):
    """Apply one tampering, chosen so the labelled data has no accidental tell.

    Half the time a structural attack that breaks a consistency rule (catchable). Half the
    time a *consistent* promotion to a RANDOM level -- crucially random, not a fixed 20 or
    25, or the level itself would leak the label and a model would 'beat' the ceiling by
    memorising which levels the attacks used rather than by learning anything real.
    """
    if rng.random() < 0.5:
        rng.choice(STRUCTURAL_ATTACKS)(document)
    else:
        _promote(document, rng.randint(1, 25))


def cmd_dataset(args):
    """Emit labelled feature vectors: consistent profiles vs tampered ones.

    Label 0 is a consistent profile, label 1 is a tampered one. The point is a detector you
    train yourself -- feed this to any classifier and it learns the same boundary the rules
    draw. It is the blue side of the exercise done with ML: nothing here trains anything to
    evade a real system, and `num_violations` is left in as a feature precisely so a model
    can be compared against simply counting rule hits.

    Written as JSONL (one object per line) or CSV.
    """
    import random
    rng = random.Random(args.seed)
    rows = []
    for _ in range(args.count):
        document = generate(level=rng.randint(1, 25), money=rng.randint(0, 2_000_000))
        label = 0 if rng.random() < 0.5 else 1
        if label:
            tamper(document, rng)
        row = features(document)
        row["label"] = label
        rows.append(row)

    written = args.out
    if written.endswith(".csv"):
        import csv
        with open(written, "w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=FEATURE_NAMES + ["label"])
            writer.writeheader()
            writer.writerows(rows)
    else:
        with open(written, "w", encoding="utf-8") as handle:
            for row in rows:
                handle.write(json.dumps(row) + "\n")
    positives = sum(r["label"] for r in rows)
    print("wrote %s: %d rows, %d tampered, %d consistent"
          % (written, len(rows), positives, len(rows) - positives))
    print("features: %s" % ", ".join(FEATURE_NAMES))
    print("\nTrain a detector on it, e.g. with scikit-learn:")
    print("  import json; rows=[json.loads(l) for l in open(%r)]" % written)
    print("  X=[[r[f] for f in %s] for r in rows]; y=[r['label'] for r in rows]"
          % FEATURE_NAMES)
    print("  from sklearn.tree import DecisionTreeClassifier")
    print("  clf=DecisionTreeClassifier(max_depth=4).fit(X, y)")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command")

    p = sub.add_parser("generate"); p.set_defaults(run=cmd_generate)
    p.add_argument("out")
    p.add_argument("--level", type=int, default=1)
    p.add_argument("--money", type=int, default=0)
    p.add_argument("--name", default="Test")
    p.add_argument("--weapon", type=int, action="append", default=[],
                   help="weapon id to place; repeatable")
    p.add_argument("--equip", action="append", default=[],
                   help="equipment as id:slot; repeatable")

    p = sub.add_parser("redteam"); p.set_defaults(run=cmd_redteam)
    p.add_argument("--progression", action="store_true",
                   help="show coverage climbing as rules are added one at a time")

    p = sub.add_parser("dataset"); p.set_defaults(run=cmd_dataset)
    p.add_argument("out", help="output path; .csv for CSV, otherwise JSONL")
    p.add_argument("--count", type=int, default=1000)
    p.add_argument("--seed", type=int, default=0)

    p = sub.add_parser("check"); p.set_defaults(run=cmd_check)
    p.add_argument("file")
    p.add_argument("--strict", action="store_true", help="exit non-zero if anything is off")

    args = parser.parse_args()
    if not args.command:
        parser.print_help()
        return 2
    return args.run(args)


if __name__ == "__main__":
    sys.exit(main())
