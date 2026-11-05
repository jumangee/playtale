## Git Workflow and Repository Awareness

### Repository Detection Protocol

This repository is a fork. `origin` is the PlayTale fork (`jumangee/playtale`) and
`upstream` is the original CrossPoint Reader project. Use the session's branch,
remote, and status checks; inspect their roles rather than assuming a name
establishes one.

### Fork Maintenance

Upstream is a read-only source of fixes to merge into the fork. Never push to it.

- Keep the fork diff narrow. Work that the fork intends to keep forever should be
  separable from fork-specific policy and UI, so an upstream merge does not turn
  into a rewrite.
- Do not reformat or mass-rename code the fork does not intend to own. A
  whitespace or rename sweep across a shared file conflicts with every upstream
  change to that file from then on. Line endings are normalized repo-wide by
  `.gitattributes`; do not reintroduce CRLF.
- Do not edit a vendored subtree or the `freeink-sdk` submodule to carry fork
  behavior. Check `git submodule status` and account for a different local
  revision instead of treating it as the pinned dependency.
- When a rebrand renames an identifier that a stored format, wire protocol, or
  public API depends on, that is a code change with its own verification, not a
  documentation edit. See the code-identifier table in the root guide.
- Merges from upstream are deliberate, reviewed work. Resolve conflicts by
  keeping the fork's product decisions and upstream's fixes; do not "take
  upstream" wholesale on a file the fork has intentionally changed.

### Git Operation Rules

- Integration and comparisons target the fork's `develop`.
- Authorization follows [human ownership](../../AGENTS.md#human-ownership).
  Before an explicitly requested push, recheck remotes and use the destination
  specified by the human; verify the URL rather than assuming a name establishes
  its role. Ask if the destination is unclear.
- No assistant co-author or generated-by trailers.
- When adapting/superseding another person's change, verify the original human
  author and credit them with `Co-Authored-By`; skip bot authors.

### Branch Naming Convention

Use `<type>/<short-description>`, where type is `feat`, `fix`, `refactor`,
`docs`, `test`, `chore`, or `perf`.

### Commit Message Format

Use `<type>: <short summary>` (summary at most 50 characters), with an optional
body containing relevant facts and verification. Types match branch prefixes.

### When to Commit

A local commit requires explicit human approval, completed requested work,
relevant passing checks, and [firmware-handoff](../skills/firmware-handoff/SKILL.md)
for firmware/build changes. A pure refactor preserves behavior. Do not commit
experiments, failed builds, build warnings, or ignored outputs; inspect status
and `.gitignore` before staging. Exclude unrelated user work. Ask if commit
authorization is unclear.

Hardware testing is required before a release, not before an approved local
commit. The human owns hardware results and release prose/publication.
