# SAS: Zombie Assault 4 — what has been established

Handover notes for whoever picks this up next. Everything here was measured against the
running game or its files; where something is inference rather than measurement, it says so.

**Goal:** unlimited cash, health and grenades, single-player/offline only. Not reached.
Two of the three obvious routes are closed with evidence, and the third is blocked on one
specific unknown.

---

## The game

`C:\Program Files (x86)\Steam\steamapps\common\SAS Zombie Assault 4\` — Steam appid 678800.

- `SAS4-Win.exe` is a **packed native binary**. Neither ASCII nor UTF-16 strings survive
  extraction from the file on disk. There is no Mono, no managed DLL, nothing to hook the
  way a Unity game can be hooked, so the BepInEx approach used for Hold Your King in
  `../HykFreedom/` does not apply here.
- `Assets/*.jet` and `AssetBundles/*.jet` are **password-encrypted ZIP archives**.
  `Assets/GameData/config.csv` — 200 KB of balance data — is inside one and cannot be read.
- Progression belongs to a Ninja Kiwi account. Under
  `%STEAM%\userdata\<user>\678800\local\Data\` there is `Docs\Profile.save` (local),
  `Cache\com.ninjakiwi.link\<accountId>\Profile.save` (server copy) and a live
  `Docs\com.ninjakiwi.link\Live\current.session`.

---

## Closed route 1: synthetic input

**The game ignores injected input entirely.** Verified with the window confirmed in the
foreground and the HUD captured before and after every attempt:

| Sent | Result |
| --- | --- |
| `SendInput` keyboard, virtual-key + scancode, scancode only, virtual-key only | ignored |
| `PostMessage` `WM_KEYDOWN` / `WM_KEYUP` to the game window | ignored |
| `SendInput` mouse click inside the play area | ignored |

Grenades stayed at 10 across every attempt and cash stayed at 4010. Ammo did drop early in
testing, which looked like success — it was the game firing on its own, and it kept
happening when nothing was being sent.

**Consequence:** a person has to drive the game. Scanning, narrowing, probing, reading the
HUD from a screenshot and freezing all still work unattended; only the trigger needs hands.

---

## Closed route 2: memory editing

**No address holds a live value for long, because values are boxed on a garbage-collected
heap and re-allocated whenever they change.**

The measurement that settles it:

```
armour 1958  ->  22 addresses held it
armour 2391  ->  23 addresses held it
overlap      ->  0
```

While a value sits still its addresses are stable — the same 22 across six seconds — so an
exact scan looks promising right up until the first narrowing pass, which collapses to
zero every time. The old addresses keep the stale number until they are collected.

This explains every dead end seen: cash, armour and grenades all produce dozens of exact
matches that never update, and `dec` / `same` narrowing always ends at 0.

One address, `0x14D00F20`, did track cash exactly. It is a **display mirror**: any write
to it is undone within a second, and freezing it does not move the HUD. `0x14E3B86C` holds
the ammo counter as ASCII text (`/722`, `/802`). Neither is the money.

Also ruled out for cash: exact scans as int32, float32 and cents, and a full
unknown-value scan over 175 MB / 46M int32 slots covering all writable committed memory
including `MEM_IMAGE` and `MEM_MAPPED`. A float64 would have survived the monotonic
narrowing and did not.

**Consequence:** getting a live value needs a pointer scan — a static base plus an offset
chain to the current box — which this toolset does not do. Cheat Engine does.

---

## Solved route: the save file

The format is fully worked out and `dgdata.py` reads, edits and writes it.

### Format

    file      = "DGDATA" + 8 hex characters + obfuscated body
    body[i]   = plain[i] + 21 + (i % 6)        (mod 256)
    plain     = UTF-8 JSON
    header    = "DGDATA%08x" % checksum(plain)

The body is JSON. A decoded profile begins:

```
{"Version":{"LastGame":[2,2,2],"OriginalVersion":[2,2,0],"Profile":[1,0,0],
 "SaveTime":1787138477,"link":"<account-id>...
```

> An earlier reading of the body as **little-endian base 97** with 55 for digit zero was
> **wrong**. It fit one two-byte armour field by coincidence and nothing else. Anything in
> this document that depends on it -- the "known field at offset 94" in particular -- is
> void.

### The checksum

It looks like CRC-32 with polynomial `0xEDB88320`, init 0 and no final xor, and it is
**not** CRC-32. The table is generated with an *arithmetic* int32 shift, the way
ActionScript's `>>` behaves, so once a value goes negative the sign bit propagates instead
of a zero:

```
table[1]  =  09073096      real CRC-32 has 77073096
```

Two things had to be right at once, and every earlier attempt had at most one of them: the
table, and hashing the **plaintext** rather than the obfuscated body.

```python
acc = 0
for byte in plain:
    acc = ((acc >> 8) & 0xFFFFFF) ^ TABLE[(acc ^ byte) & 0xFF]
```

### Verification

- All six genuine samples in `saves/` reproduce their stored checksum.
- All six round-trip **byte-identical** through `decode` then `encode`.
- The live profile the game had just written verifies (`e3f74ef9`).
- `profile-001`, the hand edit the game rejected, is correctly reported invalid.

### Where it came from

`dgdata.js` in **hemisemidemipresent/NKsku**, by way of the Python port in
**SWFplayer/SAS4Tool**. One web search, after hours of failing to derive it from samples.
The published port needed one correction before it reproduced a single real checksum -- its
shift semantics -- so verify found code against real samples rather than trusting it.

**Search for prior art before building a search harness.** The sections below record what
guessing cost.

### What was ruled out before the answer was found

Superseded, kept because it says what this class of search can and cannot do. Nine real
pairs against CRC-32 in nine polynomial variants, Adler-32, MD5/SHA prefixes, FNV-1a, djb2,
sdbm, the Java string hash, xor32 and MurmurHash3 seeds 0-3, over the raw body, the decoded
digits and UTF-16 expansions of both. Then, on 2026-08-19, `checksum_probe.py` over 352
(transform, algorithm) pairs and `checksum_window.py` over every contiguous prefix and every
suffix starting within the first 8192 bytes. None of it could have hit: the real function is
not a standard algorithm, and the real input was never one of the transforms tried.

The affine argument recorded earlier -- that a CRC's checksum XOR must match its raw CRC XOR
for equal-length payloads -- was sound, and correctly excluded the CRC family. The function
is not a CRC.


## The sample set in `saves/`, and a correction

**`saves/profile-001.save` is not a save the game wrote.** It is `profile-000` with payload
bytes 94 and 95 hand-edited and the header left untouched -- the edit the game rejected.
`profile-002`, `-003` and `-004` are the new character it started afterwards, and
`profile-005` is the byte-identical restore of the backup, so it carries the same data as
`profile-000`.

| file | checksum | payload | what it is |
| --- | --- | --- | --- |
| profile-000 | `0bca7ec1` | 122953 | genuine |
| profile-001 | `0bca7ec1` | 122953 | **hand-edited, rejected by the game** |
| profile-002 | `1a03afb7` | 119254 | genuine, new character |
| profile-003 | `f3d13daa` | 119266 | genuine, new character |
| profile-004 | `05e4e39b` | 120703 | genuine, new character |
| profile-005 | `0bca7ec1` | 122953 | genuine, byte-identical to 000 |
| profile-006 | `1e612109` | 122953 | genuine |

This matters because treating `-001` as genuine produces a false result immediately: it
differs from `-000` only at bytes 94-95 while carrying the same checksum, which reads as
"the checksum is blind to those bytes". It is not -- that pair is our own edit next to the
value it failed to update, and the game refusing it is the evidence that those bytes *are*
covered.

`-001` is still worth keeping as a negative sample: the game saw that payload carrying
`0bca7ec1` and rejected it, so the real function does not map it to that value.

## Ruled out offline on 2026-08-19

Two searches, both against the five genuine `(payload, checksum)` pairs, neither needing
the game to be running.

**`checksum_probe.py` -- 352 (transform, algorithm) pairs.** Thirty-two algorithms (twelve
CRC-32 polynomial/init/xorout variants, Adler-32, FNV-1/FNV-1a, djb2 in both forms, sdbm,
Java 31, Jenkins one-at-a-time, ELF, byte sum, index-weighted sum, xor32, add32,
rotate-add, Fletcher-32, MurmurHash3 seeds 0-3) across eleven framings of the input
(payload, `payload[96:]`, `payload[:94]`, whole file, file after header, header+payload,
decoded digits, digits from 96, reversed payload, UTF-16 and UTF-8 expansions). A candidate
had to reproduce the stored checksum on **two or more** genuine saves to count. None
reproduced it on even one.

**`checksum_window.py` -- every contiguous range, for the two fast algorithms.** CRC-32 and
Adler-32 over every prefix of every length, and every suffix starting within the first 8192
bytes, of both the payload and the whole file, for all seven samples, checked against all
seven stored checksums. No hit. The sweep was validated first by planting four windows
whose values were known and confirming it found all four, so the negative is trustworthy.

Both are narrower than they look. The affine argument in "What the checksum is not" above
already excluded the whole CRC family by proof, and the nine-pair test already covered most
of these algorithms; the genuinely new part is the *range* sweep, which shows the failure is
not simply that the wrong span of bytes was being hashed. Note also that these two scripts
work from the five samples in `saves/` only -- the other genuine pairs recorded above exist
as diffs and checksums in this document, not as payload files, so they cannot be fed to a
hash.

What remains untested is the same list as before: a checksum over the *decoded* structure
rather than the payload bytes, a salt such as the account id, or a non-standard routine.

---

## The format string, found 2026-08-19 23:37

Scanning the running game for the literal `DGDATA` (via `find_algo.py` through the daemon)
returns four addresses. Two are heap, two are module data, and one of the module ones is
the thing worth having:

```
0x26700F0   "DGDATA%08x\0\0Failure\0"
```

That is the **format string the header is written with**. It settles two questions that
were open: the eight characters are one 32-bit value, not two fields or a truncated digest,
and they are produced by an ordinary `%08x` on whatever the routine computes.

The two heap hits held a live save buffer:

```
0x10908038   "DGDATA" + "eb2ff478" + payload
0x1090F070   the same buffer
```

`eb2ff478` matched no file on disk, so this was the copy the game was holding rather than a
mirror of a saved one. **Those heap addresses are not stable and are no use as breakpoint
targets**: eight minutes later `0x1090F070` still held the stale `eb2ff478` while
`0x10908038` had been reused for something else entirely, even though the profile had been
rewritten on disk in between. Only the module address `0x26700F0` is worth arming.

### Why this opens the debugger route

Formatting the header means *reading* that string. A hardware breakpoint on read at
`0x26700F0` therefore fires exactly when the header is built, which is rare enough to be
safe - unlike breaking on a hot data address. When it fires, `RIP` is inside the writer and
the value about to be formatted is still in a register; for `sprintf(buf, fmt, value)` under
the Windows x64 convention that is `R8`.

    py hwbp.py --address 0x26700F0 --on readwrite --size 1 --hits 5 --seconds 60

Two things this needs that the assistant cannot supply: the run must be **elevated**, like
the daemon, and **a person has to make the game save** while the breakpoint is armed, since
SAS4 ignores synthetic input. The address is only valid for the current process; re-run the
`DGDATA` search after any restart.

**Tried once, inconclusive.** The breakpoint armed cleanly on 15 threads, ran its 60 seconds
and detached with the game still healthy - so the new cap does what it should - but caught
nothing, because no save happened inside the window. The profile had been rewritten at
23:42:09 when the game logged in, minutes before the breakpoint went up. The target is
untested, not disproved. Getting a result means coordinating the arming with the save; the
cheapest trigger seen so far is claiming the daily reward on the character-select screen.

Note also what a data breakpoint here can and cannot give. The caller loads the format
string with a `LEA`, which does not read memory and will not fire; what fires is the CRT
format parser walking the string. So `RIP` will land in the CRT, and the game's own routine
has to be recovered from the return address on the stack rather than read straight off the
register dump.

### Runtime needles

`find_algo.py` now merges extra patterns from `control/needles.json` when that file exists,
so searching for a specific string needs no code edit - the daemon re-imports the module on
every `find_algo` call. Entries are `"label": "ascii:..."`, `"utf16:..."` or `"hex:..."`.

---

## Recommended next step

The save file is solved, so unlimited cash offline is now editing JSON and re-encoding:

    py dgdata.py decode <Profile.save> out.json
    (edit out.json)
    py dgdata.py encode out.json <Profile.save>

Rules that still apply, both learned the hard way:

- **Back up first.** A bad edit made the game discard the character and start a new one, and
  only a byte-identical restore brought it back.
- **Close the game before writing.** It rewrites the save on its own schedule -- observed at
  roughly three to nine minute intervals -- and will overwrite an edit made underneath it.
- `verify` before and `roundtrip` after; both are one command and both are cheap.

The debugger work below is no longer needed for the checksum. `0x26700F0`, the `DGDATA%08x`
format string, is still a valid breakpoint target if some other question needs the save
routine itself.


## Editing a profile: what the fields mean

`profile_view.py` prints a decoded profile by section, and `--list` dumps every path that
holds a scalar, which is how an unknown field gets found.

What is and is not in the save:

- **Health is not a field.** It is derived from `Skills.Class`, the levels in
  `Skills.SkillsArray` (`holdtheline` is the health skill) and the armour in `Equipment`.
  Nothing in the save makes damage stop applying -- that would be a runtime change, and
  runtime editing is closed (see "Closed route 2").
- **Rates are not in the save either.** XP and strongboxes per completed round are computed
  at mission end; the profile stores only totals. The multipliers that exist
  (`ad_cash_multiplier`, `onslaught_*`, `vip_health_multiplier`) are in the settings the
  server pushes, not in the profile.
- What the save does hold: `Skills.PlayerLevel`, `Skills.PlayerTotalXp`,
  `Skills.AvailableSkillPoints`, `Global.HighestRank`, `Strongboxes.Unopened` /
  `.Claimed`, and the various ticket counters.

### What is in the file, by shape

`py sas4.py kinds` counts the document; on a real profile it is 4,597 ints, 611 booleans and
102 strings. `py sas4.py list --type bool --under Settings` narrows to one area. The
booleans are where the on/off state lives:

```
Settings          12   ScreenShakeEnabled, MusicOn, SFXOn, VSync, HudControls,
                       SkipIntroScenes, PreventAutoSignIn, DeadZone, TriggerFire, ...
Inventory         20   Loaded (per slot), ForceGiveWeapon, FirstPlayFlow,
                       FreeSkillsReset, UnlockedDailyRewards, HasReceivedCryo,
                       PerkKillingMachine, PerkHighRoller, PerkTank,
                       Equipment[n].Equipped / .Seen
Global             3   HasPlayedGame, ForceGiveArmour, ForceRemoveAds
CollectionArrayWeapon  256   one flag per weapon in the game
CollectionArrayArmour  201   one flag per armour piece
CollectionRewards       42
PurchasedIAP            33
```

`CollectionArrayWeapon` and `CollectionArrayArmour` being flat boolean arrays of 256 and 201
gives the size of the item tables the game ships with.

### Field reference from the other editors

Two published editors already document what is worth changing, and their field list matches
what is in this save. From `SWFplayer/SAS4Tool` (`_PROFILE_OPTIONS_.py`):

| what | path |
| --- | --- |
| name | `Inventory/ProfileN/Name` |
| cash | `Inventory/ProfileN/Money` |
| level, XP | `Inventory/ProfileN/Skills/PlayerLevel`, `.../PlayerTotalXp` |
| skill respec | `Inventory/ProfileN/FreeSkillsReset` |
| black keys, augment cores | `.../Skills/AvailableBlackKeys`, `.../AvailableEliteAugmentCores` |
| black strongboxes | `.../Skills/AvailableBlackStrongboxes` |
| **grenades** | `Inventory/ProfileN/Ammo[<grenade id>]` |
| **turrets** | `Inventory/ProfileN/Turrets` — append `{"TurretId": id, "TurretCount": n}` |
| weapons and armour | appended to `Inventory/ProfileN/Strongboxes/Claimed` |
| multiplayer stats | `Inventory/ProfileN/StatsData[i].val` |

**`Strongboxes.Claimed` is not a list of objects.** It is a flattened list in which bare
integers tag what follows, and **every run is four elements whatever the kind**:

```
0, {ID, EquipVersion, Grade, AugmentSlots, BonusStatsLevel, InventoryIndex},              int, int
1, {ID, EquipVersion, Grade, AugmentSlots, BonusStatsLevel, EquippedSlot, InventoryIndex}, int, int
```

So `0` introduces a weapon and `1` introduces equipment, and in both cases two more
integers close the run.

Measured, after an earlier reading of this file said equipment was two elements. That came
from reading one editor's write path and was never checked against a stream the game had
written. 36 saves captured with `watch` while boxes were opened divide exactly into runs of
four, 76 runs in all, and both published editors append four for either kind
(`0daxelagnia/SAS4Tool`, `_profile_.py` and `_global_.py`).

The trailing pair is **not validated**. Three constants are in the wild and all of them
work: `8, 0` (used here), `8, 2` (`_profile_.py`), and whatever the game itself writes,
which varies with the box -- `2, 2`, `5, 0`, `2, 0` and `2, 1` all observed. The length is
what matters, because that is what makes the stream parseable: a trailing `0` or `1` reads
as the start of the next run to a parser that steps by anything else.

`0daxelagnia/SAS4Tool` ships **`items.json`**, roughly 300 entries mapping item IDs to
names, grouped by rarity tier and by weapon type or armour slot -- pistols, smg, and so on;
helmet, vest, gloves, pants, boots; plus turrets and premium items. That is what turns
`Equipment ID 101` in this profile into something readable. It is worth fetching rather than
rebuilding.

Known editors, all Python, all working from the same decode:
`SWFplayer/SAS4Tool`, `0daxelagnia/SAS4Tool`, `dstvx/SAS4Tool`, and `getshrekt10/SAS4`.

### The level table

Cumulative XP to reach a level, from the community wiki, cross-checked against a real save:
level 3 requires 2,359 and the profile held 2,872, which lands correctly between level 3
and level 4 (4,014).

```
 1        0     6    9,045    11   39,335    16  112,745
 2    1,071     7   12,741    12   49,821    17  134,711
 3    2,359     8   17,445    13   62,211    18  159,582
 4    4,014     9   23,328    14   76,697    19  187,571
 5    6,190    10   30,565    15   93,475    20  218,895
```

Each level-up grants **one skill point and one strongbox**, up to Neodymium tier. So a
level raised by editing leaves the skill points to be granted consistently -- set
`AvailableSkillPoints` to match the number of levels added, and spend them in game rather
than writing skill levels directly.

Source: https://saszombieassault.wiki.gg/wiki/Levels_(SAS4)

### Risk, which is not the same as difficulty

Cash was one number. Skill values and HP limits are two of the three things the community
and Ninja Kiwi staff name as what the auto-banner checks -- the third being weapon upgrade
types. Raising a level and letting the game grant and spend the points keeps the derived
values self-consistent; writing skill levels or HP directly does not. Neither changes the
fact that the server holds its own copy; see the section below.

## How the game detects tampering

Measured from the game's own cached server settings, not guessed. There is **no client-side
anti-cheat binary** -- the install directory has no EasyAntiCheat, BattlEye or similar, and
the `EasyAntiCheat` folders under `Program Files (x86)` belong to other games. Enforcement
is server-side and account-based.

`Cache\com.ninjakiwi.link\nkapi\skusettings\*.json` are **DGDATA files too** -- the same
container as the save, and `dgdata.py` decodes and verifies them. Ninja Kiwi uses the format
for API payloads generally, not just profiles. Decoded, they carry the settings the server
pushes to the client:

```
profile_config:
    upload_interval          600      profile uploaded every 10 minutes
    force_upload_interval    60       and a forced upload path at 1 minute
    always_read_hacker_flag  true     the client always reads the account's hacker flag
    skip_local_flags         true     local flags are ignored; the server is the authority

store_config:
    server_validation        true     purchases are checked server-side
```

The profile itself carries `HackCheck` (0 on a clean account) and
`Global.TimeOfLastUpload`. With `skip_local_flags: true`, editing `HackCheck` locally
accomplishes nothing -- the flag that counts lives on the account.

**The server keeps its own copy of the profile**, in
`Cache\com.ninjakiwi.link\<accountId>\Profile.save`, and it decodes with the same tool.
That makes a local edit trivially visible as a diff. Measured directly after setting cash to
fifty million locally:

```
local  Docs\<acct>\Profile.save     Money 50000000   TimeOfLastUpload 1787158712098
server Cache\<acct>\Profile.save    Money     6010   TimeOfLastUpload 1787090231542
```

So on the next sign-in one of two things happens: the local profile uploads and the server
sees cash multiplied by roughly eight thousand with no gameplay behind it, or the server
copy comes down and the edit is simply gone.

**What is publicly known**, from Ninja Kiwi's forums and the Steam discussions rather than
any official technical document -- there is no published specification:

- Ninja Kiwi describe an **auto-banner**, applied to the account rather than the machine.
- The checks players and staff describe are **value-plausibility** ones: skill values,
  weapon upgrade types, and HP above what the class allows.
- Reports of bans arriving well after the fact are consistent with a server-side batch
  process over uploaded profiles rather than an instant client check.

**Consequence for this project.** The save format being solved changes nothing about the
account risk: a valid checksum makes a file the *client* accepts, and the client was never
what enforced anything. Offline edits are safe only in the sense that no one can stop them;
whether the account survives depends on a server-side process nobody outside Ninja Kiwi has
documented.

## Masteries

Masteries are the passive upgrades that come from use: one track per weapon type, one per
armour piece, and a few for grenades, turrets and HD ammo.

```
MasteryProgress/MasteryProfileN   N = 0..5, one list per character slot
  [i] = {"MasteryXp": int, "MasteryLvl": int}   27 entries, every slot
```

Measured from a real profile and from 36 saves captured while playing. Two things follow
from the shape:

- **A track cannot be edited field by field.** A profile holds over 150 identical
  `"MasteryXp":0` fragments, so `anchor_for` cannot pin one down and refuses -- correctly,
  since a guess would overwrite a different track. The list as a whole has a unique anchor,
  so `mastery` replaces `MasteryProfileN` in one write, the same way `give` replaces
  `Claimed`.
- **XP and level are written together.** Which of the two the game reads is not known;
  writing both consistently is right either way, and a level its XP cannot reach is the
  mastery version of a level-40 character holding level-3 XP. `check` objects to it.

**The XP thresholds are the one part taken on trust.** 2,400 / 12,400 / 42,400 / 142,400 /
542,400 cumulative for levels 1 to 5, from the community wiki. Every track in every capture
sat at `MasteryLvl: 0` -- the highest XP seen anywhere was 1,253, below the first threshold
-- so the game has never been observed setting a level here, and none of this is measured
the way the container format is.

Measurement has put one bound on them. A track holding 1272 XP was still level 0, in the
file and on the game's own mastery screen alike, so the first threshold is above 1272. The
wiki's 2400 is consistent with that and nothing has tested it further. Playing a single
track past the boundary and watching `MasteryLvl` flip to 1 would settle the first
threshold outright, and is the obvious next measurement.

**The index numbering is being recovered one track at a time.** It is not in the data: the
item table carries categories (ten weapon, five equipment) but no numbering that lines up
with 27 tracks, and the `Type` field in `premium_info` indexes a different space -- its ids
collide with equipment ids, so it cannot be cross-referenced. What works is measurement.

| track | what it is | how |
|---|---|---|
| 9 | high damage ammo | the game's mastery screen read 1588 while index 9 held exactly 1588 |
| 0 | pistols | the mastery screen read 1272 while index 0 held 1272. Reached first by elimination -- two missions with pistols and nothing else moved only 0 and 9, by equal amounts -- then confirmed the direct way |

Nothing else is written down, because nothing else has been established. A wrong name here
would send an edit to the wrong track, which is worse than no name.

Two things fell out of the measurement:

- **The weapon track and the HD ammo track gain at the same rate.** Both moved +591 in one
  mission and +660 in the next, exactly together. Neither figure divides by the 6-per-kill
  the community wiki quotes, so that number does not describe what is in the file.
- **Track 11 moved in one mission and not the next**, +838 on its own while 0 and 9 moved
  together. That fits armour, which the wiki says awards one randomly chosen worn piece at
  mission end -- but it is a guess and is not recorded as anything.

The cheapest way to name another is to read a number off the game's own mastery screen and
find the track holding it in `py sas4.py mastery`: that names it outright rather than
inferring it. Failing that, play a mission with one weapon type and nothing else -- no
grenades, no turrets, which have their own tracks -- and `py sas4.py watch` reports the
index that moves.

One caution learned here: **the game holds mastery XP in memory and writes it on its own
schedule.** A mission's gain did not reach the file for eleven minutes. Closing the game
flushes it.

Setting these is the largest implausible jump these tools can make -- level 5 is on the
order of ninety thousand kills per track -- and it is a consistent edit, so no single-file
rule can object. Only the server can, and it has the account's history to compare against.

## Strongboxes: what can and cannot be done

Boxes come in nine tiers (Steel, Titanium, Molybdenum, Iridium, Neodymium, Promethium,
Thulium, Nantonium, Black) and drop from bosses and enemies, from daily rewards, and in a
limited store sale of three Titanium boxes. Black boxes need a Black Key and level 25.

- **Buying any box in the store is not a save edit.** The store is server-validated
  (`store_config.server_validation: true`), so what it sells and for what is decided
  server-side; changing the profile does not change the shop.
- **Spawning an unopened, openable box is not currently possible from the save.** No
  published tool (SWFplayer, dstvx, getshrekt10, 0daxelagnia) ever writes
  `Strongboxes.Unopened`; they all write `Claimed`. The Unopened schema is undocumented, and
  since boxes drop from gameplay rather than anything the save encodes, there is no reference
  to copy. Getting it would mean capturing a real one: `py sas4.py watch --archive` while a
  box is earned but not opened, then reading the diff.
- **What works instead, and is better: grant the finished item.** `py sas4.py give <id>`
  appends a finished item to `Claimed` -- the loot without the box, and without the box's
  randomness. Grade and bonus are yours to set. Weapon and equipment ids overlap, so
  `--kind` disambiguates.
- **`Claimed` is a hand-off queue, not the inventory.** It was described here as "the
  inventory of what you already own", which the captures disprove: over 30 boxes opened in
  one sitting, `Claimed` filled to three runs and drained back to empty within a few
  seconds each time, while `Weapons` and `Equipment` grew by exactly what left it and
  `StrongboxesOpened` counted up. A profile at rest has `Claimed: []` however much the
  character owns -- this one had 12 weapons and 20 pieces of equipment and an empty queue.
  So it is the right place to *put* a grant, since the game absorbs whatever is there, but
  the wrong place to *read* what a character owns: for that, read `Weapons` and `Equipment`,
  whose dicts carry the same fields plus `Seen` and, for equipment, `Equipped`.

### "Buying for free" -- asked, and why there is no such tool

There is no free-shop or zero-cost-purchase cheat, and the reason is the same server
validation. What the public trainers call "Infinite Money" is a runtime freeze of the cash
value in Cheat Engine, so spending does not decrease it -- not a special purchase path, just
never running out. It needs a pointer scan (the value is on the GC heap and moves; see
"Closed route 2"), which is Cheat Engine's job, not this toolset's, and is not a save edit.
The community warning is consistent with the server check: freeze and spend, but load with
negative money and the account is auto-banned.

The save-edit equivalent of "buy anything" is simply a high balance, which is done. Getting
an item without the store at all is `give`. A zero-cost purchase itself is exactly the
inconsistency the store's server-side validation exists to catch, which is why nobody ships
it.

Simple box-adjacent counters -- `AvailableBlackKeys`, `AvailableEliteAugmentCores`,
`Skills.AvailableBlackStrongboxes` -- are plain fields `set` can change.

## Identity, and grafting progress

Three places hold the account id and must agree:

```
Docs\<id>\Profile.save          field  Version.link
Docs\...\Live\current.session   field  user.nkapiID
the folder                       named  <id>
```

`current.session` is a DGDATA file like the save. Decoded it carries `session.sessionID`
(a login token the server issues) and `user.nkapiID` (the account id, which is also the
folder name). `py sas4.py session` prints it with the sessionID hidden; nkapiID is shown
because it is already the folder name in every path.

The sessionID is a credential the server hands out at login. **Editing text cannot forge
it**, so changing identity fields does not let a file act as another account on the server
-- it only lines the three places up when they have deliberately been changed together.

To play another player's public progress on your own account, copy the progress and keep
your identity: `py sas4.py graft <their.save> --fields <paths>`. It refuses to copy the
identity fields even when asked, so `Version/link` stays yours. Verified: grafting money
and name from a donor save changed both while `link` stayed the account's own.

## The model: generate and check (`sas4_model.py`)

A stand-in for a profile, so tools can be built and tested without a real save. It knows
the *relationships* `sas4.py` does not:

- XP must fall in the band for its level (the table cross-checks against a real save);
- available plus spent skill points may not exceed what the level grants -- **granted
  equals the level**, inferred from a real level-3 profile carrying three points
  (paygrade 2 + holdtheline 1, none unspent);
- HighestRank is at least the level; Money fits a 32-bit range;
- `Strongboxes.Claimed` is well-formed: each bare `0`/`1` tag is followed by an item dict.

```
py sas4_model.py generate out.save --level 20 --money 1000000
py sas4_model.py check <save> [--strict]
```

`generate` can place items: `--weapon <id>` and `--equip <id>:<slot>` write into
`Strongboxes.Claimed` with the run tagging the game uses (weapon = `0,{},8,0`; equipment =
`1,{}`), so a generated save exercises the strongbox rules too.

### The red/blue exercise

`py sas4_model.py redteam` applies a catalog of tampering strategies to a clean profile and
reports which `check` catches. It is a **detection-engineering** loop: a miss that is a real
inconsistency is a rule to add to `check`; run it again and the rule catches it.

The catalog also holds deliberately *consistent* edits -- a level raised with its XP, rank
and skill points all moved to match. `check` correctly does **not** flag those, and that is
the point of the whole file: a single-file check cannot tell a legitimately-levelled profile
from an edited one that is internally consistent. Only the server can, because only the
server knows the rate the level was reached at and how the account compares to millions of
others. A save that passes every local check is not a save the server must accept.

`check` is a registry of named rules, so a rule maps to the attacks it catches.
`redteam --progression` turns the rules on one at a time and prints coverage climbing, and
then flat, because the last attacks are consistent edits no single-file rule can reach. The
counts are not written down here: they move every time a rule or an attack is added, and a
number in prose goes stale silently. Run it to see where it stands.

`dataset out.jsonl --count N` writes labelled feature vectors (a consistent profile is
label 0, a tampered one label 1) for training a detector with ordinary ML. The features are
the quantities the rules use, turned into numbers. Trained on it, a classifier learns the
same boundary the rules draw; a plain `num_violations > 0` baseline already scores about
0.90, and the missing tenth is exactly the consistent-edit cases -- which is the same wall
the rules hit, and the reason a detector that only sees one file has a ceiling. Separating
those needs the rate a level was reached at and the shape of other accounts, which is
server-side data, not something a local model or a local rule can synthesise.

This is the blue side of the exercise expressed as ML: a detector you train and study. It
does not train anything to evade a live system, and could not usefully -- the signal that
would defeat the real anti-cheat is data this side never has.

`generate` writes a minimal profile that both checks clean and verifies. `check` lists every
inconsistency; these are the easy ones a server-side plausibility pass would also catch, so
passing them is necessary, not sufficient. Verified: the real profile and both genuine
samples check clean; an XP set to 999 at level 20 is caught.

## Environment notes that cost time to discover

- **The game runs at a higher integrity level than an ordinary shell.** Attaching needs
  elevation, and `taskkill` on it returns "Access is denied". `sas4_daemon.py` exists so an
  unelevated caller can drive a privileged worker through `control\cmd.json`. It supports
  `reload`, so edited code takes effect without another elevated console.
- **The display is scaled to 125% and there are two monitors.** Screen capture must set DPI
  awareness and use the **virtual desktop** bounds, or coordinates are off by a factor of
  1.25 and the game is missed entirely when it is on the second monitor.
- **The active keyboard layout is Thai.** A synthetic `g` carries the right key code (71)
  but produces `เ`. Only matters for anything reading typed text.
- The game window can sit slightly off-screen (`-9, 0, 978, 1089` on a 1080-high display)
  and `MoveWindow` does not move it. `gameinput.py rect` reports the geometry.
- `probe_list.py` writing sentinels into candidate addresses is a plausible contributor to
  a game freeze that happened during testing, even though every write was restored and
  verified. Prefer read-only work where possible.
