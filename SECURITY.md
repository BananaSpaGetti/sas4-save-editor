# Security Policy

## Supported versions

Only the latest commit on the default branch is supported; fixes are applied there.

## Reporting a vulnerability

Please report security issues **privately**, not in a public issue.

- Preferred: open a private advisory through GitHub — the repository's **Security** tab →
  **Report a vulnerability**.
- Alternatively, contact the maintainer through GitHub.

Please include what the issue is, how to reproduce it, and the impact you see.

When reporting, do **not** attach a real `Profile.save` or anything from `saves/`,
`backups/`, or `decoded/`: those carry an account id and personal data. A minimal, redacted
example is enough.

## Scope

This tool reads, edits, and re-checksums your own local save file. In scope are things like
a crash, a write that corrupts a save the tool should have left valid, or a path traversal
in how a file argument is handled. Out of scope: that a valid checksum lets the game *client*
accept an edited save — that is the tool's purpose, and the account-side risk of editing a
server-validated profile is documented in `FINDINGS.md`, not a vulnerability in this code.
