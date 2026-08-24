"""
One tool for SAS4 profiles: read them, search them, and change values in place.

The save is DGDATA-wrapped JSON -- `dgdata.py` holds the format, this holds everything
built on top of it. It replaces the separate viewer and save-watcher.

    py sas4.py where                                show the profiles found on this machine
    py sas4.py view                                 the live profile, by section
    py sas4.py view --section skills --slot 1
    py sas4.py list --grep money                    every path whose name matches
    py sas4.py list --type bool                     every on/off switch in the file
    py sas4.py list --under Settings                everything below one path
    py sas4.py kinds                                how many values of each type, and where
    py sas4.py items                                fetch the item table, once, for names
    py sas4.py give 129                             grant a finished item (the loot, no box)
    py sas4.py level 40                             set the level, XP, points and rank together
    py sas4.py get /Inventory/Profile0/Money        one value
    py sas4.py set /Inventory/Profile0/Money 250000 change it, checksum and all
    py sas4.py verify                               would the game accept this file
    py sas4.py decode [out.json]                    plaintext JSON out
    py sas4.py encode <in.json> <out.save>          and back again
    py sas4.py watch                                diff the save each time the game writes
    py sas4.py session                              read the login session (secrets hidden)
    py sas4.py graft other.save --fields A,B        copy progress in, keeping your identity

Paths may be written with or without a leading slash. Prefer without it in Git Bash: MSYS
rewrites a leading-slash argument into a Windows path before Python ever sees it, and the
error that comes back mentions `C:` for no visible reason.

`set` is deliberately cautious, because getting this wrong once already made the game throw
a character away:

  * it refuses to run while SAS4 is running, since the game rewrites the save on its own
    schedule and would overwrite the edit;
  * it copies the file to a timestamped `backup-` directory first, every time;
  * it edits the **bytes** of the one value rather than re-serialising the document, so
    nothing else in 120 KB of JSON can shift underneath -- the game's JSON writer does not
    format exactly the way Python's does, and a re-serialise rewrites the whole file;
  * it re-reads the result from disk and verifies the checksum before reporting success.

Anything that touches a value the server also tracks carries account risk that no amount of
local care removes; `FINDINGS.md` has the detail.
"""

import argparse
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
# The folder holding sas4.bat, one above the code. Backups and dumps belong beside the
# thing that was double-clicked, not buried in the folder the modules were tidied into:
# someone looking for a backup after a bad edit looks where they launched from.
ROOT = os.path.dirname(HERE)
_spec = importlib.util.spec_from_file_location("dgdata", os.path.join(HERE, "dgdata.py"))
dgdata = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(dgdata)

_mspec = importlib.util.spec_from_file_location("sas4_model", os.path.join(HERE, "sas4_model.py"))
sas4_model = importlib.util.module_from_spec(_mspec)
_mspec.loader.exec_module(sas4_model)

GAME_PROCESS = "SAS4-Win.exe"
APPID = "678800"


def find_steam():
    """The Steam install directory, from the registry, then common fallbacks."""
    try:
        import winreg
        for root, key, name in (
                (winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam", "SteamPath"),
                (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Valve\Steam", "InstallPath")):
            try:
                with winreg.OpenKey(root, key) as handle:
                    path = winreg.QueryValueEx(handle, name)[0]
                    if path and os.path.isdir(path):
                        return path
            except OSError:
                continue
    except ImportError:
        pass                                       # not Windows; fall through
    for guess in (r"C:\Program Files (x86)\Steam", r"C:\Program Files\Steam"):
        if os.path.isdir(guess):
            return guess
    return None


def find_profiles():
    """Every SAS4 Profile.save on this machine, newest first.

    Walks Steam's userdata -- any account id, any drive -- so the tools work on a machine
    other than the one they were written on, with no path edited. The account folder under
    Docs is found rather than assumed; it is named by the Ninja Kiwi account id, which
    differs per player.
    """
    steam = find_steam()
    if not steam:
        return []
    found = []
    userdata = os.path.join(steam, "userdata")
    for user in _subdirs(userdata):
        docs = os.path.join(userdata, user, APPID, "local", "Data", "Docs")
        for account in _subdirs(docs):
            candidate = os.path.join(docs, account, "Profile.save")
            if os.path.isfile(candidate):
                found.append(candidate)
    found.sort(key=lambda p: os.path.getmtime(p), reverse=True)
    return found


def _subdirs(path):
    try:
        return [name for name in os.listdir(path) if os.path.isdir(os.path.join(path, name))]
    except OSError:
        return []


def session_for(profile_path):
    """The current.session that sits beside a given profile's Docs directory."""
    docs = os.path.dirname(os.path.dirname(profile_path))     # .../Docs
    return os.path.join(docs, "com.ninjakiwi.link", "Live", "current.session")


_PROFILES = find_profiles()
LIVE = _PROFILES[0] if _PROFILES else None
SESSION = session_for(LIVE) if LIVE else None

# Fields that carry account identity, keyed by the file they live in. Grafting another
# player's progress means copying everything EXCEPT these, so the profile stays yours.
# Both live in Version. `link` is the account the save belongs to; `analytics` holds a
# 32-hex id of its own inside a NO_LINK{...} wrapper, which is just as much a way to say
# whose file this is. It was missing here until a real profile was read field by field for
# the contribute report -- grafting `Version` was refused anyway, because `link` is in it,
# but naming `Version/analytics` on its own went straight through.
IDENTITY_FIELDS = ("link", "analytics")          # in the profile JSON
# In current.session: sessionID is the real secret -- a login token the server issues, so
# it is never printed in full. nkapiID is the account id, which is also the save folder
# name and appears in every path already, so it is shown; but editing either is gated,
# because both are what ties a file to an account.
SECRET_KEYS = ("sessionID",)
CREDENTIAL_KEYS = ("sessionID", "nkapiID")


# --- reading -----------------------------------------------------------------------------

class SaveError(Exception):
    """A save that cannot be read: missing, not DGDATA, or JSON that does not parse."""


def load(path):
    try:
        with open(path, "rb") as handle:
            raw = handle.read()
        return raw, json.loads(dgdata.decode(raw))
    except (OSError, ValueError) as problem:
        # A hand-edited save the game itself rejects lands here, and so does a wrong path.
        # Name the file and the reason: a traceback out of json or dgdata tells the reader
        # only that something deep failed, not which save or why.
        raise SaveError("cannot read %s\n  %s" % (path, problem)) from problem


def at_path(document, path):
    node = document
    for part in [p for p in path.strip("/").split("/") if p]:
        while part.endswith("]"):
            part, _, index = part[:-1].rpartition("[")
            if part:
                node = node[part]
            node = node[int(index)]
            part = ""
        if part:
            node = node[part]
    return node


def parent_of(document, path):
    """(container, key or index) for a path, so a value can be replaced."""
    parts = [p for p in path.strip("/").split("/") if p]
    tail = parts[-1]
    parent = at_path(document, "/".join(parts[:-1])) if len(parts) > 1 else document
    if tail.endswith("]"):
        name, _, index = tail[:-1].rpartition("[")
        if name:
            parent = parent[name]
        return parent, int(index)
    return parent, tail


def scalars(node, path=""):
    if isinstance(node, dict):
        for key, value in node.items():
            for item in scalars(value, path + "/" + key):
                yield item
    elif isinstance(node, list):
        for index, value in enumerate(node):
            for item in scalars(value, path + "[%d]" % index):
                yield item
    else:
        yield path, node


# --- the byte-level edit -----------------------------------------------------------------

def anchor_for(document, plain, path):
    """A byte string that appears exactly once in `plain` and ends with the value at `path`.

    Starts with `"key":value` and, if that is ambiguous, prefixes the sibling that comes
    before it in the document until it is not. Returns (anchor, value_suffix_length).
    """
    parent, key = parent_of(document, path)
    if isinstance(key, int):
        raise ValueError("set does not edit list elements; use a path ending in a key")

    old = json.dumps(parent[key], separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    anchor = b'"%s":%s' % (key.encode("utf-8"), old)
    if plain.count(anchor) == 1:
        return anchor, len(old)

    siblings = list(parent.keys())
    position = siblings.index(key)
    while plain.count(anchor) != 1 and position > 0:
        position -= 1
        previous = siblings[position]
        prefix = b'"%s":%s,' % (previous.encode("utf-8"),
                                json.dumps(parent[previous], separators=(",", ":"),
                                           ensure_ascii=False).encode("utf-8"))
        anchor = prefix + anchor

    count = plain.count(anchor)
    if count != 1:
        raise ValueError("cannot pin down %s in the file: %d matches even with context"
                         % (path, count))
    return anchor, len(old)


def coerce(text, current):
    """Read the new value as whatever type the old one was."""
    if isinstance(current, bool):
        if text.lower() in ("true", "false"):
            return text.lower() == "true"
        raise ValueError("%s is a boolean; pass true or false" % type(current).__name__)
    if isinstance(current, int):
        return int(text, 0)
    if isinstance(current, float):
        return float(text)
    if isinstance(current, str):
        return text
    raise ValueError("cannot set a %s directly; edit it with decode/encode"
                     % type(current).__name__)


def game_running():
    try:
        out = subprocess.check_output(["tasklist", "/FI", "IMAGENAME eq " + GAME_PROCESS],
                                      stderr=subprocess.STDOUT)
    except Exception:
        return False
    return GAME_PROCESS.lower().encode() in out.lower()


def _writable(directory):
    """True if a file can actually be created in `directory`."""
    try:
        os.makedirs(directory, exist_ok=True)
        probe = os.path.join(directory, ".write-probe")
        with open(probe, "w"):
            pass
        os.remove(probe)
        return True
    except OSError:
        return False


def data_dir():
    """Where backups, dumps and caches go.

    In the editor's own folder when that works -- beside `sas4.bat`, not down in `tools/` --
    which is what someone who extracted a zip to their Desktop expects: the backups sit next
    to the thing they double-clicked. But a zip extracted
    under Program Files, or opened from a read-only share, cannot be written to -- and the
    first thing every write does is take a backup, so that failure lands on the one path
    that must not fail. Fall back to the per-user location the platform provides.

    Checked once at import rather than per call: the answer does not change while running,
    and probing on every backup would mean a file created and deleted before every write.
    """
    if _writable(ROOT):
        return ROOT
    if os.name == "nt":
        base = os.environ.get("LOCALAPPDATA") or os.path.expanduser("~")
        fallback = os.path.join(base, "SAS4Trainer")
    else:
        base = os.environ.get("XDG_DATA_HOME") or os.path.join(os.path.expanduser("~"),
                                                               ".local", "share")
        fallback = os.path.join(base, "sas4trainer")
    if _writable(fallback):
        return fallback
    return tempfile.gettempdir()            # last resort: better than failing the backup


DATA = data_dir()
BACKUPS = os.path.join(DATA, "backups")
DECODED = os.path.join(DATA, "decoded")
SAVES = os.path.join(DATA, "saves")


def backup(path):
    """A timestamped copy of `path`, taken before anything writes over it.

    Raises OSError if the copy cannot be made. Callers must let that stop the write rather
    than carry on: an edit with no backup behind it is the one case FINDINGS.md records
    actually losing a character.
    """
    stamp = time.strftime("%Y%m%d-%H%M%S")
    name = os.path.basename(path)
    # The directory is named to the second, so two writes inside one second would land on
    # the same name and the second copy would overwrite the first -- losing exactly the
    # state an undo of the second write needs. Rare from the command line, ordinary from
    # the windows, where granting an item and taking it back again is two clicks. Suffix
    # until this file's name is free. Zero-padded so the sort the restore dialog does
    # still puts the newest first past nine.
    directory = os.path.join(BACKUPS, "backup-" + stamp)
    attempt = 1
    while os.path.exists(os.path.join(directory, name)):
        attempt += 1
        directory = os.path.join(BACKUPS, "backup-%s-%02d" % (stamp, attempt))
    os.makedirs(directory, exist_ok=True)
    target = os.path.join(directory, name)
    shutil.copyfile(path, target)
    return target


# --- shared edit machinery ---------------------------------------------------------------
#
# Both command lines and both windows write through these, so the backup, the byte-level
# replacement and the verify happen once, in one place, however the edit was asked for.

def level_plan(document, level, slot=0):
    """[(path, value)] that setting a character to `level` has to write.

    Four fields move together. Changing PlayerLevel alone leaves a profile that could not
    have been played into existence -- a level-40 character holding a level-3 one's XP --
    and that is what a plausibility check looks for.

    Points already spent count against the level's grant, so a profile with skills bought
    does not end up over the allowance; and HighestRank is never lowered, since it is the
    highest ever reached, not the current one.
    """
    if not 1 <= level <= sas4_model.MAX_LEVEL:
        raise ValueError("level must be between 1 and %d" % sas4_model.MAX_LEVEL)
    where = "Inventory/Profile%d" % slot
    profile = require_character(document, slot)
    skills = profile.get("Skills") or {}
    spent = sum(e.get("SkillLevel", 0) for e in skills.get("SkillsArray") or []
                if isinstance(e, dict))
    rank = document.get("Global", {}).get("HighestRank")
    return [
        ("%s/Skills/PlayerLevel" % where, level),
        ("%s/Skills/PlayerTotalXp" % where, sas4_model.xp_for_level(level)),
        ("%s/Skills/AvailableSkillPoints" % where, max(0, level - spent)),
        ("Global/HighestRank", max(level, rank if isinstance(rank, int) else 0)),
    ], spent


class EmptySlot(ValueError):
    """The named character slot exists but nothing has been played in it.

    A ValueError, because every caller already handles one and this is one more way a
    request cannot be met. Its own class only so a caller can tell it apart: `give` follows
    a failure with "list them with: py sas4.py items --catalog", which is the right advice
    for an id it could not resolve and a non sequitur for a slot with no character in it.
    """


def require_character(document, slot, flag="--slot"):
    """Raise unless `slot` holds a character. Returns the profile when it does.

    A fresh account has six slots and one character. The other five are on disk as the
    stub `{"Loaded": false}` -- no Skills, no weapons, nothing but the flag saying so.
    They are not missing, they are empty, and `if not profile` cannot tell the difference
    because a one-key dictionary is true.

    That gap was reachable two ways. `mastery --slot 4` wrote twenty-seven maxed tracks
    into a slot with no character, and every later `check` reported them -- including the
    one `level` runs on its own result, so a later edit to the real character failed and
    appeared to blame itself. `level --slot 4` skipped all three profile fields, since a
    stub has no Skills to write into, and still raised Global/HighestRank and took a
    backup to do it: a write that did nothing it was asked to do.

    `Loaded` is the field the game sets and the field `loaded_profiles` already reads, so
    deciding it here makes a plan and the check that judges it agree on what a character
    is, rather than inventing a second answer.
    """
    profile = document.get("Inventory", {}).get("Profile%d" % slot)
    if not isinstance(profile, dict):
        raise ValueError("no Profile%d in this save" % slot)
    if not profile.get("Loaded"):
        live = sorted(where.split("Profile")[-1]
                      for where, _p in sas4_model.loaded_profiles(document))
        raise EmptySlot(
            "character slot %d is empty -- nothing has been played in it.\n"
            "Slots holding a character: %s. Pass %s with one of those."
            % (slot, ", ".join(live) if live else "none", flag))
    return profile


def pending(document, plan):
    """The subset of `plan` that would actually change something, as (path, old, new)."""
    out = []
    for path, value in plan:
        try:
            current = at_path(document, path)
        except (KeyError, IndexError, TypeError):
            continue
        if current != value:
            out.append((path, current, value))
    return out


def apply_edits(file_path, plan):
    """Write [(path, value)] into a save, at byte level, over one backup.

    Returns (ok, backup_path, message). Nothing is written unless every anchor resolves and
    the rebuilt file verifies, so a partial application cannot reach the disk.
    """
    with open(file_path, "rb") as handle:
        raw = handle.read()
    try:
        saved = backup(file_path)
    except OSError as problem:
        # Every window and command writes through here, so refusing in one place is what
        # keeps a failed backup from becoming a traceback in three different UIs.
        return False, None, ("could not write a backup (%s) -- nothing written.\n"
                             "Backups go to %s" % (problem, BACKUPS))
    plain = dgdata.decode(raw)
    for path, value in plan:
        document = json.loads(plain)          # each replacement moves the next anchor
        try:
            anchor, old_length = anchor_for(document, plain, path)
        except (KeyError, IndexError, TypeError, ValueError) as problem:
            # A path that is not in this save raises KeyError out of the walk, not
            # ValueError; catching only the latter turned a mistyped field into a traceback
            # in whichever window asked for it. Nothing has been written at this point, so
            # the file is still whole.
            return False, saved, "aborted at %s: %s -- nothing written" % (path, problem)
        replacement = anchor[:-old_length] + json.dumps(value, separators=(",", ":"),
                                                        ensure_ascii=False).encode("utf-8")
        plain = plain.replace(anchor, replacement, 1)

    built = dgdata.encode(plain)
    _, _, ok = dgdata.verify(built)
    if not ok:
        return False, saved, "the rebuilt file does not verify -- nothing written"
    with open(file_path, "wb") as handle:
        handle.write(built)
    with open(file_path, "rb") as handle:
        stored, _, ok = dgdata.verify(handle.read())
    return ok, saved, "wrote %d field(s), checksum %s (%s)" % (
        len(plan), stored, "VALID" if ok else "MISMATCH")



# --- commands ----------------------------------------------------------------------------

def line(label, value, indent=2):
    print("%s%-30s %s" % (" " * indent, label, value))


def cmd_view(args):
    raw, d = load(args.file)
    stored, computed, ok = dgdata.verify(raw)
    print("file      %s" % args.file)
    print("checksum  %s / %s -- %s" % (stored, computed, "VALID" if ok else "MISMATCH"))
    profile = d.get("Inventory", {}).get("Profile%d" % args.slot)
    if not profile:
        print("no Profile%d in this save" % args.slot)
        return 1

    want = args.section or ["identity", "currency", "skills", "equipment", "weapons",
                            "boxes", "global"]

    if "identity" in want:
        print("\n-- identity --")
        line("character", "%s (slot %d)" % (profile.get("Name"), args.slot))
        line("account link", d.get("Version", {}).get("link"))
        line("game version", d.get("Version", {}).get("LastGame"))
        line("last upload", d.get("Global", {}).get("TimeOfLastUpload"))
        line("HackCheck", d.get("HackCheck"))
        line("slots in use", ", ".join(k for k, v in d.get("Inventory", {}).items()
                                       if v.get("Loaded")) or "none")
    if "currency" in want:
        print("\n-- currency and tickets --")
        line("Money", "{:,}".format(profile.get("Money", 0)))
        for key in ("AvailableBlackKeys", "AvailableEliteAugmentCores",
                    "AvailableNightmareTickets"):
            line(key, profile.get("Skills", {}).get(key))
        for key in ("ReviveTokens", "AvailablePremiumTickets"):
            line(key, d.get("Global", {}).get(key))
        line("FactionWarCredits", d.get("FactionWarCredits"))
    if "skills" in want:
        print("\n-- level and skills --")
        s = profile.get("Skills", {})
        for key in ("Class", "PlayerLevel", "PlayerTotalXp", "AvailableSkillPoints"):
            line(key, "{:,}".format(s[key]) if isinstance(s.get(key), int) else s.get(key))
        line("HighestRank", d.get("Global", {}).get("HighestRank"))
        for entry in s.get("SkillsArray") or []:
            line("  " + str(entry.get("SkillName")), "level %s" % entry.get("SkillLevel"), 4)
    if "equipment" in want:
        print("\n-- equipment --")
        for item in profile.get("Equipment") or []:
            line(describe(item.get("ID"), "equipment"), "grade %s, bonus %s, %s" % (
                item.get("Grade"), item.get("BonusStatsLevel"),
                "equipped" if item.get("Equipped") else "stored"))
        if not item_names():
            line("", "(run `py sas4.py items` to show names instead of IDs)")
    if "weapons" in want:
        print("\n-- weapons --")
        weapons = profile.get("Weapons") or []
        line("count", len(weapons))
        for item in weapons[:15]:
            if isinstance(item, dict):
                line(describe(item.get("ID"), "weapon"), "grade %s, bonus %s" % (
                    item.get("Grade"), item.get("BonusStatsLevel")))
    if "boxes" in want:
        print("\n-- strongboxes --")
        boxes = profile.get("Strongboxes") or {}
        line("Unopened", "%d  %s" % (len(boxes.get("Unopened") or []),
                                     json.dumps(boxes.get("Unopened"))[:50]))
        line("Claimed", len(boxes.get("Claimed") or []))
        line("StrongboxesOpened", profile.get("StrongboxesOpened"))
    if "global" in want:
        print("\n-- global --")
        for key, value in (d.get("Global") or {}).items():
            line(key, value)
    return 0


TYPES = {"bool": bool, "int": int, "str": str, "float": float, "null": type(None)}


def kind_of(value):
    for name, cls in TYPES.items():
        if name == "int" and isinstance(value, bool):
            continue
        if isinstance(value, cls):
            return name
    return "other"


def cmd_list(args):
    _, d = load(args.file)
    shown = 0
    for path, value in scalars(d):
        if args.grep and args.grep.lower() not in path.lower():
            continue
        if args.type and kind_of(value) != args.type:
            continue
        if args.under and not path.lower().startswith("/" + args.under.strip("/").lower()):
            continue
        print("  %-6s %-58s %s" % (kind_of(value), path, json.dumps(value)[:50]))
        shown += 1
    print("\n  %d values" % shown)
    return 0


def cmd_kinds(args):
    """How many values of each type, and where the booleans live - the map, not the values."""
    _, d = load(args.file)
    from collections import Counter
    counts = Counter()
    areas = Counter()
    for path, value in scalars(d):
        counts[kind_of(value)] += 1
        if kind_of(value) == "bool":
            # collapse array indices, or a 256-entry collection prints 256 lines
            head = path.strip("/").split("/")[0].split("[")[0]
            areas[head] += 1
    print("by type:")
    for name, count in counts.most_common():
        print("  %-8s %d" % (name, count))
    print("\nbooleans by top-level section:")
    for name, count in areas.most_common():
        print("  %-28s %d" % (name, count))
    return 0


def cmd_get(args):
    _, d = load(args.file)
    print(json.dumps(at_path(d, args.path), indent=2, ensure_ascii=False))
    return 0


def cmd_set(args):
    if game_running() and not args.force:
        print("%s is running. It rewrites the save on its own schedule and would overwrite"
              % GAME_PROCESS)
        print("this edit. Close the game first, or pass --force if you know better.")
        return 1

    raw, d = load(args.file)
    stored, computed, ok = dgdata.verify(raw)
    if not ok and not args.force:
        print("this file does not verify (%s vs %s) -- refusing to edit it" % (stored, computed))
        return 1

    try:
        current = at_path(d, args.path)
    except (KeyError, IndexError, TypeError):
        print("no such path: %s" % args.path)
        print("find one with:  py sas4.py list --grep <part of the name>")
        return 1

    try:
        new = coerce(args.value, current)
    except ValueError as problem:
        print("%s holds %s (%s); %s" % (args.path, json.dumps(current),
                                        type(current).__name__, problem))
        return 1
    if new == current:
        print("%s is already %s" % (args.path, json.dumps(current)))
        return 0

    plain = dgdata.decode(raw)
    try:
        anchor, old_length = anchor_for(d, plain, args.path)
    except ValueError as problem:
        print("%s" % problem)
        return 1
    replacement = anchor[:-old_length] + json.dumps(new, separators=(",", ":"),
                                                    ensure_ascii=False).encode("utf-8")

    print("%s" % args.path)
    print("  %s  ->  %s" % (json.dumps(current), json.dumps(new)))
    if args.dry_run:
        print("  (dry run, nothing written)")
        return 0

    saved = backup(args.file)
    print("  backup   %s" % saved)

    patched = plain.replace(anchor, replacement, 1)
    built = dgdata.encode(patched)
    _, _, ok = dgdata.verify(built)
    if not ok:
        print("  built file does not verify -- nothing written")
        return 1

    with open(args.file, "wb") as handle:
        handle.write(built)

    check_raw, check_doc = load(args.file)
    s, c, ok = dgdata.verify(check_raw)
    print("  checksum %s (%s)" % (s, "VALID" if ok else "MISMATCH"))
    print("  reads back as %s" % json.dumps(at_path(check_doc, args.path)))
    return 0 if ok else 1


# --- masteries ---------------------------------------------------------------------------

# MasteryProgress sits at the document root, not under Inventory/ProfileN, but is indexed by
# the same character slot: MasteryProgress/MasteryProfileN is a list of 27 tracks, each
# {"MasteryXp": int, "MasteryLvl": int}. Measured from a real profile; six slots, 27 entries
# each, on every one.
MASTERY_PATH = "MasteryProgress/MasteryProfile%d"
# The counts and the XP thresholds live in sas4_model, beside the rule that enforces them,
# so the writer and the checker cannot drift apart. The thresholds are the community wiki's
# and are the one part taken on trust: every track in every save captured from this game sat
# at MasteryLvl 0, so the game has never been observed setting one. `mastery` writes XP and
# level together for that reason -- whichever of the two the game reads, the pair agrees.
MASTERY_SLOTS = sas4_model.MASTERY_SLOTS
MASTERY_MAX_LEVEL = sas4_model.MASTERY_MAX_LEVEL
MASTERY_LEVEL_XP = sas4_model.MASTERY_LEVEL_XP
mastery_level_for_xp = sas4_model.mastery_level_for_xp
MASTERY_TRACKS = sas4_model.MASTERY_TRACKS
mastery_name = sas4_model.mastery_name


def mastery_rows(document, slot=0):
    """[(index, xp, level, level_the_xp_supports)] for one character's mastery tracks."""
    try:
        tracks = at_path(document, MASTERY_PATH % slot)
    except (KeyError, IndexError, TypeError):
        return []
    if not isinstance(tracks, list):
        return []
    out = []
    for index, entry in enumerate(tracks):
        if not isinstance(entry, dict):
            continue
        xp = entry.get("MasteryXp", 0) or 0
        level = entry.get("MasteryLvl", 0) or 0
        # This feeds a table, and a value that is not a number belongs in it as itself. A
        # hand-edited file holding "3" where a count goes is worth seeing; rewriting it to
        # 0 would print a tidy row for a file that is not tidy. The level the XP supports
        # is None when there is no number to work it out from, and the caller shows that
        # as a blank rather than claiming a level. Before this, the comparison inside
        # mastery_level_for_xp raised and the table never appeared at all.
        supported = mastery_level_for_xp(xp) if isinstance(xp, int) else None
        out.append((index, xp, level, supported))
    return out


def mastery_plan(document, targets, slot=0):
    """[(path, value)] setting mastery tracks to levels. `targets` is {index: level}.

    The whole list is replaced rather than each field edited, because the fields cannot be
    pinned down individually: a profile holds 154 occurrences of `"MasteryXp":0`, and
    `anchor_for` rightly refuses an anchor it cannot make unique. The list as a whole has a
    unique anchor, so this follows what `grant_plan` already does with Claimed -- one value,
    one replacement, one backup, however many tracks move.

    Two indexes have been established by measurement and are named in MASTERY_TRACKS; the
    rest are not guessed, because a wrong name here sends an edit to the wrong track. To
    name another, play one mission using one weapon type and run `py sas4.py watch` -- the
    index whose MasteryXp moves is that type -- or read the number off the game's own
    mastery screen and find the track holding it.
    """
    require_character(document, slot)
    try:
        tracks = at_path(document, MASTERY_PATH % slot)
    except (KeyError, IndexError, TypeError):
        raise ValueError("no MasteryProgress/MasteryProfile%d in this save" % slot)
    if not isinstance(tracks, list):
        raise ValueError("MasteryProgress/MasteryProfile%d is not a list" % slot)

    updated = [dict(entry) if isinstance(entry, dict) else {"MasteryXp": 0, "MasteryLvl": 0}
               for entry in tracks]
    for index, level in sorted(targets.items()):
        if not 0 <= index < len(updated):
            raise ValueError("track %d is out of range; this save has %d of them"
                             % (index, len(updated)))
        if not 0 <= level <= MASTERY_MAX_LEVEL:
            raise ValueError("mastery level %d is out of range (0-%d)"
                             % (level, MASTERY_MAX_LEVEL))
        # Key order as the game writes it, so the replacement reads like the rest of the file.
        updated[index] = {"MasteryXp": MASTERY_LEVEL_XP[level], "MasteryLvl": level}
    return [(MASTERY_PATH % slot, updated)]


# Every run in Claimed is four elements: the tag, the item dict, and two more integers.
# The pair is not validated -- three different constants are in the wild and all of them
# work: this uses 8, 0; 0daxelagnia/SAS4Tool writes 8, 2 in one file and 8, 0 in another;
# the game itself writes 2, 2 and 5, 0 and 2, 0 and 2, 1 depending on the box. What does
# matter is the length, because that is what makes the stream parseable.
CLAIMED_RUN = 4
CLAIMED_TAIL = [8, 0]


def weapon_entry(item_id, grade, bonus):
    """A finished gun for Strongboxes.Claimed. A bare 0 tags it, then the dict, then the tail."""
    return [0, {"ID": item_id, "EquipVersion": 0, "Grade": grade, "AugmentSlots": 0,
                "BonusStatsLevel": bonus, "InventoryIndex": 0}] + CLAIMED_TAIL


def equipment_entry(item_id, slot, grade, bonus):
    """A finished piece of equipment. A bare 1 tags it, then the dict, then the tail."""
    return [1, {"ID": item_id, "EquipVersion": 0, "Grade": grade, "AugmentSlots": 0,
                "BonusStatsLevel": bonus, "EquippedSlot": slot,
                "InventoryIndex": slot}] + CLAIMED_TAIL


def claimed_items(document, slotprofile=0):
    """[(index, kind, id, name, grade, bonus, slot)] for what a profile already owns.

    Claimed is a flattened stream of runs, not a list of objects. Every run is four
    elements -- tag, item dict, and two more integers -- for equipment as much as for a
    weapon. The run length is what makes it parseable, since a trailing 0 or 1 would
    otherwise read as the start of the next run, so whole runs are consumed, never
    positions.

    Read off 36 saves captured while opening boxes: 36 of 36 divide exactly into runs of
    four, and both prior-art editors append four for either kind. An earlier reading of
    two for equipment came from the write side of one of those tools and was never checked
    against a stream the game had written; it made `drop_claimed` cut a run in half.

    `index` is where the run starts, which is what `drop_claimed` needs to remove it.
    Anything that does not parse is skipped rather than guessed at.
    """
    path = "Inventory/Profile%d/Strongboxes/Claimed" % slotprofile
    try:
        claimed = at_path(document, path)
    except (KeyError, IndexError, TypeError):
        return []
    if not isinstance(claimed, list):
        return []

    names = item_names()
    out, i = [], 0
    while i < len(claimed):
        tag = claimed[i]
        if tag == 0:
            kind = "weapon"
        elif tag == 1:
            kind = "equipment"
        else:
            i += 1
            continue
        if i + CLAIMED_RUN > len(claimed):
            break
        entry = claimed[i + 1]
        if isinstance(entry, dict) and isinstance(entry.get("ID"), int):
            known = names.get(kind, {}).get(entry["ID"])
            out.append((i, kind, entry["ID"], known[0] if known else "id %d" % entry["ID"],
                        entry.get("Grade", 0), entry.get("BonusStatsLevel", 0),
                        entry.get("EquippedSlot")))
        i += CLAIMED_RUN
    return out


def drop_claimed(document, indexes, slotprofile=0):
    """[(path, value)] removing the runs starting at `indexes` from Claimed."""
    path = "Inventory/Profile%d/Strongboxes/Claimed" % slotprofile
    claimed = at_path(document, path)
    doomed = set()
    for start in indexes:
        if start >= len(claimed):
            continue
        if claimed[start] not in (0, 1):
            continue
        doomed.update(range(start, min(start + CLAIMED_RUN, len(claimed))))
    kept = [v for i, v in enumerate(claimed) if i not in doomed]
    return [(path, kept)]


def grant_plan(document, requests, slotprofile=0):
    """[(path, value)] granting several items at once, plus a label for each.

    One edit however many items: Claimed is a single value, so appending five things is one
    replacement over one backup rather than five of each.

    `requests` is [(item_id, kind, grade, bonus, slot)]; `kind` may be "auto" when the id is
    unambiguous. Raises ValueError naming the first request it cannot resolve, before
    anything is built, so a bad entry cannot half-apply.
    """
    domains = item_names()
    if not domains:
        raise ValueError("run `items` first so IDs can be resolved to weapon vs equipment")
    weapons, equipment = domains.get("weapon", {}), domains.get("equipment", {})

    # Same guard as level and mastery, and for the same reason: an unloaded slot has no
    # Strongboxes to append to, and reporting that as a missing path describes a symptom
    # rather than the thing that is actually wrong.
    require_character(document, slotprofile, "--slotprofile")
    path = "Inventory/Profile%d/Strongboxes/Claimed" % slotprofile
    try:
        claimed = at_path(document, path)
    except (KeyError, IndexError, TypeError):
        raise ValueError("no Strongboxes/Claimed in Profile%d" % slotprofile)

    added, labels = [], []
    for item_id, kind, grade, bonus, slot in requests:
        in_weapon, in_equip = item_id in weapons, item_id in equipment
        if kind == "auto":
            if in_weapon and in_equip:
                raise ValueError("id %d is both a weapon (%s) and equipment (%s) -- say which"
                                 % (item_id, weapons[item_id][0], equipment[item_id][0]))
            kind = "weapon" if in_weapon else "equipment" if in_equip else None
        if kind == "weapon" and in_weapon:
            added.extend(weapon_entry(item_id, grade, bonus))
            labels.append("%s (weapon)" % weapons[item_id][0])
        elif kind == "equipment" and in_equip:
            added.extend(equipment_entry(item_id, slot, grade, bonus))
            labels.append("%s (equipment, slot %d)" % (equipment[item_id][0], slot))
        else:
            raise ValueError("id %d is not a known %s id"
                             % (item_id, kind or "weapon or equipment"))
    return [(path, claimed + added)], labels



def give_plan(document, item_id, kind="auto", grade=0, bonus=0, slot=2, slotprofile=0):
    """[(path, value)] granting one finished item, plus a label for it.

    The item goes into Strongboxes.Claimed -- the inventory of what you already own --
    rather than as an unopened box, because no published tool writes an openable box and
    the Unopened schema is not documented anywhere. Claimed is also the reliable half: a
    box is random, a granted item is not.

    The single-item case of `grant_plan`, kept because most callers want exactly one.
    """
    plan, labels = grant_plan(document, [(item_id, kind, grade, bonus, slot)], slotprofile)
    return plan, labels[0]


def cmd_give(args):
    """Grant a finished item straight into Strongboxes.Claimed -- the loot without the box.

    py sas4.py give 129                 grant weapon 129 (Z-5 Heavy)
    py sas4.py give 101 --slot 2        grant equipment 101 into slot 2
    py sas4.py give 129 --grade 12 --bonus 10

    `py sas4.py items --catalog` writes the full list of what can be granted.
    """
    raw, d = load(args.file)
    stored, computed, ok = dgdata.verify(raw)
    if not ok and not args.force:
        print("this file does not verify (%s vs %s) -- refusing to edit it" % (stored, computed))
        return 1
    try:
        plan, label = give_plan(d, args.item, args.kind, args.grade, args.bonus,
                                args.slot, args.slotprofile)
    except ValueError as problem:
        print(problem)
        if not isinstance(problem, EmptySlot):
            print("list them with:  py sas4.py items --catalog")
        return 1

    old_len = len(at_path(d, plan[0][0]))
    print("grant %s  (id %d, grade %d, bonus %d)" % (label, args.item, args.grade, args.bonus))
    print("  Claimed: %d entries -> %d" % (old_len, len(plan[0][1])))
    if args.dry_run:
        print("  (dry run, nothing written)")
        return 0
    if game_running() and not args.force:
        print("  %s is running -- close it first" % GAME_PROCESS)
        return 1

    ok, saved, message = apply_edits(args.file, plan)
    print("  backup   %s" % saved)
    print("  %s" % message)
    return 0 if ok else 1


ITEMS_URL = "https://raw.githubusercontent.com/0daxelagnia/SAS4Tool/main/items.json"
ITEMS_CACHE = os.path.join(DECODED, "items.json")


def cmd_level(args):
    """Set a character's level, and the three values that have to agree with it.

        Skills/PlayerLevel            the level itself
        Skills/PlayerTotalXp          the cumulative XP that level starts at
        Skills/AvailableSkillPoints   one point per level, less any already spent
        Global/HighestRank            the highest rank reached, never below the level

    All four are written in one pass over one backup, and the result is run through
    `sas4_model.check` before reporting. Skill points are granted, not spent: the levels are
    yours to distribute in game. Writing skill levels directly is what FINDINGS.md warns
    against, so this does not.

    A consistent file is not a safe one. The profile uploads to Ninja Kiwi every ten minutes
    and the server keeps its own copy, so a jump this makes is a plain diff on their side
    however well the file agrees with itself.
    """
    raw, d = load(args.file)
    stored, computed, ok = dgdata.verify(raw)
    if not ok and not args.force:
        print("this file does not verify (%s vs %s) -- refusing to edit it" % (stored, computed))
        return 1
    try:
        plan, spent = level_plan(d, args.level, args.slot)
    except ValueError as problem:
        print(problem)
        return 1

    print("%s -> level %d" % (args.file, args.level))
    if spent:
        print("  %d skill point(s) already spent, so %d are granted rather than %d"
              % (spent, max(0, args.level - spent), args.level))
    for path, value in plan:
        try:
            current = at_path(d, path)
        except (KeyError, IndexError, TypeError):
            print("  skip %s -- not in this save" % path)
            continue
        print("  %-46s %s -> %s%s" % (path, json.dumps(current), json.dumps(value),
                                      "" if current != value else "   (already)"))

    changes = [(p, n) for p, _o, n in pending(d, plan)]
    if not changes:
        print("\nnothing to change")
        return 0
    if args.dry_run:
        print("\n  (dry run, nothing written)")
        return 0
    if game_running() and not args.force:
        print("\n%s is running. It rewrites the save on its own schedule and would overwrite"
              % GAME_PROCESS)
        print("this edit. Close the game first, or pass --force if you know better.")
        return 1

    ok, saved, message = apply_edits(args.file, changes)
    print("\n  backup   %s" % saved)
    print("  %s" % message)
    if not ok:
        return 1
    problems = sas4_model.check(load(args.file)[1])
    if problems:
        print("  but the result is not consistent:")
        for line in problems:
            print("    - %s" % line)
        return 1
    print("  consistent: nothing a plausibility check would flag")
    return 0


def cmd_items(args):
    """Fetch the community item table so IDs can be shown as names.

    The table is roughly 300 entries mapping item IDs to names, from
    0daxelagnia/SAS4Tool. It is downloaded rather than vendored: it is someone else's data,
    it changes when the game does, and it is only needed for display.
    """
    import urllib.request
    os.makedirs(os.path.dirname(ITEMS_CACHE), exist_ok=True)
    print("fetching %s" % ITEMS_URL)
    with urllib.request.urlopen(ITEMS_URL, timeout=30) as response:
        body = response.read()
    parsed = json.loads(body)                    # also rejects a broken download
    # Stored indented, not as the one long line it arrives as, so the cache is readable.
    with open(ITEMS_CACHE, "w", encoding="utf-8") as handle:
        json.dump(parsed, handle, indent=2, ensure_ascii=False)
    table = item_names()
    total = sum(len(entries) for entries in table.values())
    print("wrote %s (%d bytes, %d items)" % (ITEMS_CACHE, len(body), total))
    if getattr(args, "catalog", None):
        written = write_catalog(args.catalog)
        print("wrote %s (%d items, grouped by tier and category)" % (args.catalog, written))
    for domain, entries in sorted(table.items(), key=lambda kv: -len(kv[1])):
        categories = {}
        for _name, category in entries.values():
            categories[category] = categories.get(category, 0) + 1
        print("  %-12s %3d  %s" % (domain, len(entries),
                                   ", ".join("%s %d" % (c, n) for c, n in
                                             sorted(categories.items(), key=lambda kv: -kv[1]))))
    return 0


def item_catalog():
    """[(domain, tier, category, id, name)] for everything the item table knows.

    `item_names` flattens the table to {domain: {id: (name, category)}} for printing a name
    beside an id, which throws away the tier a thing belongs to (normal, red, black,
    factions). The catalogue keeps it, because when the question is "what can I ask for"
    rather than "what is this id", the tier is most of the answer.

    IDs are unique within a domain -- checked across all 437 entries -- so `give` needs only
    the id and, when a number is both a weapon and a piece of equipment, --kind.
    """
    if not os.path.exists(ITEMS_CACHE):
        return []
    try:
        with open(ITEMS_CACHE, encoding="utf-8") as handle:
            document = json.load(handle)
    except (OSError, ValueError):
        return []

    rows = []

    def walk(node, domain, tier, category):
        if isinstance(node, dict):
            if "Name" in node and isinstance(node.get("ID"), int):
                rows.append((domain, tier or "-", category or "-", node["ID"], node["Name"]))
                return
            for key, value in node.items():
                # The first level under a domain is the tier, the next is the category.
                walk(value, domain, tier or key, key if tier else category)
        elif isinstance(node, list):
            for value in node:
                walk(value, domain, tier, category)

    for section, node in document.items():
        walk(node, section.replace("_info", ""), None, None)
    return rows


def write_catalog(path):
    """A readable list of everything `give` can grant, grouped and with the command to use."""
    rows = item_catalog()
    if not rows:
        return 0
    groups = {}
    for domain, tier, category, item_id, name in rows:
        groups.setdefault((domain, tier, category), []).append((item_id, name))

    with open(path, "w", encoding="utf-8") as out:
        out.write("# What `give` can grant\n\n")
        out.write("%d items, from the community table `py sas4.py items` downloads.\n\n"
                  % len(rows))
        out.write("Grant one with its ID:\n\n```\npy sas4.py give <id>\n"
                  "py sas4.py give <id> --kind weapon      when the id is both a weapon and equipment\n"
                  "py sas4.py give <id> --grade 12 --bonus 10\n```\n\n"
                  "IDs are unique inside a domain but not across them: equipment 101 and\n"
                  "weapon 101 are different things, which is what `--kind` settles.\n\n"
                  "Tiers: **normal**, **red**, **black**, **factions**.\n\n")
        for domain in ("weapon", "equipment", "turret", "premium"):
            picked = {k: v for k, v in groups.items() if k[0] == domain}
            if not picked:
                continue
            total = sum(len(v) for v in picked.values())
            out.write("\n## %s  (%d)\n" % (domain, total))
            for (_d, tier, category), items in sorted(picked.items()):
                out.write("\n### %s / %s  (%d)\n\n" % (tier, category, len(items)))
                out.write("| ID | Name |\n|---:|------|\n")
                for item_id, name in sorted(items):
                    out.write("| %d | %s |\n" % (item_id, name))
    return len(rows)



_ITEM_CACHE = None


def item_names():
    """{domain: {id: (name, category)}} from the cached table, or {} if never fetched.

    Weapons and equipment number themselves separately -- equipment 101 and weapon 101 are
    different things -- so the table is kept split by the top-level section it came from and
    a lookup has to say which it wants. Getting this wrong silently prints a shotgun's name
    against a vest.

    Within a domain the document is walked rather than assumed: any object carrying both
    Name and ID counts, and the key it sits under becomes the category (helmet, smg, ...).
    """
    global _ITEM_CACHE
    if _ITEM_CACHE is not None:
        return _ITEM_CACHE
    _ITEM_CACHE = {}
    if not os.path.exists(ITEMS_CACHE):
        return _ITEM_CACHE
    try:
        with open(ITEMS_CACHE, encoding="utf-8") as handle:
            document = json.load(handle)
    except (OSError, ValueError) as problem:
        print("item table at %s is unreadable: %s" % (ITEMS_CACHE, problem))
        return _ITEM_CACHE

    for section, node in document.items():
        domain = section.replace("_info", "")
        table = _ITEM_CACHE.setdefault(domain, {})

        def walk(inner, category):
            if isinstance(inner, dict):
                if "Name" in inner and isinstance(inner.get("ID"), int):
                    table.setdefault(inner["ID"], (inner["Name"], category))
                    return
                for key, value in inner.items():
                    walk(value, key)
            elif isinstance(inner, list):
                for value in inner:
                    walk(value, category)

        walk(node, domain)
    return _ITEM_CACHE


def describe(item_id, domain):
    """'101 -- Special Forces Vest (vest)' when the table is present, '101' when it is not."""
    known = item_names().get(domain, {}).get(item_id)
    return "%s -- %s (%s)" % (item_id, known[0], known[1]) if known else str(item_id)


def cmd_verify(args):
    with open(args.file, "rb") as handle:
        raw = handle.read()
    stored, computed, ok = dgdata.verify(raw)
    print("stored   %s\ncomputed %s\n%s" % (stored, computed,
          "VALID" if ok else "MISMATCH - the game would reject this file"))
    return 0 if ok else 1


def cmd_decode(args):
    raw = open(args.file, "rb").read()
    out = args.out or os.path.join(DECODED, "profile.json")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    document = json.loads(dgdata.decode(raw))
    with open(out, "w", encoding="utf-8") as handle:
        json.dump(document, handle, indent=2, ensure_ascii=False)
    print("wrote %s (%d bytes)" % (out, os.path.getsize(out)))
    return 0


def cmd_encode(args):
    with open(args.json, "rb") as handle:
        plain = handle.read()
    json.loads(plain)
    built = dgdata.encode(plain)
    with open(args.out, "wb") as handle:
        handle.write(built)
    print("wrote %s, %d bytes, header %s" % (args.out, len(built), built[:14].decode()))
    return 0


def cmd_watch(args):
    """Print what changed every time the game rewrites the save.

    With --archive, each version is also copied into the saves directory as it appears. That is how the
    sample set in there was collected, and it is the only way to get a real example of a
    structure the profile does not currently hold - an unopened strongbox, say.
    """
    print("watching %s -- Ctrl+C to stop" % args.file)
    store = SAVES
    if args.archive:
        os.makedirs(store, exist_ok=True)
        print("archiving each version into %s" % store)
    index = 0
    previous = None
    last = 0
    while True:
        try:
            mtime = os.path.getmtime(args.file)
        except OSError:
            time.sleep(1)
            continue
        if mtime != last:
            last = mtime
            time.sleep(0.4)                      # let the game finish writing
            try:
                _, document = load(args.file)
            except Exception as problem:
                print("  unreadable mid-write: %s" % problem)
                continue
            if args.archive:
                copy = os.path.join(store, "watch-%03d.save" % index)
                shutil.copyfile(args.file, copy)
                index += 1
            current = dict(scalars(document))
            if previous is not None:
                changed = [(k, previous.get(k), v) for k, v in current.items()
                           if k in previous and previous[k] != v]
                added = [k for k in current if k not in previous]
                print("\n=== %s ===" % time.strftime("%H:%M:%S"))
                for key, before, after in changed[:40]:
                    print("  %-58s %s -> %s" % (key, json.dumps(before)[:24],
                                                json.dumps(after)[:24]))
                if added:
                    print("  %d new paths, e.g. %s" % (len(added), added[:3]))
                if not changed and not added:
                    print("  rewritten with no value change")
            previous = current
        time.sleep(1)


def redact(value):
    """Show the length of a secret, not the secret."""
    return "<%d chars, hidden>" % len(value) if isinstance(value, str) else value


def cmd_session(args):
    """Read the login session, or set one field of it.

    current.session is a DGDATA file like the save, carrying the account's sessionID and
    nkapiID -- the credentials the server trusts. They are printed as lengths, never in
    full: this file is worth the same care as a password.

    The one legitimate reason to edit it here is consistency. The account id appears in
    three places that must agree -- this file's nkapiID, the profile's `link`, and the name
    of the save folder -- so `session set nkapiID <id>` exists to line them up when the
    other two have been changed. It does NOT let you become another account on the server:
    the sessionID is issued by Ninja Kiwi at login and cannot be forged by editing text.
    """
    path = args.session
    raw = open(path, "rb").read()
    stored, computed, ok = dgdata.verify(raw)
    document = json.loads(dgdata.decode(raw))

    if not args.set:
        print("file      %s" % path)
        print("checksum  %s / %s -- %s" % (stored, computed, "VALID" if ok else "MISMATCH"))
        printable = json.loads(json.dumps(document))
        for section in printable.values():
            if isinstance(section, dict):
                for key in list(section):
                    if key in SECRET_KEYS:
                        section[key] = redact(section[key])
        print(json.dumps(printable, indent=2, ensure_ascii=False))
        account = document.get("user", {}).get("nkapiID")
        if account:
            print("\naccount id (nkapiID): %s" % account)
            print("  must match the profile's `link` and the save folder name")
        return 0

    key, _, new_value = args.set.partition("=")
    key = key.strip()
    if not new_value:
        print("use  session --set nkapiID=<value>")
        return 1
    if key in CREDENTIAL_KEYS:
        print("WARNING: %s is a credential the server issues at login." % key)
        print("Editing it only makes sense to line this file up with a profile `link` and")
        print("folder name you have already changed. It cannot make the server treat you as")
        print("another account.")
        if not args.yes and input("continue? [y/N] ").strip().lower() != "y":
            print("cancelled")
            return 1

    if game_running() and not args.force:
        print("%s is running -- close it first, or pass --force" % GAME_PROCESS)
        return 1

    saved = backup(path)
    print("backup   %s" % saved)
    plain = dgdata.decode(raw)
    for section, node in document.items():
        if isinstance(node, dict) and key in node:
            old = json.dumps(node[key], separators=(",", ":"), ensure_ascii=False).encode()
            anchor = b'"%s":%s' % (key.encode(), old)
            if plain.count(anchor) != 1:
                print("cannot pin down %s uniquely" % key)
                return 1
            coerced = coerce(new_value, node[key])
            replacement = anchor[:-len(old)] + json.dumps(
                coerced, separators=(",", ":"), ensure_ascii=False).encode()
            plain = plain.replace(anchor, replacement, 1)
            break
    else:
        print("no field named %s in the session" % key)
        return 1

    built = dgdata.encode(plain)
    _, _, ok = dgdata.verify(built)
    if not ok:
        print("rebuilt file does not verify -- nothing written")
        return 1
    with open(path, "wb") as handle:
        handle.write(built)
    print("set %s, checksum now %s" % (key, dgdata.verify(open(path, "rb").read())[0]))
    return 0


def cmd_graft(args):
    """Copy progress from another save into this one, keeping this account's identity.

    Taking a public save whole would drag its owner's `link` along, which then disagrees
    with the session and the folder name. Grafting copies the fields you name -- money,
    level, weapons, whatever -- and leaves identity alone, so the result is your account
    with someone else's progress, which is what was asked for.

    Nothing is written without --apply; by default it shows what would change.
    """
    def _contains_identity(value):
        """The first identity-field key found anywhere inside a value, or None."""
        if isinstance(value, dict):
            for key, sub in value.items():
                if key in IDENTITY_FIELDS:
                    return key
                found = _contains_identity(sub)
                if found:
                    return found
        elif isinstance(value, list):
            for item in value:
                found = _contains_identity(item)
                if found:
                    return found
        return None

    source_raw, source = load(args.source)
    dest_raw, dest = load(args.file)
    if not args.fields:
        print("name at least one field, e.g.")
        print("  graft other.save --fields Inventory/Profile0/Money,Inventory/Profile0/Weapons")
        return 1

    plan = []
    for field in args.fields.split(","):
        field = field.strip()
        try:
            new_value = at_path(source, field)
        except (KeyError, IndexError, TypeError):
            print("skip %s -- not in the source" % field)
            continue
        # Refuse a field that IS an identity field, or one whose value CONTAINS one nested.
        # Leaf-name matching alone let `--fields Version` copy Version.link through; the
        # README promises graft keeps your identity, so the whole subtree has to be checked.
        if field.split("/")[-1] in IDENTITY_FIELDS:
            print("skip %s -- that is an identity field, keeping yours" % field)
            continue
        buried = _contains_identity(new_value)
        if buried:
            print("skip %s -- it contains the identity field %r, keeping yours" % (field, buried))
            continue
        try:
            old_value = at_path(dest, field)
        except (KeyError, IndexError, TypeError):
            old_value = "<absent>"
        plan.append((field, old_value, new_value))

    if not plan:
        print("nothing to graft")
        return 1

    print("from %s\ninto %s\n" % (args.source, args.file))
    for field, old, new in plan:
        print("  %s" % field)
        print("      %s  ->  %s" % (json.dumps(old)[:60], json.dumps(new)[:60]))

    if not args.apply:
        print("\n(preview -- pass --apply to write, after closing the game)")
        return 0
    if game_running() and not args.force:
        print("\n%s is running -- close it first" % GAME_PROCESS)
        return 1

    saved = backup(args.file)
    print("\nbackup   %s" % saved)
    plain = dgdata.decode(dest_raw)
    for field, _old, new_value in plan:
        document = json.loads(plain)
        try:
            anchor, old_length = anchor_for(document, plain, field)
        except ValueError as problem:
            print("skip %s -- %s" % (field, problem))
            continue
        replacement = anchor[:-old_length] + json.dumps(
            new_value, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
        plain = plain.replace(anchor, replacement, 1)

    built = dgdata.encode(plain)
    _, _, ok = dgdata.verify(built)
    if not ok:
        print("rebuilt file does not verify -- nothing written")
        return 1
    with open(args.file, "wb") as handle:
        handle.write(built)
    print("grafted %d field(s), checksum now %s"
          % (len(plan), dgdata.verify(open(args.file, "rb").read())[0]))
    return 0


def build_parser():
    """The command line. Separate from `main` so a test can ask what it accepts: an
    argument pair that must not be given together is a promise about the interface, and
    the only way to check it without running a command is to parse and see."""
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--file", default=LIVE, help="save file to work on")
    sub = parser.add_subparsers(dest="command")

    p = sub.add_parser("where"); p.set_defaults(run=cmd_where)

    p = sub.add_parser("view"); p.set_defaults(run=cmd_view)
    p.add_argument("--slot", type=int, default=0)
    p.add_argument("--section", action="append",
                   choices=["identity", "currency", "skills", "equipment", "weapons",
                            "boxes", "global"])

    p = sub.add_parser("list"); p.set_defaults(run=cmd_list)
    p.add_argument("--grep", help="only paths whose name contains this")
    p.add_argument("--type", choices=sorted(TYPES),
                   help="only values of this type -- `bool` is every on/off switch")
    p.add_argument("--under", help="only paths under this one, e.g. Settings")

    p = sub.add_parser("kinds"); p.set_defaults(run=cmd_kinds)

    p = sub.add_parser("get"); p.set_defaults(run=cmd_get)
    p.add_argument("path")

    p = sub.add_parser("set"); p.set_defaults(run=cmd_set)
    p.add_argument("path")
    p.add_argument("value")
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("--force", action="store_true",
                   help="edit anyway with the game running or the checksum already wrong")

    p = sub.add_parser("give"); p.set_defaults(run=cmd_give)
    p.add_argument("item", type=int, help="item id (see `items`)")
    p.add_argument("--kind", choices=["auto", "weapon", "equipment"], default="auto",
                   help="weapon and equipment ids overlap; say which when an id is both")
    p.add_argument("--grade", type=int, default=0)
    p.add_argument("--bonus", type=int, default=0)
    p.add_argument("--slot", type=int, default=2, help="equipped slot, for equipment")
    p.add_argument("--slotprofile", type=int, default=0, help="which character slot")
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("--force", action="store_true")

    p = sub.add_parser("mastery"); p.set_defaults(run=cmd_mastery)
    # Two ways to name the same thing. Given both, the code took --all and dropped --set
    # without a word, so `--set 3=1 --all 5` wrote 5 everywhere and never mentioned the 1.
    which = p.add_mutually_exclusive_group()
    which.add_argument("--set", help="tracks to raise, e.g. 3=5,7=2")
    which.add_argument("--all", type=int, metavar="LEVEL",
                       help="set every track on this character to LEVEL")
    p.add_argument("--slot", type=int, default=0, help="which character slot")
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("--force", action="store_true",
                   help="write anyway with the game running or the checksum already wrong")

    p = sub.add_parser("contribute"); p.set_defaults(run=cmd_contribute)
    p.add_argument("--slot", type=int, default=0, help="which character slot")
    p.add_argument("--print", dest="print_only", action="store_true",
                   help="print the report and write no file")

    p = sub.add_parser("level"); p.set_defaults(run=cmd_level)
    p.add_argument("level", type=int, help="the level to set (1-%d)" % sas4_model.MAX_LEVEL)
    p.add_argument("--slot", type=int, default=0, help="which character slot")
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("--force", action="store_true",
                   help="write anyway with the game running or the checksum already wrong")

    p = sub.add_parser("items"); p.set_defaults(run=cmd_items)
    p.add_argument("--catalog", nargs="?", const=os.path.join(DATA, "ITEMS.md"),
                   help="also write a readable list of every grantable item")

    p = sub.add_parser("verify"); p.set_defaults(run=cmd_verify)

    p = sub.add_parser("decode"); p.set_defaults(run=cmd_decode)
    p.add_argument("out", nargs="?")

    p = sub.add_parser("encode"); p.set_defaults(run=cmd_encode)
    p.add_argument("json")
    p.add_argument("out")

    p = sub.add_parser("watch"); p.set_defaults(run=cmd_watch)
    p.add_argument("--archive", action="store_true",
                   help="also copy each version into the saves directory as it appears")

    p = sub.add_parser("session"); p.set_defaults(run=cmd_session)
    p.add_argument("--session", default=SESSION, help="the current.session file")
    p.add_argument("--set", help="one field to change, as name=value")
    p.add_argument("--yes", action="store_true", help="skip the credential confirmation")
    p.add_argument("--force", action="store_true", help="edit with the game running")

    p = sub.add_parser("graft"); p.set_defaults(run=cmd_graft)
    p.add_argument("source", help="the save to copy progress from")
    p.add_argument("--fields", help="comma-separated paths to copy")
    p.add_argument("--apply", action="store_true", help="write the changes (default: preview)")
    p.add_argument("--force", action="store_true", help="write with the game running")

    return parser


def main():
    parser = build_parser()
    args = parser.parse_args()
    if not args.command:
        parser.print_help()
        return 2
    # Commands that read a save need one; if discovery found none and none was given, say so
    # rather than failing on open(None).
    if getattr(args, "file", "unset") is None and args.command not in ("where", "encode"):
        print("no SAS4 profile found automatically on this machine.")
        print("pass one with --file <path>, or run `py sas4.py where` to see what was found.")
        return 1
    try:
        return args.run(args)
    except SaveError as problem:
        print(problem)
        return 1


def cmd_mastery(args):
    """Read the mastery tracks, or set some of them to a level.

        py sas4.py mastery                  what each track holds now
        py sas4.py mastery --set 3=5        one track to level 5
        py sas4.py mastery --set 3=5,7=2    several, in one write
        py sas4.py mastery --all 5          every track on this character

    `mastery` with no arguments prints what each track holds and names the ones that have
    been established. The rest are not guessed. To name another, play one mission using one
    weapon type, run `py sas4.py watch`, and the track whose MasteryXp moves is that type;
    `watch` already reports these paths.

    A consistent file is not a safe one, and this is the largest implausible jump these
    tools can make: level 5 is on the order of ninety thousand kills per track. The profile
    uploads to Ninja Kiwi every ten minutes and the server keeps its own copy, so setting a
    row of them is a plain diff on their side however well the file agrees with itself.
    """
    raw, d = load(args.file)
    stored, computed, ok = dgdata.verify(raw)
    if not ok and not args.force:
        print("this file does not verify (%s vs %s) -- refusing to edit it" % (stored, computed))
        return 1

    rows = mastery_rows(d, args.slot)
    if not rows:
        print("no mastery tracks in slot %d of %s" % (args.slot, args.file))
        return 1

    targets = {}
    if args.all is not None:
        targets = {index: args.all for index, _xp, _lvl, _fits in rows}
    elif args.set:
        for piece in args.set.split(","):
            piece = piece.strip()
            if not piece:
                continue
            index, _, level = piece.partition("=")
            try:
                targets[int(index)] = int(level)
            except ValueError:
                print("cannot read %r -- write it as <track>=<level>, e.g. 3=5" % piece)
                return 1

    if not targets:
        print("%s, character slot %d" % (args.file, args.slot))
        print("  track   XP        level   what it is")
        for index, xp, level, fits in rows:
            if fits is None:
                note = "   <- this is not a number"
            elif level != fits:
                note = "   <- level disagrees with the XP"
            else:
                note = ""
            # %s, not %d: a value odd enough to be worth a note has to survive being
            # printed, and %d on a string is the crash the note exists to describe.
            print("  %5d   %-9s %-7s %s%s"
                  % (index, xp, level, mastery_name(index) or "?", note))
        print("\n%d track(s). Levels reach %s XP." % (len(rows), MASTERY_LEVEL_XP[-1]))
        known = len(MASTERY_TRACKS)
        print("%d of them are named; the rest are not guessed. To name one, read its XP off"
              % known)
        print("the game's own mastery screen and find the track holding that number above.")
        return 0

    try:
        plan = mastery_plan(d, targets, args.slot)
    except ValueError as problem:
        print(problem)
        return 1

    print("%s, character slot %d" % (args.file, args.slot))
    for index, level in sorted(targets.items()):
        was = next((r for r in rows if r[0] == index), None)
        print("  track %-3d  xp %-9s -> %-9d   level %s -> %d"
              % (index, was[1] if was else "?", MASTERY_LEVEL_XP[level],
                 was[2] if was else "?", level))

    # Asking for the levels a character already has is a no-op, and `level` has always
    # stopped here rather than write one. Doing it anyway costs a backup folder, and the
    # cost is not the disk: `Restore backup...` lists them newest first, so a run of
    # backups that changed nothing pushes the one worth restoring down the list.
    if not pending(d, plan):
        print("\nnothing to change")
        return 0
    if args.dry_run:
        print("\n(dry run -- nothing written)")
        return 0
    if game_running() and not args.force:
        print("\n%s is running -- close it first" % GAME_PROCESS)
        return 1

    ok, saved, message = apply_edits(args.file, plan)
    print("\n%s" % message)
    if not ok:
        return 1
    print("backup   %s" % saved)
    problems = sas4_model.check(load(args.file)[1])
    if problems:
        # `level` reports the same failure as an exit code and this did not, so a script
        # that wrote a profile the checker rejects was told it had succeeded.
        print("check    the result is not consistent:")
        for line in problems:
            print("           - %s" % line)
        print("         the backup above is the way back.")
        return 1
    print("check    clean")
    print("\nThe server keeps its own copy of this profile. A row of maxed masteries is a"
          "\nplain diff on their side, however well the file agrees with itself.")
    return 0


# --- contributing a reading, for the people who want to ------------------------------------

# What a shared report may contain, and nothing else.
#
# The report is BUILT FROM NAMED FIELDS, never by walking the profile and removing what looks
# private. The difference decides what happens to a field the game adds in a later patch: a
# walk-and-filter emits it by default and only a rule stops it, so the failure is silent and
# lands on the person who trusted the tool. Building from names can only ever emit what is
# typed here -- a new field is invisible until someone adds it on purpose. This is the same
# reason make_dist.py packages a named file list rather than a glob.
#
# The one section that cannot work that way is the schema list, which exists precisely to
# report fields nobody has seen. It emits path NAMES AND TYPES ONLY, never a value. A path
# name cannot carry an account id.

CONTRIBUTE_VERSION = 1

# Refuse to write a report containing any of these. A backstop behind the allowlist, not the
# defence itself -- if one of these fires, the allowlist above already has a mistake in it.
#
# The thresholds are measured, not guessed. Against a real profile: 24-hex matches
# `Version/link` (24 lowercase hex) and the id inside `Version/analytics` (32); a 15-digit
# run appears nowhere at all, while a 9-digit rule -- the obvious first guess -- fires on
# fourteen legitimate timestamps and would make the command refuse on every real save. The
# path patterns matter because the save's own location carries the Steam account number in
# it, so the path is never printed even though it is the most natural thing to put in a
# report header.
ID_PATTERNS = [
    (r"[0-9a-f]{24,}", "a long hex string, which is the shape of an account id"),
    (r"[0-9]{15,}", "a very long number, which is the shape of an account id"),
    (r"[A-Za-z]:[\\/]", "a path on your machine"),
    (r"[Uu]serdata", "a Steam userdata path, which contains your account number"),
]


def scan_report_for_ids(text):
    """[(pattern description, what matched)] for anything in `text` shaped like an id."""
    found = []
    for pattern, description in ID_PATTERNS:
        for match in re.findall(pattern, text):
            found.append((description, match))
    return found


def _mastery_section(document, slot):
    lines = ["| track | XP | level | what it is |", "|---:|---:|---:|---|"]
    for index, xp, level, _fits in mastery_rows(document, slot):
        lines.append("| %d | %s | %s | %s |"
                     % (index, xp, level, mastery_name(index) or ""))
    return lines


def _item_section(document, slot, key):
    """Item and augment ids owned. Catalogue numbers, the same for everyone who owns one."""
    try:
        rows = at_path(document, "Inventory/Profile%d/%s" % (slot, key))
    except (KeyError, IndexError, TypeError):
        return ["(none in this save)"]
    if not isinstance(rows, list) or not rows:
        return ["(none in this save)"]
    out = ["| id | grade | augment 1 | augment 2 |", "|---:|---:|---:|---:|"]
    for row in rows:
        if not isinstance(row, dict):
            continue
        out.append("| %s | %s | %s | %s |"
                   % (row.get("ID"), row.get("Grade"),
                      row.get("Augment1ID"), row.get("Augment2ID")))
    return out


def _schema_section(document):
    """Every path in the file, with the types seen at it. Names and types, never values."""
    seen = {}

    def walk(node, path=""):
        if isinstance(node, dict):
            for key, value in node.items():
                walk(value, path + "/" + str(key))
        elif isinstance(node, list):
            seen.setdefault(path, set()).add("list")
            for value in node:
                walk(value, path + "/*")
        else:
            seen.setdefault(path, set()).add(type(node).__name__)

    walk(document)
    return ["%s  %s" % (path, "/".join(sorted(seen[path]))) for path in sorted(seen)]


def contribution_report(document, slot=0):
    """A report about one save that carries no identity, as Markdown.

    Every field is named here explicitly. Notably absent, and absent on purpose:
    `Version/link` and `Version/analytics` (both account identifiers), `Settings/Name` and
    `Inventory/ProfileN/Name` (the player's chosen names), the path the save was read from
    (it contains the Steam account number), and money -- which is nobody's business and
    answers no question the format still has open.
    """
    version = document.get("Version") or {}
    globals_ = document.get("Global") or {}
    skills = (document.get("Inventory", {}).get("Profile%d" % slot) or {}).get("Skills") or {}
    try:
        claimed = at_path(document, "Inventory/Profile%d/Strongboxes/Claimed" % slot)
    except (KeyError, IndexError, TypeError):
        claimed = []

    lines = [
        "## SAS4 save reading (report format %d)" % CONTRIBUTE_VERSION,
        "",
        "Produced by `py sas4.py contribute`. It holds no account id, no player name and no",
        "file path -- only numbers about a character and the shape of the file. Read it before",
        "you post it; that is what it is printed for.",
        "",
        "### The file",
        "",
        "| | |",
        "|---|---|",
        "| game version | %s |" % (version.get("LastGame"),),
        "| original version | %s |" % (version.get("OriginalVersion"),),
        "| profile format | %s |" % (version.get("Profile"),),
        "| character slot | %d |" % slot,
        "| character level | %s |" % skills.get("PlayerLevel"),
        "| highest rank | %s |" % globals_.get("HighestRank"),
        "| class | %s |" % skills.get("Class"),
        "",
        "### Masteries",
        "",
        "**This is the part that needs people.** Twenty-five of the twenty-seven tracks below",
        "have no name yet. If you know what one is -- play a mission with one weapon type and",
        "watch which row moves, or read a number off the game's own mastery screen and find it",
        "here -- say so in the issue and it gets named in the next release.",
        "",
    ]
    lines += _mastery_section(document, slot)
    lines += [
        "",
        "### Weapons owned",
        "",
    ]
    lines += _item_section(document, slot, "Weapons")
    lines += [
        "",
        "### Equipment owned",
        "",
    ]
    lines += _item_section(document, slot, "Equipment")

    runs = claimed_items(document, slot) if isinstance(claimed, list) else []
    lines += [
        "",
        "### Strongboxes/Claimed",
        "",
        "| | |",
        "|---|---|",
        "| raw length | %s |" % (len(claimed) if isinstance(claimed, list) else "not a list"),
        "| runs parsed | %d |" % len(runs),
        "| length accounted for | %d of %s |"
        % (len(runs) * CLAIMED_RUN, len(claimed) if isinstance(claimed, list) else "?"),
        "",
        "A mismatch between the last two rows is worth reporting on its own: it means the",
        "four-element run this tool reads is not what the game wrote into your file.",
        "",
        "### Every path in the file",
        "",
        "Names and types only -- no values. This is how fields nobody has documented get",
        "found, and it is the one section not built from a list of named fields, which is why",
        "it may not carry values.",
        "",
        "<details><summary>%d paths</summary>" % len(_schema_section(document)),
        "",
        "```",
    ]
    lines += _schema_section(document)
    lines += ["```", "", "</details>", ""]
    return "\n".join(lines)


def cmd_contribute(args):
    """Write a report about a save that can be shared, holding no identity.

        py sas4.py contribute              write it, and print it
        py sas4.py contribute --print      print it and write nothing

    Twenty-five of the twenty-seven mastery tracks have no name, the first level threshold
    is bounded but not pinned, and the format has fields nobody has explained. All three are
    answered by more readings than one person's account can produce.

    Nothing here sends anything anywhere. The report is written to a file and printed in
    full, and posting it is a thing you do yourself, having read it. A save editor that
    phoned home would deserve to be uninstalled, and no amount of anonymising would change
    that -- the objection is to the sending, not to the contents.

    What it carries is named field by field in `contribution_report`. What it refuses to
    carry is checked for a second time before the file is written, against the shapes an
    account id takes, so a mistake in that list fails loudly here rather than quietly on
    somebody's GitHub issue.
    """
    _raw, document = load(args.file)
    report = contribution_report(document, args.slot)

    leaked = scan_report_for_ids(report)
    if leaked:
        print("refusing to write this report -- it contains something shaped like an id:")
        for description, match in leaked[:10]:
            print("  %-56s %s" % (match, description))
        print("\nThis is a bug in the tool, not something you did. Please report it, with the")
        print("lines above and without the report itself.")
        return 1

    print(report)
    if args.print_only:
        print("\n(--print: nothing written)")
        return 0

    os.makedirs(DATA, exist_ok=True)
    out = os.path.join(DATA, "contribution.md")
    with open(out, "w", encoding="utf-8") as handle:
        handle.write(report)
    print("\nwritten to  %s" % out)
    print("\nEverything above is the whole of it. If you are happy to share it, open an issue")
    print("at https://github.com/BananaSpaGetti/sas4-save-editor/issues -- there is a")
    print("'Mastery track' template -- and paste it in. Nothing was sent.")
    return 0


def cmd_where(args):
    """Show the SAS4 profiles discovered on this machine."""
    steam = find_steam()
    print("Steam: %s" % (steam or "not found"))
    print("writes to: %s%s"
          % (DATA, "" if DATA == ROOT else "   (beside the tools is not writable)"))
    profiles = find_profiles()
    if not profiles:
        print("no SAS4 profile found. Pass --file <path> to any command to point at one.")
        return 1
    print("profiles (newest first):")
    for path in profiles:
        marker = "  <- default" if path == LIVE else ""
        print("  %s%s" % (path, marker))
    print("session: %s" % (SESSION or "n/a"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
