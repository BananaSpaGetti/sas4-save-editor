# Contributing to sas4-save-editor

Thanks for your interest. Contributions are welcome.

## Reporting a bug

Open an issue with:

- the command you ran and what happened
- your Windows version and Python version (`py --version`)
- whether the game was running at the time (edits are refused while it is)

Please do **not** attach your `Profile.save` or any file under `saves/`, `backups/`, or
`decoded/` — those carry your account id and personal data.

## Suggesting a feature

Open an issue describing what you want to read or change in a profile. The save format is
documented in `FINDINGS.md`; a feature that needs a field not yet mapped there is a good
issue to open with a sample of the (redacted) structure.

## Submitting a change

1. Fork the repository and create a branch.
2. Keep it to standard-library Python 3.
3. Test against a copy of a save, never the live profile — `sas4_model.py generate` makes a
   throwaway profile you can edit freely, and `sas4_model.py check` verifies consistency.
4. Run `py sas4.py verify <file>` after any change that writes, to confirm the checksum.
5. Open a pull request describing what changed and how you tested it.

## A note on account risk

A valid local checksum makes a file the game *client* accepts; the profile is still uploaded
to and validated by Ninja Kiwi's servers. This tool is for offline, single-player editing of
your own save. Please keep contributions within that purpose, and see the "How the game
detects tampering" section of `FINDINGS.md`.
