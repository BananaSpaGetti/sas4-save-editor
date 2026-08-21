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
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("dgdata", os.path.join(HERE, "dgdata.py"))
dgdata = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(dgdata)

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
IDENTITY_FIELDS = ("link",)                      # in the profile JSON
# In current.session: sessionID is the real secret -- a login token the server issues, so
# it is never printed in full. nkapiID is the account id, which is also the save folder
# name and appears in every path already, so it is shown; but editing either is gated,
# because both are what ties a file to an account.
SECRET_KEYS = ("sessionID",)
CREDENTIAL_KEYS = ("sessionID", "nkapiID")


# --- reading -----------------------------------------------------------------------------

def load(path):
    raw = open(path, "rb").read()
    return raw, json.loads(dgdata.decode(raw))


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


BACKUPS = os.path.join(HERE, "backups")


def backup(path):
    directory = os.path.join(BACKUPS, "backup-" + time.strftime("%Y%m%d-%H%M%S"))
    os.makedirs(directory, exist_ok=True)
    target = os.path.join(directory, os.path.basename(path))
    shutil.copyfile(path, target)
    return target


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


def weapon_entry(item_id, grade, bonus):
    """A finished gun for Strongboxes.Claimed. A bare 0 tags it, then the dict, then 8, 0."""
    return [0, {"ID": item_id, "EquipVersion": 0, "Grade": grade, "AugmentSlots": 0,
                "BonusStatsLevel": bonus, "InventoryIndex": 0}, 8, 0]


def equipment_entry(item_id, slot, grade, bonus):
    """A finished piece of equipment. A bare 1 tags it, then the dict."""
    return [1, {"ID": item_id, "EquipVersion": 0, "Grade": grade, "AugmentSlots": 0,
                "BonusStatsLevel": bonus, "EquippedSlot": slot, "InventoryIndex": slot}]


def cmd_give(args):
    """Grant a finished item straight into Strongboxes.Claimed -- the loot without the box.

    No published tool writes an unopened, openable strongbox, and the Unopened schema is not
    documented anywhere; the boxes drop from bosses and enemies, not from anything a save
    encodes. What every tool does instead, and what this does, is put the finished item
    directly into Claimed, which is the inventory of what you already own. That is more
    reliable than spawning a box, since a box is random and this is not.

    py sas4.py give 129                 grant weapon 129 (Z-5 Heavy)
    py sas4.py give 101 --slot 2        grant equipment 101 into slot 2
    py sas4.py give 129 --grade 12 --bonus 10
    """
    domains = item_names()
    weapon_ids = domains.get("weapon", {})
    equip_ids = domains.get("equipment", {})
    if not domains:
        print("run `py sas4.py items` first so IDs can be resolved to weapon vs equipment")
        return 1

    in_weapon = args.item in weapon_ids
    in_equip = args.item in equip_ids
    # Weapon and equipment ids overlap -- 101 is both a gun and a vest -- so when an id is in
    # both tables the caller has to say which with --kind.
    kind = args.kind
    if kind == "auto":
        if in_weapon and in_equip:
            print("id %d is both a weapon (%s) and equipment (%s)."
                  % (args.item, weapon_ids[args.item][0], equip_ids[args.item][0]))
            print("say which with --kind weapon  or  --kind equipment")
            return 1
        kind = "weapon" if in_weapon else "equipment" if in_equip else None

    if kind == "weapon" and in_weapon:
        entry = weapon_entry(args.item, args.grade, args.bonus)
        label = "%s (weapon)" % weapon_ids[args.item][0]
    elif kind == "equipment" and in_equip:
        entry = equipment_entry(args.item, args.slot, args.grade, args.bonus)
        label = "%s (equipment, slot %d)" % (equip_ids[args.item][0], args.slot)
    else:
        print("id %d is not a known %s id" % (args.item, kind or "weapon or equipment"))
        print("list them with:  py sas4.py items")
        return 1

    raw, d = load(args.file)
    stored, computed, ok = dgdata.verify(raw)
    if not ok and not args.force:
        print("this file does not verify (%s vs %s) -- refusing to edit it" % (stored, computed))
        return 1
    profile = d.get("Inventory", {}).get("Profile%d" % args.slotprofile)
    if not profile:
        print("no Profile%d in this save" % args.slotprofile)
        return 1

    path = "Inventory/Profile%d/Strongboxes/Claimed" % args.slotprofile
    old_claimed = at_path(d, path)
    new_claimed = old_claimed + entry

    print("grant %s  (id %d, grade %d, bonus %d)" % (label, args.item, args.grade, args.bonus))
    print("  Claimed: %d entries -> %d" % (len(old_claimed), len(new_claimed)))
    if args.dry_run:
        print("  (dry run, nothing written)")
        return 0
    if game_running() and not args.force:
        print("  %s is running -- close it first" % GAME_PROCESS)
        return 1

    plain = dgdata.decode(raw)
    anchor, old_length = anchor_for(d, plain, path)
    replacement = anchor[:-old_length] + json.dumps(new_claimed, separators=(",", ":"),
                                                    ensure_ascii=False).encode("utf-8")
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
    print("  done, checksum now %s" % dgdata.verify(open(args.file, "rb").read())[0])
    return 0


ITEMS_URL = "https://raw.githubusercontent.com/0daxelagnia/SAS4Tool/main/items.json"
ITEMS_CACHE = os.path.join(HERE, "decoded", "items.json")


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
    for domain, entries in sorted(table.items(), key=lambda kv: -len(kv[1])):
        categories = {}
        for _name, category in entries.values():
            categories[category] = categories.get(category, 0) + 1
        print("  %-12s %3d  %s" % (domain, len(entries),
                                   ", ".join("%s %d" % (c, n) for c, n in
                                             sorted(categories.items(), key=lambda kv: -kv[1]))))
    return 0


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
    raw = open(args.file, "rb").read()
    stored, computed, ok = dgdata.verify(raw)
    print("stored   %s\ncomputed %s\n%s" % (stored, computed,
          "VALID" if ok else "MISMATCH - the game would reject this file"))
    return 0 if ok else 1


def cmd_decode(args):
    raw = open(args.file, "rb").read()
    out = args.out or os.path.join(HERE, "decoded", "profile.json")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    document = json.loads(dgdata.decode(raw))
    with open(out, "w", encoding="utf-8") as handle:
        json.dump(document, handle, indent=2, ensure_ascii=False)
    print("wrote %s (%d bytes)" % (out, os.path.getsize(out)))
    return 0


def cmd_encode(args):
    plain = open(args.json, "rb").read()
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
    store = os.path.join(HERE, "saves")
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


def main():
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

    p = sub.add_parser("items"); p.set_defaults(run=cmd_items)

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
    return args.run(args)


def cmd_where(args):
    """Show the SAS4 profiles discovered on this machine."""
    steam = find_steam()
    print("Steam: %s" % (steam or "not found"))
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
