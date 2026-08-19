# SAS4Trainer

Tools for SAS: Zombie Assault 4. The save format is solved, so the working route is reading
and editing the profile on disk. The memory-scanning work that came first is a documented
dead end and lives in `memscan/`.

`FINDINGS.md` is the handover document: what was established, how, and what is closed.

## Layout

```
dgdata.py     the DGDATA container -- decode, encode, checksum
sas4.py       the command line: view, list, get, set, verify, decode, encode, watch
sas4_gui.py   a window over the same machinery; sas4-gui.bat starts it with a double click
FINDINGS.md   what has been established about the game
memscan/      the memory scanner and its supporting tools, plus their own README
saves/        sample profiles                       (not tracked)
backup-*/     automatic backups taken before edits  (not tracked)
decoded/      plaintext dumps                       (not tracked)
```

Nothing under `saves/`, `backup-*/` or `decoded/` is committed: they hold the player's
profile, which carries account data.

## The format

```
file      = "DGDATA" + 8 hex characters + obfuscated body
body[i]   = plain[i] + 21 + (i % 6)        (mod 256)
plain     = UTF-8 JSON
header    = "DGDATA%08x" % checksum(plain)
```

The checksum resembles CRC-32 (polynomial `0xEDB88320`, init 0, no final xor) but is not
CRC-32: its table is built with an arithmetic int32 shift, the way ActionScript's `>>`
behaves. `dgdata.py` has the detail and the credit.

The same container is used for Ninja Kiwi's API payloads, not only saves -- the settings the
game caches under `Cache\com.ninjakiwi.link\nkapi\skusettings\` decode with the same tool.

## Using it

```
py sas4.py view                                  the live profile, by section
py sas4.py view --section skills --slot 1
py sas4.py list --grep money                     every path whose name matches
py sas4.py list --type bool --under Settings     only the on/off switches, one area
py sas4.py kinds                                 how many values of each type, and where
py sas4.py items                                 fetch the item table once, for names
py sas4.py give 129 [--kind weapon]              grant a finished item, no box needed
py sas4.py get Inventory/Profile0/Money          one value
py sas4.py set Inventory/Profile0/Money 250000   change it, checksum and all
py sas4.py set ... --dry-run                     show the change without writing
py sas4.py verify                                would the game accept this file
py sas4.py decode [out.json]                     plaintext JSON out
py sas4.py encode <in.json> <out.save>           and back again
py sas4.py watch [--archive]                     diff the save each time the game writes it
py sas4.py session                               read the login session (secrets hidden)
py sas4.py graft other.save --fields A,B         copy progress in, keeping your identity
```

`--file` points any of them at a save other than the live one. With no `--file`, the profile
is **found automatically**: the tools read the Steam install location from the registry and
walk `userdata\<any id>78800\...\Docs\<account>\Profile.save`, so they work on any
machine with SAS4 installed, no path edited. `py sas4.py where` shows what was found.

For a window instead, run `sas4-gui.bat` (or `py sas4_gui.py`). It lists every value in the
profile with a filter box, stages edits without touching the disk, and applies them all on
Save through exactly the same code as `set` -- same refusal while the game is running, same
automatic backup, same byte-level replacement, same verify afterwards. Restore backup… puts
an earlier copy back.

Item IDs print as names once `items` has been run. Weapons and equipment number themselves
separately -- equipment 101 is not weapon 101 -- and the lookup is scoped accordingly.

In Git Bash, write paths without a leading slash. MSYS rewrites a leading-slash argument
into a Windows path before Python sees it, and the resulting error mentions `C:` for no
visible reason.

## Rules that came from getting it wrong

- **Back up before writing.** `set` does it automatically; a hand edit that skipped it once
  made the game discard the character, and only a byte-identical restore brought it back.
- **Close the game first.** It rewrites the save on its own schedule -- observed every three
  to nine minutes -- and will overwrite an edit made underneath it. `set` refuses to run
  while the game is up.
- **Edit bytes, not the document.** The game's JSON writer does not format the way Python's
  does, so re-serialising rewrites the whole file. `set` replaces the bytes of the one
  value and leaves everything else identical.
- **A valid checksum is not safety.** It makes the *client* accept a file. The profile is
  uploaded to Ninja Kiwi roughly every ten minutes and the server keeps its own copy, so a
  local edit is visible to them as a plain diff. See "How the game detects tampering" in
  `FINDINGS.md`.

## The model

`sas4_model.py` is a stand-in for a profile, for building and testing tools without a real
save. `generate out.save --level N --money M` writes a consistent minimal profile that
verifies; `check <save> [--strict]` lists inconsistencies -- XP that does not match the
level, skill points that do not add up, a malformed inventory. These are the easy ones a
server-side plausibility pass would also catch, so passing them is necessary, not
sufficient.

`check` is a registry of named rules. `redteam` reports how much of a tampering catalog they
catch; `redteam --progression` shows coverage climbing as rules are added, and plateauing
at the consistent edits no single-file rule can reach. `dataset out.jsonl` writes labelled
feature vectors for training a detector with ML -- the blue side of the exercise.
`sas4_train.py` trains a from-scratch decision tree on it and evaluates it against the rule
baseline; on honest data it ties the baseline and cannot beat it, because a local detector
shares the rules' ceiling -- the consistent half of the tampering has no tell a single file
carries.

## Identity and grafting

The account id lives in three places that must agree: the profile's `Version.link`, the
session's `user.nkapiID`, and the save folder name. `session` reads current.session (the
sessionID is a login token, shown only as a length). `graft <their.save> --fields <paths>`
copies progress from another save while refusing the identity fields, so it stays your
account with their progress.

## What is not in the save

Health is not a stored value -- it comes from class, skill levels and armour. Neither are
rates: XP and strongboxes per completed round are computed at mission end, and the profile
holds only totals. Nothing here can make damage stop applying; that would be a runtime
change, and the runtime route is closed. `FINDINGS.md` explains why.
