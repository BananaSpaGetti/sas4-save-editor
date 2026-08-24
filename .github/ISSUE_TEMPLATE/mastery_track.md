---
name: Mastery track, or a reading from your save
about: Name a mastery track, pin a level threshold, or report a field nobody has documented
labels: data
---

Twenty-five of the twenty-seven mastery tracks have no name. The level thresholds are
bounded but not measured. Both are answered by readings from more than one account, which is
the only thing one person cannot produce alone.

**Please do not attach your save file.** It carries your account id, and there is nothing in
it this needs that the report below does not already have.

### If you know what a track is

The most useful thing anyone can send. Two ways to find out, either is fine:

- Play one mission using only one weapon type, then run `py tools/sas4.py mastery` and see
  which row moved.
- Read a number off the game's own mastery screen, run the same command, and find the track
  holding exactly that number.

| track number | what it is | how you know |
|---|---|---|
| | | |

### If you watched a track cross a level

`MasteryLvl` going from 0 to 1 pins the first threshold, which is currently only known to be
somewhere above 1272.

- XP just before it changed:
- XP just after:
- The level it changed to:

### The report

`py tools/sas4.py contribute` writes and prints one. It contains no account id, no player
name and no file path — it prints the whole thing so you can read it before deciding. Paste
it here.

<!-- paste the contents of contribution.md below this line -->

```

```

### Anything else

If a field in the "Every path in the file" section looks like something you recognise from
the game, say so. That section is names and types only, and half of it is unexplained.
