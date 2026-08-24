# sas4-save-editor

Tools for SAS: Zombie Assault 4. The save format is solved, so the working route is reading
and editing the profile on disk. `docs/FINDINGS.md` is the handover document -- what was
established, how, and what is closed, including why memory editing does not work on this
game.

## Requirements

- Python 3 (`py` on Windows), from <https://www.python.org/downloads/> -- tick **Add
  python.exe to PATH** on the installer's first screen. Nothing here needs a third-party
  package. The one optional
  extra is scikit-learn: `sas4_train.py` adds a comparison against its decision tree when it
  is installed, and says so and carries on when it is not.
- **Windows** for anything that touches the installed game. Finding the profile by itself
  reads the Steam install location from the registry, and the refusal to write while the
  game is running shells out to `tasklist`. Elsewhere, point `--file` at a save you copied
  over: decoding, editing, verifying and the model all work on any platform.
- **SAS4 installed** for that automatic discovery. Without it, pass `--file` every time.
- The window (`sas4_gui.py`) needs `tkinter`, which ships with the python.org build.

You do not need the game running -- the opposite, in fact: it must be closed before a write.

## Getting started

**Extract the zip first.** Windows will open a file previewed inside a `.zip`, but the rest
of the folder is not there and nothing will run. Extract the whole folder — Desktop or
Downloads is fine — and run it from the extracted copy. Any drive works.

**Then, in order**

```
sas4.bat                        double-click: all three panels, as tabs
```

That is the whole tool for most people. There is nothing else in the folder to run.

To open just one panel instead, the three launchers for those are in `tools\`:

```
tools\sas4-quick.bat             level, money, grant an item
tools\sas4-gear.bat              browse and grant weapons and equipment
tools\sas4-gui.bat               every value in the file
```

or, from a terminal, the command line — every `py sas4.py ...` line in this README is run
from `tools\`, so change into it once:

```
cd tools
py sas4.py where                did it find your profile?
py sas4.py view                 read it
py sas4.py set <path> <value>   change one value
```

`where` first: everything else defaults to the profile it finds, so if `where` finds
nothing, the rest needs `--file <path>`.

**If the window does not open**

Each `.bat` keeps its console open and prints what went wrong. The three common ones:

| What it says | What to do |
|---|---|
| `No working Python was found` | Install Python 3 and tick *Add python.exe to PATH* |
| `Cannot find tools\sas4_hub.py next to this file` | The zip was not extracted — extract it, then run from the extracted folder |
| `needs tkinter` | Re-run the Python installer, choose *Modify*, tick *tcl/tk and IDLE* |

A dialog saying **No SAS4 profile found** is not a failure: it means SAS4 is not installed
on that machine, or has never been saved on that account. Press OK to open a `.save` by hand,
or Cancel to quit.

## Layout

One file at the top is meant to be run, so there is nothing to choose between:

```
sas4.bat          <- double-click this
README.md
LICENSE
tools/            the code, and the launchers for a single panel
docs/             FINDINGS.md and the rest of the writing
```

Inside `tools/`:

```
sas4-quick.bat    just level, money, grant an item
sas4-gear.bat     just the armoury
sas4-gui.bat      just the full editor

sas4.py           the command line, and where the editing machinery lives
sas4_hub.py       the combined window: the tabs, and the save they share
sas4_quick.py     the quick panel
sas4_gear.py      the armoury panel
sas4_gui.py       the full editor panel
sas4_ui.py        what the panels share: parented dialogs, the start-up path
sas4_model.py     generate a consistent profile, and check one (a red/blue exercise)
sas4_train.py     train a from-scratch detector on generated data
dgdata.py         the DGDATA container -- decode, encode, checksum

tests/            `py tests/test_sas4.py` runs them, no dependencies
```

Three directories appear the first time something needs one: `backups/`, a timestamped copy
taken before every edit; `decoded/`, plaintext dumps and the cached item table; and `saves/`,
the versions `watch --archive` keeps. They sit beside `sas4.bat` when that folder can be
written to and somewhere per-user when it cannot -- see [Where it writes](#where-it-writes),
or run `py sas4.py where`, which prints the one in use. All three hold your own profile data,
so keep them out of anything you share.

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
py sas4.py where                                 which profile was found -- run this first
py sas4.py view                                  the live profile, by section
py sas4.py view --section skills --slot 1
py sas4.py list --grep money                     every path whose name matches
py sas4.py list --type bool --under Settings     only the on/off switches, one area
py sas4.py kinds                                 how many values of each type, and where
py sas4.py items                                 fetch the item table once, for names
py sas4.py items --catalog                       ...and write ITEMS.md, everything grantable
py sas4.py level 40                              level, XP, skill points and rank together
py sas4.py mastery                               what each mastery track holds
py sas4.py mastery --set 3=5,7=2                 raise tracks; --all 5 for every one
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

With no `--file`, the profile is **found automatically**: the tools read the Steam install
location from the registry and walk `userdata\<id>\678800\...\Docs\<account>\Profile.save`,
so they work on any machine with SAS4 installed, no path edited. That is what `where` prints,
and why it is worth running first -- if it finds nothing, every later command needs `--file`
pointing at a save.

### One window, or three

`sas4.bat` opens all three panels as tabs of a single window, which is the one to use:
wanting to grant a gun and then set a level was otherwise two windows and two file
pickers. One save is open at a time and every tab shares it, so opening a profile opens
it everywhere, and an edit made in one tab is showing in the others when you switch.

Tabs are built the first time you select them rather than up front -- each one loads the
profile and walks the item catalogue, and doing that three times before the window
appears is a wait for two panels you may not touch.

The individual `.bat` files in `tools\` still open one panel each, unchanged, if that is
all you want. They are down there rather than beside `sas4.bat` so that the folder you
extract has one thing in it to run.

### The three panels

**`tools\sas4-quick.bat`** is the Quick tab on its own. Three panels -- level, money, grant an item --
and a field at the top for which save is open, so switching profiles does not mean restarting.
The item picker is searchable and shows tier and category, which beats knowing an ID by heart.

**`tools\sas4-gear.bat`** is the armoury, for weapons and equipment specifically: the catalogue
on the left with filters for tier and type, what the character already owns on the right,
read back out of the save. Ctrl-click grants several at once -- Claimed is a single value in
the file, so five items is one write over one backup -- and anything granted can be removed
again, which matters because a granted item is not an undo away once the game has saved.

**`tools\sas4-gui.bat`** is the full editor: every value in the profile behind a filter box, staged
edits that touch nothing until Save, and Restore backup… to put an earlier copy back. Use it
to find out what is in a file, or to change something the quick window does not cover.

Both go through exactly the same code as `set` -- same refusal while the game is running,
same automatic backup, same byte-level replacement, same verify afterwards.

### Masteries

Masteries are the passive upgrades that come from use: one track per weapon type, one per
armour piece, and a few for grenades, turrets and HD ammo. A profile carries 27 tracks per
character, each `{MasteryXp, MasteryLvl}`, under `MasteryProgress/MasteryProfileN`.

```
py sas4.py mastery                  the tracks, and the level each one's XP supports
py sas4.py mastery --set 3=5        one track to level 5
py sas4.py mastery --set 3=5,7=2    several, in one write
py sas4.py mastery --all 5          every track on this character
py sas4.py mastery --slot 1         a different character
```

XP and level are written together. They are two fields describing one thing, and whichever
of them the game reads, a level its XP cannot reach is a profile that could not have been
played -- `check` objects to exactly that.

**Which track is which is only recorded where it has been established**, and is not guessed
anywhere else -- a wrong name would send an edit to the wrong track. So far:

| track | what it is | how that was settled |
|---|---|---|
| 0 | pistols | the mastery screen read 1272 while the save held 1272 there; reached by elimination first, then confirmed |
| 9 | high damage ammo | the game's own mastery screen read 1588 while the save held exactly 1588 there |

To name another, the quickest way is to read a number off the game's mastery screen and find
the track holding it in `py sas4.py mastery`. Failing that, play a mission using a single
weapon type and nothing else -- no grenades, no turrets, which have tracks of their own --
and run `py sas4.py watch`, which reports these paths; the track whose `MasteryXp` moves is
that type.

A weapon track and the HD ammo track were seen gaining identical amounts per mission, so
both are driven by kills at the same rate.

The whole list is replaced in one write rather than each field edited. A profile holds well
over a hundred identical `"MasteryXp":0` fragments, so a single track cannot be pinned down
by its bytes; the list as a whole can. Same move `give` makes with `Claimed`.

This is the largest implausible jump these tools can produce -- level 5 is on the order of
ninety thousand kills per track. See the warning under "Rules that came from getting it
wrong": the profile uploads to Ninja Kiwi every ten minutes and the server keeps its own
copy, so a row of maxed masteries is a plain diff on their side however well the file agrees
with itself.

### Levels

A level is not one number. `level 40` writes four fields that have to agree:

```
Skills/PlayerLevel            the level
Skills/PlayerTotalXp          the cumulative XP that level starts at
Skills/AvailableSkillPoints   one per level, less any already spent
Global/HighestRank            never below the level
```

Setting `PlayerLevel` alone leaves a character that could not have been played into
existence, which is exactly what a plausibility check looks for. Points are granted, not
spent -- distribute them in game; writing skill levels directly is what `docs/FINDINGS.md` warns
against.

The cumulative XP comes from a closed form rather than the tabulated 25 levels:
`1000 + 70.7·n² + 0.7·n³` XP to go from level *n* to *n*+1, which reproduces every tabulated
value exactly. That is what makes levels up to the cap of 100 real numbers instead of a
straight-line guess -- though it is still derived from 24 observed points, so if the game
bends the curve above 25 this follows the points, not the game.

### Granting items

`py sas4.py items --catalog` writes `ITEMS.md`: all 437 items grouped by domain, tier
(normal, red, black, factions) and category, with the ID `give` wants. It is generated
rather than shipped, because the table is someone else's data and changes when the game does.

Item IDs print as names once `items` has been run. Weapons and equipment number themselves
separately -- equipment 101 is not weapon 101 -- and the lookup is scoped accordingly.

In Git Bash, write paths without a leading slash. MSYS rewrites a leading-slash argument
into a Windows path before Python sees it, and the resulting error mentions `C:` for no
visible reason.

## Tests

```
py tests/test_sas4.py      run them
py tests/test_sas4.py -v   and say what each one checked
```

Standard library only, so they run wherever the tools do. Every save they touch is generated
by `sas4_model.py` in a temporary directory -- they never read a real profile and never write
outside that directory.

They cover the three things that can destroy a save while still looking like they worked: the
checksum, `anchor_for` resolving to the wrong place (which leaves a *valid* file with the
wrong value in it), and the run-length parsing of `Strongboxes/Claimed`. A golden vector pins
the format constants, because encode and decode share them and a roundtrip cannot tell you
they changed.

## Where it writes

Backups, decoded dumps and the item cache go beside `sas4.bat` -- the folder you extracted,
not the `tools/` folder the code sits in -- when that directory is writable, which is what someone who extracted a zip to their Desktop expects. Extracted under
`Program Files`, or opened from a read-only share, it falls back to the per-user location --
`%LOCALAPPDATA%\SAS4Trainer` on Windows. `py sas4.py where` prints which one is in use.

This matters because the first thing every write does is take a backup. If that cannot be
written, nothing is written at all, and you are told why.

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
  `docs/FINDINGS.md`.

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
change, and the runtime route is closed. `docs/FINDINGS.md` explains why.
